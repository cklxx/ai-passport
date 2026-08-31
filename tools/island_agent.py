#!/usr/bin/env python3
"""Claude usage island — PC data source.

The merged 7-day quota number only exists on the machine running Claude Code,
in the statusline stdin JSON under `rate_limits.seven_day`. This script turns
that into the 7-byte packet the device parses (see main/island_quota.h).

Two roles, one file:

  1. Statusline hook (Claude Code calls it, feeds JSON on stdin):
        island_agent.py statusline [--emit PATH]
     Prints your normal statusline to stdout AND writes the latest packet to
     --emit (default ~/.claude/island_quota.bin). Claude Code only sends
     rate_limits to Pro/Max subscribers, so on a plain API key it emits the
     "unknown" packet — the device shows 未知 rather than a stale ring.

  2. Forwarder (send the packet over the device link):
        island_agent.py send --port /dev/tty.usbmodem1234   # USB-serial
     Reads --emit and writes the 7 raw bytes to the serial port. BLE transport
     is device-firmware-dependent; USB-serial needs no extra device RAM and
     shares Path-B's cable. Run this on a timer (cron/launchd) or loop.

Wire format is the single source of truth in main/island_quota.h. Keep the two
in sync; test_island_quota.c is the device-side check, `selftest` here is ours.

    island_agent.py selftest    # asserts pack/round-trip, no hardware
"""
import argparse
import json
import os
import struct
import sys

MAGIC = 0x51
UNKNOWN = 0xFF
DEFAULT_EMIT = os.path.expanduser("~/.claude/island_quota.bin")


def pack(used_percentage, resets_at):
    """Build the 7-byte packet. used_percentage None -> unknown packet."""
    if used_percentage is None:
        used, resets = UNKNOWN, 0
    else:
        used = max(0, min(100, round(used_percentage)))
        resets = int(resets_at or 0) & 0xFFFFFFFF
    body = struct.pack("<BBI", MAGIC, used, resets)  # 6 bytes, little-endian
    xor = 0
    for b in body:
        xor ^= b
    return body + bytes([xor])


def _seven_day(rate_limits):
    """Extract (used_percentage, resets_at) from the seven_day bucket, or None."""
    if not isinstance(rate_limits, dict):
        return None, None
    bucket = rate_limits.get("seven_day")
    if not isinstance(bucket, dict):
        return None, None
    used = bucket.get("used_percentage")
    resets = bucket.get("resets_at")
    if not isinstance(used, (int, float)):
        return None, None
    # resets_at may be a unix int or an ISO-8601 string; accept the int form and
    # fall back to 0 (device shows the ring without a countdown) otherwise.
    if isinstance(resets, (int, float)):
        return used, int(resets)
    return used, 0


def cmd_statusline(args):
    try:
        data = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        data = {}
    used, resets = _seven_day(data.get("rate_limits"))
    try:
        os.makedirs(os.path.dirname(args.emit) or ".", exist_ok=True)
        with open(args.emit, "wb") as f:
            f.write(pack(used, resets))
    except OSError as e:
        print(f"island: emit failed: {e}", file=sys.stderr)
    # Preserve a usable statusline. Keep it minimal; users can extend.
    model = (data.get("model") or {}).get("display_name", "Claude")
    remaining = "余量未知" if used is None else f"7天剩余 {100 - round(used)}%"
    print(f"{model} · {remaining}")
    return 0


def cmd_send(args):
    try:
        with open(args.emit, "rb") as f:
            packet = f.read()
    except OSError as e:
        print(f"island: no packet to send: {e}", file=sys.stderr)
        return 1
    if len(packet) != 7:
        print(f"island: bad packet length {len(packet)}", file=sys.stderr)
        return 1
    try:
        import serial  # pyserial; only needed for the send role
    except ImportError:
        print("island: pip install pyserial for USB-serial send", file=sys.stderr)
        return 1
    with serial.Serial(args.port, args.baud, timeout=1) as ser:
        ser.write(packet)
    return 0


