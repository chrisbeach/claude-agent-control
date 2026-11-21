#!/usr/bin/env python3
"""Renders every scene to out/*.png plus a side-by-side sheet. Run via `make png`.
`render.py --raw FILE OUT.png` converts one raw RGB565 frame instead (a device /screenshot)."""
import pathlib
import subprocess
import sys

from PIL import Image, ImageDraw

HERE = pathlib.Path(__file__).parent
OUT = HERE / "out"
W, H = 800, 480
# scene -> (fixture or link state, action)
SCENES = {
    "calm": ("fixtures/fleet_calm.json", "none"),
    "mixed": ("fixtures/fleet_mixed.json", "none"),
    "tap-opening": ("fixtures/fleet_mixed.json", "tap:1"),
    "tap-raised": ("fixtures/fleet_mixed.json", "raised:1"),
    "tap-refused": ("fixtures/fleet_mixed.json", "refused:3"),
    "tap-waiting": ("fixtures/fleet_mixed.json", "tap:0"),
    "tap-last-row": ("fixtures/fleet_mixed.json", "raised:6"),
    "new-opening": ("fixtures/fleet_mixed.json", "new"),
    "new-opened": ("fixtures/fleet_mixed.json", "new-opened"),
    "new-refused": ("fixtures/fleet_mixed.json", "new-refused"),
    "filter": ("fixtures/fleet_many.json", "filter:BUILDBOX"),
    "many": ("fixtures/fleet_many.json", "none"),
    "many-scrolled": ("fixtures/fleet_many.json", "scroll:400"),
    "idle": ("fixtures/fleet_idle.json", "none"),
    "empty": ("fixtures/fleet_empty.json", "none"),
    "wifi": ("wifi", "none"),
    "fetching": ("fetching", "none"),
    "lost": ("lost", "none"),
    "error": ("error", "none"),
}


def rgb565_to_image(raw: bytes) -> Image.Image:
    out = bytearray(W * H * 3)
    for i in range(W * H):
        px = raw[2 * i] | (raw[2 * i + 1] << 8)
        r, g, b = (px >> 11) & 0x1F, (px >> 5) & 0x3F, px & 0x1F
        out[3 * i], out[3 * i + 1], out[3 * i + 2] = (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)
    return Image.frombytes("RGB", (W, H), bytes(out))


def render(scene: str) -> Image.Image:
    what, action = SCENES[scene]
    raw = OUT / f"{scene}.raw"
    subprocess.run([str(HERE / "build/sim"), what, action, str(raw)], check=True, cwd=HERE, stdout=subprocess.DEVNULL)
    img = rgb565_to_image(raw.read_bytes())
    raw.unlink()
    return img


def sheet(images, path, scale=0.5):
    pad, label_h = 24, 28
    tw, th = int(W * scale), int(H * scale)
    cols = min(3, len(images))
    rows = (len(images) + cols - 1) // cols
    canvas = Image.new("RGB", (pad + cols * (tw + pad), pad + rows * (th + label_h + pad)), (40, 40, 40))
    draw = ImageDraw.Draw(canvas)
    for i, (name, img) in enumerate(images):
        x, y = pad + (i % cols) * (tw + pad), pad + (i // cols) * (th + label_h + pad)
        draw.text((x, y + 6), name, fill=(220, 220, 220))
        canvas.paste(img.resize((tw, th), Image.LANCZOS), (x, y + label_h))
    canvas.save(path)


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--raw":
        rgb565_to_image(pathlib.Path(sys.argv[2]).read_bytes()[: W * H * 2]).save(sys.argv[3])
        return 0
    OUT.mkdir(exist_ok=True)
    rendered = []
    for scene in SCENES:
        img = render(scene)
        img.save(OUT / f"{scene}.png")
        rendered.append((scene, img))
    sheet(rendered, OUT / "all-states.png", 0.5)
    print(f"wrote {len(rendered)} screenshots to {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
