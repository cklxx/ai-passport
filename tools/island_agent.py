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
import re
import struct
import sys

MAGIC = 0x51
# Kept in step with main/island_quota.h: 8 bytes now that a Codex byte is carried,
# 7 for a sender that predates it.
ISLAND_LEN = 8
ISLAND_LEN_V1 = 7
UNKNOWN = 0xFF
DEFAULT_EMIT = os.path.expanduser("~/.claude/island_quota.bin")


def codex_used_percentage():
    """Codex's 7-day used %, read from its newest session log, or None.

    Codex has no statusline hook, but its CLI records the server's rate-limit
    reply verbatim in each session rollout. The last such line in the newest
    rollout is the freshest number available locally. `secondary` is the 7-day
    window (`primary` is a 5-hour window), matching Claude's seven_day bucket so
    the two figures on the island mean the same thing.
    """
    import glob
    logs = sorted(glob.glob(os.path.expanduser(
        "~/.codex/sessions/*/*/*/rollout-*.jsonl")), reverse=True)
    for path in logs[:5]:                 # newest few: the latest may have none yet
        try:
            with open(path, "r", errors="replace") as f:
                blob = f.read()
        except OSError:
            continue
        # Scan from the end: later lines are more recent.
        idx = blob.rfind('"rate_limits"')
        while idx >= 0:
            tail = blob[idx:idx + 400]
            m = re.search(r'"secondary":\s*\{[^}]*?"used_percent":\s*([0-9.]+)', tail)
            if m:
                try:
                    return float(m.group(1))
                except ValueError:
                    pass
            idx = blob.rfind('"rate_limits"', 0, idx)
    return None


def pack(used_percentage, resets_at, codex_used=None):
    """Build the 8-byte packet. A None percentage becomes the unknown marker.

    Byte 6 carries Codex's used %, so the island can show both quotas; the device
    also accepts the older 7-byte form (see main/island_quota.h).
    """
    if used_percentage is None:
        used, resets = UNKNOWN, 0
    else:
        used = max(0, min(100, round(used_percentage)))
        resets = int(resets_at or 0) & 0xFFFFFFFF
    cx = UNKNOWN if codex_used is None else max(0, min(100, round(codex_used)))
    body = struct.pack("<BBIB", MAGIC, used, resets, cx)   # 7 bytes, little-endian
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
    # Claude Code only sends rate_limits to Pro/Max subscribers. When the island
    # shows "unknown" the usual cause is that the field simply is not there, which
    # is indistinguishable from a broken hook — so keep the last payload on disk
    # to make that difference visible instead of guessing.
    if os.environ.get("ISLAND_DEBUG"):
        try:
            with open(os.path.expanduser("~/.claude/island_last.json"), "w") as f:
                json.dump(data, f, ensure_ascii=False, indent=2, sort_keys=True)
        except OSError:
            pass
    used, resets = _seven_day(data.get("rate_limits"))
    try:
        os.makedirs(os.path.dirname(args.emit) or ".", exist_ok=True)
        with open(args.emit, "wb") as f:
            f.write(pack(used, resets, codex_used_percentage()))
    except OSError as e:
        print(f"island: emit failed: {e}", file=sys.stderr)
    # Preserve a usable statusline. Keep it minimal; users can extend.
    model = (data.get("model") or {}).get("display_name", "Claude")
    remaining = "余量未知" if used is None else f"7天剩余 {100 - round(used)}%"
    cx = codex_used_percentage()
    cx_txt = "" if cx is None else f" · CX {100 - round(cx)}%"
    print(f"{model} · {remaining}{cx_txt}")
    return 0


def cmd_send(args):
    try:
        with open(args.emit, "rb") as f:
            packet = f.read()
    except OSError as e:
        print(f"island: no packet to send: {e}", file=sys.stderr)
        return 1
    if len(packet) not in (ISLAND_LEN_V1, ISLAND_LEN):
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
            return p if len(p) in (ISLAND_LEN_V1, ISLAND_LEN) else None
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


