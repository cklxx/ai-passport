#!/usr/bin/env python3
"""Measure recognition accuracy end to end, so audio changes can be judged.

The problem this solves: judging a change by speaking a phrase and reading the
result cannot separate the change from the take. Measured RMS varied by 19 dB
across three recordings of "the same" phrase, which is far more than any firmware
change moved it — so every A/B was dominated by how the speaker happened to talk.

This replays ONE recording through the whole chain instead. The audio is identical
every run, so the only thing that varies is the code.

    # once: record a reference take and write down what you said
    tools/measure_accuracy.py record --out ref/take1.wav
    #   speak, then type the exact words when prompted

    # after every change: replay it and score the result
    tools/measure_accuracy.py score ref/take1.wav

It plays into the same virtual device the agent uses, holds Doubao's push-to-talk
key exactly as the agent does, then reads the transcription back with select-all
plus copy and reports the character error rate against the reference.

CER is edit distance over reference length: 0% is perfect, and the same phrase
scored twice should agree to within a percent or two. If it does not, the setup is
too noisy to draw conclusions from and no audio change should be trusted.

Three devices with three distinct jobs, which are easy to confuse:

  the built-in microphone   where a reference take is RECORDED from
  BlackHole 2ch             where audio is PLAYED INTO, as the agent does
  an Aggregate Device       what Doubao READS, wrapping BlackHole (see the README:
                            Doubao rejects a device whose transport is Virtual)

Note that the aggregate device is often the system default INPUT, so recording from
the default captures the loopback and yields silence. This tool therefore picks the
built-in microphone explicitly.

Requires the same Accessibility permission the agent needs, Doubao focused on an
empty text field, and its shortcut set to right Option.
"""
import argparse
import json
import os
import sys
import time
import wave

SRC_RATE = 16000


def _deps():
    try:
        import numpy as np
        import sounddevice as sd
        import pyautogui
        from AppKit import NSPasteboard
    except ImportError as e:
        print(f"measure_accuracy: missing dependency ({e}). "
              "pip install numpy sounddevice pyautogui pyobjc-framework-Cocoa",
              file=sys.stderr)
        raise SystemExit(1)
    pyautogui.PAUSE = 0
    return np, sd, pyautogui, NSPasteboard


def find_output(np, sd, name):
    """Index of the virtual device to play into, matched the way the agent does."""
    if name:
        return name
    for i, d in enumerate(sd.query_devices()):
        n = d["name"].lower()
        if d["max_output_channels"] > 0 and (
                "blackhole" in n or "cable" in n or "vb-audio" in n):
            return i
    print("measure_accuracy: no BlackHole/VB-Cable found; pass --device",
          file=sys.stderr)
    raise SystemExit(1)


def cer(ref, hyp):
    """Character error rate: Levenshtein distance over reference length.

    Whitespace and the punctuation an IME inserts on its own are stripped first —
    they are not what an audio change affects, and counting them would swamp the
    signal we are looking for.
    """
    drop = " \t\n，。、！？；：""''（）《》…—·,.!?;:'\"()<>"
    r = [c for c in ref if c not in drop]
    h = [c for c in hyp if c not in drop]
    if not r:
        return 0.0 if not h else 100.0
    prev = list(range(len(h) + 1))
    for i, rc in enumerate(r, 1):
        cur = [i] + [0] * len(h)
        for j, hc in enumerate(h, 1):
            cur[j] = min(prev[j] + 1,          # deletion
                         cur[j - 1] + 1,        # insertion
                         prev[j - 1] + (rc != hc))
        prev = cur
    return prev[len(h)] / len(r) * 100.0


