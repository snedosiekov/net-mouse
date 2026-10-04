"""Generates the application/tray icons in src/ui/win/res (requires Pillow).

    python tools/make_icons.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
OUT = Path(__file__).resolve().parent.parent / "src" / "ui" / "win" / "res"

VARIANTS = {
    "app.ico": ((52, 101, 164), (24, 52, 92)),      # idle: blue
    "active.ico": ((255, 140, 0), (150, 70, 0)),    # pointer on a slave / being controlled: orange
}


def draw(size: int, fill, outline) -> Image.Image:
    s = 8 * size  # supersample, then downscale for smooth edges
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Small icons get relatively thicker strokes so the details survive downscaling.
    stroke = max(int(s * (0.09 if size <= 24 else 0.06)), 1)
    w, h = s * 0.62, s * 0.92
    left, top = (s - w) / 2, (s - h) / 2
    body = (left, top, left + w, top + h)
    d.rounded_rectangle(body, radius=w / 2, fill=fill, outline=outline, width=stroke)

    split_y = top + h * 0.42
    cx = s / 2
    d.line((left + stroke / 2, split_y, left + w - stroke / 2, split_y), fill=outline, width=stroke)
    d.line((cx, top + stroke / 2, cx, split_y), fill=outline, width=stroke)
    if size >= 24:
        wheel_w, wheel_h = w * 0.16, h * 0.16
        d.rounded_rectangle(
            (cx - wheel_w / 2, top + h * 0.13, cx + wheel_w / 2, top + h * 0.13 + wheel_h),
            radius=wheel_w / 2, fill=(255, 255, 255), outline=outline, width=max(stroke // 2, 1))
    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for name, (fill, outline) in VARIANTS.items():
        images = [draw(sz, fill, outline) for sz in SIZES]
        largest = images[-1]
        largest.save(OUT / name, format="ICO", sizes=[(sz, sz) for sz in SIZES], append_images=images[:-1])
        print("wrote", OUT / name)
    # Preview strip for a quick visual check.
    preview = Image.new("RGBA", (sum(SIZES) * 2 + 40, 256 + 20), (255, 255, 255, 255))
    x = 10
    for name, (fill, outline) in VARIANTS.items():
        for sz in SIZES[:-1]:
            preview.alpha_composite(draw(sz, fill, outline), (x, 10))
            x += sz + 4
    preview.save(OUT.parent.parent.parent.parent / "build" / "icon-preview.png")


if __name__ == "__main__":
    main()
