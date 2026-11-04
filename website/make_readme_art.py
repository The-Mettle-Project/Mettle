import argparse
import base64
import os
import re
import shutil
import subprocess
import sys
import urllib.request

import suite3_facts

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CACHE = os.path.join(REPO, ".tmp", "readme-art")
OUT = os.path.join(HERE, "media", "readme")
WIDTH = 1000
MIN_SPAN = 0.35
SPAN_MARGIN = 1.1

PHOTOS = {
    "cold": 32888759,
    "moss": 33931095,
    "dusk": 5111061,
}

SECTIONS = [
    ("close", "cold", "Everything else<br>stays close to C.", 420, "50% 58%", True),
    ("powers", "moss", "What you can do.", 400, "50% 62%", False),
    ("try", "dusk", "Try it.", 400, "50% 60%", False),
]

STYLE = """
@font-face { font-family: "Geist Serif"; src: url(data:font/woff2;base64,%s) format("woff2"); font-weight: 400; font-style: normal; }
:root { --bg: #101215; --ink: #f1efe9; --ink-2: #a8a69f; --dim: #85857f; --accent: #e8a468; --serif: "Geist Serif", "Times New Roman", Times, serif; --mono: "IBM Plex Mono", ui-monospace, "Cascadia Mono", Consolas, Menlo, monospace; color-scheme: dark; }
* { box-sizing: border-box; }
html { background: var(--bg); width: %dpx; overflow: hidden; }
body { margin: 0; background: var(--bg); color: var(--ink); font-family: var(--serif); font-synthesis: none; font-size: 20px; line-height: 1.42; -webkit-font-smoothing: antialiased; width: %dpx; overflow: hidden; }
.art-photo { position: relative; background-size: cover; }
.art-photo::after { content: ""; position: absolute; left: 0; right: 0; bottom: 0; height: 62%%; background: linear-gradient(to bottom, rgba(16, 18, 21, 0), rgba(16, 18, 21, 0.62) 58%%, #101215); }
.art-photo h2 { position: absolute; z-index: 1; left: 44px; bottom: 30px; margin: 0; color: #fff; font-size: 70px; line-height: 1.03; font-weight: 400; letter-spacing: -0.026em; }
.art-body { padding: 8px 44px 52px; }
.how { margin: 0; font-size: 27px; line-height: 1.3; color: #c9c7c0; text-wrap: pretty; }
.c-grid { margin-top: 36px; }
.c-figs { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); column-gap: 34px; margin-bottom: 40px; }
.fig { margin: 0; min-width: 0; }
.fig b { display: block; font-weight: 400; font-size: 76px; line-height: 1; letter-spacing: -0.035em; color: var(--ink); }
.fig figcaption { margin-top: 8px; font-size: 22px; line-height: 1.28; color: #c9c7c0; text-wrap: pretty; }
.fig figcaption code { font-family: var(--mono); font-size: 0.82em; white-space: nowrap; color: var(--ink); }
.spread { margin: 0; min-width: 0; }
.s-head, .s-row { display: grid; grid-template-columns: 11em minmax(0, 1fr) 3.6em; column-gap: 14px; align-items: center; }
.s-head { font-size: 21px; color: var(--ink-2); margin-bottom: 8px; }
.s-axis { display: flex; justify-content: space-between; }
.s-row { height: 36px; font-family: var(--mono); font-size: 19px; color: var(--ink-2); }
.s-row .val { text-align: right; color: var(--ink); font-variant-numeric: tabular-nums; }
.s-track { position: relative; height: 100%%; }
.s-track::before { content: ""; position: absolute; left: 50%%; top: 0; bottom: 0; width: 1px; background: var(--ink-2); }
.s-track .o3 { position: absolute; top: 9px; bottom: 9px; background: var(--dim); }
.s-track .o3.ahead { background: var(--accent); }
.s-track .o3.up { left: 50%%; }
.s-track .o3.down { right: 50%%; }
.s-track .nat { position: absolute; top: 4px; bottom: 4px; width: 3px; margin-left: -1px; background: var(--ink); }
.spread figcaption { display: flex; flex-wrap: wrap; align-items: center; gap: 4px 8px; margin-top: 16px; font-size: 20px; color: var(--ink-2); }
.spread figcaption code { font-family: var(--mono); font-size: 0.82em; margin-right: 14px; }
.s-key { display: inline-block; width: 22px; height: 11px; background: var(--dim); }
.s-key.tick { width: 2px; height: 19px; background: var(--ink); }
"""


