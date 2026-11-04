import argparse
import datetime
import functools
import hashlib
import html
import http.server
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import threading

HERE = pathlib.Path(__file__).resolve().parent
PACKAGES = HERE / "registry" / "packages"
SITE_ASSETS = {
    "site.css": HERE / "registry" / "site.css",
    "site.js": HERE / "registry" / "site.js",
    "geist-serif.woff2": HERE / "fonts" / "GeistSerif-Regular.woff2",
    "ridge.jpg": HERE / "media" / "card-frame.jpg",
    "canyon.jpg": HERE / "media" / "canyon-poster.jpg",
}
REPOSITORY = "https://github.com/The-Mettle-Project/Mettle"
SOURCE_URL = f"{REPOSITORY}/blob/master/website/registry/packages"
DOCS_URL = f"{REPOSITORY}/blob/master/docs/packages.md"
INDEX_HEADER = "mettle-registry-1"
MANIFEST = "mettle.package"
LEDGER = "versions"
TAR_BLOCK = 512
MAX_ARCHIVE = 16 << 20
MAX_FILES = 4096
API_LINES = 4
NAME = re.compile(r"[a-z][a-z0-9_]{0,63}\Z")
VERSION = re.compile(r"(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\Z")
COMPONENT = re.compile(r"[A-Za-z0-9_+.-]+\Z")
RESERVED_NAMES = {"std", "mettle", "con", "prn", "aux", "nul", "v1"}
RESERVED_STEMS = {"con", "prn", "aux", "nul"}
BINARY = {".exe", ".dll", ".so", ".dylib", ".o", ".obj", ".a", ".lib", ".pdb", ".tar", ".zip", ".gz"}
SKIPPED_ROOT_FILES = {"mettle.deps", "mettle.lock"}
CODE_KEYWORD = re.compile(
    r"(import|fn|var|const|for|in|return|if|else|break|continue|while|struct|enum|extern|export|type|defer|switch|case|default|match)\Z"
)
CODE_TYPE = re.compile(r"(u?int(8|16|32|64)|float(16|32|64)|bool|char|string|cstring|rawptr)\Z")
CODE_TOKEN = re.compile(r'("(?:[^"\\]|\\.)*")|([A-Za-z_][A-Za-z0-9_]*)|(\s+)|(.)')
EMBLEM = (
    '<svg viewBox="0 0 150 150" aria-hidden="true">'
    '<polygon points="56.4 63.7 69.5 34.8 113.7 32.3 133.5 51 122.3 74.9 130.2 88.3 147.6 48 119.6 20.8 62.4 20.9 42.4 63.7"/>'
    '<polygon points="60.6 63.7 68.9 57.7 95.6 57.7 108.9 63.6"/>'
    '<polygon points="47.9 67 109.2 88.8 114.3 97.6 138 109.5 114.2 66.9"/>'
    '<path d="m140.4 114.6-94.1-45.1-2.9 6 43.7 41.8-23.6 0.1-20.8-40.3-24.2 51.9 129.7-0.1-7.8-14.3z"/>'
    '<path d="m14.2 63-12.1 5.5 1.4 2.1 23.1 32.4 13.9-29.1-26.3-10.9zm14.7 22.3-17.3-19.9 24.4 10.3-7.1 9.6z"/>'
    "</svg>"
)


class RegistryError(Exception):
    pass


def version_key(text):
    return tuple(int(part) for part in text.split("."))


def name_is_valid(name):
    if not NAME.match(name) or name in RESERVED_NAMES:
        return False
    return not re.fullmatch(r"(com|lpt)[0-9]", name)


def component_is_valid(component):
    if not COMPONENT.match(component) or component.startswith(".") or component.endswith("."):
        return False
    stem = component.split(".")[0].lower()
    return stem not in RESERVED_STEMS and not re.fullmatch(r"(com|lpt)[0-9]", stem)


def path_is_valid(path):
    return len(path) <= 240 and all(component_is_valid(part) for part in path.split("/"))


