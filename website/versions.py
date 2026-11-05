import argparse
import datetime
import json
import pathlib
import re
import shutil
import sys

import registry

HERE = pathlib.Path(__file__).resolve().parent
SOURCE = HERE / "versions"
REPOSITORY = registry.REPOSITORY
SKY = HERE / "media" / "versions-sky.jpg"
INSTALL = [
    ("$ ", "curl -fsSL https://raw.githubusercontent.com/The-Mettle-Project/Mettle/master/install.sh | sh"),
    ("PS> ", "irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/master/install.ps1 | iex"),
]
LINK = re.compile(r"\[([^\]]+)\]\(((?:https?://|#)[^)\s]+)\)")
CODE = re.compile(r"`([^`]+)`")


def escape(text):
    return registry.text_escape(text)


def inline(text):
    spans = []

    def hold(match):
        spans.append(f"<code>{escape(match.group(1))}</code>")
        return f"\x00{len(spans) - 1}\x00"

    out = escape(CODE.sub(hold, text))
    out = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", out)
    out = LINK.sub(lambda m: f'<a href="{m.group(2)}">{m.group(1)}</a>', out)
    return re.sub(r"\x00(\d+)\x00", lambda m: spans[int(m.group(1))], out)


def slug(title):
    return re.sub(r"[^a-z0-9]+", "-", re.sub(r"[`*]", "", title).lower()).strip("-")


def read_list(lines, at):
    items = []
    while at < len(lines) and (lines[at].startswith("- ") or lines[at].startswith("  ")):
        if lines[at].startswith("- "):
            items.append(lines[at][2:].strip())
        else:
            items[-1] += " " + lines[at].strip()
        at += 1
    return "<ul>" + "".join(f"<li>{inline(item)}</li>" for item in items) + "</ul>", at


def read_code(lines, at):
    at += 1
    code = []
    while at < len(lines) and not lines[at].startswith("```"):
        code.append(lines[at])
        at += 1
    return f"<pre><code>{registry.highlight_code(chr(10).join(code))}</code></pre>", at + 1


def read_paragraph(lines, at):
    words = []
    while at < len(lines) and lines[at].strip() and not re.match(r"(```|#|- |!\[)", lines[at]):
        words.append(lines[at].strip())
        at += 1
    return f"<p>{inline(' '.join(words))}</p>", at


def sections(markdown):
    found = [{"title": None, "id": "", "blocks": []}]
    lines = markdown.splitlines()
    at = 0
    while at < len(lines):
        line = lines[at]
        if line.startswith("![") or line.startswith("# ") or not line.strip():
            at += 1
        elif line.startswith("## "):
            title = line[3:].strip()
            found.append({"title": title, "id": slug(title), "blocks": []})
            at += 1
        elif line.startswith("### "):
            title = line[4:].strip()
            found[-1]["blocks"].append(f'<h3 id="{slug(title)}">{inline(title)}</h3>')
            at += 1
        elif line.startswith("```"):
            block, at = read_code(lines, at)
            found[-1]["blocks"].append(block)
        elif line.startswith("- "):
            block, at = read_list(lines, at)
            found[-1]["blocks"].append(block)
        else:
            block, at = read_paragraph(lines, at)
            found[-1]["blocks"].append(block)
    return [section for section in found if section["blocks"]]


def load():
    releases = json.loads((SOURCE / "versions.json").read_text(encoding="utf-8"))
    for release in releases:
        if release.get("notes"):
            release["sections"] = sections((SOURCE / release["notes"]).read_text(encoding="utf-8"))
    return sorted(releases, key=lambda r: registry.version_key(r["version"]), reverse=True)


def release_url(release):
    return f"{REPOSITORY}/releases/tag/v{release['version']}"


def compare_url(release):
    if not release.get("previous"):
        return f"{REPOSITORY}/commits/v{release['version']}"
    return f"{REPOSITORY}/compare/v{release['previous']}...v{release['version']}"


def notes_href(release, up):
    return f"{up}{release['version']}/" if release.get("notes") else release_url(release)


def long_date(text):
    day = datetime.date.fromisoformat(text)
    return f"{day.day} {day.strftime('%B %Y')}"


