#!/usr/bin/env python3
"""voice_ab.py — measure a voice take, and A/B two takes, for the BLE mic.

Why this exists: AGC and VAD are guesses until a number says whether they
helped. Several "optimisations" this session measurably made recognition worse,
and each one was defended with a figure that turned out to be measuring
something else. This script computes only figures whose relationship to what
豆包's acoustic front end actually sees can be stated, and it refuses to hide
the three ways this project has already fooled itself:

  1. A LEVEL METRIC AVERAGED OVER NON-AUDIO TIME.  RMS over a whole file is a
     function of how long the speaker paused, not of how loud they were. Two
     takes of the same phrase with different pause lengths differ in whole dB.
     So the headline level is measured over speech-active frames only, gated at
     a FIXED absolute threshold so the same sample is classified the same way in
     both files. `rms_all_dbfs` is still printed, labelled as not comparable.

  2. A METRIC WHOSE OWN REFERENCE MOVES WITH THE THING BEING TESTED.  An
     adaptive, floor-relative gate is exactly wrong for AGC: AGC lifts the noise
     floor along with the speech, the gate lifts too, and `active%` reads
     unchanged while the SNR collapses. So the absolute gate drives every level
     figure and the adaptive gate is used ONLY for utterance structure
     (segments, gaps), where a relative threshold is what you want. Both are
     reported, each labelled with which question it answers.

  3. FIGURES THAT CONTRADICT EACH OTHER AND NOBODY NOTICES.  `voice_ab.py
     session` re-derives every number in the agent's session line from the
     others and prints the residual, so "lost=0f ... 0.0%" sitting next to "89%
     of 16k" is flagged as the contradiction it is rather than read as health.

Usage
  voice_ab.py report A.wav [B.wav ...]        per-file metrics
  voice_ab.py ab BEFORE.wav AFTER.wav        paired diff + ship/revert verdict
  voice_ab.py session "island: session end …" cross-check the agent's counters
  voice_ab.py hpf IN.wav [OUT.wav]           apply the device's exact Q15 90 Hz
                                             high-pass, to separate the filter's
                                             effect from a different take's

All input is 16 kHz mono int16 — the raw device stream as written by
`island_agent.py recv-ble --dump`.

DUMP CAVEATS — verified by reading tools/island_agent.py, not assumed
  WHERE IT TAPS.  --dump appends `pcm.copy()` inside on_audio (line 509), before
  `q.append` at line 520. So it is the RAW DEVICE STREAM: post the device's own
  Q15 high-pass, but BEFORE every one of the agent's own transforms — the 3x
  sample repeat, the 48 kHz resample, PREBUF, MAXBUF trimming, and the underrun
  hold. That is the right tap for judging device-side AGC and the wrong tap for
  judging anything the agent does. A dump can never show an agent-side underrun.

  IT IS A ROLLING 8 s WINDOW, NOT A RECORDING.  At line 511, once 8 s have
  accumulated the file is written AND `dump_frames.clear()` runs. So the WAV
  holds only audio since the last 8 s boundary: a 26 s session leaves a file
  containing its last 2 s, and every earlier write is overwritten. /tmp/dumprun.log
  shows this happening three times in one session. NEVER A/B two files of
  different length from long sessions — you are comparing different remainders.
  Keep every take under 8 s, or fix the tool to append.

  IT IS NOT SESSION-SCOPED.  dump_frames is never cleared on VOICE_CTRL_START
  (contrast on_ctrl, which resets every other counter). Frames from consecutive
  sessions concatenate, and the file survives across BLE reconnects.

  IT DISCARDS LOSS SILENTLY.  A lost BLE notify is detected (`stats["lost"]`) but
  the dump appends the next frame anyway, with no gap inserted. Lost audio
  becomes a splice, the file gets shorter, and its duration is NOT wall time.
  This is why `splice_ratio` exists and why `received_s` carries a warning.

  A FAIR A/B THEREFORE NEEDS: takes under 8 s; a fresh --dump path per take
  (the file is overwritten, so copy it out immediately); the agent's session line
  and the device's `recording STOP … i2s_ovf=` log captured alongside each WAV,
  because the audio file alone cannot show frames the device never captured.

Read DUMP CAVEATS above before trusting any file that tool produced.
"""