def parse_manifest(path):
    fields = {"require": {}}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise RegistryError(f"{path}:{number}: expected `key = value`")
        key, value = (part.strip() for part in line.split("=", 1))
        if key == "require":
            parts = value.split()
            if len(parts) != 2 or not name_is_valid(parts[0]) or not VERSION.match(parts[1]):
                raise RegistryError(f"{path}:{number}: a require line is `require = <name> <version>`")
            fields["require"][parts[0]] = parts[1]
        elif key in ("name", "version", "description", "license", "source", "compiler"):
            fields[key] = value
        else:
            raise RegistryError(f"{path}:{number}: unknown key `{key}`")
    return fields


def check_manifest(fields, where):
    for key in ("name", "version", "description", "license"):
        if not fields.get(key):
            raise RegistryError(f"{where}: {MANIFEST} needs {key}")
    if not name_is_valid(fields["name"]):
        raise RegistryError(f"{where}: `{fields['name']}` is not a package name")
    if not VERSION.match(fields["version"]):
        raise RegistryError(f"{where}: `{fields['version']}` is not a version")
    if fields.get("compiler") and not VERSION.match(fields["compiler"]):
        raise RegistryError(f"{where}: `{fields['compiler']}` is not a version")
    if "\t" in fields["description"]:
        raise RegistryError(f"{where}: a description holds no tabs")


def collect_files(directory):
    found = []
    for root, folders, files in os.walk(directory, followlinks=False):
        relative_root = pathlib.Path(root).relative_to(directory).as_posix()
        at_root = relative_root == "."
        for folder in list(folders):
            full = pathlib.Path(root) / folder
            if folder.startswith("."):
                folders.remove(folder)
            elif full.is_symlink():
                raise RegistryError(f"{full} is a link; a package holds regular files")
        for name in files:
            if name.startswith(".") or (at_root and name in SKIPPED_ROOT_FILES):
                continue
            path = name if at_root else f"{relative_root}/{name}"
            if (pathlib.Path(root) / name).is_symlink():
                raise RegistryError(f"{path} is a link; a package holds regular files")
            if not path_is_valid(path):
                raise RegistryError(f"{directory}: `{path}` is not a valid package path")
            if pathlib.PurePosixPath(path).suffix.lower() in BINARY:
                raise RegistryError(f"{directory}: `{path}`: a package holds source, not binaries or archives")
            found.append(path)
    if len(found) > MAX_FILES:
        raise RegistryError(f"{directory}: a package holds at most {MAX_FILES} files")
    folded = {}
    for path in found:
        if path.lower() in folded:
            raise RegistryError(f"{directory}: {folded[path.lower()]} and {path} differ only by case")
        folded[path.lower()] = path
    return sorted(found)


def tar_octal(value, width):
    return b"%0*o\0" % (width - 1, value)


def tar_header(path, size):
    encoded = path.encode("ascii")
    name, prefix = encoded, b""
    if len(encoded) > 100:
        for at in (i for i, byte in enumerate(encoded) if byte == 0x2F):
            if at <= 155 and len(encoded) - at - 1 <= 100:
                name, prefix = encoded[at + 1:], encoded[:at]
                break
        else:
            raise RegistryError(f"{path}: path does not fit a tar header")
    header = bytearray(TAR_BLOCK)
    header[0 : len(name)] = name
    header[100:108] = b"0000644\0"
    header[108:116] = b"0000000\0"
    header[116:124] = b"0000000\0"
    header[124:136] = tar_octal(size, 12)
    header[136:148] = b"00000000000\0"
    header[148:156] = b" " * 8
    header[156] = ord("0")
    header[257:263] = b"ustar\0"
    header[263:265] = b"00"
    header[329:337] = b"0000000\0"
    header[337:345] = b"0000000\0"
    header[345 : 345 + len(prefix)] = prefix
    header[148:156] = tar_octal(sum(header), 7) + b" "
    return bytes(header)


def pack(directory):
    parts = []
    files = collect_files(directory)
    for path in files:
        data = (directory / path).read_bytes()
        parts.append(tar_header(path, len(data)))
        parts.append(data)
        parts.append(b"\0" * (-len(data) % TAR_BLOCK))
    parts.append(b"\0" * (TAR_BLOCK * 2))
    archive = b"".join(parts)
    if len(archive) > MAX_ARCHIVE:
        raise RegistryError(f"{directory}: a package is at most 16 MiB")
    return archive, files


