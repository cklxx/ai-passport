#!/usr/bin/env python3
"""Reliable glyph test for an lv_font_conv generated LVGL font .c file.

Reimplements LVGL v9's get_glyph_dsc_id() (managed_components/lvgl__lvgl/src/
font/fmt_txt/lv_font_fmt_txt.c:283) byte for byte, then adds the two extra
checks that C cannot do safely: glyph-id array bounds, and empty bitmap box.

A codepoint RENDERS iff:
  1. it falls inside some cmap range (rcp = cp - range_start < range_length), and
  2. that range's type resolves it to a NON-ZERO glyph id, and
  3. the glyph id is < len(glyph_dsc)  (otherwise C reads out of bounds), and
  4. glyph_dsc[gid].box_w * box_h > 0 or it is a whitespace advance.

Being inside a declared range is NOT sufficient: FORMAT0_FULL stores offset 0
for holes, and SPARSE_* stores an explicit codepoint list.
"""
import re
import sys
import unicodedata

PATH = sys.argv[1] if len(sys.argv) > 1 else "main/lv_font_ai_passport_14.c"
src = open(PATH, encoding="utf-8", errors="replace").read()


def slice_array(name, decl_re):
    m = re.search(decl_re, src)
    if not m:
        return None
    start = src.index("{", m.end() - 1) if False else m.end()
    end = src.index("};", start)
    return src[start:end]


# ---- glyph_dsc -------------------------------------------------------------
gd_body = slice_array("glyph_dsc", r"static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc\[\]\s*=\s*\{")
GD_RE = re.compile(
    r"\.bitmap_index\s*=\s*(-?\d+).*?\.adv_w\s*=\s*(-?\d+).*?\.box_w\s*=\s*(-?\d+)"
    r".*?\.box_h\s*=\s*(-?\d+).*?\.ofs_x\s*=\s*(-?\d+).*?\.ofs_y\s*=\s*(-?\d+)"
)
glyph_dsc = [tuple(int(x) for x in m.groups()) for m in GD_RE.finditer(gd_body)]

# ---- helper arrays (unicode_list_N / glyph_id_ofs_list_N) ------------------
helpers = {}
for m in re.finditer(r"static const (uint8_t|uint16_t) (\w+)\[\]\s*=\s*\{(.*?)\};", src, re.S):
    helpers[m.group(2)] = [int(t, 0) for t in re.findall(r"0x[0-9a-fA-F]+|\d+", m.group(3))]

# ---- cmaps ----------------------------------------------------------------
cm_body = slice_array("cmaps", r"static const lv_font_fmt_txt_cmap_t cmaps\[\]\s*=\s*\{")
CM_RE = re.compile(
    r"\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+),\s*\.glyph_id_start\s*=\s*(\d+),\s*"
    r"\.unicode_list\s*=\s*(\w+),\s*\.glyph_id_ofs_list\s*=\s*(\w+),\s*\.list_length\s*=\s*(\d+),\s*"
    r"\.type\s*=\s*(\w+)",
    re.S,
)
cmaps = []
for m in CM_RE.finditer(cm_body):
    rs, rl, gs, ul, gol, ll, ty = m.groups()
    cmaps.append(dict(range_start=int(rs), range_length=int(rl), glyph_id_start=int(gs),
                      unicode_list=None if ul == "NULL" else helpers[ul],
                      glyph_id_ofs_list=None if gol == "NULL" else helpers[gol],
                      list_length=int(ll), type=ty.rsplit("_CMAP_", 1)[-1]))

font_line_height = int(re.search(r"\.line_height\s*=\s*(\d+)", src).group(1))
font_base_line = int(re.search(r"\.base_line\s*=\s*(\d+)", src).group(1))


def gid_of(cp):
    """Exact port of LVGL get_glyph_dsc_id(). Returns 0 when not found."""
    if cp == 0:
        return 0
    for c in cmaps:
        rcp = cp - c["range_start"]
        if rcp < 0 or rcp >= c["range_length"]:
            continue
        t = c["type"]
        if t == "FORMAT0_TINY":
            return c["glyph_id_start"] + rcp
        if t == "FORMAT0_FULL":
            ofs = c["glyph_id_ofs_list"][rcp]
            if ofs == 0 and cp != c["range_start"]:
                continue  # hole: declared range, no glyph
            return c["glyph_id_start"] + ofs
        if t in ("SPARSE_TINY", "SPARSE_FULL"):
            try:
                i = c["unicode_list"].index(rcp)
            except ValueError:
                return 0
            if t == "SPARSE_TINY":
                return c["glyph_id_start"] + i
            return c["glyph_id_start"] + c["glyph_id_ofs_list"][i]
        raise SystemExit("unknown cmap type " + t)
    return 0