import argparse
import json
import math
import re
import sys
import wave

import numpy as np

FS_FULL = 32768.0          # int16 full scale
SR = 16000
DEV_FRAME = 240            # samples per BLE frame == the device's capture block
ABS_GATE_DBFS = -40.0      # fixed speech gate; see trap 1 above
ADAPT_GATE_DB = 12.0       # dB above the measured floor, for structure only
SPEC_WIN = 1024            # 64 ms → 15.6 Hz bins, fine enough for an 80 Hz edge
HPF_Q15 = 31610            # main/demo_voice.c: a = 0.9646, fc ≈ 90 Hz @ 16 kHz
BANDS = ((0, 80), (80, 300), (300, 3400), (3400, 8000))

# Ship/revert thresholds for device-side AGC. Every one of these is a number the
# script prints, so a claim can be checked instead of asserted.
GATE = {
    # AGC's entire job: land speech in a window that leaves headroom for the
    # loudest syllable. -20 dBFS is where the current fixed-24 dB take already
    # sits at normal distance, so this window says "hold that level when the
    # speaker moves", which is the actual requirement.
    "rms_active_dbfs": (-24.0, -17.0),
    # Peak is the definitive AGC criterion, and the one the current build fails:
    # a take that reaches full scale has already lost information no downstream
    # stage can recover. 1 dB of headroom, no more, because headroom bought by
    # lowering the level costs SNR.
    "peak_dbfs_max": -1.0,
    # 0.01% of a 5 s take is ~8 samples. Not zero: a single sample at the rail
    # on the loudest syllable is inaudible and unmeasurable by an ASR whose
    # analysis frame is 400 samples. What matters is that no RUN survives —
    # 3 samples is 0.19 ms, far inside one frame's tolerance for a step.
    "clip_pct_max": 0.01,
    "clip_run_max": 3,
    "crest_db_min": 10.0,                # below this the AGC is squashing speech
    "snr_db_drop_max": 1.0,              # AGC must not raise the floor with the voice
    "band_speech_drop_max": 2.0,         # % points of 300-3400 Hz energy
    # NOT an absolute. The sub-80 Hz figure is set by the ORDER of the existing
    # one-pole high-pass, not by AGC: at 80 Hz that filter is only -3.5 dB
    # (|H| = 0.67, verified against its own Q15 coefficient), so it cannot clear
    # the band and no AGC change will make it. Demanding an absolute 2% here
    # would have been exactly this project's recurring bug — a threshold set by
    # what we wished for rather than by what the arithmetic permits — and it
    # would fail every build forever, training everyone to ignore the gate.
    # AGC is scale-invariant in the frequency domain, so the honest test is that
    # it must not make the ratio WORSE.
    "band_rumble_rise_max": 1.5,         # % points vs the BEFORE take
    "dc_dbfs_max": -60.0,                # DC offset relative to full scale
}


# --- helpers -----------------------------------------------------------------

def _db(v, ref=FS_FULL):
    return 20.0 * math.log10(max(float(v), 1e-9) / ref)


def _runs(mask):
    """Lengths of the runs of True in a boolean array."""
    m = np.asarray(mask).astype(np.int8)
    if m.size == 0 or not m.any():
        return np.zeros(0, dtype=np.int64)
    d = np.diff(np.concatenate(([0], m, [0])))
    return np.flatnonzero(d == -1) - np.flatnonzero(d == 1)


def read_wav(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise SystemExit(f"{path}: need 16-bit PCM, got {w.getsampwidth()*8}-bit")
        if w.getnchannels() != 1:
            raise SystemExit(f"{path}: need mono, got {w.getnchannels()} ch")
        sr = w.getframerate()
        raw = w.readframes(w.getnframes())
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64)
    return x, sr


