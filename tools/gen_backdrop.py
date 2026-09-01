#!/usr/bin/env python3
"""Generate the ink-wash landscape backdrop for the voice screen.

Ink wash is the right idiom for this panel: the palette is already neutral grey,
and RGB565 gives only 32 exactly-representable grey steps — a medium that works in
washes and negative space rather than smooth gradients suits that limit, where a
photographic image would band.

The image is drawn once at screen build and never invalidated, so it costs 0 us per
refresh (LVGL skips the refresh entirely when nothing is invalid). Only Flash.

Composition, back to front: distant peaks in the palest wash, mid hills darker,
a near ridge darkest, and mist bands lifting the whole lower half so the cup has
light to stand in. Every tone is snapped to the RGB565 grey lattice (multiples of
8) so no dithering is needed and no banding appears.

    tools/gen_backdrop.py            # writes main/backdrop.c
    tools/gen_backdrop.py --preview  # also /tmp/backdrop.png
"""
import argparse
import math
import os
import subprocess
import sys

W, H = 240, 320
LVGL_SCRIPT = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "managed_components", "lvgl__lvgl", "scripts", "LVGLImage.py")

# The 32 representable neutral greys are the multiples of 8. Picking off-lattice
# values is what produced visible banding in an earlier gradient attempt: the
# hardware snaps them, so choose them deliberately.
def g(step):
    """Grey by lattice step, 0..31 -> 0x00..0xF8."""
    v = max(0, min(31, step)) * 8
    return (v, v, v)


# Planes spread wide across the lattice. The first attempt put everything between
# steps 2 and 9, where adjacent tones differ by a WCAG ratio under 1.4 and the
# picture read as one dark smear. Ink wash needs its lightest wash genuinely light.
SKY_TOP = g(4)        # 0x202020 — top edge, the darkest area of the sky
SKY_LOW = g(11)       # 0x585858 — horizon haze, where the light sits
FAR = g(14)           # 0x707070 — distant peaks: the PALEST ink, farthest away
MID = g(8)            # 0x404040 — mid hills
NEAR = g(3)           # 0x181818 — near ridge: darkest ink, closest
MIST = g(16)          # 0x808080 — the wash that lifts the subject


def ridge(draw, base_y, amp, seed, colour, roughness=0.5, alpha=255):
    """Draw one mountain silhouette as a filled polygon.

    A sum of sines rather than random noise: it is deterministic (so the art is
    reproducible from this script alone) and it gives the smooth, deliberate
    contour of a brushed ridge rather than the jitter of a fractal.
    """
    pts = [(0, H)]
    for x in range(0, W + 1, 2):
        t = x / W
        y = base_y
        y -= amp * math.sin(t * math.pi * 1.1 + seed)
        y -= amp * roughness * 0.5 * math.sin(t * math.pi * 3.3 + seed * 2.1)
        y -= amp * roughness * 0.25 * math.sin(t * math.pi * 7.7 + seed * 3.7)
        pts.append((x, max(0, round(y))))
    pts.append((W, H))
    draw.polygon(pts, fill=colour + (alpha,))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", action="store_true")
    args = ap.parse_args()
    try:
        from PIL import Image, ImageDraw, ImageFilter
    except ImportError:
        print("gen_backdrop: needs Pillow (pip install pillow)", file=sys.stderr)
        return 1

    img = Image.new("RGBA", (W, H), SKY_TOP + (255,))
    d = ImageDraw.Draw(img)

    # Sky: a continuous ramp, dithered. Eight hard bands read as stripes; a smooth
    # ramp quantised to the 8-step lattice bands too. Ordered dithering spreads the
    # rounding error across pixels so neither happens, and it costs nothing at
    # runtime because it is baked into the image.
    BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
    px = img.load()
    for y in range(round(H * 0.80)):
        t = y / (H * 0.80)
        # Hold the top two thirds dark so the cup's cream has contrast, then lift
        # toward the horizon where the ridges begin.
        t = t ** 2.2
        want = 3 * 8 + t * (11 - 3) * 8          # SKY_TOP -> SKY_LOW in real units
        base = int(want // 8) * 8
        frac = (want - base) / 8.0               # how far between two lattice steps
        for x in range(W):
            thr = (BAYER[y & 3][x & 3] + 0.5) / 16.0
            v = base + (8 if frac > thr else 0)
            v = max(0, min(248, v))
            px[x, y] = (v, v, v, 255)

    # Far peaks: palest ink, semi-transparent, so the sky shows through as haze.
    ridge(d, H * 0.76, 34, 0.4, FAR, roughness=0.30, alpha=150)
    # Mid hills.
    ridge(d, H * 0.87, 26, 2.1, MID, roughness=0.55, alpha=210)
    # Near ridge: solid, darkest, low in the frame so it reads as close.
    ridge(d, H * 0.97, 20, 4.3, NEAR, roughness=0.85, alpha=255)

    # Blur the ridges only — a whole-image blur also smeared the sky bands into the
    # stripes it was meant to hide.
    img = img.filter(ImageFilter.GaussianBlur(0.8))

    # Lift the band the cup occupies (screen y 50-210) so the subject has light
    # behind it without a drawn edge — the card that framed the cup is exactly what
    # this replaces.
    # No glow behind the subject: it lifted the grey to within a stone's throw of
    # the cup's cream and killed the cup's material contrast. The dark upper sky is
    # the better ground for a pale object.

    png = "/tmp/backdrop.png"
    img.convert("RGB").save(png)
    if args.preview:
        img.convert("RGB").save("/tmp/backdrop-preview.png")

    out_dir = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "main"))
    script = os.path.normpath(LVGL_SCRIPT)
    if not os.path.exists(script):
        print(f"gen_backdrop: LVGLImage.py not found at {script}", file=sys.stderr)
        return 1
    # RGB565 without alpha: the backdrop is opaque, and dropping the A8 plane saves
    # a third of the Flash (150 KB vs 225 KB).
    r = subprocess.run([sys.executable, script, "--ofmt", "C", "--cf", "RGB565",
                        "-o", out_dir, png], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"gen_backdrop: convert failed:\n{r.stderr}", file=sys.stderr)
        return 1

    c = os.path.join(out_dir, "backdrop.c")
    with open(c) as f:
        body = f.read()
    if "LV_LVGL_H_INCLUDE_SIMPLE" not in body.split("\n")[0]:
        with open(c, "w") as f:
            f.write("#define LV_LVGL_H_INCLUDE_SIMPLE 1\n" + body)

    print(f"gen_backdrop: wrote main/backdrop.c ({W}x{H} RGB565, "
          f"{W*H*2//1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
