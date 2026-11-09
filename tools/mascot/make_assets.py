"""Builds moogle's bundled image assets from the high-resolution mascot renders.

Inputs are the PNGs produced by render_obj.py (light = Kingdom Hearts PS2 moogle,
dark = Kingdom Hearts 358/2 Days XIII moogle). Outputs:
  resources/mascot/moogle-{light,dark}.png   trimmed sprites for the editor overlay
  resources/icons/<n>-apps-moogle.png         head-crop application icons
  resources/icons/sc-apps-moogle.svg          scalable icon wrapping the 256px PNG
  resources/banner.png                        About dialog banner

Usage: python -I make_assets.py LIGHT_1024.png DARK_1024.png REPO_ROOT [FONT_TTF]
"""
import base64
import io
import json
import os
import sys
import time

from PIL import Image, ImageDraw, ImageFont

ICON_SIZES = (16, 22, 32, 48, 64, 128, 256)
SPRITE_HEIGHT = 256


def log(level, msg, **kw):
    rec = {"timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"), "level": level,
           "context": "make_assets", "message": msg}
    rec.update(kw)
    print(json.dumps(rec), file=sys.stderr)


def trim(img):
    bbox = img.getbbox()
    if bbox is None:
        raise ValueError("render is fully transparent")
    return img.crop(bbox)


def sprite(img):
    t = trim(img)
    w = round(t.width * SPRITE_HEIGHT / t.height)
    return t.resize((w, SPRITE_HEIGHT), Image.LANCZOS)


def head_crop(img):
    """Square crop around the pom-pom and head, which reads far better at
    16-64px than the full body does."""
    t = trim(img)
    # The head (with pom-pom and antenna) occupies roughly the top 66% of the
    # model's height for both rips.
    head_h = int(t.height * 0.66)
    side = max(head_h, t.width)
    cx = t.width // 2
    left = cx - side // 2
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.alpha_composite(t.crop((max(0, left), 0, min(t.width, left + side), head_h)),
                           (max(0, -left), (side - head_h) // 2))
    return canvas


def write_icons(head, root):
    out = {}
    for n in ICON_SIZES:
        # Keep a small transparent margin so the icon does not touch its bounds.
        inner = max(1, round(n * 0.94))
        ic = head.resize((inner, inner), Image.LANCZOS)
        canvas = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        canvas.alpha_composite(ic, ((n - inner) // 2, (n - inner) // 2))
        path = os.path.join(root, "resources", "icons", f"{n}-apps-moogle.png")
        canvas.save(path)
        out[n] = canvas
    buf = io.BytesIO()
    out[256].save(buf, "PNG")
    b64 = base64.b64encode(buf.getvalue()).decode()
    svg = ('<?xml version="1.0" encoding="UTF-8"?>\n'
           '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
           'width="256" height="256" viewBox="0 0 256 256">\n'
           f'  <image width="256" height="256" xlink:href="data:image/png;base64,{b64}"/>\n'
           '</svg>\n')
    with open(os.path.join(root, "resources", "icons", "sc-apps-moogle.svg"), "w") as fh:
        fh.write(svg)


def write_banner(light, dark, root, font_path):
    W, H = 721, 269
    bg_top, bg_bot = (40, 42, 54), (24, 25, 33)
    banner = Image.new("RGBA", (W, H))
    px = banner.load()
    for y in range(H):
        f = y / (H - 1)
        c = tuple(round(bg_top[i] * (1 - f) + bg_bot[i] * f) for i in range(3))
        for x in range(W):
            px[x, y] = c + (255,)
    h = H - 30
    for img, x in ((light, 24), (dark, None)):
        t = trim(img)
        w = round(t.width * h / t.height)
        s = t.resize((w, h), Image.LANCZOS)
        banner.alpha_composite(s, (x if x is not None else W - w - 24, 15))
    d = ImageDraw.Draw(banner)
    try:
        big = ImageFont.truetype(font_path, 64)
        small = ImageFont.truetype(font_path.replace("-Bold", "-Regular"), 22)
    except OSError as exc:
        log("warn", "banner font unavailable, using PIL default", error=str(exc), font=font_path)
        big = small = ImageFont.load_default()
    for text, font, y, col in (("moogle", big, 82, (240, 236, 228)),
                               ("markdown, kupo!", small, 166, (226, 74, 92))):
        tw = d.textlength(text, font=font)
        d.text(((W - tw) / 2, y), text, font=font, fill=col)
    banner.convert("RGB").save(os.path.join(root, "resources", "banner.png"))


def main():
    if len(sys.argv) < 4:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    light_p, dark_p, root = sys.argv[1:4]
    font = sys.argv[4] if len(sys.argv) > 4 else "/usr/share/fonts/noto/NotoSerif-Bold.ttf"
    t0 = time.time()
    try:
        light = Image.open(light_p).convert("RGBA")
        dark = Image.open(dark_p).convert("RGBA")
        os.makedirs(os.path.join(root, "resources", "mascot"), exist_ok=True)
        sprite(light).save(os.path.join(root, "resources", "mascot", "moogle-light.png"))
        sprite(dark).save(os.path.join(root, "resources", "mascot", "moogle-dark.png"))
        write_icons(head_crop(light), root)
        write_banner(light, dark, root, font)
        log("info", "assets written", root=root, seconds=round(time.time() - t0, 2))
    except Exception as exc:  # noqa: BLE001 - top-level CLI guard
        import traceback
        log("error", "asset build failed", error=str(exc), stack=traceback.format_exc())
        sys.exit(1)


if __name__ == "__main__":
    main()