def device_hpf(x):
    """Bit-exact emulation of the worker's in-place high-pass (demo_voice.c).

    y[n] = (31610 * (y[n-1] + x[n] - x[n-1])) >> 15, state kept UNCLAMPED, only
    the emitted sample saturated. Python's >> floors on negatives, as does GCC's
    arithmetic shift on int64, so this matches the device sample for sample.
    """
    out = np.empty(len(x), dtype=np.int16)
    hx = hy = 0
    for i, xi in enumerate(x.astype(np.int64)):
        xi = int(xi)
        y = (HPF_Q15 * (hy + xi - hx)) >> 15
        hx, hy = xi, y
        out[i] = 32767 if y > 32767 else (-32768 if y < -32768 else y)
    return out.astype(np.float64)


# --- individual measurements --------------------------------------------------

def frame_levels(x, frame=DEV_FRAME):
    """Per-frame RMS in dBFS, on frames aligned to the device's own block."""
    nf = len(x) // frame
    if nf == 0:
        return np.zeros(0), np.zeros(0)
    F = x[: nf * frame].reshape(nf, frame)
    rms = np.sqrt((F * F).mean(axis=1))
    return rms, np.array([_db(v) for v in rms])


def gates(fdb):
    """Two masks and a floor.

    abs_mask   fixed -40 dBFS gate. Classifies the same waveform identically in
               every file, so level metrics computed under it are comparable.
               This is the ONLY gate any level figure may use.
    adapt_mask floor + 12 dB, two-pass. Tracks the take's own dynamics, so it
               segments utterances correctly — but it MOVES when AGC moves the
               floor, so it must never gate a level.
    floor_db   median of the frames the adaptive gate calls silence.
    """
    if fdb.size == 0:
        return np.zeros(0, bool), np.zeros(0, bool), float("nan")
    abs_mask = fdb > ABS_GATE_DBFS
    floor = float(np.percentile(fdb, 10))
    for _ in range(2):                     # 2 passes: floor → gate → floor
        quiet = fdb <= floor + ADAPT_GATE_DB
        if quiet.sum() >= 5:
            floor = float(np.median(fdb[quiet]))
    return abs_mask, fdb > floor + ADAPT_GATE_DB, floor


def band_energies(x, sr, sample_mask):
    """Fraction of power in each speech band, over speech frames only.

    Windows are 64 ms so the 80 Hz band edge falls between bins rather than
    inside one. A window counts only if most of its samples are speech: band
    ratios measured through the pauses are a measurement of the room, not the
    voice, and the 0-80 Hz figure in particular is dominated by silence.
    """
    win = SPEC_WIN
    hop = win // 2
    if len(x) < win:
        return {f"{lo}-{hi}": float("nan") for lo, hi in BANDS}, 0
    w = np.hanning(win)
    freqs = np.fft.rfftfreq(win, 1.0 / sr)
    acc = np.zeros(len(freqs))
    used = 0
    for s in range(0, len(x) - win + 1, hop):
        if sample_mask is not None and sample_mask[s:s + win].mean() < 0.5:
            continue
        sp = np.fft.rfft(x[s:s + win] * w)
        acc += (sp.real ** 2 + sp.imag ** 2)
        used += 1
    out = {}
    if used == 0 or acc.sum() <= 0:
        return {f"{lo}-{hi}": float("nan") for lo, hi in BANDS}, 0
    total = acc.sum()
    for lo, hi in BANDS:
        sel = (freqs >= lo) & (freqs < hi)
        out[f"{lo}-{hi}"] = 100.0 * acc[sel].sum() / total
    return out, used