def read_ledger(path):
    entries = {}
    if not path.exists():
        return entries
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        parts = raw.split()
        if not parts:
            continue
        if len(parts) not in (3, 4) or not VERSION.match(parts[0]) or not re.fullmatch(r"[0-9a-f]{64}", parts[1]):
            raise RegistryError(f"{path}:{number}: a line is `<version> <sha256> <date> [yanked]`")
        if len(parts) == 4 and parts[3] != "yanked":
            raise RegistryError(f"{path}:{number}: the fourth field can only be `yanked`")
        if parts[0] in entries:
            raise RegistryError(f"{path}:{number}: {parts[0]} appears twice")
        entries[parts[0]] = {"sha256": parts[1], "date": parts[2], "yanked": len(parts) == 4}
    return entries


def write_ledger(path, entries):
    lines = []
    for version in sorted(entries, key=version_key):
        entry = entries[version]
        tail = " yanked" if entry["yanked"] else ""
        lines.append(f"{version} {entry['sha256']} {entry['date']}{tail}\n")
    path.write_bytes("".join(lines).encode("ascii"))


def load_package_version(name, version, entry):
    directory = PACKAGES / name / version
    where = f"{name} {version}"
    if not directory.is_dir():
        raise RegistryError(f"{where}: the ledger names it and the folder is missing")
    manifest = parse_manifest(directory / MANIFEST)
    check_manifest(manifest, where)
    if manifest["name"] != name or manifest["version"] != version:
        raise RegistryError(f"{where}: {MANIFEST} says {manifest['name']} {manifest['version']}")
    if not (directory / f"{name}.mettle").is_file():
        raise RegistryError(f"{where}: {name}.mettle is missing")
    if not (directory / "LICENSE").is_file():
        raise RegistryError(f"{where}: LICENSE is missing")
    archive, files = pack(directory)
    digest = hashlib.sha256(archive).hexdigest()
    if digest != entry["sha256"]:
        raise RegistryError(
            f"{where}: the files hash to {digest} and the ledger says {entry['sha256']}. "
            "A published version never changes; publish a new version"
        )
    return {
        "name": name,
        "version": version,
        "sha256": digest,
        "date": entry["date"],
        "yanked": entry["yanked"],
        "manifest": manifest,
        "archive": archive,
        "files": files,
        "directory": directory,
    }


def load_registry():
    registry = {}
    for folder in sorted(PACKAGES.iterdir()) if PACKAGES.is_dir() else []:
        if not folder.is_dir():
            continue
        name = folder.name
        if not name_is_valid(name):
            raise RegistryError(f"`{name}` is not a package name")
        ledger = read_ledger(folder / LEDGER)
        on_disk = {child.name for child in folder.iterdir() if child.is_dir()}
        for extra in on_disk - set(ledger):
            raise RegistryError(f"{name} {extra}: the folder exists and the ledger does not name it")
        registry[name] = {
            version: load_package_version(name, version, ledger[version])
            for version in sorted(ledger, key=version_key)
        }
        if not registry[name]:
            raise RegistryError(f"{name}: no versions")
    check_requirements(registry)
    return registry


def check_requirements(registry):
    for name, versions in registry.items():
        for version, package in versions.items():
            for need, wanted in package["manifest"]["require"].items():
                if need not in registry or wanted not in registry[need]:
                    raise RegistryError(f"{name} {version} requires {need} {wanted}, which the registry does not have")
                if need == name:
                    raise RegistryError(f"{name} {version} requires itself")
    for name, versions in registry.items():
        for version in versions:
            walk_requirements(registry, name, version, [])


def walk_requirements(registry, name, version, trail):
    here = (name, version)
    if here in trail:
        chain = " -> ".join(f"{n} {v}" for n, v in trail + [here])
        raise RegistryError(f"requirement cycle: {chain}")
    for need, wanted in registry[name][version]["manifest"]["require"].items():
        walk_requirements(registry, need, wanted, trail + [here])


def latest_of(versions):
    live = [version for version, package in versions.items() if not package["yanked"]]
    return max(live or list(versions), key=version_key)


def text_escape(value):
    return html.escape(value, quote=True)


