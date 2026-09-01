#!/usr/bin/env python3
"""Generate the "AI in a jar" mascot frames for the voice screen.

The mascot is drawn here rather than hand-authored as pixel art so the shapes stay
editable and the output is reproducible: run this, commit the generated C, and the
art can be re-derived from the parameters instead of from a binary.

The figure matches the Cupai app icon (arle-app-demo/cupai-ui/Cupai.app): a cream
cup with a handle on a dark ground, and a four-pointed gold spark above it. The
spark is the live element — it grows and gains companions with microphone level —
so the picture answers "is it hearing me?", which is the device's only local proof
that audio is really being captured.

Output is opaque LV_COLOR_FORMAT_RGB565: each frame has the matching slice of the
ink-wash backdrop composited behind it. An RGB565A8 frame would need LVGL to
re-blend the backdrop on every swap (lv_image.c:810 reports any alpha format as
NOT_COVER), which measurably cost delivered audio on this single core. Baking the
背景 in trades Flash for draw time, which is the right way round here.

    tools/gen_mascot.py            # writes main/mascot_frames.c + .h
    tools/gen_mascot.py --preview  # also writes /tmp/mascot-*.png to eyeball

Frames, indexed by state and level:
    waiting   pale spark, small        (waiting for the PC)
    idle      gold spark               (ready)
    quiet     gold spark + 1 satellite     (recording, level 0-33)
    loud      yellow spark + 2 satellites  (recording, level 34-66)
    peak      orange spark + 4 satellites  (recording, level 67-100)
    fault     cup struck through, red  (bluetooth or audio unavailable)
"""
import argparse
import math
import os
import subprocess
import sys

SIZE = 160
LVGL_SCRIPT = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "managed_components", "lvgl__lvgl", "scripts", "LVGLImage.py")

# Palette straight from main/ui_pixel.h so the mascot cannot drift from the UI.
INK = (0x17, 0x20, 0x2A)
PAPER = (0xF4, 0xF4, 0xEA)
MUTED = (0xD9, 0xE7, 0xEC)
YELLOW = (0xFF, 0xD9, 0x28)
ORANGE = (0xFF, 0xB2, 0x3E)
GRASS = (0x82, 0xBE, 0x2D)
RED = (0xE4, 0x3B, 0x2F)
SKY = (0x16, 0x89, 0xE8)


def blend(a, b, t):
    """Linear blend, t=0 -> a, t=1 -> b. Used for the core's glow falloff."""
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def draw_frame(draw, spark_rgb, spark_scale, sparks, cracked=False, fill=0.0):
    """Draw one mascot frame: a cup with an AI spark rising from it.

    Follows the Cupai app icon (arle-app-demo/cupai-ui): a cream cup with a handle
    on a dark ground, and a four-pointed gold spark above it. The spark is what
    reacts to microphone level — it grows and gains companions as the user speaks —
    so the picture answers "is it hearing me?".

    spark_rgb    colour of the spark
    spark_scale  0..1+, size of the main spark
    sparks       0..2 extra small sparks flanking it
    cracked      fault state: the cup is struck through
    fill         0..1 liquid level inside the cup, driven by remaining quota — a
                 glanceable form of a number nobody wants to read
    """
    cup = PAPER if not cracked else blend(PAPER, RED, 0.35)
    cx = SIZE // 2

    # --- the spark: a four-pointed star, two crossed tapered diamonds. Above the
    # --- cup, which is what makes it read as rising out of it.
    if spark_scale > 0.01:
        for dx, dy, scale in _spark_layout(sparks, spark_scale):
            _star(draw, cx + dx, 44 + dy, scale, spark_rgb)

    # --- the cup: tapered body, rim, handle. Filled, so it anchors the eye while
    # --- the spark and waves animate around it.
    draw.polygon([(46, 88), (114, 88), (105, 142), (55, 142)], fill=cup + (255,))
    # Liquid: a darker inset wedge, inset from the cup walls so a rim of cream
    # always frames it. Drawn on top of the body, following the same taper.
    if fill > 0.01:
        top = 142 - round(fill * 48)                 # 94 at full, 142 at empty
        t = (142 - top) / 48.0
        wl = 50 + round(4 * (1 - t))                 # left wall at this height
        wr = 110 - round(4 * (1 - t))                # right wall
        draw.polygon([(wl, top), (wr, top), (105, 138), (55, 138)],
                     fill=blend(INK, SKY, 0.75) + (255,))
    draw.rounded_rectangle([42, 81, 118, 93], radius=5, fill=cup + (255,))
    draw.arc([106, 95, 138, 127], -80, 80, fill=cup + (255,), width=5)
    draw.line([40, 149, 120, 149], fill=MUTED + (160,), width=3)

    if cracked:
        draw.line([50, 78, 116, 148], fill=RED + (255,), width=5)


# Satellite sparks: (x offset, y offset, scale factor). Deliberately asymmetric —
# a mirrored pair reads as a diagram, a scattered group reads as sparks.
SATELLITES = [
    (-31, 14, 0.34),
    (26, -8, 0.42),
    (34, 26, 0.26),
    (-22, -18, 0.30),
]