def clip_stats(x):
    """Rails counted SEPARATELY.

    The brief's take peaks at 32768, which is |-32768| — the clipping is on the
    negative rail. A one-sided test (`x >= 32767`) undercounts it, which is the
    same class of bug as a counter incremented on one of two code paths. So both
    rails are counted and both are printed; an asymmetry is itself a finding
    (a DC offset pushing one rail closer).
    """
    pos = x >= 32767
    neg = x <= -32768
    both = pos | neg
    r = _runs(both)
    return {
        "clip_pos": int(pos.sum()),
        "clip_neg": int(neg.sum()),
        "clip_total": int(both.sum()),
        "clip_pct": 100.0 * both.sum() / max(1, len(x)),
        "clip_runs": int(len(r)),
        "clip_run_max": int(r.max()) if len(r) else 0,
    }


def held_stats(x):
    """Longest run of identical consecutive samples.

    The agent's underrun path emits `out[:] = last[0]` — a DC plateau. That is
    on the OUTPUT side, so a --dump file should show none: any long run here
    came from the device or from a genuinely dead mic. It is the one metric that
    distinguishes "the device sent silence" from "the agent invented silence".
    """
    if len(x) < 2:
        return {"held_run_max": 0, "held_pct": 0.0}
    same = np.diff(x) == 0
    r = _runs(same)
    return {
        "held_run_max": int(r.max()) + 1 if len(r) else 1,
        "held_pct": 100.0 * same.sum() / len(same),
    }


def splice_score(x, frame=DEV_FRAME):
    """Recovers the frame loss that --dump silently discards.

    A dropped BLE notify is simply absent from the dump: the next frame is
    concatenated onto the previous one and the file gets shorter, with no marker.
    But splicing two unrelated waveform positions leaves a step exactly at a
    240-sample boundary. Comparing the median jump at boundaries to the median
    jump everywhere else detects that from the file alone. ~1.0 means clean;
    well above 1 means frames are missing.
    """
    if len(x) < 4 * frame:
        return float("nan")
    d = np.abs(np.diff(x))
    idx = np.arange(frame, len(x), frame) - 1     # x[k*frame] - x[k*frame-1]
    idx = idx[idx < len(d)]
    if len(idx) < 4:
        return float("nan")
    interior = np.median(np.delete(d, idx))
    if interior <= 0:
        return float("nan")
    return float(np.median(d[idx]) / interior)


# --- one file ----------------------------------------------------------------

def measure(x, sr, label):
    rms_f, fdb = frame_levels(x)
    abs_mask, adapt_mask, floor_db = gates(fdb)

    # Sample-resolution speech mask from the fixed gate, for the spectrum.
    smask = np.repeat(abs_mask, DEV_FRAME)
    smask = np.concatenate([smask, np.zeros(len(x) - len(smask), bool)])

    act = x[smask] if smask.any() else np.zeros(0)
    rms_all = math.sqrt((x * x).mean()) if len(x) else 0.0
    rms_act = math.sqrt((act * act).mean()) if len(act) else 0.0
    peak_act = float(np.abs(act).max()) if len(act) else 0.0

    # Noise floor from the frames the ADAPTIVE gate calls silence, so it is the
    # real floor of this take, then SNR against the absolute-gated speech level.
    quiet = fdb[~adapt_mask] if adapt_mask.size else np.zeros(0)
    noise_db = float(np.median(quiet)) if quiet.size >= 3 else floor_db

    # Utterance structure, from the adaptive gate. A gap inside a phrase is what
    # a streaming recogniser reads as a sentence boundary, so the longest
    # INTERIOR gap matters and leading/trailing silence does not.
    seg = _runs(adapt_mask)
    gaps = _runs(~adapt_mask)
    if adapt_mask.size and adapt_mask.any():
        first, lastp = np.argmax(adapt_mask), adapt_mask.size - 1 - np.argmax(adapt_mask[::-1])
        interior = ~adapt_mask[first:lastp + 1]
        gap_runs = _runs(interior)
    else:
        gap_runs = np.zeros(0, dtype=np.int64)

    bands, nwin = band_energies(x, sr, smask if smask.any() else None)
    bands_all, _ = band_energies(x, sr, None)

    m = {
        "label": label,
        "samples": int(len(x)),
        "received_s": len(x) / sr,
        "dc_offset": float(x.mean()) if len(x) else 0.0,
        "dc_dbfs": _db(abs(x.mean())) if len(x) else -999.0,
        "rms_all_dbfs": _db(rms_all),
        "rms_active_dbfs": _db(rms_act),
        "noise_floor_dbfs": noise_db,
        "snr_db": _db(rms_act) - noise_db,
        "peak": float(np.abs(x).max()) if len(x) else 0.0,
        "peak_dbfs": _db(np.abs(x).max()) if len(x) else -999.0,
        "crest_db": _db(peak_act) - _db(rms_act) if len(act) else float("nan"),
        "active_pct_abs": 100.0 * abs_mask.mean() if abs_mask.size else 0.0,
        "active_pct_adapt": 100.0 * adapt_mask.mean() if adapt_mask.size else 0.0,
        "segments": int(len(seg)),
        "longest_gap_ms": float(gap_runs.max() * DEV_FRAME / sr * 1000) if len(gap_runs) else 0.0,
        "splice_ratio": splice_score(x),
        "spec_windows": nwin,
        "bands_active": bands,
        "bands_all": bands_all,
    }
    m.update(clip_stats(x))
    m.update(held_stats(x))
    return m