def write_index_files(out, registry):
    data = out / "v1"
    (data / "packages").mkdir(parents=True, exist_ok=True)
    (data / "archives").mkdir(parents=True, exist_ok=True)
    summary_lines = [INDEX_HEADER]
    summary = {"format": 1, "packages": []}
    for name, versions in registry.items():
        latest = versions[latest_of(versions)]
        lines = [INDEX_HEADER, f"name\t{name}", f"description\t{latest['manifest']['description']}"]
        releases = []
        for version, package in versions.items():
            manifest = package["manifest"]
            needs = sorted(manifest["require"].items())
            needs_text = ",".join(f"{n}@{v}" for n, v in needs) or "-"
            state = "yanked" if package["yanked"] else "live"
            lines.append(
                f"release\t{version}\t{package['sha256']}\t{len(package['archive'])}\t"
                f"{manifest.get('compiler') or '-'}\t{state}\t{needs_text}"
            )
            (data / "archives" / f"{name}-{version}.tar").write_bytes(package["archive"])
            releases.append(
                {
                    "version": version,
                    "sha256": package["sha256"],
                    "size": len(package["archive"]),
                    "published": package["date"],
                    "yanked": package["yanked"],
                    "compiler": manifest.get("compiler"),
                    "requires": dict(needs),
                    "archive": f"archives/{name}-{version}.tar",
                }
            )
        (data / "packages" / f"{name}.txt").write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
        summary_lines.append(f"package\t{name}\t{latest['version']}\t{latest['manifest']['description']}")
        summary["packages"].append(
            {
                "name": name,
                "latest": latest["version"],
                "description": latest["manifest"]["description"],
                "license": latest["manifest"]["license"],
                "source": latest["manifest"].get("source"),
                "versions": releases,
            }
        )
    (data / "index.txt").write_bytes(("\n".join(summary_lines) + "\n").encode("utf-8"))
    (data / "index.json").write_bytes((json.dumps(summary, indent=1) + "\n").encode("utf-8"))


def exports_of(package):
    groups = []
    for path in package["files"]:
        if not path.endswith(".mettle"):
            continue
        source = (package["directory"] / path).read_text(encoding="utf-8")
        found = []
        for match in re.finditer(r"^export\s+([^\n{;=]*(?:\n[ \t]+[^\n{;=]*)*)", source, re.M):
            found.append(re.sub(r"\s+", " ", match.group(1)).strip())
        if found:
            groups.append((path, found))
    return groups


def export_listing(found):
    constants = [line.split(None, 1)[1] for line in found if line.startswith("const ")]
    others = sorted((line for line in found if not line.startswith("const ")), key=lambda line: line.startswith("fn "))
    if constants:
        others.append("const " + ", ".join(constants))
    return "\n".join(others)


def highlight_code(text):
    out = []
    for match in CODE_TOKEN.finditer(text):
        token = match.group(0)
        if match.group(1):
            out.append(f'<span class="st">{text_escape(token)}</span>')
        elif match.group(2) and CODE_KEYWORD.match(token):
            out.append(f'<span class="k">{token}</span>')
        elif match.group(2) and CODE_TYPE.match(token):
            out.append(f'<span class="t">{token}</span>')
        else:
            out.append(text_escape(token))
    return "".join(out)


def inline_markdown(text):
    pieces = re.split(r"(`[^`]+`)", text)
    out = []
    for piece in pieces:
        if piece.startswith("`") and piece.endswith("`") and len(piece) > 2:
            out.append(f"<code>{text_escape(piece[1:-1])}</code>")
            continue
        escaped = text_escape(piece)
        escaped = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", escaped)
        escaped = re.sub(
            r"\[([^\]]+)\]\((https?://[^)\s]+)\)",
            lambda m: f'<a href="{m.group(2)}" rel="nofollow">{m.group(1)}</a>',
            escaped,
        )
        out.append(escaped)
    return "".join(out)