def cmd_record(args):
    np, sd, pyautogui, _ = _deps()
    print(f"measure_accuracy: recording {args.seconds}s from the built-in "
          "microphone — speak now.")
    print("  This is the Mac's own microphone, not the card. The point is a fixed")
    print("  signal to replay; the card's audio path is what we are measuring")
    print("  changes in, so it must not be in the reference.")
    time.sleep(1.0)
    # Explicit input: the system default is typically the aggregate device that the
    # agent plays into, so recording from the default captures the loopback — i.e.
    # silence — rather than the room. Prefer the built-in microphone, and refuse the
    # virtual devices outright rather than recording nothing.
    src = args.input
    if src is None:
        for i, d in enumerate(sd.query_devices()):
            n = d["name"].lower()
            if d["max_input_channels"] == 0:
                continue
            if "blackhole" in n or "cable" in n or "vb-audio" in n:
                continue                    # our own playback path, not a mic
            if d["max_output_channels"] > 0:
                continue                    # duplex means aggregate or loopback
            src = i
            print(f"  using input [{i}] {d['name']}")
            break
        if src is None:
            print("measure_accuracy: no real microphone found; pass --input",
                  file=sys.stderr)
            return 1
    audio = sd.rec(int(args.seconds * SRC_RATE), samplerate=SRC_RATE,
                   channels=1, dtype="int16", device=src)
    sd.wait()
    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with wave.open(args.out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SRC_RATE)
        w.writeframes(audio.tobytes())
    text = input("Type exactly what you said: ").strip()
    with open(args.out + ".txt", "w") as f:
        f.write(text + "\n")
    print(f"measure_accuracy: wrote {args.out} and its reference text")
    return 0


def cmd_score(args):
    np, sd, pyautogui, NSPasteboard = _deps()
    ref_path = args.wav + ".txt"
    if not os.path.exists(ref_path):
        print(f"measure_accuracy: no reference text at {ref_path} — "
              "record the take with this tool so the words are known",
              file=sys.stderr)
        return 1
    with open(ref_path) as f:
        ref = f.read().strip()

    with wave.open(args.wav) as w:
        rate = w.getframerate()
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    device = find_output(np, sd, args.device)
    out_rate = int(round(sd.query_devices(device)["default_samplerate"]))
    up = max(1, out_rate // rate)
    ch = min(2, sd.query_devices(device)["max_output_channels"])
    # Same upsample the agent uses: integer sample repeat to the device's rate.
    block = np.repeat(pcm, up)
    stereo = np.repeat(block[:, None], ch, axis=1)

    pb = NSPasteboard.generalPasteboard()
    results = []
    for run in range(args.runs):
        pyautogui.hotkey("command", "a")
        pyautogui.press("delete")           # start from an empty field
        time.sleep(0.2)
        pyautogui.keyDown("optionright")    # arm Doubao, as the agent does
        time.sleep(0.10)                    # the agent's prebuffer
        sd.play(stereo, samplerate=out_rate, device=device)
        sd.wait()
        time.sleep(0.45)                    # Doubao revises the tail
        pyautogui.keyUp("optionright")
        time.sleep(args.settle)

        pb.clearContents()
        pyautogui.hotkey("command", "a")
        pyautogui.hotkey("command", "c")
        time.sleep(0.25)
        hyp = pb.stringForType_("public.utf8-plain-text") or ""
        hyp = hyp.strip()
        score = cer(ref, hyp)
        results.append({"run": run + 1, "cer": score, "text": hyp})
        print(f"  run {run + 1}: CER {score:5.1f}%   {hyp[:60]}")

    scores = [r["cer"] for r in results]
    mean = sum(scores) / len(scores)
    spread = max(scores) - min(scores)
    print(f"\nreference : {ref}")
    print(f"CER       : {mean:.1f}% mean over {len(scores)} runs, "
          f"spread {spread:.1f}")
    if spread > 5.0:
        print("WARNING: the spread between identical runs exceeds 5 points, so this "
              "setup cannot resolve a change smaller than that. Check that Doubao is "
              "focused on an empty field and nothing else is using the audio device.")
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"reference": ref, "mean_cer": mean, "runs": results}, f,
                      ensure_ascii=False, indent=2)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("record", help="capture a reference take and its text")
    r.add_argument("--out", default="ref/take1.wav")
    r.add_argument("--seconds", type=float, default=8.0)
    r.add_argument("--input", default=None,
                   help="input device name or index (default: the built-in mic)")
    r.set_defaults(func=cmd_record)

    s = sub.add_parser("score", help="replay a take and score the transcription")
    s.add_argument("wav")
    s.add_argument("--runs", type=int, default=3,
                   help="repeat, to see how noisy the measurement is")
    s.add_argument("--device", default=None, help="output device name or index")
    s.add_argument("--settle", type=float, default=1.2,
                   help="seconds to wait for the IME to commit before reading back")
    s.add_argument("--json", default=None, help="also write results here")
    s.set_defaults(func=cmd_score)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