def _spark_layout(extra, scale):
    """(x offset, y offset, scale) per star, main one first."""
    out = [(0, 0, scale)]
    for dx, dy, f in SATELLITES[:extra]:
        out.append((dx, dy, scale * f))
    return out


def _star(draw, cx, cy, scale, rgb):
    """Four-pointed star: long vertical spike, shorter horizontal one."""
    v = max(3, round(26 * scale))        # vertical reach
    h = max(2, round(10 * scale))        # horizontal reach
    w = max(1, round(6 * scale))         # waist half-width
    draw.polygon([(cx, cy - v), (cx + w, cy), (cx, cy + v), (cx - w, cy)],
                 fill=rgb + (255,))
    draw.polygon([(cx - h * 2, cy), (cx, cy - w), (cx + h * 2, cy), (cx, cy + w)],
                 fill=rgb + (255,))
    # A white-hot centre: a flat-filled star reads as a painted shape, a blown-out
    # core reads as a light source. Only on stars big enough to show it.
    if scale > 0.5:
        c = max(1, round(3 * scale))
        hot = blend(rgb, PAPER, 0.75)
        draw.ellipse([cx - c, cy - c, cx + c, cy + c], fill=hot + (255,))


# The icon's gold reads muddy on a near-black panel — it was picked against macOS's
# light dock. Brighten the whole ramp toward UI_YELLOW so the spark looks emissive.
GOLD = (0xFF, 0xC8, 0x4A)

FRAMES = [
    # name       spark colour             scale  satellites  cracked
    ("waiting", blend(INK, MUTED, 0.55),  0.50,  0,          False),
    ("idle",     GOLD,                    0.80,  0,          False),
    ("quiet",    GOLD,                    0.95,  1,          False),
    ("loud",     YELLOW,                  1.10,  2,          False),
    # Peak stays warm rather than going full orange: ORANGE alone lost the
    # brightness the loudest frame should have, so blend it back toward yellow.
    ("peak",     blend(ORANGE, YELLOW, 0.5), 1.25, 4,        False),
    ("fault",    RED,                     0.00,  0,          True),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", action="store_true",
                    help="also write /tmp/mascot-*.png for visual inspection")
    args = ap.parse_args()
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("gen_mascot: needs Pillow (pip install pillow)", file=sys.stderr)
        return 1

    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(here, "..", "main")
    png_dir = "/tmp/mascot-png"
    os.makedirs(png_dir, exist_ok=True)

    names = []
    # The backdrop slice that sits behind the mascot on screen, so each frame can
    # ship opaque. MASCOT_Y must match the lv_obj_align in demo_voice.c.
    MASCOT_Y = 50
    try:
        bg = Image.open("/tmp/backdrop.png").convert("RGB")
        bg_slice = bg.crop(((240 - SIZE) // 2, MASCOT_Y,
                            (240 - SIZE) // 2 + SIZE, MASCOT_Y + SIZE))
    except OSError:
        print("gen_mascot: run tools/gen_backdrop.py first", file=sys.stderr)
        return 1

    for name, colour, scale, extra, cracked in FRAMES:
        img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        draw_frame(ImageDraw.Draw(img), colour, scale, extra, cracked)
        flat = bg_slice.copy()
        flat.paste(img, (0, 0), img)
        img = flat.convert("RGB")
        path = os.path.join(png_dir, f"mascot_{name}.png")
        img.save(path)
        names.append((name, path))
        if args.preview:
            img.resize((SIZE * 2, SIZE * 2), Image.NEAREST).save(
                f"/tmp/mascot-{name}-3x.png")

    # Convert with LVGL's own script so the C output matches this LVGL version's
    # lv_image_dsc_t exactly — hand-written descriptors rot across LVGL releases.
    script = os.path.normpath(LVGL_SCRIPT)
    if not os.path.exists(script):
        print(f"gen_mascot: LVGLImage.py not found at {script}", file=sys.stderr)
        return 1
    for name, path in names:
        cmd = [sys.executable, script, "--ofmt", "C", "--cf", "RGB565",
               "-o", os.path.normpath(out_dir), path]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"gen_mascot: convert failed for {name}:\n{r.stderr}",
                  file=sys.stderr)
            return 1

    # LVGLImage.py emits a #else branch that includes "lvgl/lvgl.h", which does not
    # exist in this project's include path (the managed component is reached as
    # plain "lvgl.h"). LVGL's own LV_LVGL_H_INCLUDE_SIMPLE switch selects the right
    # form, so define it at the top of each generated file rather than patching the
    # include itself — that keeps the files regenerable.
    for name, _ in names:
        c = os.path.join(out_dir, f"mascot_{name}.c")
        with open(c) as f:
            body = f.read()
        if "LV_LVGL_H_INCLUDE_SIMPLE" in body.split("\n")[0]:
            continue                      # already patched
        with open(c, "w") as f:
            f.write("#define LV_LVGL_H_INCLUDE_SIMPLE 1\n" + body)

    print(f"gen_mascot: wrote {len(names)} frames to main/ "
          f"({SIZE}x{SIZE} opaque RGB565, ~{SIZE*SIZE*2//1024} KB each)")
    if args.preview:
        print("gen_mascot: previews at /tmp/mascot-*-3x.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