def cmd_recv(args):
    """Receive the device's voice UDP stream: PCM -> virtual mic, ctrl -> keys.

    Streaming design: a UDP thread fills a ring buffer; a sounddevice callback
    drains it at the device's own rate (silence on underrun). This decouples
    network jitter from audio timing so the mic stays smooth. Audio is 16 kHz
    mono; it is fed to a virtual input device (BlackHole on macOS, VB-Cable on
    Windows). Set that device as the system default microphone and 豆包/any app
    picks it up automatically. Control packets inject Enter (发送)/Backspace (删除).
    Wire format mirrors main/voice_proto.h.
    """
    import socket
    import threading
    MAGIC, T_AUDIO = 0x56, 0x00
    CTRL = {1: "enter", 2: "backspace", 3: "start", 4: "stop"}
    RATE = 16000

    try:
        import numpy as np
        import sounddevice as sd
    except ImportError:
        print("island: pip install numpy sounddevice for recv", file=sys.stderr)
        return 1
    try:
        import pyautogui
    except ImportError:
        pyautogui = None

    device = args.device
    if device is None:
        # Auto-detect a virtual audio device by name.
        for i, d in enumerate(sd.query_devices()):
            name = d["name"].lower()
            if d["max_output_channels"] > 0 and (
                    "blackhole" in name or "cable" in name or "vb-audio" in name):
                device = i
                print(f"island: auto-selected virtual device [{i}] {d['name']}")
                break
        if device is None:
            print("island: no BlackHole/VB-Cable found; pass --device or install one",
                  file=sys.stderr)
            return 1

    # Ring buffer of int16 samples, guarded by a lock. ~2 s capacity.
    ring = np.zeros(RATE * 2, dtype=np.int16)
    w = 0  # write index
    r = 0  # read index
    lock = threading.Lock()

    def available():
        return (w - r) % len(ring)

    def audio_cb(outdata, frames, time_info, status):
        nonlocal r
        with lock:
            have = available()
            n = min(frames, have)
            if n:
                end = r + n
                if end <= len(ring):
                    outdata[:n, 0] = ring[r:end]
                else:                       # wrap
                    first = len(ring) - r
                    outdata[:first, 0] = ring[r:]
                    outdata[first:n, 0] = ring[:n - first]
                r = end % len(ring)
        if n < frames:
            outdata[n:, 0] = 0  # underrun -> silence, never block

    stream = sd.OutputStream(samplerate=RATE, channels=1, dtype="int16",
                             device=device, blocksize=320, callback=audio_cb)
    stream.start()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", args.port))
    print(f"island: streaming udp/{args.port} -> device {device} (Ctrl+C to stop)")

    def read_quota_packet():
        try:
            with open(args.emit, "rb") as f:
                p = f.read()
            return p if len(p) == 7 else None
        except OSError:
            return None

    pkt_count = 0
    try:
        while True:
            data, src = sock.recvfrom(2048)
            if len(data) < 2 or data[0] != MAGIC:
                continue
            if data[1] == T_AUDIO and len(data) > 4:
                pcm = np.frombuffer(data[4:], dtype="<i2")
                m = len(pcm)
                with lock:
                    end = w + m
                    if end <= len(ring):
                        ring[w:end] = pcm
                    else:
                        first = len(ring) - w
                        ring[w:] = pcm[:first]
                        ring[:m - first] = pcm[first:]
                    w = end % len(ring)
                    if available() < m:      # overran: keep read behind write
                        r = (w + 1) % len(ring)
                pkt_count += 1
                if pkt_count % 250 == 1:
                    q = read_quota_packet()
                    if q:
                        sock.sendto(q, (src[0], args.port))
            elif data[1] in CTRL:
                action = CTRL[data[1]]
                if action in ("enter", "backspace"):
                    if pyautogui:
                        pyautogui.press(action)
                    else:
                        print(f"island: {action} (pip install pyautogui to inject)",
                              file=sys.stderr)
    except KeyboardInterrupt:
        pass
    finally:
        stream.stop(); stream.close(); sock.close()
    return 0


# BLE UUIDs must match main/voice_ble.c.
BLE_NAME = "AI-Passport-Mic"
BLE_UUID_AUDIO = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
BLE_UUID_CTRL = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"


