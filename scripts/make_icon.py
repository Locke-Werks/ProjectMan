"""Generate assets/projectman.ico.

The mark is the "//" eyebrow prefix from the ARCHON / Specter Point design
language, in the signature red on black. It is drawn geometrically at every
size rather than downscaled from one large bitmap, because the notification
area renders this at 16px and a resampled glyph turns to mush there.

The ICO container is written by hand. Pillow's ICO writer resamples a single
source image to every requested size, which is exactly the behaviour this
script exists to avoid. Entries are PNG-compressed at all sizes, which Windows
has accepted since Vista and this product requires Windows 11 anyway.
"""

import io
import struct
from pathlib import Path

from PIL import Image, ImageDraw

BLACK = (0, 0, 0, 255)
BORDER = (31, 31, 31, 255)  # #1F1F1F hairline
RED = (255, 0, 0, 255)  # #FF0000, the only accent

SIZES = [16, 24, 32, 48, 64, 128, 256]


def slash(draw: ImageDraw.ImageDraw, x: float, w: float, top: float, bot: float,
          lean: float) -> None:
    """One leaning bar of the // mark, as a filled parallelogram."""
    draw.polygon(
        [(x + lean, top), (x + lean + w, top), (x - lean + w, bot), (x - lean, bot)],
        fill=RED,
    )


def render(s: int) -> Image.Image:
    # Supersample everywhere except 16px, where hinting the geometry to whole
    # pixels reads sharper than any amount of antialiasing.
    scale = 1 if s == 16 else 4
    n = s * scale
    img = Image.new("RGBA", (n, n), BLACK)
    d = ImageDraw.Draw(img)

    d.rectangle([0, 0, n - 1, n - 1], outline=BORDER, width=max(1, scale))

    top, bot = n * 0.24, n * 0.76
    w = n * 0.13     # bar thickness
    lean = n * 0.10  # horizontal offset, top vs bottom
    gap = n * 0.20   # space between the bars

    x0 = (n - (w * 2 + gap)) / 2
    slash(d, x0, w, top, bot, lean)
    slash(d, x0 + w + gap, w, top, bot, lean)

    return img.resize((s, s), Image.LANCZOS) if scale > 1 else img


def write_ico(path: Path, images: list[Image.Image]) -> None:
    blobs = []
    for im in images:
        buf = io.BytesIO()
        im.save(buf, format="PNG", optimize=True)
        blobs.append(buf.getvalue())

    n = len(blobs)
    header = struct.pack("<HHH", 0, 1, n)          # reserved, type=icon, count
    offset = len(header) + 16 * n

    entries = bytearray()
    for im, blob in zip(images, blobs):
        # A dimension of 256 is stored as 0.
        w = 0 if im.width >= 256 else im.width
        h = 0 if im.height >= 256 else im.height
        entries += struct.pack(
            "<BBBBHHII", w, h, 0, 0, 1, 32, len(blob), offset
        )
        offset += len(blob)

    path.write_bytes(header + bytes(entries) + b"".join(blobs))


def main() -> None:
    out = Path(__file__).resolve().parent.parent / "assets" / "projectman.ico"
    out.parent.mkdir(parents=True, exist_ok=True)

    write_ico(out, [render(s) for s in SIZES])
    print(f"wrote {out} with {len(SIZES)} images: {SIZES}")


if __name__ == "__main__":
    main()