def print_report(m):
    b, ba = m["bands_active"], m["bands_all"]
    print(f"\n=== {m['label']} ===")
    print(f"  received            {m['received_s']:.2f} s ({m['samples']} samples)")
    print(f"                      NOT wall clock — see DUMP CAVEATS")
    print(f"  DC offset          {m['dc_offset']:+8.2f}  ({m['dc_dbfs']:.1f} dBFS)")
    print(f"  RMS  speech-gated  {m['rms_active_dbfs']:8.2f} dBFS   <- the level figure")
    print(f"  RMS  whole file    {m['rms_all_dbfs']:8.2f} dBFS   (NOT comparable across takes)")
    print(f"  noise floor        {m['noise_floor_dbfs']:8.2f} dBFS")
    print(f"  SNR                {m['snr_db']:8.2f} dB")
    print(f"  peak               {m['peak']:8.0f}  ({m['peak_dbfs']:.2f} dBFS)")
    print(f"  crest factor       {m['crest_db']:8.2f} dB")
    print(f"  clipped            {m['clip_total']} samples "
          f"({m['clip_pct']:.4f}%)  pos={m['clip_pos']} neg={m['clip_neg']}  "
          f"runs={m['clip_runs']} longest={m['clip_run_max']}")
    print(f"  held-sample runs   longest={m['held_run_max']}  ({m['held_pct']:.2f}% flat)")
    print(f"  splice ratio       {m['splice_ratio']:8.2f}   (~1 = no missing frames)")
    print(f"  speech active      {m['active_pct_abs']:.1f}% @ -40 dBFS abs  |  "
          f"{m['active_pct_adapt']:.1f}% @ floor+12 dB")
    print(f"  utterances         {m['segments']} segments, longest interior gap "
          f"{m['longest_gap_ms']:.0f} ms")
    print(f"  bands (speech frames, {m['spec_windows']} windows)")
    for k in b:
        print(f"      {k+' Hz':14s} {b[k]:5.1f}%   (whole file {ba[k]:5.1f}%)")


# --- A/B ---------------------------------------------------------------------