def close_body(facts):
    deviations = []
    for r in facts["rows"]:
        deviations.append(abs(r["m"] / r["o3"] - 1))
        deviations.append(abs(r["m"] / r["native"] - 1))
    span = max(MIN_SPAN, max(deviations) * SPAN_MARGIN)
    rows = ""
    for r in facts["rows"]:
        ratio = r["m"] / r["o3"]
        native = r["m"] / r["native"]
        kind = "down ahead" if ratio < 1 else "up"
        rows += '<div class="s-row"><span>%s</span><span class="s-track"><i class="o3 %s" style="width:%.2f%%"></i><i class="nat" style="left:%.2f%%"></i></span><span class="val">%.2f&times;</span></div>\n' % (r["name"], kind, abs(ratio - 1) / span * 50, 50 + (native - 1) / span * 50, ratio)
    return """<div class="art-body">
<p class="how">Every whole program in the benchmark suite, including parsers, compressors, interpreters and solvers.</p>
<div class="c-grid">
<div class="c-figs">
<figure class="fig"><b>%s&times;</b><figcaption>the run time of <code>clang -O3</code>, on average, with the same answers on all 16</figcaption></figure>
<figure class="fig"><b>%s&times;</b><figcaption>the run time of <code>clang -O3 -march=native</code>, which leads on %s</figcaption></figure>
<figure class="fig"><b>%s&times;</b><figcaption>faster to build a generated 90,000-line file, %s&nbsp;s against %s&nbsp;s</figcaption></figure>
</div>
<figure class="spread">
<div class="s-head"><span></span><span class="s-axis"><span>Mettle faster</span><span>clang faster</span></span><span></span></div>
%s<figcaption><i class="s-key"></i> against <code>-O3</code> <i class="s-key tick"></i> against <code>-march=native</code></figcaption>
</figure>
</div>
</div>
""" % (facts["c_o3"], facts["c_native"], facts["native_leads"], facts["build_ratio"], facts["build_m"], facts["build_c"], rows)


def section_page(photo_url, heading, height, position, body):
    with open(os.path.join(HERE, "fonts", "GeistSerif-Regular.woff2"), "rb") as f:
        serif = base64.b64encode(f.read()).decode("ascii")
    measure = """<script>
var last = document.currentScript.previousElementSibling;
var m = document.createElement("meta");
m.id = "art-height";
m.content = String(Math.ceil(last.getBoundingClientRect().bottom));
document.head.appendChild(m);
</script>"""
    return """<!doctype html><html lang="en"><head><meta charset="utf-8">
<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&display=swap" rel="stylesheet">
<style>%s</style></head><body>
<div class="art-photo" style="height:%dpx;background-image:url('%s');background-position:%s"><h2>%s</h2></div>
%s%s</body></html>""" % (STYLE % (serif, WIDTH, WIDTH), height, photo_url, position, heading, body, measure)


def edge():
    for p in (r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe", r"C:\Program Files\Google\Chrome\Application\chrome.exe"):
        if os.path.exists(p) and subprocess.run([p, "--version"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=60).stdout.strip():
            return p
    sys.exit("make_readme_art.py: no working Edge or Chrome")


def fetch_photo(key):
    path = os.path.join(CACHE, key + ".jpg")
    if not os.path.exists(path):
        pid = PHOTOS[key]
        url = "https://images.pexels.com/photos/%d/pexels-photo-%d.jpeg?auto=compress&cs=tinysrgb&w=2400" % (pid, pid)
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req) as r, open(path, "wb") as out:
            shutil.copyfileobj(r, out)
    return "file:///" + path.replace("\\", "/")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sections", nargs="*")
    parser.add_argument("--data", default=suite3_facts.EXHIBITS)
    parser.add_argument("--out", default=OUT)
    args = parser.parse_args()
    os.makedirs(CACHE, exist_ok=True)
    os.makedirs(args.out, exist_ok=True)
    browser = edge()
    facts = suite3_facts.load(args.data)
    profile = os.path.join(CACHE, "profile")
    base = [browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-prefers-reduced-motion", "--user-data-dir=" + profile, "--virtual-time-budget=2500", "--allow-file-access-from-files"]
    for name, key, heading, height, position, charted in SECTIONS:
        if args.sections and name not in args.sections:
            continue
        body = close_body(facts) if charted else ""
        html = section_page(fetch_photo(key), heading, height, position, body)
        tmp = os.path.join(CACHE, name + ".html")
        with open(tmp, "wb") as f:
            f.write(html.encode("utf-8"))
        url = "file:///" + tmp.replace("\\", "/")
        dom = subprocess.run(base + ["--window-size=%d,1200" % WIDTH, "--dump-dom", url], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=120).stdout.decode("utf-8", "replace")
        m = re.search(r'id="art-height" content="(\d+)"', dom) or re.search(r'content="(\d+)" id="art-height"', dom)
        if not m:
            sys.exit("make_readme_art.py: no height for " + name)
        h = int(m.group(1))
        png = os.path.join(CACHE, name + ".png")
        subprocess.run(base + ["--force-device-scale-factor=2", "--window-size=%d,%d" % (WIDTH, h), "--screenshot=" + png, url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
        jpg = os.path.join(args.out, name + ".jpg")
        subprocess.run(["magick", png, "-strip", "-quality", "84", "-sampling-factor", "4:2:0", jpg], check=True)
        print(name, "%dx%d" % (WIDTH * 2, h * 2), os.path.getsize(jpg) // 1024, "KB")


if __name__ == "__main__":
    main()