def footer(up, latest):
    commands = "".join(
        f'\n    <div class="cmd"><pre><span class="ps">{prompt}</span>{escape(command)}</pre>'
        f'<button type="button" data-copy="{escape(command)}">Copy</button></div>'
        for prompt, command in INSTALL
    )
    return f"""<footer class="end" data-image="{up}assets/canyon.jpg" data-fade="0.14 0.4 0.5 0.95">
  <div class="wrap">
    <h2>Install {latest['version']}.</h2>{commands}
    <ul class="go">
      <li><a href="{up}">Every version</a></li>
      <li><a href="{REPOSITORY}/blob/master/docs/getting-started.md">Getting started</a></li>
      <li><a href="{REPOSITORY}/blob/master/docs/known-limitations.md">What does not work yet</a></li>
      <li><a href="{REPOSITORY}/releases">Releases on GitHub</a></li>
    </ul>
    <p class="fine">Photo by <a href="https://www.pexels.com/@ichi/">ICHI HAND</a>. Footage by <a href="https://www.pexels.com/video/dramatic-aerial-view-of-utah-s-rugged-canyons-34305686/">Alex Moliski</a>. Dither after <a href="https://motion-core.dev/docs/dithered-image">Motion Core</a>.</p>
  </div>
</footer>"""


def page(title, description, depth, hero, body, latest):
    up = "../" * depth
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{escape(title)}</title>
<meta name="description" content="{escape(description)}">
<meta name="theme-color" content="#5f7046">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&display=swap" rel="stylesheet">
<link rel="stylesheet" href="{up}assets/site.css">
<link rel="icon" href="{up}assets/emblem.svg">
</head>
<body>
{registry.site_nav(up + "../", "versions")}
{hero}
<main>
{body}
</main>
{footer(up, latest)}
<script src="{up}assets/site.js"></script>
</body>
</html>
"""


def history(releases):
    days = [datetime.date.fromisoformat(r["date"]) for r in releases]
    first, last = min(days), max(days)
    span = max(1, (last - first).days)
    most = max(r["commits"] for r in releases)
    bars = []
    for release, day in sorted(zip(releases, days), key=lambda pair: pair[1]):
        left = (day - first).days / span * 100
        height = max(1.5, release["commits"] / most * 100)
        mark = " now" if release is releases[0] else ""
        label = f'{release["version"]}, {long_date(release["date"])}, {release["commits"]} commits'
        bars.append(
            f'<a class="bar{mark}" href="{notes_href(release, "")}" style="left:{left:.3f}%;height:{height:.2f}%" '
            f'aria-label="{escape(label)}" title="{escape(label)}"></a>'
        )
    months = []
    month = datetime.date(first.year, first.month, 1)
    while month <= last:
        if month >= first:
            left = (month - first).days / span * 100
            months.append(f'<span style="left:{left:.3f}%">{month.strftime("%b")}</span>')
        month = datetime.date(month.year + (month.month == 12), month.month % 12 + 1, 1)
    count = len(releases)
    total = sum(r["commits"] for r in releases)
    return f"""<figure class="history wrap">
  <figcaption>{count} releases and {total:,} commits since {long_date(releases[-1]["date"])}. Each bar is one release, as tall as its commits.</figcaption>
  <div class="h-plot" aria-hidden="true">{"".join(bars)}</div>
  <div class="h-axis" aria-hidden="true">{"".join(months)}</div>
</figure>"""


def ledger_row(release):
    notes = release.get("notes")
    link = f'<a href="{notes_href(release, "")}">{"Notes" if notes else "Notes on GitHub"}</a>'
    claim = f'<p>{escape(release["claim"])}</p>' if release.get("claim") else ""
    return f"""<li class="rel{' full' if notes else ''}">
  <h3><a href="{notes_href(release, '')}">{release['version']}</a></h3>
  <time datetime="{release['date']}">{long_date(release['date'])}</time>
  <span class="n"><a href="{compare_url(release)}">{release['commits']} {'commit' if release['commits'] == 1 else 'commits'}</a></span>
  <span class="go-notes">{link}</span>
  {claim}
</li>"""


def band(image):
    return f'<div class="sky band" data-image="{image}" data-fade="0.42 0.17 -1 0" data-focus="0.38" aria-hidden="true"><div class="wrap"></div></div>'


def index_page(releases):
    latest = releases[0]
    hero = f"""{band("assets/sky.jpg")}
<header class="intro wrap">
  <h1 class="hide">Mettle versions</h1>
  <p class="latest"><a href="{latest['version']}/">{latest['version']}</a></p>
  <p class="what">{escape(latest['claim'])}</p>
  <ul class="facts">
    <li><time datetime="{latest['date']}">{long_date(latest['date'])}</time></li>
    <li><a href="{latest['version']}/">Read the notes</a></li>
    <li><a href="{release_url(latest)}">Download from GitHub</a></li>
  </ul>
</header>"""
    rows = "\n".join(ledger_row(release) for release in releases)
    body = f"""{history(releases)}
