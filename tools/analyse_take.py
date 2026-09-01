#!/usr/bin/env python3
"""Measure a recorded take, so an audio change can be judged instead of guessed.

Run it on two dumps of the same phrase at the same distance — before and after a
change — and compare. Several "optimisations" in this project measurably made
recognition worse; the ones that helped were found by looking at numbers like these.

    tools/island_agent.py recv-ble --dump /tmp/before.wav
    tools/analyse_take.py /tmp/before.wav /tmp/after.wav

What the metrics mean for recognition:

  clipped        Unrecoverable distortion. A recogniser degrades sharply on it.
                 Must be 0. Finding 74 clipped samples is what prompted lowering
                 the input gain from 30 dB to 24 dB.
  peak / RMS     Speech wants to sit near -20 dBFS RMS with the peak below about
                 -3 dBFS. Too low wastes bits, too high clips.
  crest factor   peak/RMS in dB. Speech is naturally 12-18 dB. Much lower means
                 something is compressing or clipping; much higher means the take
                 is mostly silence with isolated bursts.
  band energy    The 300-3400 Hz band carries speech. Energy below 300 Hz is
                 handling noise and body rumble: it carries no information, eats
                 the headroom that then clips, and skews the recogniser's features.
                 A take measured 11.5% below 80 Hz with only 36% in the speech
                 band, which is why a 90 Hz high-pass was added.
  flat runs      Consecutive identical samples, 200 or more. These are the
                 receiver's hold-last-sample repair for a starved queue, i.e. a DC
                 step in the recogniser's input. Should be 0.
  DC offset      A bias wastes headroom and biases features. Should be near 0.
"""
import argparse
import sys
import wave


def load(path):
    try:
        import numpy as np
    except ImportError:
        print("analyse_take: needs numpy", file=sys.stderr)
        raise SystemExit(1)
    with wave.open(path) as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 1:
            print(f"analyse_take: {path} is not 16-bit mono", file=sys.stderr)
            raise SystemExit(1)
        rate = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    return x.astype(np.float64), rate


def measure(path):
    import numpy as np
    x, rate = load(path)
    n = len(x)
    if n == 0:
        print(f"analyse_take: {path} is empty", file=sys.stderr)
        raise SystemExit(1)

    full = 32768.0
    rms = float(np.sqrt((x ** 2).mean()))
    peak = float(np.abs(x).max())
    db = lambda v: 20 * np.log10(max(v, 1e-9) / full)

    # Flat runs: the receiver repeats one sample for a whole 15 ms block (240
    # samples) when its queue is dry, so anything at or above 200 is a repair.
    same = np.diff(x) == 0
    runs, cur = [], 0
    for v in same:
        if v:
            cur += 1
        elif cur:
            runs.append(cur + 1)
            cur = 0
    if cur:
        runs.append(cur + 1)
    runs = np.array(runs) if runs else np.array([0])
    held = runs[runs >= 200]

    # Band energies over the whole take. A window avoids spectral leakage from the
    # abrupt ends of the recording.
    spec = np.abs(np.fft.rfft(x * np.hanning(n))) ** 2
    freq = np.fft.rfftfreq(n, 1.0 / rate)
    total = spec.sum() or 1.0
    bands = {}
    for lo, hi in ((0, 80), (80, 300), (300, 3400), (3400, rate // 2)):
        bands[(lo, hi)] = float(spec[(freq >= lo) & (freq < hi)].sum() / total * 100)

    # Speech-frame ratio: frames whose RMS is within 25 dB of the loudest frame.
    # A rough proxy for how much of the take is actually speech.
    fr = 240
    frames = x[: n // fr * fr].reshape(-1, fr)
    f_rms = np.sqrt((frames ** 2).mean(axis=1))
    loud = f_rms.max() or 1.0
    voiced = float((f_rms > loud / 17.8).mean() * 100)   # 17.8 = 25 dB

    return {
        "path": path,
        "seconds": n / rate,
        "rate": rate,
        "dc": float(x.mean()),
        "rms_db": db(rms),
        "peak_db": db(peak),
        "crest_db": db(peak) - db(rms),
        "clipped": int((np.abs(x) >= 32700).sum()),
        "held_samples": int(held.sum()),
        "held_blocks": int(len(held)),
        "voiced_pct": voiced,
        "bands": bands,
    }


def show(m):
    print(f"\n{m['path']}  {m['seconds']:.1f}s @ {m['rate']} Hz")
    flag = lambda ok: "  " if ok else " !"
    print(f"{flag(m['clipped'] == 0)}clipped        {m['clipped']:>8d}"
          f"   (must be 0 — unrecoverable distortion)")
    print(f"{flag(-26 < m['rms_db'] < -14)}RMS            {m['rms_db']:>8.1f} dBFS"
          f"   (want about -20)")
    print(f"{flag(m['peak_db'] < -2.5)}peak           {m['peak_db']:>8.1f} dBFS"
          f"   (want below -3)")
    print(f"{flag(10 < m['crest_db'] < 20)}crest factor   {m['crest_db']:>8.1f} dB"
          f"   (speech is 12-18)")
    print(f"{flag(abs(m['dc']) < 50)}DC offset      {m['dc']:>+8.1f}"
          f"   (want near 0)")
    print(f"{flag(m['held_blocks'] == 0)}held blocks    {m['held_blocks']:>8d}"
          f"   ({m['held_samples'] / m['rate'] * 1000:.0f} ms of repair — want 0)")
    print(f"  voiced frames  {m['voiced_pct']:>8.1f} %")
    for (lo, hi), pct in m["bands"].items():
        speech = (lo, hi) == (300, 3400)
        mark = flag(pct > 45) if speech else "  "
        print(f"{mark}{lo:5d}-{hi:5d} Hz {pct:>8.1f} %"
              f"{'   <- speech band, want the largest share' if speech else ''}")


def compare(a, b):
    print("\n--- change ---")
    rows = [
        ("clipped", a["clipped"], b["clipped"], "fewer is better", 0),
        ("RMS dBFS", a["rms_db"], b["rms_db"], "closer to -20", 1),
        ("peak dBFS", a["peak_db"], b["peak_db"], "below -3", 1),
        ("crest dB", a["crest_db"], b["crest_db"], "12-18", 1),
        ("held blocks", a["held_blocks"], b["held_blocks"], "fewer is better", 0),
        ("speech band %", a["bands"][(300, 3400)], b["bands"][(300, 3400)],
         "higher is better", 1),
        ("sub-300 Hz %", a["bands"][(0, 80)] + a["bands"][(80, 300)],
         b["bands"][(0, 80)] + b["bands"][(80, 300)], "lower is better", 1),
    ]
    for name, x, y, want, prec in rows:
        print(f"  {name:<15} {x:>9.{prec}f} -> {y:>9.{prec}f}   ({want})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("wav", nargs="+", help="one take, or two to compare")
    args = ap.parse_args()
    ms = [measure(p) for p in args.wav]
    for m in ms:
        show(m)
    if len(ms) == 2:
        compare(ms[0], ms[1])
    # Non-zero exit on the two faults that make recognition worse outright, so this
    # can gate a change rather than merely describe it.
    bad = [m for m in ms if m["clipped"] or m["held_blocks"]]
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
