#!/usr/bin/env python3
"""Generate site/explain/ from the compiler's own help tables.

The prose for every diagnostic and every optimizer decision code lives in
src/error/error_explain.c and is what `mettle explain <CODE>` prints. This
script asks the compiler for it as JSON and renders one page per code, so the
site and the terminal cannot disagree.

    python3 tools/gen-site-reference.py --compiler ./bin/mettle.exe
    python3 tools/gen-site-reference.py --check      # CI: fail if stale

Regenerate whenever error_explain.c changes; CI runs --check.
"""

import argparse
import html
import json
import os
import re
import subprocess
import sys

BASE_URL = "https://suidvandiewereld.github.io/Mettle"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "site", "explain")

GROUP_ORDER = [
    ("diagnostic", "Compile diagnostics",
     "Errors and warnings the compiler reports against your source. Every one "
     "is printed with a caret under the exact span, and every one is listed "
     "here."),
    ("vectorization-refusal", "Why a loop did not vectorize",
     "The optimizer prints one of these in brackets after a NOT vectorized "
     "verdict. Each names the specific shape the loop had that no SIMD kernel "
     "could claim."),
    ("inline-refusal", "Why a call was not inlined",
     "Printed after a NOT inlined verdict. The inliner declines for reasons "
     "that are all budget, shape, or contract, and each has a name."),
    ("applied", "What the optimizer applied",
     "The positive verdicts. These say what the compiler did do, and what it "
     "had to prove first."),
]

NAV = [
    ("../index.html", "Home"),
    ("../internals.html", "Internals"),
    ("../reading-diagnostics.html", "Diagnostics"),
    ("../reading-the-report.html", "The report"),
    ("index.html", "Codes"),
]


def esc(text):
    return html.escape(text, quote=True)


def page_shell(title, description, body, nav_current, depth=1, extra_head=""):
    up = "../" * depth
    nav = []
    for href, label in NAV:
        if depth == 0:
            href = href[3:] if href.startswith("../") else "explain/" + href
        current = ' aria-current="page"' if label == nav_current else ""
        nav.append(f'<a href="{esc(href)}"{current}>{esc(label)}</a>')
    nav_html = "".join(nav)
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>{esc(title)}</title>
<meta name="description" content="{esc(description)}" />
<link rel="icon" href="{up}favicon.svg" type="image/svg+xml" />
<link rel="preconnect" href="https://fonts.googleapis.com" />
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin />
<link href="https://fonts.googleapis.com/css2?family=Crimson+Pro:ital,wght@0,400;0,500;0,600;0,700;1,400&family=Courier+Prime:ital,wght@0,400;0,700&display=swap" rel="stylesheet" />
<link rel="stylesheet" href="{up}assets/doc.css" />
{extra_head}</head>
<body>
<header class="bar"><div class="wrap">
  <a class="brand" href="{up}index.html"><b>Mettle</b><span>compiler</span></a>
  <nav>{nav_html}</nav>
</div></header>
<main class="wrap">
{body}
</main>
<footer class="foot"><div class="wrap"><div class="row">
  <span>Generated from <code>src/error/error_explain.c</code> &mdash; the same text <code>mettle explain</code> prints.</span>
  <span class="sp"><a href="https://github.com/suidvandiewereld/Mettle">GitHub</a></span>
