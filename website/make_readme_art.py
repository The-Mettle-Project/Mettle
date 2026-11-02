import os
import re
import shutil
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CACHE = os.path.join(REPO, ".tmp", "readme-art")
OUT = os.path.join(HERE, "media", "readme")
WIDTH = 1000

PHOTOS = {
    "force": 32833863,
    "cold": 32888759,
    "moss": 33931095,
    "dusk": 5111061,
}

SECTIONS = [
    ("power", "force", "Faster than llama.cpp,<br>on the same card.", 420, "50% 72%", 0),
    ("close", "cold", "Everything else<br>stays close to C.", 420, "50% 58%", 1),
    ("powers", "moss", "What you can do.", 400, "50% 62%", None),
    ("try", "dusk", "Try it.", 400, "50% 60%", None),
]

STYLE = """
html, body { width: %dpx; overflow: hidden; }
* { transition: none !important; animation: none !important; }
.art-photo { position: relative; background-size: cover; }
.art-photo::after { content: ""; position: absolute; left: 0; right: 0; bottom: 0; height: 62%%; background: linear-gradient(to bottom, rgba(16, 18, 21, 0), rgba(16, 18, 21, 0.62) 58%%, #101215); }
.art-photo h2 { position: absolute; z-index: 1; left: 44px; bottom: 30px; margin: 0; color: #fff; font-size: 70px; line-height: 1.03; font-weight: 400; letter-spacing: -0.026em; }
.art-body { padding: 8px 44px 52px; }
.art-body .how { margin: 0; max-width: none; font-size: 27px; line-height: 1.3; color: #c9c7c0; }
.art-body .figs { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); column-gap: 52px; row-gap: 40px; margin-top: 40px; }
.art-body .fig.range b { font-size: 58px; }
.art-body .fig figcaption { min-height: 0; margin-top: 8px; font-size: 26px; color: #f1efe9; }
.art-body .bar { margin-top: 15px; }
.art-body .bar .row { font-size: 21px; }
.art-body .bar i { height: 7px; }
.art-body .c-grid { display: block; margin-top: 36px; }
.art-body .s-cols { display: block; }
.art-body .s-col + .s-col .s-head { display: none; }
.art-body .c-figs { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); column-gap: 34px; margin-bottom: 40px; }
.art-body .c-figs .fig + .fig { margin-top: 0; }
.art-body .c-figs .fig b { font-size: 76px; }
.art-body .c-figs .fig figcaption { font-size: 22px; line-height: 1.28; color: #c9c7c0; }
.art-body .c-figs code, .art-body .spread figcaption code { font-size: 0.82em; }
.art-body .s-head, .art-body .s-row { grid-template-columns: 11em minmax(0, 1fr) 3.6em; }
.art-body .s-head { font-size: 21px; margin-bottom: 8px; }
.art-body .s-row { height: 36px; font-size: 19px; }
.art-body .s-track .o3 { top: 9px; bottom: 9px; }
.art-body .s-track .nat { top: 4px; bottom: 4px; width: 3px; }
.art-body .spread figcaption { font-size: 20px; margin-top: 16px; }
""" % WIDTH


def script(photo_url, heading, height, position, part):
    return """<script>
(function () {
  var proof = document.querySelector(".proof");
  var how = ["Systm's Potentia runs LLMs on Mettle. Its GPU kernels are Mettle, compiled straight to PTX with no nvcc, C++ or Python in the pipeline. The build fails unless the GPU matches an independent CPU reference, logit for logit.", "Every whole program in the benchmark suite, including parsers, compressors, interpreters and solvers."];
  var figs = proof.querySelector(".figs");
  var grid = document.createElement("div");
  grid.className = "c-grid";
  grid.appendChild(proof.querySelector(".c-figs"));
  grid.appendChild(proof.querySelector(".spread"));
  var part = %s;
  document.querySelectorAll(".spread, .figs").forEach(function (e) { e.classList.add("shown"); });
  var photo = document.createElement("div");
  photo.className = "art-photo";
  photo.style.height = "%dpx";
  photo.style.backgroundImage = "url('%s')";
  photo.style.backgroundPosition = "%s";
  photo.innerHTML = "<h2>%s</h2>";
  var nodes = [photo];
  if (part !== null) {
    var body = document.createElement("div");
    body.className = "art-body";
    var lead = document.createElement("p");
    lead.className = "how";
    lead.textContent = how[part];
    body.appendChild(lead);
    body.appendChild(part === 0 ? figs : grid);
    nodes.push(body);
  }
  document.body.textContent = "";
  nodes.forEach(function (n) { document.body.appendChild(n); });
  var m = document.createElement("meta");
  m.id = "art-height";
  m.content = String(Math.ceil(nodes[nodes.length - 1].getBoundingClientRect().bottom));
  document.head.appendChild(m);
})();
</script>
""" % ("null" if part is None else part, height, photo_url, position, heading)


def edge():
    for p in (r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe", r"C:\Program Files\Google\Chrome\Application\chrome.exe"):
        if os.path.exists(p):
            return p
    sys.exit("make_readme_art.py: no Edge or Chrome")


def main():
    os.makedirs(CACHE, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    browser = edge()
    with open(os.path.join(HERE, "index.html"), "rb") as f:
        page = f.read().decode("utf-8")
    for key, pid in PHOTOS.items():
        path = os.path.join(CACHE, key + ".jpg")
        if not os.path.exists(path):
            url = "https://images.pexels.com/photos/%d/pexels-photo-%d.jpeg?auto=compress&cs=tinysrgb&w=2400" % (pid, pid)
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(req) as r, open(path, "wb") as out:
                shutil.copyfileobj(r, out)
    profile = os.path.join(CACHE, "profile")
    base = [browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-prefers-reduced-motion", "--user-data-dir=" + profile, "--virtual-time-budget=2500", "--allow-file-access-from-files"]
    for name, key, heading, height, position, part in SECTIONS:
        photo = "file:///" + os.path.join(CACHE, key + ".jpg").replace("\\", "/")
        html = page.replace("</head>", "<style>" + STYLE + "</style></head>", 1).replace("</body>", script(photo, heading, height, position, part) + "</body>", 1)
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
        jpg = os.path.join(OUT, name + ".jpg")
        subprocess.run(["magick", png, "-strip", "-quality", "84", "-sampling-factor", "4:2:0", jpg], check=True)
        print(name, "%dx%d" % (WIDTH * 2, h * 2), os.path.getsize(jpg) // 1024, "KB")


if __name__ == "__main__":
    main()
