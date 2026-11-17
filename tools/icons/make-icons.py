#!/usr/bin/env python3
"""Render the desktop icon set from the shared FujiNet Go mark.

There is no PMD 85 Android app to take launcher art from, so, as for the
Atari 800 desktop app, the family mark (data/icons/src/
fujinet-go-pmd85-foreground.png, the same file as the CoCo/Atari repos'
foreground) is put on the machine's own colours, sampled from a 3D render
of the PMD 85-1: the case's dark grey as background, the mark white, and
its centre disc (black in the artwork) in the blue of the K0-K11 function
keys. The mark is recoloured by brightness (white -> MARK, black -> CENTRE).
Everything else matches the siblings' make-icons.py: rounded square, every
hicolor size, and the macOS .icns.

Outputs are committed; re-run only when the art or colours change:

    python3 tools/icons/make-icons.py
"""

import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
FOREGROUND = ROOT / "data/icons/src/fujinet-go-pmd85-foreground.png"
OUTDIR = ROOT / "data/icons/hicolor"
ICNS_OUT = ROOT / "data/icons/fujinet-go-pmd85.icns"

BACKGROUND = (0x57, 0x56, 0x54, 0xFF)    # #575654, the case's dark grey
MARK = (0xFF, 0xFF, 0xFF)                # white, as the family's mark
CENTRE = (0x68, 0x94, 0xB2)              # #6894B2, its blue function keys
MASTER = 1024                            # render big, downsample with LANCZOS
CORNER_RADIUS = 0.22                     # fraction of the edge
FOREGROUND_ZOOM = 1.5                    # the Android launcher icons' mark size (siblings: 1.18)
SIZES = (16, 24, 32, 48, 64, 128, 192, 256, 512)
ICNS_SIZES = (32, 64, 128, 256, 512, 1024)  # every size macOS's icns TOC references


def render_master() -> Image.Image:
    art = Image.open(FOREGROUND).convert("RGBA")
    # Recolour by brightness (alpha untouched: it is the mark's shape):
    # white -> MARK, black (the centre disc) -> CENTRE, edges in between.
    r, g, b, a = art.split()
    lum = Image.merge("RGB", (r, g, b)).convert("L")
    mark_fill = Image.new("RGB", art.size, MARK)
    centre_fill = Image.new("RGB", art.size, CENTRE)
    art = Image.composite(mark_fill, centre_fill, lum).convert("RGBA")
    art.putalpha(a)

    # Rounded-square background on a transparent canvas, drawn at 4x and
    # downsampled so the corners are antialiased.
    scale = 4
    big = Image.new("RGBA", (MASTER * scale, MASTER * scale), (0, 0, 0, 0))
    ImageDraw.Draw(big).rounded_rectangle(
        (0, 0, MASTER * scale - 1, MASTER * scale - 1),
        radius=int(MASTER * scale * CORNER_RADIUS),
        fill=BACKGROUND,
    )
    icon = big.resize((MASTER, MASTER), Image.LANCZOS)

    art_size = int(MASTER * FOREGROUND_ZOOM)
    art = art.resize((art_size, art_size), Image.LANCZOS)
    offset = (MASTER - art_size) // 2
    overlay = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    overlay.paste(art, (offset, offset), art)

    # Keep the foreground inside the rounded silhouette.
    composed = Image.alpha_composite(icon, overlay)
    composed.putalpha(Image.composite(composed.getchannel("A"),
                                      Image.new("L", (MASTER, MASTER), 0),
                                      icon.getchannel("A")))
    return composed


def main() -> int:
    if not FOREGROUND.exists():
        print(f"missing artwork: {FOREGROUND}", file=sys.stderr)
        return 1

    master = render_master()
    for size in SIZES:
        out = OUTDIR / f"{size}x{size}" / "apps" / "fujinet-go-pmd85.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        master.resize((size, size), Image.LANCZOS).save(out, optimize=True)
        print(f"wrote {out.relative_to(ROOT)}")

    # Pillow's ICNS writer works on any platform (no iconutil needed): it
    # just packs PNGs into the icns TOC. Pass every non-master size in
    # explicitly, LANCZOS-downsampled from the 1024 master like the hicolor
    # set above, so nothing gets a blurry re-resize from a smaller source.
    variants = [master.resize((size, size), Image.LANCZOS)
                for size in ICNS_SIZES if size != master.width]
    master.save(ICNS_OUT, format="ICNS", append_images=variants)
    print(f"wrote {ICNS_OUT.relative_to(ROOT)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
