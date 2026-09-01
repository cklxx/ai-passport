<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport — a wireless microphone for Doubao IME

Dictate into the Doubao input method from across the room, with no network.

This firmware turns a [FoloToy AI Passport](docs/hardware-design/specifications.md)
card into a Bluetooth LE microphone. Press a button, speak, and the text appears
wherever your cursor is. The card captures audio and streams it to a small agent
on your Mac, which feeds it to Doubao as if it were a local microphone.

There is no Wi-Fi, no cloud service of ours, and no phone in the path — which is
the point. It works in a meeting room with no network, on a walk, or anywhere the
card is in Bluetooth range of your laptop.

```
  card                          Mac
  ┌──────────┐   BLE GATT   ┌─────────────┐
  │ mic      │  16 kHz PCM  │ recv-ble    │
  │ 3 buttons├─────────────►│ agent       │
  │ screen   │  240 samples │      ↓      │
  └──────────┘  per notify  │ virtual mic │
                            │      ↓      │
                            │ Doubao IME  │──► text at your cursor
                            └─────────────┘
```

## What the buttons do

| Key | Action |
| --- | --- |
| DOWN | Start / stop recording |
| OK | Send — the agent presses Enter |
| UP | Delete the last utterance (Backspace) |
| UP, held | Clear the whole line |
| OK, held | Leave voice mode for Wi-Fi / Feishu setup |

The screen shows a cup with a spark above it. The spark grows and gains
companions as you speak — **that is your proof the microphone is really hearing
you**, and the cheapest way to tell a live link from a dead one. It also shows
battery, and Claude / Codex quota if you use those.

## Requirements