def cmd_ab(args):
    xa, sra = read_wav(args.before)
    xb, srb = read_wav(args.after)
    a = measure(xa, sra, f"BEFORE  {args.before}")
    b = measure(xb, srb, f"AFTER   {args.after}")
    print_report(a)
    print_report(b)

    print("\n=== paired diff (AFTER - BEFORE) ===")
    rows = [
        ("rms_active_dbfs", "speech RMS", "dB"),
        ("noise_floor_dbfs", "noise floor", "dB"),
        ("snr_db", "SNR", "dB"),
        ("crest_db", "crest factor", "dB"),
        ("peak_dbfs", "peak", "dB"),
        ("clip_pct", "clipped", "%"),
        ("clip_run_max", "longest clip run", "smp"),
        ("dc_dbfs", "DC", "dB"),
        ("active_pct_abs", "speech active (abs gate)", "%"),
        ("longest_gap_ms", "longest interior gap", "ms"),
        ("splice_ratio", "splice ratio", ""),
        ("held_run_max", "longest held run", "smp"),
    ]
    for k, name, unit in rows:
        print(f"  {name:26s} {a[k]:9.3f} -> {b[k]:9.3f}   {b[k]-a[k]:+8.3f} {unit}")
    for k in a["bands_active"]:
        print(f"  band {k+' Hz':17s} {a['bands_active'][k]:9.1f} -> "
              f"{b['bands_active'][k]:9.1f}   {b['bands_active'][k]-a['bands_active'][k]:+8.1f} %")

    # --- comparability guard: refuse to bless an unmatched pair ---------------
    print("\n=== comparability ===")
    warn = []
    for m in (a, b):
        if m["active_pct_abs"] <= 0.0:
            warn.append(f"{m['label'].split()[0]}: NO SPEECH above {ABS_GATE_DBFS} dBFS — "
                        f"dead mic, wrong file, or a dump written before the take. "
                        f"Every level figure for it is meaningless (shown as nan).")
    dact = abs(b["active_pct_abs"] - a["active_pct_abs"])
    if dact > 15:
        warn.append(f"speech-active fraction differs by {dact:.0f} points — these are "
                    f"probably not the same phrase at the same distance")
    dlen = abs(b["received_s"] - a["received_s"])
    if dlen > 0.35 * max(a["received_s"], b["received_s"]):
        warn.append(f"lengths differ by {dlen:.1f} s — a level diff here may just be "
                    f"a different amount of pause")
    for m in (a, b):
        if m["splice_ratio"] == m["splice_ratio"] and m["splice_ratio"] > 1.6:
            warn.append(f"{m['label'].split()[0]}: splice ratio {m['splice_ratio']:.2f} — "
                        f"frames are missing from this dump; its timeline is compressed")
    if not warn:
        print("  pair looks matched (length and speech fraction within tolerance)")
    for w in warn:
        print(f"  WARN  {w}")

    # --- verdict -------------------------------------------------------------
    print("\n=== verdict ===")
    lo, hi = GATE["rms_active_dbfs"]
    checks = [
        ("speech RMS in target window",
         lo <= b["rms_active_dbfs"] <= hi,
         f"{b['rms_active_dbfs']:.2f} dBFS, want {lo}..{hi}"),
        ("headroom left at peak",
         b["peak_dbfs"] <= GATE["peak_dbfs_max"],
         f"{b['peak_dbfs']:.2f} dBFS, want <={GATE['peak_dbfs_max']}"),
        ("clipping within budget",
         b["clip_pct"] <= GATE["clip_pct_max"] and b["clip_run_max"] <= GATE["clip_run_max"],
         f"{b['clip_pct']:.4f}% / longest run {b['clip_run_max']}, "
         f"want <={GATE['clip_pct_max']}% and <={GATE['clip_run_max']}"),
        ("speech dynamics preserved",
         b["crest_db"] >= GATE["crest_db_min"],
         f"crest {b['crest_db']:.2f} dB, want >={GATE['crest_db_min']}"),
        ("SNR not degraded",
         b["snr_db"] >= a["snr_db"] - GATE["snr_db_drop_max"],
         f"{a['snr_db']:.2f} -> {b['snr_db']:.2f} dB, allowed drop "
         f"{GATE['snr_db_drop_max']}"),
        ("speech-band energy not lost",
         b["bands_active"]["300-3400"] >= a["bands_active"]["300-3400"]
         - GATE["band_speech_drop_max"],
         f"{a['bands_active']['300-3400']:.1f}% -> "
         f"{b['bands_active']['300-3400']:.1f}%, allowed drop "
         f"{GATE['band_speech_drop_max']} pts"),
        ("sub-80 Hz share not worsened",
         b["bands_active"]["0-80"] <= a["bands_active"]["0-80"]
         + GATE["band_rumble_rise_max"],
         f"{a['bands_active']['0-80']:.1f}% -> {b['bands_active']['0-80']:.1f}%, "
         f"allowed rise {GATE['band_rumble_rise_max']} pts"),
        ("DC negligible",
         b["dc_dbfs"] <= GATE["dc_dbfs_max"],
         f"{b['dc_dbfs']:.1f} dBFS, want <={GATE['dc_dbfs_max']}"),
        ("no new interior gaps",
         b["longest_gap_ms"] <= max(a["longest_gap_ms"], 1.0) + 1e-9,
         f"{a['longest_gap_ms']:.0f} -> {b['longest_gap_ms']:.0f} ms"),
    ]
    bad = 0
    for name, ok, detail in checks:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name:32s} {detail}")
        bad += not ok
    print(f"\n  {len(checks)-bad}/{len(checks)} audio checks pass.")
    print("  AUDIO IS HALF THE GATE. The device's i2s_ovf and the agent's "
          "'% of 16k' / underrun\n  must also be recorded for this pair — a dump "
          "cannot show frames the device\n  never captured. Run `voice_ab.py session "
          "'<the agent line>'` on both.")
    return 1 if bad else 0