def probe(cp):
    gid = gid_of(cp)
    if gid == 0:
        return False, gid, None, "no glyph id (missing from cmaps or FULL-list hole)"
    if gid >= len(glyph_dsc):
        return False, gid, None, f"gid {gid} out of bounds (glyph_dsc has {len(glyph_dsc)})"
    d = glyph_dsc[gid]
    bi, adv, bw, bh = d[0], d[1], d[2], d[3]
    if bw * bh == 0:
        return (adv > 0), gid, d, "zero-size box (blank advance)" if adv else "zero box and zero advance"
    return True, gid, d, f"box {bw}x{bh} adv {adv/16:.2f}px"


# --------------------------------------------------------------------------
print(f"font        : {PATH}")
print(f"glyph_dsc   : {len(glyph_dsc)} entries (id 0 reserved -> max valid gid {len(glyph_dsc)-1})")
print(f"cmaps       : {len(cmaps)}  types={sorted({c['type'] for c in cmaps})}")
print(f"metrics     : line_height={font_line_height} base_line={font_base_line}")
declared = sum(c["range_length"] for c in cmaps)
resolvable = sum(1 for c in cmaps for r in range(c["range_length"]) if gid_of(c["range_start"] + r))
print(f"declared cps: {declared}   resolvable: {resolvable}   holes: {declared - resolvable}")
oob = [gid_of(c["range_start"] + r) for c in cmaps for r in range(c["range_length"])
       if gid_of(c["range_start"] + r) >= len(glyph_dsc)]
print(f"out-of-bounds gids reachable from cmaps: {len(oob)}")
print()

CANDIDATES = """● ✕ · ▮ ▯ ○ ◉ ▲ ▼ ■ □ ★ ☆ ⏺ ⏸ ─ │ ┌ ┐ └ ┘ ━ ┃ █ ▉ ▊ ▋ ▌ ▍ ▎ ▏ ░ ▒ ▓
◆ ◇ ▬ ▪ ▫ ✓ ✔ ✗ ✘ × ÷ ± → ← ↑ ↓ ⇒ ‧ • ° ¦ ‖ ⋯ … ⌁ ⚡ ♦ ♥ ⬛ ⬜ ▁ ▂ ▃ ▄ ▅ ▆ ▇
「 」 【 】 （ ） ： ， 。 、 ？ ！ · ‥ 〜 ～ － ＿ ｜ ＝ ＋ ＊ ＃ ＠ ％ ＄"""
cands = [ch for ch in CANDIDATES if not ch.isspace()]

print("=== SYMBOL / DECORATIVE CANDIDATES ===")
print(f"{'ch':<3} {'U+':<7} {'ok':<4} {'gid':<7} note   name")
for ch in cands:
    cp = ord(ch)
    ok, gid, d, note = probe(cp)
    try:
        nm = unicodedata.name(ch)
    except ValueError:
        nm = "?"
    print(f"{ch:<3} U+{cp:04X} {'YES' if ok else 'no ':<4} {gid:<7} {note:<38} {nm}")
print()

# ---- every char used in the firmware source ------------------------------
import glob
import os

srcdir = os.path.dirname(os.path.abspath(PATH))
used = {}
for f in sorted(glob.glob(os.path.join(srcdir, "*.c")) + glob.glob(os.path.join(srcdir, "*.h"))):
    if os.path.basename(f).startswith("lv_font"):
        continue
    txt = open(f, encoding="utf-8", errors="replace").read()
    for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', txt):
        for ch in lit:
            if ord(ch) > 0x7E or ord(ch) < 0x20:
                used.setdefault(ch, set()).add(os.path.basename(f))

print("=== NON-ASCII CHARACTERS IN main/*.c *.h STRING LITERALS ===")
bad = []
for ch in sorted(used, key=ord):
    ok, gid, d, note = probe(ord(ch))
    if not ok:
        bad.append((ch, note, sorted(used[ch])))
print(f"total distinct non-ASCII chars in literals: {len(used)}")
print(f"UNRENDERABLE: {len(bad)}")
for ch, note, files in bad:
    try:
        nm = unicodedata.name(ch)
    except ValueError:
        nm = "?"
    print(f"  {ch!r} U+{ord(ch):04X} {nm} -- {note} -- {', '.join(files)}")
print()

# ---- ASCII sanity --------------------------------------------------------
missing_ascii = [c for c in range(0x20, 0x7F) if not probe(c)[0]]
print(f"ASCII 0x20-0x7E unrenderable: {missing_ascii if missing_ascii else 'none'}")

# ---- declared range map --------------------------------------------------
print("\n=== CMAP RANGES ===")
for i, c in enumerate(cmaps):
    hole = sum(1 for r in range(c["range_length"]) if not gid_of(c["range_start"] + r))
    print(f"[{i:2}] U+{c['range_start']:04X}..U+{c['range_start']+c['range_length']-1:04X} "
          f"len={c['range_length']:<4} gid_start={c['glyph_id_start']:<6} {c['type']:<13} holes={hole}")