</div></div></footer>
</body>
</html>
"""


LABEL = re.compile(r"^(Example|Examples|Fix|Fixes|Note|Notes|Rule|Why it "
                   r"matters|Reported when|Not reported when):\s*$")
BULLET = re.compile(r"^\s{0,3}-\s+")


def render_body(text):
    """Turn a help body into HTML, following the conventions the tables use.

    A line at four spaces is code, at eight inside a bullet. `  - ` opens a
    bullet whose continuations sit at four. A line that is only a label
    (`Example:`, `Fix:`) heads whatever follows it. Everything else is prose.
    """
    out = []
    para, code, item = [], [], []
    in_list = False
    code_indent = 4

    def flush_para():
        joined = " ".join(p.strip() for p in para).strip()
        para.clear()
        if joined:
            target = item if in_list else out
            target.append(f"<p>{inline(joined)}</p>")

    def flush_code():
        if not code:
            return
        while code and not code[-1].strip():
            code.pop()
        body = chr(10).join(code)
        code.clear()
        if body.strip():
            target = item if in_list else out
            target.append(f"<pre><code>{esc(body)}</code></pre>")

    def flush_item():
        nonlocal in_list
        flush_code()
        flush_para()
        if item:
            out.append("<li>" + "".join(item) + "</li>")
            item.clear()
        in_list = False

    def close_list():
        if in_list or item:
            flush_item()
        # Merge adjacent <li> runs into one <ul> at assembly time.

    for raw in text.replace("\r\n", "\n").split("\n"):
        line = raw.rstrip()
        indent = len(line) - len(line.lstrip(" ")) if line.strip() else 0

        if not line.strip():
            flush_code()
            flush_para()
            continue

        if BULLET.match(line):
            close_list()
            in_list = True
            code_indent = 8
            para.append(BULLET.sub("", line))
            continue

        if indent >= code_indent:
            flush_para()
            code.append(line[code_indent:])
            continue

        if code:
            flush_code()

        if in_list and indent >= 4:
            para.append(line)
            continue

        if in_list:
            flush_item()
            code_indent = 4

        m = LABEL.match(line.strip())
        if m:
            flush_para()
            out.append(f"<h3>{esc(m.group(1))}</h3>")
            continue

        para.append(line)

    close_list()
    flush_code()
    flush_para()

    html_out = chr(10).join(out)
    # Wrap consecutive list items in a single <ul>.
    return re.sub(r"(?:<li>.*?</li>\s*)+",
                  lambda m: "<ul>" + re.sub(r"\s+(?=<li>)", "", m.group(0)).strip() + "</ul>",
                  html_out, flags=re.DOTALL)


def inline(text):
    """Backticked spans become <code>; everything else is escaped."""
    parts = re.split(r"(`[^`]+`)", text)
    out = []
    for part in parts:
        if part.startswith("`") and part.endswith("`") and len(part) > 2:
            out.append(f"<code>{esc(part[1:-1])}</code>")
        else:
            out.append(esc(part))
    return "".join(out)


def code_page(entry, groups):
    code = entry["code"]
    title = entry["title"]
    group_title = entry["groupTitle"]
    is_diag = entry["kind"] == "diagnostic"

    siblings = [e for e in groups[entry["group"]] if e["code"] != code]
    related = "".join(
        f'<li><a href="{esc(e["code"])}.html"><span class="c">{esc(e["code"])}</span>'
        f'<span class="t">{esc(e["title"])}</span></a></li>'
        for e in siblings[:8])

    where = ("printed by the compiler as an error or warning"
             if is_diag else
             "printed by <code>--explain</code> in brackets after a verdict")

    body = f"""<div class="crumb"><a href="../index.html">Mettle</a> / <a href="index.html">Codes</a> / {esc(code)}</div>
<div class="kicker">{esc(group_title)}</div>
<h1><span class="code">{esc(code)}</span>{esc(title)}</h1>
<p class="lede">{where}.</p>
<pre><code>mettle explain {esc(code)}</code></pre>

{render_body(entry["body"])}

<h2>Related</h2>
<ul class="codelist">{related}</ul>
"""
    return page_shell(f"{code}: {title} — Mettle", f"{code}: {title}", body,
                      "Codes", depth=1)


def index_page(entries, groups):
    sections = []
    for slug, title, blurb in GROUP_ORDER:
        items = groups.get(slug, [])
        if not items:
            continue
        rows = "".join(
            f'<li data-code="{esc(e["code"].lower())}" '
            f'data-title="{esc(e["title"].lower())}" data-group="{esc(slug)}">'
            f'<a href="{esc(e["code"])}.html">'
            f'<span class="c">{esc(e["code"])}</span>'
            f'<span class="t">{esc(e["title"])}</span></a></li>'
            for e in items)
        sections.append(
            f'<section data-group="{esc(slug)}">'
            f"<h2>{esc(title)}</h2><p>{esc(blurb)}</p>"
            f'<ul class="codelist">{rows}</ul></section>')

    buttons = "".join(
        f'<button type="button" data-filter="{esc(slug)}" aria-pressed="false">'
        f"{esc(title)}</button>"
        for slug, title, _ in GROUP_ORDER if groups.get(slug))

    script = """<script>