def cmd_recv_ble(args):
    """Connect to the device over BLE, stream its PCM audio to a virtual mic.

    The device has no network in its use environment, so audio arrives over BLE
    GATT notifications (see main/voice_ble.c) rather than Wi-Fi. Frames are raw
    16 kHz 16-bit PCM (stateless: a dropped notify costs one frame, not the whole
    stream) and are upsampled to the virtual device's native rate (BlackHole runs
    at 48 kHz — mismatched rates play back as garbled 3x-speed audio). Control
    notifications hold 豆包's push-to-talk key and inject Enter/Backspace.
    Same virtual-mic setup as `recv`: set the device as the system default mic.
    """
    import asyncio
    import threading
    try:
        import numpy as np
        import sounddevice as sd
    except ImportError:
        print("island: pip install numpy sounddevice for recv-ble", file=sys.stderr)
        return 1
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        print("island: pip install bleak for recv-ble", file=sys.stderr)
        return 1
    try:
        import pyautogui
    except ImportError:
        pyautogui = None

    SRC_RATE = 16000                              # device audio rate
    device = args.device
    if device is None:
        for i, d in enumerate(sd.query_devices()):
            name = d["name"].lower()
            if d["max_output_channels"] > 0 and (
                    "blackhole" in name or "cable" in name or "vb-audio" in name):
                device = i
                print(f"island: auto-selected virtual device [{i}] {d['name']}")
                break
        if device is None:
            print("island: no BlackHole/VB-Cable found; pass --device", file=sys.stderr)
            return 1

    # BlackHole runs at its own native rate (typically 48 kHz). If we open the
    # stream at 16 kHz but the device clocks at 48 kHz, any app reading it hears
    # 3x-speed garbage. Open at the device's native rate and upsample the 16 kHz
    # device audio to match (integer factor, sample-repeat — cheap and clean).
    RATE = int(round(sd.query_devices()[device]["default_samplerate"]))
    UP = max(1, RATE // SRC_RATE)                 # 48000 // 16000 = 3

    ring = np.zeros(RATE // 2, dtype=np.int16)   # 500 ms cap
    LEAD = RATE // 200                            # ~5 ms target latency
    w = r = 0
    lock = threading.Lock()

    def audio_cb(outdata, frames, time_info, status):
        nonlocal r
        try:
            n = 0
            with lock:
                have = (w - r) % len(ring)
                n = min(frames, have)
                if n:
                    end = r + n
                    if end <= len(ring):
                        chunk = ring[r:end]
                    else:
                        first = len(ring) - r
                        chunk = np.concatenate((ring[r:], ring[:n - first]))
                    # BlackHole is 2-ch: write the mono stream to every channel,
                    # or an app reading the right/stereo channel gets silence.
                    for c in range(outdata.shape[1]):
                        outdata[:n, c] = chunk
                    r = end % len(ring)
            if n < frames:
                outdata[n:, :] = 0
        except Exception as e:            # a raised callback is silently disabled
            print(f"island: audio_cb error: {e!r}", file=sys.stderr)
            outdata[:] = 0

    ch = 2 if sd.query_devices()[device]["max_output_channels"] >= 2 else 1
    stream = sd.OutputStream(samplerate=RATE, channels=ch, dtype="int16",
                             device=device, blocksize=RATE // 100, callback=audio_cb)
    stream.start()

    dump_frames = []                              # raw 16k PCM, for --dump analysis

    def on_audio(_char, data):
        nonlocal w, r
        # Raw 16-bit PCM from the device (stateless; no ADPCM decode needed).
        pcm = np.frombuffer(bytes(data), dtype="<i2")
        if args.dump:
            dump_frames.append(pcm.copy())
        if UP > 1:
            pcm = np.repeat(pcm, UP)     # 16 kHz -> device rate
        m = len(pcm)
        with lock:
            end = w + m
            if end <= len(ring):
                ring[w:end] = pcm
            else:
                first = len(ring) - w
                ring[w:] = pcm[:first]
                ring[:m - first] = pcm[first:]
            w = end % len(ring)
            # Low-latency: never let backlog grow. If the reader is falling
            # behind, snap it to a small fixed lead (~60 ms) behind the writer,
            # dropping stale audio so 豆包 sees a live stream, not a delayed one.
            backlog = (w - r) % len(ring)
            if backlog > LEAD:
                r = (w - LEAD) % len(ring)

    def on_ctrl(_char, data):
        nonlocal w, r
        if not data:
            return
        code = data[0]
        # START/STOP hold the 豆包 push-to-talk key (right Option) for exactly the
        # recording window: press on START, release on STOP. SEND/DELETE are
        # one-shot Enter/Backspace.
        if code == 3:                       # VOICE_CTRL_START
            # Clear the ring so a new take starts from silence, not stale audio.
            with lock:
                w = r = 0
            if pyautogui: pyautogui.keyDown("optionright")
            return
        if code == 4:                       # VOICE_CTRL_STOP
            if pyautogui: pyautogui.keyUp("optionright")
            return
        action = {1: "enter", 2: "backspace"}.get(code)
        if action and pyautogui:
            pyautogui.press(action)
        elif action:
            print(f"island: {action} (pip install pyautogui to inject)",
                  file=sys.stderr)

    async def run():
        print(f"island: scanning for {BLE_NAME} ...")
        dev = await BleakScanner.find_device_by_name(BLE_NAME, timeout=15.0)
        if dev is None:
            print("island: device not found (is it advertising?)", file=sys.stderr)
            return 1
        async with BleakClient(dev) as client:
            print(f"island: connected {dev.address}; streaming (Ctrl+C to stop)")
            await client.start_notify(BLE_UUID_AUDIO, on_audio)
            await client.start_notify(BLE_UUID_CTRL, on_ctrl)
            # Push the Claude quota packet over the same link: the statusline hook
            # keeps args.emit fresh; write it to the control characteristic so the
            # device's island updates. Every 30 s (and once right away).
            last_q = None
            tick = 0
            while client.is_connected:
                if tick % 60 == 0:          # 60 * 0.5 s = 30 s
                    try:
                        with open(args.emit, "rb") as f:
                            q = f.read()
                        if len(q) == 7 and q != last_q:
                            await client.write_gatt_char(BLE_UUID_CTRL, q, response=False)
                            last_q = q
                    except OSError:
                        pass
                tick += 1
                await asyncio.sleep(0.5)
        return 0

    try:
        return asyncio.run(run())
    except KeyboardInterrupt:
        return 0
    finally:
        stream.stop(); stream.close()
        if args.dump and dump_frames:
            import wave
            pcm = np.concatenate(dump_frames)
            with wave.open(args.dump, "wb") as wf:
                wf.setnchannels(1); wf.setsampwidth(2); wf.setframerate(SRC_RATE)
                wf.writeframes(pcm.tobytes())
            print(f"island: dumped {len(pcm)/SRC_RATE:.1f}s raw 16k PCM to {args.dump}",
                  file=sys.stderr)


def cmd_selftest(_args):
    # Round-trip and edge cases must match main/island_quota.c exactly.
    p = pack(30, 1893456000)
    assert len(p) == 7 and p[0] == MAGIC and p[1] == 30
    assert struct.unpack("<I", p[2:6])[0] == 1893456000
    assert p[6] == (p[0] ^ p[1] ^ p[2] ^ p[3] ^ p[4] ^ p[5])
    assert pack(None, 0)[1] == UNKNOWN
    assert pack(150, 0)[1] == 100 and pack(-5, 0)[1] == 0   # clamped
    # seven_day extraction
    assert _seven_day({"seven_day": {"used_percentage": 40, "resets_at": 9}}) == (40, 9)
    assert _seven_day({"seven_day": {}}) == (None, None)
    assert _seven_day({}) == (None, None)
    assert _seven_day(None) == (None, None)
    print("island_agent selftest: PASS")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("statusline", help="Claude Code statusline hook")
    s.add_argument("--emit", default=DEFAULT_EMIT)
    s.set_defaults(func=cmd_statusline)

    d = sub.add_parser("send", help="forward the packet over USB-serial")
    d.add_argument("--port", required=True)
    d.add_argument("--baud", type=int, default=115200)
    d.add_argument("--emit", default=DEFAULT_EMIT)
    d.set_defaults(func=cmd_send)

    r = sub.add_parser("recv", help="receive voice UDP -> virtual mic + keys")
    r.add_argument("--port", type=int, default=55123)
    r.add_argument("--device", default=None,
                   help="output device name/index (VB-Cable 'CABLE Input')")
    r.add_argument("--emit", default=DEFAULT_EMIT,
                   help="quota packet file to push back to the device")
    r.set_defaults(func=cmd_recv)

    rb = sub.add_parser("recv-ble", help="receive voice over BLE -> virtual mic + keys")
    rb.add_argument("--device", default=None,
                    help="output device name/index (BlackHole/VB-Cable)")
    rb.add_argument("--emit", default=DEFAULT_EMIT,
                    help="quota packet file to push to the device over BLE")
    rb.add_argument("--dump", default=None,
                    help="write received raw 16k PCM to this WAV on exit (debug)")
    rb.set_defaults(func=cmd_recv_ble)

    t = sub.add_parser("selftest", help="assert pack/extract, no hardware")
    t.set_defaults(func=cmd_selftest)

    args = parser.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
