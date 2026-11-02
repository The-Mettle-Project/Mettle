import os
import subprocess

from make_readme_art import CACHE, HERE, edge

HEADLINE = "The future of programming."
BRAND = '<svg viewBox="0 0 150 150"><polygon points="56.4 63.7 69.5 34.8 113.7 32.3 133.5 51 122.3 74.9 130.2 88.3 147.6 48 119.6 20.8 62.4 20.9 42.4 63.7"/><polygon points="60.6 63.7 68.9 57.7 95.6 57.7 108.9 63.6"/><polygon points="47.9 67 109.2 88.8 114.3 97.6 138 109.5 114.2 66.9"/><path d="m140.4 114.6-94.1-45.1-2.9 6 43.7 41.8-23.6 0.1-20.8-40.3-24.2 51.9 129.7-0.1-7.8-14.3z"/><path d="m14.2 63-12.1 5.5 1.4 2.1 23.1 32.4 13.9-29.1-26.3-10.9zm14.7 22.3-17.3-19.9 24.4 10.3-7.1 9.6z"/></svg>Mettle'

CARDS = [
    ("og", 1200, 630, 82, 40, 44, BRAND),
    ("readme-banner", 1600, 760, 104, 140, 62, ""),
]

PAGE = """<!doctype html>
<meta charset="utf-8">
<style>
@font-face { font-family: "Geist Serif"; src: url("%(font)s") format("woff2"); }
html, body { margin: 0; width: %(w)dpx; height: %(h)dpx; overflow: hidden; background: #101215; }
img { position: absolute; inset: 0; width: 100%%; height: 100%%; object-fit: cover; object-position: 50%% 100%%; }
.fade { position: absolute; left: 0; right: 0; bottom: 0; height: 26%%; background: linear-gradient(to bottom, rgba(16, 18, 21, 0), rgba(16, 18, 21, 0.72) 62%%, #101215); }
h1 { position: absolute; left: %(x)dpx; bottom: %(y)dpx; margin: 0; font: 400 %(size)dpx/1.02 "Geist Serif", serif; letter-spacing: -0.028em; color: #fff; font-synthesis: none; }
.brand { position: absolute; left: 40px; top: 22px; display: flex; align-items: center; gap: 10px; font: 400 21px "Geist Serif", serif; color: #121211; }
.brand svg { width: 24px; height: 24px; fill: currentColor; }
</style>
<img src="%(poster)s">
<div class="fade"></div>
<div class="brand">%(brand)s</div>
<h1>%(headline)s</h1>
"""


def uri(*parts):
    return "file:///" + os.path.join(HERE, *parts).replace("\\", "/")


def main():
    os.makedirs(CACHE, exist_ok=True)
    browser = edge()
    profile = os.path.join(CACHE, "profile")
    for name, w, h, size, x, y, brand in CARDS:
        tmp = os.path.join(CACHE, name + ".html")
        with open(tmp, "wb") as f:
            f.write((PAGE % {"font": uri("fonts", "GeistSerif-Regular.woff2"), "poster": uri("media", "hero.jpg"), "w": w, "h": h, "size": size, "x": x, "y": y, "brand": brand, "headline": HEADLINE}).encode("utf-8"))
        png = os.path.join(CACHE, name + ".png")
        subprocess.run([browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--user-data-dir=" + profile, "--virtual-time-budget=2500", "--allow-file-access-from-files", "--window-size=%d,%d" % (w, h), "--screenshot=" + png, "file:///" + tmp.replace("\\", "/")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120, check=True)
        jpg = os.path.join(HERE, "media", name + ".jpg")
        subprocess.run(["magick", png, "-strip", "-quality", "88", "-sampling-factor", "4:2:0", jpg], check=True)
        print(name, "%dx%d" % (w, h), os.path.getsize(jpg) // 1024, "KB")


if __name__ == "__main__":
    main()