def render_readme(text):
    blocks = []
    lines = text.splitlines()
    at = 0
    title_skipped = False
    while at < len(lines):
        line = lines[at]
        if line.startswith("```"):
            at += 1
            code = []
            while at < len(lines) and not lines[at].startswith("```"):
                code.append(lines[at])
                at += 1
            at += 1
            blocks.append(f"<pre><code>{highlight_code(chr(10).join(code))}</code></pre>")
        elif line.startswith("#"):
            level = len(line) - len(line.lstrip("#"))
            title = line[level:].strip()
            at += 1
            if level == 1 and not title_skipped:
                title_skipped = True
                continue
            tag = f"h{min(max(level + 1, 3), 4)}"
            blocks.append(f"<{tag}>{inline_markdown(title)}</{tag}>")
        elif re.match(r"[-*] ", line):
            items = []
            while at < len(lines) and (re.match(r"[-*] ", lines[at]) or lines[at].startswith("  ")):
                if re.match(r"[-*] ", lines[at]):
                    items.append(lines[at][2:].strip())
                else:
                    items[-1] += " " + lines[at].strip()
                at += 1
            body = "".join(f"<li>{inline_markdown(item)}</li>" for item in items)
            blocks.append(f"<ul>{body}</ul>")
        elif not line.strip():
            at += 1
        else:
            paragraph = []
            while at < len(lines) and lines[at].strip() and not re.match(r"(```|#|[-*] )", lines[at]):
                paragraph.append(lines[at].strip())
                at += 1
            blocks.append(f"<p>{inline_markdown(' '.join(paragraph))}</p>")
    return "\n".join(blocks)


def format_size(size):
    if size < 1024:
        return f"{size} B"
    return f"{size / 1024:.1f} KB" if size < 1024 * 1024 else f"{size / 1024 / 1024:.1f} MB"


def add_button(name):
    command = f"mettle pkg add {name}"
    return f'<button class="add" type="button" data-copy="{command}"><code>{command}</code><span>Copy</span></button>'


def footer(up):
    command = "python website/registry.py add path/to/mylib"
    return f"""<footer class="end" data-image="{up}assets/canyon.jpg" data-fade="0.14 0.4 0.5 0.95">
  <div class="wrap">
    <h2>Publish yours.</h2>
    <div class="cmd"><pre><span class="ps">$ </span>{command}</pre><button type="button" data-copy="{command}">Copy</button></div>
    <ul class="go">
      <li><a href="{DOCS_URL}#publishing">How publishing works</a></li>
      <li><a href="{DOCS_URL}">Using packages</a></li>
      <li><a href="{up}v1/index.json">The index as JSON</a></li>
      <li><a href="{REPOSITORY}/tree/master/website/registry/packages">Every package's source</a></li>
    </ul>
    <p class="fine">Footage by <a href="https://www.pexels.com/video/falling-16296843/">Film Composite</a> and <a href="https://www.pexels.com/video/dramatic-aerial-view-of-utah-s-rugged-canyons-34305686/">Alex Moliski</a>. Dither after <a href="https://motion-core.dev/docs/dithered-image">Motion Core</a>.</p>
  </div>
</footer>"""


def page(title, description, depth, hero, body, current=False):
    up = "../" * depth
    site = up + "../"
    mark = ' aria-current="page"' if current else ""
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{text_escape(title)}</title>
<meta name="description" content="{text_escape(description)}">
<meta name="theme-color" content="#9db04a">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&display=swap" rel="stylesheet">
<link rel="stylesheet" href="{up}assets/site.css">
<link rel="icon" href="{up}assets/emblem.svg">
</head>
<body>
<header class="top">
  <div class="wrap">
    <a class="brand" href="{site}" aria-label="Mettle, home">{EMBLEM} Mettle</a>
    <nav aria-label="Site">
      <a href="{up}"{mark}>Packages</a>
      <a href="{REPOSITORY}/blob/master/docs/README.md">Docs</a>
      <a href="{REPOSITORY}">Source</a>
    </nav>
  </div>
</header>
{hero}
<main>
{body}
</main>
{footer(up)}
<script src="{up}assets/site.js"></script>
</body>
</html>
"""


def main_module_signatures(package):
    source_name = f"{package['name']}.mettle"
    for path, found in exports_of(package):
        if path == source_name:
            return [line for line in found if line.startswith("fn ")]
    return []


def index_row(name, versions):
    latest = versions[latest_of(versions)]
    manifest = latest["manifest"]
    signatures = main_module_signatures(latest)
    every_export = " ".join(line for _, found in exports_of(latest) for line in found)
    search = text_escape(f"{name} {manifest['description']} {every_export}".lower())
    api = highlight_code("\n".join(signatures[:API_LINES]))
    facts = [latest["version"], text_escape(manifest["license"]), format_size(len(latest["archive"]))]
    needs = sorted(manifest["require"])
    if needs:
        links = ", ".join(f'<a href="{need}/">{need}</a>' for need in needs)
        facts.append(f"requires {links}")
    facts_html = "".join(f"<li>{fact}</li>" for fact in facts)
    return f"""<li class="pkg wrap" data-search="{search}">
  <div>
    <h2><a href="{name}/">{name}</a></h2>
    {add_button(name)}
  </div>
  <div>
    <p class="desc">{text_escape(manifest['description'])}</p>
    <pre class="api"><code>{api}</code></pre>
    <ul class="facts">{facts_html}</ul>
  </div>