- A FoloToy AI Passport card (ESP32-C3)
- macOS, with [Doubao IME](https://www.doubao.com/) installed
- [BlackHole 2ch](https://existential.audio/blackhole/) — `brew install blackhole-2ch`
- Python 3.9+
- [ESP-IDF v5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/get-started/)
  to build the firmware. Skip this if you flash a released binary.

Windows and Linux are not supported yet: the agent uses macOS APIs for the
keystroke injection and expects a Core Audio virtual device.

## Setup

### 1. Flash the firmware

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32c3
idf.py -p /dev/cu.usbmodemXXXX flash
```

Do not edit `partitions.csv`. The `cardid` partition at `0x356000` and the
`recovery` partition at `0x700000` are a fixed contract with the vendor's
mini-program installer; moving them breaks over-the-air installation and the
UP-key recovery boot.

### 2. Install the Mac agent

```sh
python3 -m pip install bleak sounddevice numpy pyautogui pyobjc-framework-Cocoa
tools/install-mic-agent.sh
```

That registers a launchd agent, so it starts at login and restarts if it dies. It
scans for a device named `AI-Passport-Mic` and reconnects on its own when the card
sleeps or moves out of range.

```sh
tail -f /tmp/aipassport-mic.log            # what it is doing
launchctl print gui/$(id -u)/com.aipassport.mic | grep state
tools/install-mic-agent.sh --uninstall
```

`bleak`, `sounddevice` and `numpy` are required. Without `pyautogui` the agent
still streams audio but cannot press keys, so Doubao never starts listening.
`pyobjc` only keeps the agent out of the Dock.

### 3. Grant permissions

**System Settings → Privacy & Security**:

- **Accessibility** → add your Python binary. Without it macOS silently discards
  the synthetic keystrokes and nothing is ever transcribed.
- **Bluetooth** → add the same binary, or the agent never finds the card.

Find which Python the agent uses with
`grep -A2 ProgramArguments ~/Library/LaunchAgents/com.aipassport.mic.plist`.

### 4. Make Doubao accept the virtual microphone

**This is the step everything hinges on, and it is not obvious.**

Doubao filters microphones by their Core Audio transport type and refuses any
device reporting `Virtual` — which is exactly what BlackHole reports. BlackHole
will not appear in Doubao's microphone list no matter how it is configured.

An Aggregate Device reports its transport as `Unknown`, which passes the filter.
So wrap BlackHole in one:

1. Open **Audio MIDI Setup** (in `/Applications/Utilities`).
2. Click **+** at the bottom left → **Create Aggregate Device**.
3. Tick **BlackHole 2ch** in the list of subdevices.
4. Name it something you will recognise, e.g. `card`.
5. In **Doubao → Settings → Voice**, select that aggregate device as the input.

### 5. Set Doubao's dictation key to right Option

The agent holds **right Option** down for the length of each recording, because
Doubao only consumes audio while its push-to-talk key is held. Set Doubao's
shortcut to right Option, or the audio arrives with nobody listening.

### 6. Check the chain

Press DOWN and count to three out loud.

- The spark on the card's screen should grow with your voice. If it does not, the
  microphone or the BLE link is the problem, not Doubao.
- Text should appear at your cursor within a couple of hundred milliseconds.
- Press DOWN again to stop.

## When it does not work

Several failure modes look like success. This table is ordered by how often they
actually happened during development.

| Symptom | Likely cause | Check |
| --- | --- | --- |
| Text appears, but it is what was said in the room, not into the card | Doubao is listening to the built-in microphone | Doubao → Settings → Voice: is the aggregate device selected? |
| No text at all, spark grows normally | Doubao's shortcut is not right Option, or Accessibility is not granted | Hold right Option manually — does Doubao start listening? |
| BlackHole is missing from Doubao's list | Expected — see step 4 | Select the aggregate device, not BlackHole |
| The card is not in macOS Bluetooth settings | By design — it advertises connectable but **not** discoverable, so a nearby iPhone cannot claim its single connection | `tail /tmp/aipassport-mic.log` should say `connected` |
| Doubao keeps recording after you stop | The push-to-talk key is stuck down | Press and release right Option by hand; the agent also releases it after 20 s of silence |
| Screen says "waiting for the computer" | The agent is not running, or Bluetooth permission is missing | `launchctl print gui/$(id -u)/com.aipassport.mic \| grep state` |
| Recognition is poor | Speak at a normal distance; the input gain is set for that | `tools/island_agent.py recv-ble --dump /tmp/take.wav` and listen |

The agent logs one line per session with the numbers that matter:

```
island: session end 19.4s in=303360 (15658 Hz = 98% of 16k) underrun=23 ... | lost=0f in 0 gaps 0.0%
```

`lost=0` is the one to watch — it means the BLE link delivered every frame. The
percentage is how closely the device kept to realtime; anything above ~90% is
inaudible in practice.

## How it works

Raw 16 kHz PCM goes over the air, not a compressed codec. A codec like ADPCM is
stateful, so one dropped notification corrupts everything after it; raw PCM costs
one frame. Frames are 240 samples — 15 ms, 480 bytes — because BLE passes roughly
one notification per connection event and macOS settles near a 15 ms interval. At
10 ms per frame the link structurally starved and delivered only 68% of realtime.

Each frame carries a 16-bit sequence number. BLE notifications are never
retransmitted, so without it a lost frame is indistinguishable from a device that
is merely running slow — an ambiguity that sent debugging to the wrong layer more
than once.

On the Mac the frames land in an elastic queue and drain into a Core Audio output
stream that is opened once and never stopped. A streaming recogniser tolerates
latency but not holes: injecting silence makes its voice-activity detector cut a
sentence in half, and closing the stream between utterances looks like the
microphone being unplugged.

More detail: [`docs/software-design/`](docs/software-design/) for the design
documents, [`docs/hardware-design/specifications.md`](docs/hardware-design/specifications.md)
for the hardware, [`AGENTS.md`](AGENTS.md) for how the repository is organised.

## Also on the card

Behind an OK long-press there is a Feishu (Lark) messenger with its own Wi-Fi and
QR-code setup: conversation list, message history, voice replies. It predates the
voice microphone and is documented in
[`docs/software-design/feishu-messenger.md`](docs/software-design/feishu-messenger.md).
It needs Wi-Fi and your own Feishu app credentials, and none of it is required for
dictation.

## Contributing

```sh
tools/validate.sh      # repository checks, host tests, firmware build
```

Run it before opening a pull request; CI runs the same script. Documentation is
English by default with a paired `.zh_CN.md`, commits and PR titles use English
Conventional Commits, and the UI strings are Simplified Chinese. The details are
in [`docs/contribution/`](docs/contribution/).

The mascot art and the backdrop are generated, not hand-drawn — edit the
parameters and regenerate:

```sh
python3 -m pip install pillow pypng
tools/gen_backdrop.py --preview
tools/gen_mascot.py --preview
```

Never commit credentials. Firmware contains no Feishu App ID or Secret; owner
credentials are written over USB at provisioning time and live in NVS.

## License

MIT — see [LICENSE](LICENSE). This is a fork of
[FoloToy AI Passport](https://github.com/FoloToy); the upstream hardware guide and
BSP are theirs, and the original copyright notice is retained.
