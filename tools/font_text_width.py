#!/usr/bin/env python3
"""Measure rendered pixel width of UI strings against an LVGL font .c file.

Reuses font_glyph_probe.py's exact port of LVGL get_glyph_dsc_id() so a string is
measured with the SAME cmap resolution the firmware performs, and reports any
codepoint that resolves to glyph id 0 (a placeholder box on the panel).

Width model matches lv_text_get_width(): sum(adv_w >> 4) + letter_space per gap.
letter_space defaults to 0 for a plain lv_label. Screen is 240 px wide; the voice
screen's labels are LV_SIZE_CONTENT and never wrap, so > 240 px is clipped.
"""
import re
import sys
import unicodedata

FONT = "main/lv_font_ai_passport_14.c"
src = open(FONT, encoding="utf-8", errors="replace").read()

gd_m = re.search(r"static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc\[\]\s*=\s*\{", src)
gd_body = src[gd_m.end():src.index("};", gd_m.end())]
GD_RE = re.compile(
    r"\.bitmap_index\s*=\s*(-?\d+).*?\.adv_w\s*=\s*(-?\d+).*?\.box_w\s*=\s*(-?\d+)"
    r".*?\.box_h\s*=\s*(-?\d+).*?\.ofs_x\s*=\s*(-?\d+).*?\.ofs_y\s*=\s*(-?\d+)")
glyph_dsc = [tuple(int(x) for x in m.groups()) for m in GD_RE.finditer(gd_body)]

helpers = {}
for m in re.finditer(r"static const (uint8_t|uint16_t) (\w+)\[\]\s*=\s*\{(.*?)\};", src, re.S):
    helpers[m.group(2)] = [int(t, 0) for t in re.findall(r"0x[0-9a-fA-F]+|\d+", m.group(3))]

cm_m = re.search(r"static const lv_font_fmt_txt_cmap_t cmaps\[\]\s*=\s*\{", src)
cm_body = src[cm_m.end():src.index("};", cm_m.end())]
CM_RE = re.compile(
    r"\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+),\s*\.glyph_id_start\s*=\s*(\d+),\s*"
    r"\.unicode_list\s*=\s*(\w+),\s*\.glyph_id_ofs_list\s*=\s*(\w+),\s*\.list_length\s*=\s*(\d+),\s*"
    r"\.type\s*=\s*(\w+)", re.S)
cmaps = []
for m in CM_RE.finditer(cm_body):
    rs, rl, gs, ul, gol, ll, ty = m.groups()
    cmaps.append(dict(range_start=int(rs), range_length=int(rl), glyph_id_start=int(gs),
                      unicode_list=None if ul == "NULL" else helpers[ul],
                      glyph_id_ofs_list=None if gol == "NULL" else helpers[gol],
                      list_length=int(ll), type=ty.rsplit("_CMAP_", 1)[-1]))


def gid_of(cp):
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
                continue
            return c["glyph_id_start"] + ofs
        if t in ("SPARSE_TINY", "SPARSE_FULL"):
            try:
                i = c["unicode_list"].index(rcp)
            except ValueError:
                return 0
            if t == "SPARSE_TINY":
                return c["glyph_id_start"] + i
            return c["glyph_id_start"] + c["glyph_id_ofs_list"][i]
    return 0


def renders(cp):
    """True iff the codepoint draws something (or is a real whitespace advance)."""
    gid = gid_of(cp)
    if gid == 0 or gid >= len(glyph_dsc):
        return False
    bi, adv, bw, bh = glyph_dsc[gid][:4]
    return bw * bh > 0 or adv > 0


LINE_HEIGHT = int(re.search(r"\.line_height\s*=\s*(\d+)", src).group(1))
# lv_font_get_glyph_dsc(): with LV_USE_FONT_PLACEHOLDER=y (sdkconfig:2639) a
# codepoint with no glyph is NOT skipped — it becomes a hollow 1 px-border box
# of box_w = line_height/2, adv_w = box_w + 2. So missing glyphs still consume
# width AND are visible on the panel.
PLACEHOLDER_ADV = LINE_HEIGHT // 2 + 2


def width(s, letter_space=0):
    """lv_text_get_width() for one line. Returns (px, [missing chars])."""
    px = 0
    missing = []
    for ch in s:
        gid = gid_of(ord(ch))
        if gid == 0 or gid >= len(glyph_dsc):
            missing.append(ch)
            px += PLACEHOLDER_ADV
        else:
            # lv_font_fmt_txt.c:250 rounds: (adv_w + 8) >> 4, not a bare shift.
            px += (glyph_dsc[gid][1] + 8) >> 4
        px += letter_space
    if s:
        px -= letter_space
    return px, missing


def report(label, s, limit=240):
    for i, line in enumerate(s.split("\n")):
        px, missing = width(line)
        tag = f"{label} L{i}" if "\n" in s else label
        verdict = "FIT " if px <= limit else "OVER"
        miss = ""
        if missing:
            miss = "  MISSING:" + " ".join(
                f"{c!r}=U+{ord(c):04X}({unicodedata.name(c, '?')})" for c in dict.fromkeys(missing))
        print(f"{verdict} {px:4d}px  {tag:<26} {line!r}{miss}")


if __name__ == "__main__":
    if len(sys.argv) > 1:
        for arg in sys.argv[1:]:
            report("argv", arg)
        sys.exit(0)

    print("=== CURRENT voice-screen strings (main/demo_voice.c) ===")
    for name, s in [
        ("title:315", "语音输入"),
        ("island:253", "CL 未知   CX 12%"),
        ("island:335", "Claude --"),
        ("big:262 idle", "○\n╱│╲\n ╱╲"),
        ("sub:263", "就绪"),
        ("big:257", "连接中"),
        ("sub:258", "等待电脑蓝牙连接"),
        ("big:282 rec", "♫  ♪\n╲◉╱\n │\n╱ ╲"),
        ("sub:287 rec", "12.3s  ====........"),
        ("sub:278 weak", "12.3s  信号弱  ====........"),
        ("big:292 err", "◎\n╱│╲\n ╳"),
        ("sub:293 err", "蓝牙或音频不可用"),
        ("sub:353 oom", "内存不足，请重启设备"),
    ]:
        report(name, s)

    print("\n=== PROPOSED strings ===")
    for name, s in [
        ("title", "语音输入"),
        ("island both", "Claude 42%  Codex 12%"),
        ("island cl n/a", "Claude 未订阅  Codex 12%"),
        ("island no link", "Claude --  Codex --"),
        ("island short", "CC 42%  CX 12%"),
        ("island alt", "克劳德 42%  科덱 12%"),
        ("sub connecting", "连接电脑…"),
        ("sub connecting2", "等待电脑连接"),
        ("sub idle", "就绪"),
        ("sub rec", "12.3s  ============"),
        ("sub weak", "12.3s  丢帧  ========...."),
        ("err bt", "蓝牙启动失败，请重启设备"),
        ("err audio", "麦克风故障，请重启设备"),
        ("err oom", "内存不足，请重启设备"),
        ("big idle safe", "就绪"),
        ("big rec safe", "录音中"),
    ]:
        report(name, s)
