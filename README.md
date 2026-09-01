<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport — a wireless microphone for Doubao IME

Dictate into the Doubao input method from across the room, with no network. Press
a button on the card, speak, and text appears at your cursor.

```
  card                          Mac
  ┌──────────┐   BLE GATT   ┌─────────────┐
  │ mic      │ 16 kHz μ-law │ recv-ble    │
  │ 3 buttons├─────────────►│ agent       │──► virtual mic ──► Doubao ──► text
  │ screen   │  480 samples │             │
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
| OK | Stop and send (fires on press, not release) |
| UP | Delete the last utterance |
| UP, held | Erase continuously; stops the moment you let go |
| OK, held 2.5 s | Leave voice mode for Wi-Fi / Feishu setup |

## Install

```sh
brew install blackhole-2ch python@3.11        # bleak needs Python 3.10+
python3.11 -m pip install bleak sounddevice numpy pyautogui pyobjc-framework-Cocoa

. $IDF_PATH/export.sh                          # ESP-IDF v5.5.3
idf.py -p /dev/cu.usbmodemXXXX flash

tools/install-mic-agent.sh                     # launchd agent, starts at login
```

Install the packages before running the script: it does not install anything, it
searches for an interpreter that already has them. It also only checks for
`bleak`, `sounddevice` and `numpy` — miss `pyautogui` and you get an agent that
streams audio happily and never types a character.

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

**μ-law, not a stateful codec.** ADPCM is stateful, so one dropped notification
corrupts everything after it. G.711 μ-law is per-sample, so a lost frame costs
exactly that frame — the same property raw PCM had, at half the bytes.

**The link limits frames per second, not bytes per second.** BLE passes roughly
one notification per connection event, and macOS pins the interval at 30 ms and
refuses to negotiate (the firmware asks for 15 ms and is rejected with rc=554).
That caps the link near 34 notifications/s. A 240-sample PCM frame is 15 ms of
audio and needs 66.7/s — a deficit of exactly half, and the measured delivery was
47-56%. It never surfaced as an error: unsent notifications piled up in NimBLE's
mbuf pool until allocation returned NULL, and the device dropped those frames
itself, before the air.

So a frame is 480 μ-law samples: 30 ms, 482 bytes on the wire — byte for byte the
same packet — at 33.3 notifications/s. Delivery is 101-105% of realtime with zero
frames lost and pool_dry at 0.

Bandwidth was never the constraint. The original choice of raw PCM came from
"256 kbps fits inside BLE's ~700 kbps", which is true and answers the wrong
question. Do not fill the MTU further either: 252 samples (506 of 507 usable
bytes) measured *worse*, 89% → 69%, because a fuller packet costs more of the
controller's ACL buffers. Headroom beats occupancy in both directions.

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
takes a block every 30 ms. One lost audio frame is 30 ms of sound; one lost STOP
leaves Doubao recording forever. So control sends retry and are only cleared once
they actually go out. Hold-to-erase sends only its two edges for the same reason —
the host runs the repetition, so a two-second hold costs the link two
notifications instead of eighty — and the host carries a 10 s dead-man deadline in
case the closing edge is the one that goes missing.

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
