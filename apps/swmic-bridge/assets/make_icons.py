# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow"]
# ///
"""Generate all app / tray icons from icon-source.png.

    uv run apps/swmic-bridge/assets/make_icons.py

Outputs (next to this script, committed to git):
  glyph.png      black glyph on transparent, square, 64 px  -> macOS menu bar (template image)
  tile.png       glyph on a white rounded tile, 64 px        -> Windows tray
  app.ico        Windows exe icon (16-256 px)
  app.icns       macOS app icon
"""

import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent
SRC = HERE / "icon-source.png"


def glyph_square(src: Image.Image, pad: float) -> Image.Image:
    """Crop to the glyph's bounding box and center it on a transparent square
    with `pad` (fraction of the side) of empty margin."""
    src = src.convert("RGBA")
    # the drawing is black/white on transparent; trust alpha for the bbox
    bbox = src.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
    g = src.crop(bbox)
    side = int(max(g.size) / (1 - 2 * pad))
    out = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    out.paste(g, ((side - g.width) // 2, (side - g.height) // 2), g)
    return out


def glyph_mask(src: Image.Image, size: int, pad: float) -> Image.Image:
    """Black-only glyph (dark strokes become opaque black, the white face and
    the background become transparent) - suitable as a macOS template image."""
    sq = glyph_square(src, pad).resize((size, size), Image.LANCZOS)
    r, g, b, a = sq.split()
    lum = Image.merge("RGB", (r, g, b)).convert("L")
    # opaque where the source is opaque AND dark
    alpha = Image.eval(Image.merge("LA", (lum, a)).split()[0], lambda v: 255 - v)
    alpha = Image.composite(alpha, Image.new("L", sq.size, 0), a)
    out = Image.new("RGBA", sq.size, (0, 0, 0, 255))
    out.putalpha(alpha)
    return out


def rounded_tile(size: int, inset: float, radius: float, shadow: bool) -> Image.Image:
    """White rounded square (macOS-like) with an optional soft drop shadow."""
    ss = 4  # supersample for smooth edges
    S = size * ss
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    x0 = int(S * inset)
    x1 = S - x0
    r = int((x1 - x0) * radius)
    if shadow:
        sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        ImageDraw.Draw(sh).rounded_rectangle((x0, x0 + S // 64, x1, x1 + S // 64), r, fill=(0, 0, 0, 90))
        img = Image.alpha_composite(img, sh.filter(ImageFilter.GaussianBlur(S / 80)))
    ImageDraw.Draw(img).rounded_rectangle((x0, x0, x1, x1), r, fill=(255, 255, 255, 255),
                                           outline=(0, 0, 0, 28), width=max(1, S // 512))
    return img.resize((size, size), Image.LANCZOS)


def tile_icon(src: Image.Image, size: int, inset: float, glyph_frac: float, shadow: bool) -> Image.Image:
    tile = rounded_tile(size, inset, 0.225, shadow)
    gs = int(size * glyph_frac)
    g = glyph_square(src, 0.0).resize((gs, gs), Image.LANCZOS)
    tile.alpha_composite(g, ((size - gs) // 2, (size - gs) // 2))
    return tile


def main() -> None:
    src = Image.open(SRC)

    # menu bar template glyph (macOS) and Windows tray tile
    glyph_mask(src, 64, 0.04).save(HERE / "glyph.png")
    tile_icon(src, 64, 0.03, 0.80, shadow=False).save(HERE / "tile.png")

    # Windows .ico: nearly full-bleed tile, several sizes
    ico_sizes = [16, 24, 32, 48, 64, 128, 256]
    big = tile_icon(src, 256, 0.02, 0.80, shadow=False)
    big.save(HERE / "app.ico", sizes=[(s, s) for s in ico_sizes])

    # macOS .icns: Apple grid (824/1024 body, shadow), via iconutil
    with tempfile.TemporaryDirectory() as tmp:
        iconset = Path(tmp) / "app.iconset"
        iconset.mkdir()
        for pt in [16, 32, 128, 256, 512]:
            for scale in (1, 2):
                px = pt * scale
                name = f"icon_{pt}x{pt}{'@2x' if scale == 2 else ''}.png"
                tile_icon(src, px, 100 / 1024, 0.62, shadow=px >= 64).save(iconset / name)
        subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(HERE / "app.icns")], check=True)

    print("wrote glyph.png tile.png app.ico app.icns")


if __name__ == "__main__":
    main()