<section class="releases wrap" aria-labelledby="every">
  <h2 id="every">Every release</h2>
  <ol>
{rows}
  </ol>
</section>"""
    return page("Mettle versions", "Every release of the Mettle compiler, with its notes.", 0, hero, body, latest)


def chart(data):
    rows = data["rows"]
    ratios = [value for r in rows for value in (r["before"], r["after"])]
    span = max(abs(v - 1) for v in ratios) * 1.08
    x = lambda v: 50 + (v - 1) / span * 50
    lines = []
    for r in sorted(rows, key=lambda r: r["after"]):
        left, right = sorted((x(r["before"]), x(r["after"])))
        ahead = " ahead" if r["after"] < 1 else ""
        lines.append(
            f'<li><span class="nm">{escape(r["name"])}</span><span class="tr">'
            f'<i class="mv" style="left:{left:.2f}%;width:{right - left:.2f}%"></i>'
            f'<i class="was" style="left:{x(r["before"]):.2f}%"></i>'
            f'<i class="now{ahead}" style="left:{x(r["after"]):.2f}%"></i></span>'
            f'<span class="vl"><s>{r["before"]:.2f}</s> {r["after"]:.2f}&times;</span></li>'
        )
    return f"""<figure class="gap">
  <h2>{escape(data['title'])}</h2>
  <p class="cap">{escape(data['caption'])}</p>
  <div class="g-head" aria-hidden="true"><span></span><span><span>Mettle faster</span><span>clang faster</span></span><span></span></div>
  <ol class="g-rows">{"".join(lines)}</ol>
  <p class="g-key"><i class="was"></i>{escape(data['before'])}<i class="now"></i>{escape(data['after'])}<span>Run time against clang -O3, lower is faster.</span></p>
</figure>"""


def version_page(release, releases):
    lead = ""
    parts = []
    for section in release["sections"]:
        content = "\n".join(section["blocks"])
        if section["title"]:
            parts.append(f'<section id="{section["id"]}"><h2>{inline(section["title"])}</h2>{content}</section>')
        else:
            lead = content
    if release.get("chart"):
        parts.insert(0, chart(release["chart"]))
    toc = "".join(
        f'<li><a href="#{section["id"]}">{inline(section["title"])}</a></li>'
        for section in release["sections"]
        if section["title"]
    )
    index = releases.index(release)
    newer = releases[index - 1] if index > 0 else None
    older = releases[index + 1] if index + 1 < len(releases) else None
    steps = []
    if older:
        steps.append(f'<li>Previous release <a href="{notes_href(older, "../")}">{older["version"]}</a></li>')
    if newer:
        steps.append(f'<li>Next release <a href="{notes_href(newer, "../")}">{newer["version"]}</a></li>')
    hero = f"""{band("../assets/sky.jpg")}
<header class="intro wrap">
  <h1 class="latest">{release['version']}</h1>
  <p class="what">{escape(release['claim'])}</p>
  <ul class="facts">
    <li><time datetime="{release['date']}">{long_date(release['date'])}</time></li>
    <li><a href="{compare_url(release)}">{release['commits']} commits since {release['previous']}</a></li>
    <li><a href="{release_url(release)}">Download from GitHub</a></li>
  </ul>
</header>"""
    body = f"""<div class="wrap notes">
  <nav class="toc" aria-label="Sections">
    <ol>{toc}</ol>
    <ul class="steps">{"".join(steps)}</ul>
  </nav>
  <article>
    <div class="lead">{lead}</div>
{chr(10).join(parts)}
  </article>
</div>"""
    return page(f"Mettle {release['version']}", release["claim"], 1, hero, body, releases[0])


def build(out):
    releases = load()
    assets = out / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    for published, source in registry.SITE_ASSETS.items():
        shutil.copyfile(source, assets / published)
    shutil.copyfile(SKY, assets / "sky.jpg")
    (assets / "emblem.svg").write_bytes(
        registry.EMBLEM.replace(
            '<svg viewBox="0 0 150 150" aria-hidden="true">',
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 150 150" fill="#121211">',
        ).encode("utf-8")
    )
    (out / "index.html").write_bytes(index_page(releases).encode("utf-8"))
    pages = [release for release in releases if release.get("notes")]
    for release in pages:
        folder = out / release["version"]
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "index.html").write_bytes(version_page(release, releases).encode("utf-8"))
    print(f"{len(releases)} releases, {len(pages)} with notes on this site, written to {out}")


def main():
    parser = argparse.ArgumentParser(description="Build the Mettle versions pages.")
    parser.add_argument("--out", required=True)
    arguments = parser.parse_args()
    build(pathlib.Path(arguments.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
