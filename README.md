<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport — a wireless microphone for Doubao IME

Dictate into the Doubao input method from across the room, with no network. Press
a button on the card, speak, and text appears at your cursor.

```
  card                          Mac
  ┌──────────┐   BLE GATT   ┌─────────────┐
  │ mic      │  16 kHz PCM  │ recv-ble    │
  │ 3 buttons├─────────────►│ agent       │──► virtual mic ──► Doubao ──► text
  │ screen   │  240 samples │             │
  └──────────┘  per notify  └─────────────┘
```

## Why it is nice to use

- **No network anywhere in the path.** Bluetooth LE to your laptop, nothing else.
  Works in a meeting room with no Wi-Fi.
- **Stops when you stop.** Three seconds of silence ends the take by itself.
- **One press to send.** OK while recording stops, waits for Doubao to finish
  revising the sentence, then presses Enter.
- **The screen tells you it is working.** A spark above a cup grows with your
  voice, so a live link never looks like a dead one.
- **Starts at login and heals itself.** The agent reconnects when the card sleeps
  or walks out of range; launchd restarts it if it dies.

## Keys

| Key | Action |
| --- | --- |
| DOWN | Start / stop recording |
| OK | Stop and send |
| UP | Delete the last utterance |
| UP, held | Clear the line |
| OK, held | Leave voice mode for Wi-Fi / Feishu setup |

## Install

```sh
brew install blackhole-2ch
python3 -m pip install bleak sounddevice numpy pyautogui pyobjc-framework-Cocoa

. $IDF_PATH/export.sh          # ESP-IDF v5.5.3
idf.py -p /dev/cu.usbmodemXXXX flash

tools/install-mic-agent.sh     # launchd agent, starts at login
```

Then three settings that are easy to miss:

1. **System Settings → Privacy & Security → Accessibility and Bluetooth**: add the
   Python the agent runs on. Without Accessibility, macOS silently drops the
   synthetic keystrokes and nothing is ever transcribed.
2. **Audio MIDI Setup → + → Create Aggregate Device**, tick **BlackHole 2ch**, name
   it, and select *that* as Doubao's microphone. Doubao rejects any device whose
   Core Audio transport type is `Virtual`, which is what BlackHole reports; an
   aggregate device reports `Unknown` and passes. **BlackHole itself will never
   appear in Doubao's list — this is the step everything hinges on.**
3. **Doubao's dictation shortcut must be right Option**, because the agent holds
   that key for the length of each recording.

Check it: press DOWN, count to three aloud. The spark should grow, and text should
appear. `tail -f /tmp/aipassport-mic.log` shows what the agent is doing, and
[the full README](docs/software-design/) covers the failure modes that look like
success — the first being Doubao quietly listening to the built-in microphone.

## How it works

**Raw PCM, not a codec.** ADPCM is stateful, so one dropped notification corrupts
everything after it. Raw 16 kHz PCM costs exactly one frame, and 256 kbps fits
BLE's budget with room to spare.

**Frames sized to the connection interval.** BLE passes roughly one notification
per connection event, and macOS settles near 15 ms. So a frame is 240 samples —
15 ms, 480 bytes, the largest whole PCM frame inside ATT MTU 512. At 10 ms per
frame the link structurally starved and delivered only 68% of realtime; at 15 ms
it delivers 92-100%.

**A sequence number on every frame.** BLE notifications are never retransmitted,
so without one a lost frame is indistinguishable from a device that is merely
running slow. That ambiguity sent debugging to the wrong layer three times; the
counter settled it in one session, and the loss turned out to be inside the
firmware, not on the air.

**Backpressure instead of a queue.** The capture worker keeps one retry slot, not
a buffer. A 4-frame queue was tried and reverted: it only drained at the link's
pace while capture kept filling it, so 92% of loop iterations found it full and
sent nothing.

**An output stream that never closes.** A streaming recogniser tolerates latency
but not holes. Injecting silence makes its voice-activity detector cut a sentence
in half, and closing the audio stream between utterances looks like the microphone
being unplugged — so the stream is opened once, and a 100 ms prebuffer absorbs BLE
jitter.

**Control codes outrank audio.** They share the BLE mbuf pool with a stream that
takes a block every 15 ms. One lost audio frame is 15 ms of sound; one lost STOP
leaves Doubao recording forever. So control sends retry and are only cleared once
they actually go out.

**Signal conditioning on the device.** Input gain sits at 24 dB, not 30 — at 30 an
ordinary speaking voice clipped, and clipping is unrecoverable distortion that a
recogniser degrades sharply on. A one-pole 90 Hz high-pass removes handling noise
and body rumble, which carried 11% of the energy while the 300-3400 Hz speech band
held only 36%.

## Also on the card

A Feishu (Lark) messenger lives behind an OK long-press, with its own Wi-Fi and
QR-code setup. It predates the microphone, needs your own Feishu app credentials,
and is not required for dictation. See
[`docs/software-design/feishu-messenger.md`](docs/software-design/feishu-messenger.md).

## Contributing

```sh
tools/validate.sh      # repository checks, host tests, firmware build
```

CI runs the same script. Documentation is English by default with a paired
`.zh_CN.md`; commits use English Conventional Commits; UI strings are Simplified
Chinese. Details in [`docs/contribution/`](docs/contribution/).

Do not edit `partitions.csv` — `cardid` at `0x356000` and `recovery` at `0x700000`
are a fixed contract with the vendor's installer. The mascot and backdrop are
generated: `tools/gen_mascot.py --preview`, `tools/gen_backdrop.py --preview`.

## License

MIT — see [LICENSE](LICENSE). A fork of
[FoloToy AI Passport](https://github.com/FoloToy); the upstream hardware guide and
BSP are theirs, with the original copyright retained.