</li>"""


def index_page(registry):
    count = len(registry)
    noun = "package" if count == 1 else "packages"
    rows = "\n".join(index_row(name, versions) for name, versions in registry.items())
    hero = f"""<div class="sky" data-image="assets/ridge.jpg" data-fade="0.42 0.17 -1 0" data-focus="0.9">
  <div class="wrap">
    <h1 class="hide">Mettle packages</h1>
    <label class="find"><span class="hide">Find a package by name, description or function</span><input id="filter" type="search" placeholder="Find a package" autocomplete="off" spellcheck="false"></label>
    <p class="tally" id="tally">{count} {noun}</p>
  </div>
</div>"""
    body = f"""<ul class="ledger" id="ledger">
{rows}
</ul>
<div class="pkg wrap none" id="none" hidden>
  <div>
    <h2>None yet.</h2>
  </div>
  <div>
    <p class="desc">No package has &ldquo;<span id="none-query"></span>&rdquo; in its name, description or functions.</p>
    <ul class="facts">
      <li><button id="clear" type="button">Show all {count} {noun}</button></li>
    </ul>
  </div>
</div>"""
    return page("Mettle packages", "Packages for the Mettle language.", 0, hero, body, current=True)


def required_by(registry, target):
    users = []
    for name, versions in registry.items():
        latest = versions[latest_of(versions)]
        if target in latest["manifest"]["require"]:
            users.append(name)
    return users


def rail_facts(registry, name, latest):
    manifest = latest["manifest"]
    rows = [
        ("Version", latest["version"]),
        ("License", text_escape(manifest["license"])),
        ("Size", format_size(len(latest["archive"]))),
        ("Published", latest["date"]),
    ]
    if manifest.get("compiler"):
        rows.append(("Compiler", f"{text_escape(manifest['compiler'])} or newer"))
    if manifest["require"]:
        needs = "".join(
            f'<li><a href="../{need}/">{need}</a> {wanted}</li>' for need, wanted in sorted(manifest["require"].items())
        )
        rows.append(("Requires", f"<ul>{needs}</ul>"))
    users = required_by(registry, name)
    if users:
        rows.append(("Used by", "<ul>" + "".join(f'<li><a href="../{user}/">{user}</a></li>' for user in users) + "</ul>"))
    if manifest.get("source") and re.match(r"https?://", manifest["source"]):
        rows.append(("Source", f'<a href="{text_escape(manifest["source"])}">Repository</a>'))
    files = "".join(
        f'<li><a href="{SOURCE_URL}/{name}/{latest["version"]}/{path}">{text_escape(path)}</a></li>'
        for path in latest["files"]
    )
    rows.append(("Files", f"<ul>{files}</ul>"))
    return "".join(f"<dt>{label}</dt><dd>{value}</dd>" for label, value in rows)


def version_rows(name, versions):
    rows = []
    for version in sorted(versions, key=version_key, reverse=True):
        package = versions[version]
        label = f"<s>{version}</s>" if package["yanked"] else version
        state = " yanked" if package["yanked"] else ""
        rows.append(
            f'<div class="ver"><div>{label}</div><div>{package["date"]}{state} '
            f'<a href="../v1/archives/{name}-{version}.tar">{format_size(len(package["archive"]))}</a>'
            f'<code>sha256 {package["sha256"]}</code></div></div>'
        )
    return "".join(rows)


def package_page(registry, name):
    versions = registry[name]
    latest = versions[latest_of(versions)]
    manifest = latest["manifest"]
    sections = []
    groups = exports_of(latest)
    if groups:
        blocks = []
        for path, found in groups:
            blocks.append(f'<p class="file">{text_escape(path)}</p>')
            blocks.append(f'<pre class="api"><code>{highlight_code(export_listing(found))}</code></pre>')
        sections.append(("Exports", "".join(blocks)))
    readme_path = latest["directory"] / "README.md"
    if readme_path.is_file():
        rendered = render_readme(readme_path.read_text(encoding="utf-8"))
        sections.append(("Readme", f'<div class="readme">{rendered}</div>'))
    sections.append(("Versions", version_rows(name, versions)))
    content = "\n".join(f"<section><h2>{title}</h2>{html_text}</section>" for title, html_text in sections)
    hero = f"""<div class="sky low" data-image="../assets/ridge.jpg" data-fade="0.62 0.17 -1 0" data-focus="0.9">
  <div class="wrap">
    <h1>{name}</h1>
    <p class="what">{text_escape(manifest['description'])}</p>
  </div>