# --- session-line cross-check ------------------------------------------------

SESSION_RE = re.compile(
    r"session end\s+(?P<el>[\d.]+)s\s+in=(?P<in>\d+).*?"
    r"underrun=(?P<under>\d+)\s+stretch=(?P<stretch>\d+)\s+trim=(?P<trim>\d+)\s+"
    r"lowater=(?P<lo>\d+)ms"
    r"(?:.*?lost=(?P<lost>\d+)f in (?P<bursts>\d+) gaps \(worst (?P<worst>\d+)f\)"
    r"\s+(?P<lostpct>[\d.]+)%)?", re.S)


def cmd_session(args):
    """Re-derive every figure in the agent's session line from the others.

    This project has already shipped a line whose numbers contradict each other
    in plain sight: `89% of 16k` (2.8 s of audio missing from a 26 s take) next
    to `lost=0f … 0.0%`. Both are true and they measure different things — link
    loss versus total shortfall — but the 0.0% reads as "healthy" and stopped
    the investigation. Printing the residual makes that impossible to miss.
    """
    m = SESSION_RE.search(args.line)
    if not m:
        raise SystemExit("could not parse a session line out of that string")
    g = m.groupdict()
    el = float(g["el"]); rx = int(g["in"]); under = int(g["under"])
    stretch = int(g["stretch"]); trim = int(g["trim"])
    lost = int(g["lost"] or 0)
    expect = el * SR
    deficit = expect - rx
    held = under * DEV_FRAME

    print(f"  wall clock            {el:.2f} s  -> {expect:.0f} samples expected")
    print(f"  received (in=)        {rx} samples = {rx/SR:.2f} s "
          f"({rx/el:.0f} Hz = {rx/el/SR*100:.1f}% of 16k)")
    print(f"  shortfall             {deficit:.0f} samples = {deficit/SR:.2f} s "
          f"({deficit/expect*100:.1f}%)")
    print(f"  BLE frames lost       {lost}  = {lost*DEV_FRAME} samples "
          f"({lost*DEV_FRAME/SR:.2f} s)")
    print(f"  underrun output       {under} blocks x {DEV_FRAME} = {held} samples "
          f"= {held/SR:.2f} s of HELD-SAMPLE (invented) output")
    print(f"  trim / stretch        {trim} / {stretch}")

    print("\n  --- residuals ---")
    unexplained = deficit - lost * DEV_FRAME
    print(f"  shortfall not explained by link loss: {unexplained:.0f} samples "
          f"= {unexplained/SR:.2f} s")
    if abs(held - deficit) / max(1.0, expect) < 0.02:
        print(f"  underrun*{DEV_FRAME} ({held}) matches the shortfall ({deficit:.0f}) to "
              f"{abs(held-deficit):.0f} samples")
        print(f"  -> the agent papered over the whole shortfall with held samples.")

    print("\n  --- contradictions ---")
    flag = False
    if lost == 0 and deficit > 0.03 * expect:
        flag = True
        print(f"  CONTRADICTION: the line reports 0 lost frames, i.e. an unbroken "
              f"sequence,\n    yet {deficit/SR:.2f} s ({deficit/expect*100:.0f}%) of "
              f"audio never arrived. Nothing was lost\n    IN FLIGHT, so the device did "
              f"not PRODUCE it. `lost%` is blind to this and\n    must never be quoted "
              f"as link health. The figures that move are '% of 16k',\n    'underrun', "
              f"and the device's own i2s_ovf.")
    if stretch == 0 and under > 0:
        flag = True
        print(f"  DEAD COUNTER: stretch=0 with underrun={under}. The elastic path needs "
              f"0<got<need,\n    but BLE delivers whole {DEV_FRAME}-sample frames and the "
              f"callback consumes exactly\n    {DEV_FRAME}, so that state never occurs. "
              f"stretch=0 means 'never ran', not 'not needed'.")
    if int(g["lo"]) == 0 and under > 0:
        flag = True
        print(f"  lowater=0 ms with {under} underruns: the queue is not merely dipping, "
              f"it is\n    chronically dry. PREBUF is not the problem; supply is.")
    if not flag:
        print("  none found")
    return 0