(function(){
  var q=document.getElementById('q'),count=document.getElementById('count');
  var items=[].slice.call(document.querySelectorAll('.codelist li[data-code]'));
  var sections=[].slice.call(document.querySelectorAll('section[data-group]'));
  var buttons=[].slice.call(document.querySelectorAll('.tags button'));
  var group='';
  function apply(){
    var t=q.value.trim().toLowerCase(),shown=0;
    items.forEach(function(li){
      var ok=(!t||li.dataset.code.indexOf(t)>=0||li.dataset.title.indexOf(t)>=0)
        &&(!group||li.dataset.group===group);
      li.hidden=!ok; if(ok)shown++;
    });
    sections.forEach(function(s){
      s.hidden=!s.querySelector('li[data-code]:not([hidden])');
    });
    count.textContent=shown+' of '+items.length;
  }
  q.addEventListener('input',apply);
  buttons.forEach(function(b){
    b.addEventListener('click',function(){
      group=(group===b.dataset.filter)?'':b.dataset.filter;
      buttons.forEach(function(o){
        o.setAttribute('aria-pressed',String(o.dataset.filter===group));
      });
      apply();
    });
  });
  apply();
})();
</script>"""

    body = f"""<div class="crumb"><a href="../index.html">Mettle</a> / Codes</div>
<div class="kicker">Reference</div>
<h1>Every code the compiler can print</h1>
<p class="lede">{len(entries)} of them. Each is also available in the terminal as
<code>mettle explain &lt;CODE&gt;</code> &mdash; these pages are generated from the
same table, so they say the same thing.</p>

<div class="filter">
  <input id="q" type="search" placeholder="filter by code or title, e.g. store-only or narrowing"
         autocomplete="off" spellcheck="false" aria-label="Filter codes" />
  <span class="count" id="count"></span>
</div>
<div class="tags">{buttons}</div>

{''.join(sections)}
"""
    return page_shell("Codes — Mettle", "Every diagnostic and optimizer "
                      "decision code the Mettle compiler can print.", body,
                      "Codes", depth=1, extra_head="") .replace(
                          "</main>", "</main>\n" + script)


def load_codes(compiler):
    out = subprocess.run([compiler, "explain", "--json"], check=True,
                         capture_output=True)
    return json.loads(out.stdout.decode("utf-8"))["codes"]


def build(entries):
    groups = {}
    for e in entries:
        groups.setdefault(e["group"], []).append(e)

    files = {"index.html": index_page(entries, groups)}
    for e in entries:
        files[e["code"] + ".html"] = code_page(e, groups)
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=os.path.join(ROOT, "bin",
                                                       "mettle.exe"))
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if the committed pages are stale")
    args = ap.parse_args()

    if not os.path.exists(args.compiler):
        alt = os.path.join(ROOT, "bin", "mettle")
        if os.path.exists(alt):
            args.compiler = alt
        else:
            sys.exit(f"compiler not found: {args.compiler}")

    files = build(load_codes(args.compiler))

    if args.check:
        stale = []
        for name, content in files.items():
            path = os.path.join(OUT_DIR, name)
            if not os.path.exists(path):
                stale.append(name + " (missing)")
                continue
            with open(path, encoding="utf-8", newline="") as fh:
                if fh.read() != content:
                    stale.append(name)
        existing = set(os.listdir(OUT_DIR)) if os.path.isdir(OUT_DIR) else set()
        for extra in sorted(existing - set(files)):
            if extra.endswith(".html"):
                stale.append(extra + " (orphan)")
        if stale:
            print("site/explain/ is stale; run tools/gen-site-reference.py")
            for name in stale:
                print("  " + name)
            return 1
        print(f"site/explain/ is up to date ({len(files)} pages)")
        return 0

    os.makedirs(OUT_DIR, exist_ok=True)
    for extra in list(os.listdir(OUT_DIR)):
        if extra.endswith(".html") and extra not in files:
            os.remove(os.path.join(OUT_DIR, extra))
    for name, content in files.items():
        with open(os.path.join(OUT_DIR, name), "w", encoding="utf-8",
                  newline="") as fh:
            fh.write(content)
    print(f"wrote {len(files)} pages to site/explain/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