</div>"""
    body = f"""<div class="wrap page">
  <aside class="rail">
    {add_button(name)}
    <dl>{rail_facts(registry, name, latest)}</dl>
  </aside>
  <div class="body">
{content}
  </div>
</div>"""
    return page(f"{name} package", manifest["description"], 1, hero, body)


def write_pages(out, registry):
    assets = out / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    for published, source in SITE_ASSETS.items():
        shutil.copyfile(source, assets / published)
    (assets / "emblem.svg").write_bytes(
        EMBLEM.replace('<svg viewBox="0 0 150 150" aria-hidden="true">', '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 150 150" fill="#121211">').encode("utf-8")
    )
    (out / "index.html").write_bytes(index_page(registry).encode("utf-8"))
    for name in registry:
        folder = out / name
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "index.html").write_bytes(package_page(registry, name).encode("utf-8"))


def command_build(arguments):
    registry = load_registry()
    out = pathlib.Path(arguments.out)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    write_index_files(out, registry)
    write_pages(out, registry)
    total = sum(len(versions) for versions in registry.values())
    print(f"{len(registry)} packages, {total} versions, written to {out}")
    return 0


def git_show(reference, path):
    done = subprocess.run(
        ["git", "show", f"{reference}:{path}"], cwd=HERE.parent, capture_output=True
    )
    return done.stdout.decode("utf-8") if done.returncode == 0 else None


def check_append_only(base):
    done = subprocess.run(
        ["git", "ls-tree", "-r", "--name-only", base, "website/registry/packages"],
        cwd=HERE.parent, capture_output=True, text=True,
    )
    if done.returncode != 0:
        raise RegistryError(f"cannot read git ref {base}: {done.stderr.strip()}")
    checked = 0
    for path in done.stdout.split():
        if not path.endswith(f"/{LEDGER}"):
            continue
        name = pathlib.PurePosixPath(path).parent.name
        old = {}
        for raw in (git_show(base, path) or "").splitlines():
            parts = raw.split()
            if parts:
                old[parts[0]] = parts[1]
        now = read_ledger(PACKAGES / name / LEDGER)
        for version, digest in old.items():
            checked += 1
            if version not in now:
                raise RegistryError(f"{name} {version} was published and is gone; yank it instead")
            if now[version]["sha256"] != digest:
                raise RegistryError(f"{name} {version} changed hash from {digest} to {now[version]['sha256']}")
    print(f"{checked} published versions unchanged since {base}")


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *arguments):
        pass


def run_tool(command, cwd, env=None):
    merged = dict(os.environ)
    merged.update(env or {})
    done = subprocess.run(command, cwd=cwd, env=merged, capture_output=True, text=True)
    if done.returncode != 0:
        raise RegistryError(f"{' '.join(str(c) for c in command)} failed in {cwd}:\n{done.stdout}{done.stderr}")
    return done.stdout


def check_with_compiler(registry, compiler):
    compiler = str(pathlib.Path(compiler).resolve())
    for name, versions in registry.items():
        for version, package in versions.items():
            printed = run_tool([compiler, "pkg", "pack", str(package["directory"])], HERE)
            if f"sha256 {package['sha256']}" not in printed:
                raise RegistryError(f"{name} {version}: the compiler and this script pack it differently:\n{printed}")
    print("the compiler packs every version to the bytes this script hashes")
    with tempfile.TemporaryDirectory(prefix="mettle-registry-") as scratch:
        scratch = pathlib.Path(scratch)
        out = scratch / "site"
        out.mkdir()
        write_index_files(out, registry)
        handler = functools.partial(QuietHandler, directory=str(out))
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        environment = {
            "METTLE_REGISTRY": f"http://127.0.0.1:{server.server_address[1]}/v1",
            "METTLE_CACHE": str(scratch / "cache"),
        }
        try:
            for name, versions in registry.items():
                check_latest(name, versions[latest_of(versions)], compiler, scratch, environment)
        finally:
            server.shutdown()
            server.server_close()


def check_latest(name, package, compiler, scratch, environment):
    project = scratch / f"use-{name}"
    project.mkdir()
    run_tool([compiler, "pkg", "add", f"{name}@{package['version']}"], project, environment)
    (project / "main.mettle").write_text(f'import "{name}";\n\nfn main() -> int32 {{\n  return 0;\n}}\n', encoding="ascii")
    run_tool([compiler, "main.mettle", "-o", "main.obj"], project)
    installed = project / ".mettle" / "packages" / f"{name}-{package['version']}"
    for path in package["files"]:
        if path.endswith(".mettle") and "@test" in (installed / path).read_text(encoding="utf-8"):
            output = run_tool([compiler, "test", path], installed)
            print(f"{name} {package['version']}: {output.strip().splitlines()[-1]}")


def command_check(arguments):
    registry = load_registry()
    total = sum(len(versions) for versions in registry.values())
    print(f"{len(registry)} packages, {total} versions, every hash matches its ledger")
    if arguments.base:
        check_append_only(arguments.base)
    if arguments.compiler:
        check_with_compiler(registry, arguments.compiler)
    return 0


def command_add(arguments):
    source = pathlib.Path(arguments.directory).resolve()
    manifest = parse_manifest(source / MANIFEST)
    check_manifest(manifest, str(source))
    name, version = manifest["name"], manifest["version"]
    if not (source / f"{name}.mettle").is_file():
        raise RegistryError(f"{source}: {name}.mettle is missing")
    if not (source / "LICENSE").is_file():
        raise RegistryError(f"{source}: LICENSE is missing")
    ledger_path = PACKAGES / name / LEDGER
    ledger = read_ledger(ledger_path)
    if version in ledger:
        raise RegistryError(f"{name} {version} is already published; a published version never changes")
    existing = load_registry()
    for need, wanted in manifest["require"].items():
        if need not in existing or wanted not in existing[need]:
            raise RegistryError(f"{name} {version} requires {need} {wanted}, which the registry does not have")
    archive, files = pack(source)
    target = PACKAGES / name / version
    target.mkdir(parents=True)
    for path in files:
        destination = target / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / path, destination)
    digest = hashlib.sha256(archive).hexdigest()
    ledger[version] = {
        "sha256": digest,
        "date": arguments.date or datetime.date.today().isoformat(),
        "yanked": False,
    }
    write_ledger(ledger_path, ledger)
    try:
        load_registry()
    except RegistryError:
        shutil.rmtree(target)
        del ledger[version]
        if ledger:
            write_ledger(ledger_path, ledger)
        else:
            shutil.rmtree(PACKAGES / name)
        raise
    print(f"{name} {version} sha256 {digest}")
    return 0


def command_yank(arguments):
    path = PACKAGES / arguments.name / LEDGER
    ledger = read_ledger(path)
    if arguments.version not in ledger:
        raise RegistryError(f"{arguments.name} {arguments.version} is not published")
    ledger[arguments.version]["yanked"] = not arguments.undo
    write_ledger(path, ledger)
    return 0


def main():
    parser = argparse.ArgumentParser(description="Build and check the static Mettle package registry.")
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build", help="write the static site")
    build.add_argument("--out", required=True)
    build.set_defaults(run=command_build)
    check = commands.add_parser("check", help="check every package")
    check.add_argument("--base", help="git ref whose published versions must be unchanged")
    check.add_argument("--compiler", help="also pack, install, compile and test with this compiler")
    check.set_defaults(run=command_check)
    add = commands.add_parser("add", help="copy a package folder into the registry")
    add.add_argument("directory")
    add.add_argument("--date")
    add.set_defaults(run=command_add)
    yank = commands.add_parser("yank", help="stop new projects picking a version")
    yank.add_argument("name")
    yank.add_argument("version")
    yank.add_argument("--undo", action="store_true")
    yank.set_defaults(run=command_yank)
    arguments = parser.parse_args()
    try:
        return arguments.run(arguments)
    except RegistryError as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