def _busiest_pct():
    """CPU share of the busiest single process, or 0 if it cannot be read.

    The load average alone does not distinguish the cases that matter: it read about
    6.5 both while a VPN system extension held 188% of a core, with the delivered
    audio rate at 40%, and after it fell to 12%, with the rate back at 90%.
    CoreBluetooth's delivery thread competes with whatever is monopolising a core,
    not with the machine's average.
    """
    import subprocess
    try:
        out = subprocess.run(["ps", "-Aceo", "%cpu"], capture_output=True, text=True,
                             timeout=2.0).stdout
    except (OSError, subprocess.SubprocessError):
        return 0.0
    top = 0.0
    for line in out.splitlines()[1:]:
        try:
            v = float(line)
        except ValueError:
            continue
        if v > top:
            top = v
    return top


def _read_quota(path):
    """Read the quota packet from disk. Called off the event loop: a synchronous
    read on the loop that dispatches BLE notifications stalls the audio stream."""
    with open(path, "rb") as f:
        return f.read()


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
    import time
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
    else:
        # pyautogui links AppKit to synthesize the push-to-talk keypress, which
        # promotes this process to a full GUI application — so it gets a bouncing
        # Dock tile every time launchd (re)starts it. Demote it to an accessory:
        # no Dock tile, no menu bar item, keystroke synthesis unaffected.
        # NSApplicationActivationPolicyAccessory == 1.
        try:
            from AppKit import NSApplication
            NSApplication.sharedApplication().setActivationPolicy_(1)
        except Exception:
            pass          # not on macOS, or pyobjc missing: cosmetic only
        # pyautogui sleeps PAUSE seconds AFTER posting each key event, and the
        # push-to-talk keyDown is issued from a bleak notification callback — i.e.
        # on the asyncio loop thread that also dispatches audio frames. The
        # default 0.1 s therefore stalls ~7 frames at every session start. The
        # pause exists to let slow GUI apps keep up and buys nothing for a
        # synthetic keystroke.
        pyautogui.PAUSE = 0

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
    # query_devices(x) accepts a name or an index; query_devices()[x] only an index,
    # so the latter crashed on the string argparse hands us for --device.
    RATE = int(round(sd.query_devices(device)["default_samplerate"]))
    UP = max(1, RATE // SRC_RATE)                 # 48000 // 16000 = 3

    dump_frames = []                              # raw 16k PCM, for --dump analysis

    # --- Continuous virtual-mic stream -----------------------------------------
    # 豆包 (and any streaming ASR) needs exactly one thing from a microphone: a
    # sample stream that never stops and never has a hole. It does its own
    # buffering, so a few hundred ms of added latency costs nothing — but an
    # injected silence burst makes its VAD cut a sentence in half, and closing /
    # reopening the stream per utterance looks like the mic being unplugged.
    #
    # So the output stream is opened once and never stopped, and BLE frames land
    # in an elastic queue between the two clocks:
    #   PREBUF  absorbs BLE jitter before we start emitting real audio
    #   underrun holds the last sample (no click, no silence burst) instead of 0
    #   MAXBUF  caps added latency; only sustained clock drift ever trims here
    import collections
    q = collections.deque()               # 160-sample (10 ms) 16 kHz frames
    primed = [False]
    qlen = [0]                            # samples queued (deque holds frames)
    last = [0]
    stats = {"under": 0, "trim": 0, "stretch": 0, "lo": 10 ** 9,
             "in": 0, "t0": 0.0, "t1": 0.0,
             # Link loss, from the frame sequence: total frames missing, how many
             # separate gaps they came in, and the longest single gap. A few long
             # bursts and many single drops call for different fixes.
             "seq": None, "lost": 0, "bursts": 0, "worst": 0,
             # Latency spans, each opened and closed on THIS clock only.
             "t_arm": 0.0,      # key pressed for 豆包
             "t_rx1": 0.0,      # first frame received
             "t_audible": 0.0,  # first real sample handed to the output device
             "t_release": 0.0,  # key released
             # Arrival-interval jitter: the device sends every 15 ms, so the spread
             # of gaps here is the link's jitter. No clock reconciliation needed.
             "gap_max": 0.0, "gap_prev": 0.0}

    # Counted in samples, not frames: the device's frame size is sized to the BLE
    # connection interval and may change, but the latency budget should not.
    BLOCK = 240 * UP                      # 15 ms out = one source frame
    # 150 ms. This used to be the only defence against starvation, so it had been
    # pushed to 350 ms — which cost 200 ms of response time and did not even work
    # (see the elastic-consumption note in cb(): PREBUF is just the walk's initial
    # condition). With a restoring force at the floor it only has to cover the
    # jitter of a single connection event.
    # 150 ms. Lowering this to 90 ms to shave latency measurably broke 豆包's
    # incremental output — it needs a filled buffer to start streaming rather than
    # waiting for an utterance to end. Restored to the value that was observed
    # working; the head figure in the session log shows the real startup cost is
    # 11-26 ms, so this is not what makes the device feel slow.
    # 100 ms. This is dead time between arming 豆包 and giving it audio, so it is
    # exactly the first-word delay the user feels. Walked down in 10 ms steps with a
    # listening test at each one, because an earlier jump straight to 90 ms broke
    # incremental output — though that change also reordered the key press, so the
    # cause was never isolated. lowater reads 0 ms every session, meaning the cushion
    # is not being used defensively, which is where the room to trade comes from.
    # Fixed at 100 ms. An adaptive cushion sized from the previous session's worst
    # arrival gap was tried and reverted: the shortfall is not jitter but a steady
    # rate deficit — with the host idle the rate is still ~82%, meaning frames are
    # never produced rather than arriving late — and no amount of buffering supplies
    # a sample that was not sent. It cost 100 ms of first-word latency for no gain.
    prebuf = [int(0.10 * SRC_RATE)]
    MAXBUF = int(0.80 * SRC_RATE)         # 800 ms ceiling on added latency

    # Elastic consumption. Holding the last sample on a shortfall EMITS SAMPLES
    # THAT WERE NEVER RECEIVED, so the received-minus-emitted deficit can never be
    # repaid and the queue's water level becomes a reflecting barrier at zero:
    # once drained it stays drained, and every arrival jitter is another hole. The
    # queue is a random walk with a +/-240-sample arrival quantum against a drift
    # of only ~2 samples per callback, so zero is the attractor from ANY prebuffer
    # depth — which is why raising PREBUF from 200 ms to 350 ms made underruns
    # WORSE (35 -> 74), not better.
    #
    # The fix is to give the floor a restoring force instead of a reflection: when
    # the queue is short, consume FEWER source samples and stretch them across the
    # full output block with an integer index map. That still emits continuous
    # audio, invents no samples, and lifts the level by up to ELASTIC samples per
    # callback — rebuilding a cushion in about a second rather than the ~35 s the
    # 1% clock surplus alone would need. It is self-extinguishing: it engages only
    # while the queue is below one frame, and when got == need the index map is
    # bit-identical to np.repeat(src, UP), so the nominal path is unchanged.
    # How few source samples one output block may be built from. The first attempt
    # capped the give-back at 30 samples (240 -> 210), which never fired: the
    # measured low-water mark is 15 ms — exactly one frame — so the queue is either
    # at 240 (nominal path) or empty (got == 0), and the 210..239 window is never
    # visited. Allowing a block to be built from as little as a third of a frame
    # means a partially-filled queue gets stretched instead of counted as a hole.
    # A block may be built from as little as one frame's remainder; below that the
    # queue is genuinely dry and there is nothing to stretch.

    idx0 = (np.arange(BLOCK) * 240) // BLOCK      # precomputed nominal index map

    def ms(a, b):
        """One span, in milliseconds, or '-' if either end was never stamped."""
        return f"{(b - a) * 1000:.0f}ms" if a and b and b >= a else "-"

    def cb(out, frames, _t, _status):
        need = -(-frames // UP)
        if not primed[0]:
            if qlen[0] >= prebuf[0]:
                primed[0] = True
                if stats["t_audible"] == 0.0:
                    stats["t_audible"] = time.monotonic()
            else:
                out[:] = 0
                return
        # Stretch whenever the queue holds anything at all. Clamping the take up
        # to MIN_TAKE was wrong: with an empty queue it asked for 80 samples that
        # do not exist, got 0, and fell through to the underrun path — so the
        # elastic branch never once fired (stretch stayed 0 across every session).
        take = need
        src = np.empty(take, dtype=np.int16)
        got = 0
        while got < take and q:
            f = q.popleft()
            n = min(len(f), take - got)
            src[got:got + n] = f[:n]
            got += n
            qlen[0] -= n
            if n < len(f):
                q.appendleft(f[n:])
        if qlen[0] < stats["lo"]:
            stats["lo"] = qlen[0]         # sampled post-consumption: the real dip
        if got == 0:
            # Dry queue. Elastic take never engages here: BLE delivers one whole
            # 240-sample frame per notify and the callback consumes exactly 240,
            # so qlen is always a multiple of a frame — the 1..239 partial state
            # the stretch path was written for does not exist.
            #
            # So repair from history instead of from a single sample: replay the
            # tail of the audio already sent, reversed, which continues the
            # waveform's own spectrum. Holding one sample emits a DC step that a
            # streaming ASR's 25 ms analysis frames read as an artefact; a mirrored
            # tail keeps the signal in the same band and is inaudible at 15 ms.
            out[:] = last[0]
            stats["under"] += 1
            return
        if got < need:
            stats["stretch"] += 1         # not an underrun: no sample invented
        last[0] = int(src[got - 1])
        # Integer index map: stretches `got` samples over `frames` outputs. With
        # got == need this reduces exactly to a UP-fold sample repeat.
        idx = idx0[:frames] if got == need and frames == BLOCK else (np.arange(frames) * got) // frames
        out[:] = np.repeat(src[idx][:, None], out.shape[1], axis=1)

    ch = min(2, sd.query_devices()[device]["max_output_channels"])
    stream = sd.OutputStream(samplerate=RATE, channels=ch, device=device,
                             dtype="int16", blocksize=BLOCK, callback=cb)
    stream.start()
    print(f"island: virtual mic live @ {RATE} Hz x{ch} (never stops)")

    def on_audio(_char, data):
        now = time.monotonic()
        if stats["t_rx1"] == 0.0:
            stats["t_rx1"] = now
        elif last_rx[0]:
            gap = now - last_rx[0]
            if gap > stats["gap_max"]:
                stats["gap_max"] = gap
        last_rx[0] = now
        raw = bytes(data)
        # Frames carry a 16-bit little-endian sequence number (see voice_ble.c).
        # BLE GATT notifications are never retransmitted, so without this a lost
        # frame is indistinguishable from the device running slow — which is exactly
        # the ambiguity that sent several rounds of debugging to the wrong layer.
        seq = raw[0] | (raw[1] << 8)
        pcm = np.frombuffer(raw, dtype="<i2", offset=2)
        prev = stats["seq"]
        if prev is not None:
            gap = (seq - prev - 1) & 0xFFFF
            if gap > 1000:
                gap = 0                   # device restarted its counter, not loss
            if gap:
                stats["lost"] += gap
                stats["bursts"] += 1
                if gap > stats["worst"]:
                    stats["worst"] = gap
        stats["seq"] = seq
        if args.dump:
            dump_frames.append(pcm.copy())
            if sum(len(f) for f in dump_frames) >= 16000 * 8:
                import wave
                buf = np.concatenate(dump_frames)
                with wave.open(args.dump, "wb") as wf:
                    wf.setnchannels(1); wf.setsampwidth(2); wf.setframerate(16000)
                    wf.writeframes(buf.tobytes())
                print(f"island: auto-dumped {len(buf)/16000:.1f}s to {args.dump}",
                      file=sys.stderr)
                dump_frames.clear()
        q.append(pcm.copy())
        qlen[0] += len(pcm)
        stats["in"] += len(pcm)
        while q and qlen[0] > MAXBUF:     # sustained drift: shed the oldest frame
            qlen[0] -= len(q.popleft())
            stats["trim"] += 1

    # 豆包's push-to-talk is a physical key held down, so keyDown/keyUp must stay
    # strictly paired: a duplicate keyDown (repeated START, or a new session
    # arriving before the previous drain finished) left the key stuck down, and
    # the next press then could not stop 豆包. Track the held state and make both
    # directions idempotent.
    held = [False]
    sessions = [0]                        # bumped per START so a stale drain bails
    last_rx = [0.0]                       # monotonic time of the last audio frame
    def hold_key(down):
        if not pyautogui or held[0] == down:
            return
        (pyautogui.keyDown if down else pyautogui.keyUp)("optionright")
        held[0] = down
        # Stamp the release here, not at the call sites: it happens on either the
        # drain thread or the backstop timer, and stamping in only one of them left
        # the span unreported whenever the other won.
        if not down and stats["t_release"] == 0.0:
            stats["t_release"] = time.monotonic()

    def on_ctrl(_char, data):
        if not data:
            return
        raw = bytes(data)
        code = raw[0]
        if code == 6 and len(raw) >= 25:      # VOICE_CTRL_STATS
            (ovf, first_ms, read_ms, send_ms, retry_ms, att, acc,
             alloc_fail, notify_fail, rc, oversize, mtu) = struct.unpack(
                 "<12H", raw[1:25])
            rc = -(rc & 0x7FFF) if rc & 0x8000 else rc
            # Printed next to the host-side figures because that pairing is what
            # separates "the device never produced the frames" from "they arrived
            # late". retry dominating means the sender is waiting on the BLE pool;
            # ovf means capture is being dropped before it is ever sent.
            # attempts vs accepted says how many frames the worker even tried to
            # send; alloc_fail means the mbuf pool was dry and notify_fail means the
            # host stack refused a queued notification. Those want opposite fixes.
            print(f"island: device read={read_ms}ms send={send_ms}ms "
                  f"retry={retry_ms}ms first_frame={first_ms}ms i2s_ovf={ovf} "
                  f"tried={att} sent={acc} pool_dry={alloc_fail} "
                  f"refused={notify_fail} rc={rc} oversize={oversize} mtu={mtu}",
                  file=sys.stderr)
            return
        if code == 3:                     # VOICE_CTRL_START
            sessions[0] += 1              # invalidate any drain still sleeping
            hold_key(False)               # a pending drain must not outlive this
            stats["t0"] = time.monotonic()
            q.clear(); qlen[0] = 0; primed[0] = False
            stats["under"] = stats["trim"] = stats["stretch"] = stats["in"] = 0
            stats["lo"] = 10 ** 9
            stats["seq"] = None
            stats["lost"] = stats["bursts"] = stats["worst"] = 0
            stats["t_rx1"] = stats["t_audible"] = stats["t_release"] = 0.0
            stats["gap_max"] = 0.0
            hold_key(True)                # hold 豆包 push-to-talk for the session
            stats["t_arm"] = time.monotonic()
            # Re-assert on a fresh keyDown: pyautogui's key state and 豆包's idea of
            # it can disagree after a dropped notify or a manual Option press, and a
            # no-op keyDown on an already-held key leaves 豆包 unarmed with no way to
            # tell. Cheap, and it is why "sometimes it just does not open".
            if pyautogui and not held[0]:
                pyautogui.keyDown("optionright")
                held[0] = True
            print("island: session start", file=sys.stderr)
            return
        if code == 4:                     # VOICE_CTRL_STOP — drain, then release
            # Stamp the end here, not after draining: the drain wait is not part
            # of the capture window, and counting it made short sessions look
            # starved (a 2.6 s take measured 73% of realtime purely from the
            # 400 ms tail).
            stats["t1"] = time.monotonic()
            gen = sessions[0]
            # Release the key on a timer that cannot be delayed by the drain loop.
            # It used to happen only after draining plus a finalize wait — around
            # 450 ms of work on a thread that can be descheduled — so an occasional
            # stall left 豆包 recording with the stop key apparently dead. A
            # separate short timer makes the release unconditional.
            def release():
                if sessions[0] == gen:
                    hold_key(False)
            threading.Timer(0.9, release).start()   # backstop; drain releases sooner

            def drain():
                # Drain what is queued, but do not wait long: the queue holds at
                # most MAXBUF of audio and the old 2 s ceiling meant a stall at the
                # end of a take delayed the release by seconds, which the user feels
                # as "the last words take forever to appear".
                for _ in range(12):       # up to 120 ms
                    if not q:
                        break
                    time.sleep(0.01)
                # Close the output gate before the finalize wait. The stream is
                # never stopped, so without this the callback keeps running on an
                # empty queue and the dry-repair path replays the same reversed
                # 15 ms block over and over — a -25 dBFS drone fed to 豆包 for the
                # whole wait, while the key is still held. Those callbacks were
                # also counted against the session, which is the entire source of
                # the constant ~34 "underruns" (0.51 s of tail / 15 ms).
                # Let the output stream play out what it already holds before
                # closing the gate. Closing immediately after the drain loop cut the
                # last frames — the queue being empty does not mean the audio has
                # reached 豆包 yet, and a truncated tail is exactly the "last few
                # words are slow or wrong" symptom.
                time.sleep(0.05)
                # 豆包 needs a moment of held key after the audio ends to finalise
                # the last sentence, but 400 ms was more than it needs and every
                # millisecond here is the user waiting for their final words.
                # 豆包 revises the tail of an utterance after the audio ends, so
                # this wait is not dead time — releasing early commits a rough first
                # pass instead of the corrected text.
                time.sleep(0.45)
                # Only now close the gate. The order matters: this used to run before
                # the hold above, so the stream was gated for the whole 450 ms while
                # 豆包 was still listening and revising, and it revised against silence.
                primed[0] = False
                # Release FIRST, then check for a newer session. The guard used to
                # sit above this, so a second press arriving during the 0.4 s
                # finalize wait made the drain return without ever releasing — 豆包
                # stayed recording and the stop key looked dead.
                if sessions[0] == gen:
                    hold_key(False)
                el = stats["t1"] - stats["t0"] if stats["t0"] else 0.0
                if not (0.0 < el < 3600.0):
                    # No matching START: the agent restarted mid-session. Reporting
                    # the monotonic clock's uptime as a session length is worse than
                    # reporting nothing.
                    print("island: session end (no start stamp; agent restarted)",
                          file=sys.stderr)
                    return
                rate = stats["in"] / el if el > 0 else 0
                lo = 0 if stats["lo"] > 10 ** 8 else stats["lo"]
                print(f"island: session end {el:.1f}s in={stats['in']} "
                      f"({rate:.0f} Hz = {rate/SRC_RATE*100:.0f}% of 16k) "
                      f"underrun={stats['under']} stretch={stats['stretch']} "
                      f"trim={stats['trim']} lowater={lo/SRC_RATE*1000:.0f}ms "
                      f"| arm->rx1={ms(stats['t_arm'], stats['t_rx1'])} "
                      f"rx1->audible={ms(stats['t_rx1'], stats['t_audible'])} "
                      f"stop->release={ms(stats['t1'], stats['t_release'])} "
                      f"gapmax={stats['gap_max'] * 1000:.0f}ms "
                      f"load={os.getloadavg()[0]:.1f} busiest={_busiest_pct():.0f}% "
                      f"prebuf={prebuf[0] * 1000 // SRC_RATE}ms "
                      f"| lost={stats['lost']}f in {stats['bursts']} gaps "
                      f"(worst {stats['worst']}f) "
                      f"{stats['lost']*100.0/max(1, stats['lost'] + stats['in']//240):.1f}%",
                      file=sys.stderr)
            threading.Thread(target=drain, daemon=True).start()
            return
        if code == 5:                     # VOICE_CTRL_DELETE_ALL — repeat backspace
            # Repeated backspace, not select-all-and-delete. Cmd+A selects the whole
            # field, so in a chat box it wiped text the user had typed before ever
            # touching the card — the gesture is "keep deleting", not "erase
            # everything". 40 backspaces at 25 ms covers a long utterance in a second
            # and stops at the start of the line by itself.
            if not pyautogui:
                print("island: delete-all (pip install pyautogui to inject)",
                      file=sys.stderr)
                return

            def erase():
                for _ in range(40):
                    pyautogui.press("backspace")
                    time.sleep(0.025)
            threading.Thread(target=erase, daemon=True).start()
            return
        action = {1: "enter", 2: "backspace"}.get(code)
        if action == "enter" and held[0]:
            # Enter must not reach 豆包 while its push-to-talk key is still held: in
            # that state 豆包 treats it as cancelling the in-progress utterance and
            # the whole transcription disappears. OK during recording legitimately
            # arrives here before the release timer has fired, so wait for the key to
            # come up, then press Enter.
            def send_enter():
                for _ in range(120):      # up to 1.2 s
                    if not held[0]:
                        break
                    time.sleep(0.01)
                time.sleep(0.18)          # let 豆包 commit the finalised text first
                if pyautogui:
                    pyautogui.press("enter")
            threading.Thread(target=send_enter, daemon=True).start()
            return
        if action and pyautogui:
            pyautogui.press(action)
        elif action:
            print(f"island: {action} (pip install pyautogui to inject)",
                  file=sys.stderr)

    async def connect_once():
        """One scan+connect+stream cycle. Returns when the link drops."""
        dev = await BleakScanner.find_device_by_name(BLE_NAME, timeout=15.0)
        if dev is None:
            print("island: device not found (is it advertising?)", file=sys.stderr)
            return
        async with BleakClient(dev) as client:
            print(f"island: connected {dev.address}; streaming (Ctrl+C to stop)")
            # start_notify can fail with "Resources are insufficient" when macOS
            # has leaked stale BLE subscriptions. Retry a few times, then drop the
            # link so the reconnect loop gets a fresh connection instead of sitting
            # on a connected-but-unsubscribed (silent) session.
            for attempt in range(3):
                try:
                    await client.start_notify(BLE_UUID_AUDIO, on_audio)
                    await client.start_notify(BLE_UUID_CTRL, on_ctrl)
                    break
                except Exception as e:
                    print(f"island: subscribe failed ({e}); retry {attempt+1}/3",
                          file=sys.stderr)
                    await asyncio.sleep(1.5)
            else:
                print("island: could not subscribe; dropping link to reconnect",
                      file=sys.stderr)
                return   # reconnect loop will scan + reconnect fresh
            # Push the Claude quota packet over the same link: the statusline hook
            # keeps args.emit fresh; write it to the control characteristic so the
            # device's island updates. Every 30 s (and once right away).
            last_q = None
            tick = 0
            while client.is_connected:
                # The quota push is housekeeping and must not disturb the audio
                # stream. Two ways it did: a synchronous file read on the event loop
                # that also dispatches BLE notifications, and a GATT write that
                # competes for the same connection events the audio frames need.
                # So read off-loop, and never write while a take is in progress —
                # the number changes every 30 s and nobody is watching it mid-take.
                if tick % 15 == 0 and not held[0]:   # 15 * 2 s = 30 s
                    try:
                        q = await asyncio.to_thread(_read_quota, args.emit)
                    except OSError:
                        q = None
                    if q and len(q) in (ISLAND_LEN_V1, ISLAND_LEN) and q != last_q:
                        await client.write_gatt_char(BLE_UUID_CTRL, q, response=False)
                        last_q = q
                # Watchdog: the key is only legitimately held while audio is
                # flowing. If it is held but nothing has arrived for 2 s, the STOP
                # notify was lost or the device stopped without telling us — hold
                # it any longer and the user cannot stop 豆包 at all.
                # Watchdog: the key is only legitimately held while audio flows. If
                # it is held but nothing has arrived for 20 s the link is gone and
                # the STOP notify will never come, so release rather than leave 豆包
                # stuck recording. 20 s, not 2 s: at 2 s this fired during normal
                # dictation pauses and cut the transcription off after a few words.
                if held[0] and last_rx[0] and time.monotonic() - last_rx[0] > 20.0:
                    print("island: link silent 20 s; releasing key", file=sys.stderr)
                    hold_key(False)
                tick += 1
                await asyncio.sleep(2.0)     # was 0.5: fewer wake-ups on the loop
                                            # that also dispatches audio frames

    async def run():
        # Auto-reconnect forever: when the device sleeps, moves out of range, or
        # macOS drops the link, scan and reconnect instead of exiting. Ctrl+C
        # still stops cleanly (KeyboardInterrupt propagates out of asyncio.run).
        print(f"island: scanning for {BLE_NAME} (auto-reconnect on) ...")
        while True:
            try:
                await connect_once()
            except Exception as e:
                print(f"island: connection error: {e!r}", file=sys.stderr)
            # drop stale audio so the next session starts from a clean prebuffer
            q.clear(); qlen[0] = 0; primed[0] = False
            stats["seq"] = None           # device may have rebooted; resync
            last_rx[0] = 0.0
            hold_key(False)         # link is gone; never leave the key held
            await asyncio.sleep(2)
            print("island: reconnecting ...", file=sys.stderr)
        return 0

    # 豆包's push-to-talk is a key physically held down, so the held state lives
    # only in this process's memory: if the process dies while holding it, the key
    # stays stuck and no later press can stop 豆包 (the next keyDown is a no-op on
    # an already-held key). launchd sends SIGTERM on restart, so release on
    # signals too — not just on the normal return path.
    import signal
    def _release_and_die(signum, _frame):
        hold_key(False)
        raise SystemExit(128 + signum)
    for _sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        try:
            signal.signal(_sig, _release_and_die)
        except (ValueError, OSError):
            pass          # not on the main thread / unsupported: best effort

    try:
        return asyncio.run(run())
    except (KeyboardInterrupt, SystemExit):
        return 0
    finally:
        hold_key(False)
        try:
            stream.stop(); stream.close()
        except Exception:
            pass
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
    assert len(p) == ISLAND_LEN and p[0] == MAGIC and p[1] == 30
    assert struct.unpack("<I", p[2:6])[0] == 1893456000
    # The wire property, rather than a byte position that moves when a field is
    # added: main/island_quota.c checks that the XOR of the whole packet is zero.
    fold = 0
    for b in p:
        fold ^= b
    assert fold == 0
    assert p[6] == UNKNOWN or 0 <= p[6] <= 100        # Codex byte
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
    rb.add_argument("--batch", action="store_true",
                    help="play the whole take on STOP (vs realtime stream) — compare recognition")
    rb.set_defaults(func=cmd_recv_ble)

    t = sub.add_parser("selftest", help="assert pack/extract, no hardware")
    t.set_defaults(func=cmd_selftest)

    args = parser.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
