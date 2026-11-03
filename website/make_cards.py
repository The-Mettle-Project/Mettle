import math
import os
import re
import subprocess

import numpy as np
from PIL import Image

from make_readme_art import CACHE, HERE, edge

HEADLINE = "This is your language."
BRAND = '<svg viewBox="0 0 150 150"><polygon points="56.4 63.7 69.5 34.8 113.7 32.3 133.5 51 122.3 74.9 130.2 88.3 147.6 48 119.6 20.8 62.4 20.9 42.4 63.7"/><polygon points="60.6 63.7 68.9 57.7 95.6 57.7 108.9 63.6"/><polygon points="47.9 67 109.2 88.8 114.3 97.6 138 109.5 114.2 66.9"/><path d="m140.4 114.6-94.1-45.1-2.9 6 43.7 41.8-23.6 0.1-20.8-40.3-24.2 51.9 129.7-0.1-7.8-14.3z"/><path d="m14.2 63-12.1 5.5 1.4 2.1 23.1 32.4 13.9-29.1-26.3-10.9zm14.7 22.3-17.3-19.9 24.4 10.3-7.1 9.6z"/></svg>Mettle'
GROUND = np.array([23, 24, 26], dtype=np.float32) / 255
WEIGHTS = np.array([0.299, 0.587, 0.114], dtype=np.float32)

CARDS = [
    ("og", 1200, 630, 2, 2, 108, 48, 46, BRAND, [(48, 26, 170, 66)]),
    ("readme-banner", 1600, 760, 1, 2, 128, 64, 56, "", []),
]

PAGE = """<!doctype html>
<meta charset="utf-8">
<style>
@font-face { font-family: "Geist Serif"; src: url("%(font)s") format("woff2"); }
html, body { margin: 0; width: %(w)dpx; height: %(h)dpx; overflow: hidden; background: #17181a; }
img { position: absolute; inset: 0; width: 100%%; height: 100%%; image-rendering: pixelated; }
h1 { position: absolute; left: %(x)dpx; bottom: %(y)dpx; margin: 0; font: 400 %(size)dpx/1.02 "Geist Serif", serif; letter-spacing: -0.028em; color: #fff; font-synthesis: none; }
.brand { position: absolute; left: 48px; top: 30px; display: flex; align-items: center; gap: 12px; font: 400 28px "Geist Serif", serif; color: #fff; }
.brand svg { width: 32px; height: 32px; fill: currentColor; }
</style>
<img src="%(picture)s">
<div class="brand">%(brand)s</div>
<h1>%(headline)s</h1>
"""


def uri(path):
    return "file:///" + path.replace("\\", "/")


def smooth(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def halftone_table():
    with open(os.path.join(HERE, "template.html"), "rb") as f:
        text = f.read().decode("utf-8")
    values = re.search(r"var HALFTONE = \[([^\]]+)\]", text).group(1)
    return np.array([int(v) for v in values.split(",")], dtype=np.float32).reshape(8, 8) / 64


def dithered(w, h, cell, density, clearings):
    table = halftone_table()
    w *= density
    h *= density
    gw = math.ceil(w / cell)
    gh = math.ceil(h / cell)
    poster = Image.open(os.path.join(HERE, "media", "card-frame.jpg")).convert("RGB")
    scale = max(gw / poster.width, gh / poster.height)
    poster = poster.resize((math.ceil(poster.width * scale), math.ceil(poster.height * scale)), Image.LANCZOS)
    left = (poster.width - gw) // 2
    top = (poster.height - gh) // 2
    rgb = np.asarray(poster.crop((left, top, left + gw, top + gh)), dtype=np.float32) / 255
    lum = rgb @ WEIGHTS
    grey = lum[..., None]
    ink = np.clip((grey + (rgb - grey) * 1.25) * 0.95 * np.power(np.maximum(grey, 0.02), -0.38), 0, 1)
    ys, xs = np.mgrid[0:gh, 0:gw]
    shaped = np.power(lum, 0.62) * smooth(0.0, 0.42, 1 - (ys + 0.5) / gh)
    for x0, y0, x1, y1 in clearings:
        dx = np.maximum(np.maximum(x0 * density - (xs + 0.5) * cell, (xs + 0.5) * cell - x1 * density), 0)
        dy = np.maximum(np.maximum(y0 * density - (ys + 0.5) * cell, (ys + 0.5) * cell - y1 * density), 0)
        shaped = shaped * smooth(2.0 * density, 76.0 * density, np.hypot(dx, dy))
    limit = table[(gh - 1 - ys) % 8, xs % 8]
    on = (shaped >= limit + 0.034)[..., None]
    grid = Image.fromarray((np.clip(GROUND * (1 - on) + ink * on, 0, 1) * 255 + 0.5).astype(np.uint8))
    return grid.resize((gw * cell, gh * cell), Image.NEAREST).crop((0, 0, w, h))


def main():
    os.makedirs(CACHE, exist_ok=True)
    browser = edge()
    profile = os.path.join(CACHE, "profile")
    for name, w, h, density, cell, size, x, y, brand, clearings in CARDS:
        picture = os.path.join(CACHE, name + "-dither.png")
        dithered(w, h, cell, density, clearings).save(picture)
        tmp = os.path.join(CACHE, name + ".html")
        with open(tmp, "wb") as f:
            f.write((PAGE % {"font": uri(os.path.join(HERE, "fonts", "GeistSerif-Regular.woff2")), "picture": uri(picture), "w": w, "h": h, "size": size, "x": x, "y": y, "brand": brand, "headline": HEADLINE}).encode("utf-8"))
        png = os.path.join(CACHE, name + ".png")
        subprocess.run([browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=%d" % density, "--user-data-dir=" + profile, "--virtual-time-budget=2500", "--allow-file-access-from-files", "--window-size=%d,%d" % (w, h), "--screenshot=" + png, uri(tmp)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120, check=True)
        jpg = os.path.join(HERE, "media", name + ".jpg")
        subprocess.run(["magick", png, "-strip", "-quality", "93", "-sampling-factor", "4:4:4", jpg], check=True)
        print(name, "%dx%d" % (w * density, h * density), os.path.getsize(jpg) // 1024, "KB")


if __name__ == "__main__":
    main()