# --- entry -------------------------------------------------------------------

def cmd_report(args):
    out = []
    for p in args.files:
        x, sr = read_wav(p)
        m = measure(x, sr, p)
        out.append(m)
        if not args.json:
            print_report(m)
    if args.json:
        print(json.dumps(out, indent=2))
    return 0


def cmd_hpf(args):
    x, sr = read_wav(args.infile)
    y = device_hpf(x)
    a = measure(x, sr, f"{args.infile}  (raw)")
    b = measure(y, sr, f"{args.infile}  (+ device Q15 90 Hz HPF)")
    print_report(a)
    print_report(b)
    print("\n=== what the filter actually leaves ===")
    for k in a["bands_active"]:
        print(f"  {k+' Hz':14s} {a['bands_active'][k]:5.1f}% -> "
              f"{b['bands_active'][k]:5.1f}%   {b['bands_active'][k]-a['bands_active'][k]:+6.1f} pts")
    print(f"  speech RMS     {a['rms_active_dbfs']:.2f} -> {b['rms_active_dbfs']:.2f} dBFS")
    print(f"  peak           {a['peak']:.0f} -> {b['peak']:.0f}")
    print(f"  clipped        {a['clip_total']} -> {b['clip_total']} samples")
    print(f"  crest          {a['crest_db']:.2f} -> {b['crest_db']:.2f} dB")
    if args.outfile:
        with wave.open(args.outfile, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
            w.writeframes(y.astype("<i2").tobytes())
        print(f"  wrote {args.outfile}")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("report", help="per-file metrics")
    r.add_argument("files", nargs="+")
    r.add_argument("--json", action="store_true")
    r.set_defaults(func=cmd_report)

    a = sub.add_parser("ab", help="paired before/after diff + verdict")
    a.add_argument("before"); a.add_argument("after")
    a.set_defaults(func=cmd_ab)

    s = sub.add_parser("session", help="cross-check the agent's session line")
    s.add_argument("line")
    s.set_defaults(func=cmd_session)

    h = sub.add_parser("hpf", help="apply the device's exact Q15 high-pass")
    h.add_argument("infile"); h.add_argument("outfile", nargs="?")
    h.set_defaults(func=cmd_hpf)

    args = p.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
