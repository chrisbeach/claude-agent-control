#!/usr/bin/env python3
"""Renders every scene to out/*.png plus a side-by-side sheet. Run via `make png`.
`render.py --gif` plays the demos through the simulator's touch screen instead and saves each as
out/<demo>.gif, a circle marking where the finger touched. Run via `make gif`.
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
# demo -> (fixture, steps): see sim_main.c for the steps. A tap goes through LVGL as a press and a
# release where the row, host block or "+" is drawn; raised/opened are the hub's answers.
DEMOS = {
    "calm": ("fixtures/fleet_calm.json", "wait:4600"),  # two passes of the WORKING pills' glint
    "mixed": ("fixtures/fleet_mixed.json", "wait:4800"),  # three pulses of NEEDS INPUT
    "tap": ("fixtures/fleet_mixed.json", "wait:1200;tap:row:1;wait:800;raised;wait:5800"),
    "hosts": ("fixtures/fleet_many.json",
              "wait:1200;tap:host:BUILDBOX;wait:2200;tap:host:NAS;wait:2200;tap:host:NAS;wait:1400"),
    "new": ("fixtures/fleet_mixed.json", "wait:1200;tap:plus;wait:1800;opened;wait:6000"),
}
FRAME_MS = 40  # sim_main.c's FRAME_MS: one frame per LVGL pump
FINGER_R = 28  # px: the circle shown while the finger is down ...
RIPPLE_FRAMES = 9  # ... which then spreads and fades over this many frames


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


def play(demo: str) -> tuple[list[Image.Image], list[tuple[int, int, int, int]]]:
    """The demo's frames, and its touches as (first frame, last frame, x, y)."""
    fixture, steps = DEMOS[demo]
    raw = OUT / f"{demo}.raw"
    run = subprocess.run([str(HERE / "build/sim"), fixture, "play:" + steps, str(raw)],
                         check=True, cwd=HERE, capture_output=True, text=True)
    data = raw.read_bytes()
    raw.unlink()
    size = W * H * 2
    frames = [Image.frombytes("RGB", (W, H), data[i:i + size], "raw", "BGR;16") for i in range(0, len(data), size)]
    touches = [tuple(int(v) for v in line.split()[1:]) for line in run.stdout.splitlines() if line.startswith("touch ")]
    return frames, touches


def mark_touches(frames, touches):
    """A pale disc under the finger while it is down, then a ring spreading out from it and fading."""
    for first, last, x, y in touches:
        for i in range(first, min(last + RIPPLE_FRAMES + 1, len(frames))):
            layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            draw = ImageDraw.Draw(layer)
            if i <= last:
                r = FINGER_R
                draw.ellipse((x - r, y - r, x + r, y + r), fill=(255, 255, 255, 130), outline=(255, 255, 255, 220), width=3)
            else:
                t = (i - last) / RIPPLE_FRAMES
                r = int(FINGER_R + 30 * t)
                draw.ellipse((x - r, y - r, x + r, y + r), outline=(255, 255, 255, int(220 * (1 - t))), width=3)
            frames[i] = Image.alpha_composite(frames[i].convert("RGBA"), layer).convert("RGB")


def save_gif(frames, path):
    # one palette for the whole animation, taken from frames across it, so no colour flickers
    # between frames; no dither, so the flat LCARS colours stay flat and compress well
    picks = frames[:: max(1, len(frames) // 12)]
    strip = Image.new("RGB", (W, H * len(picks)))
    for i, frame in enumerate(picks):
        strip.paste(frame, (0, H * i))
    palette = strip.quantize(256, method=Image.Quantize.MEDIANCUT)
    indexed = [frame.quantize(palette=palette, dither=Image.Dither.NONE) for frame in frames]
    indexed[0].save(path, save_all=True, append_images=indexed[1:], duration=FRAME_MS, loop=0)


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
    if sys.argv[1:] == ["--gif"]:
        for demo in DEMOS:
            frames, touches = play(demo)
            mark_touches(frames, touches)
            save_gif(frames, OUT / f"{demo}.gif")
            print(f"{demo}.gif: {len(frames)} frames, {(OUT / f'{demo}.gif').stat().st_size // 1024} KB")
        return 0
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
