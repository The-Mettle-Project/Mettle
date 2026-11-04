import argparse
import datetime
import hashlib
import html
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
PACKAGES = HERE / "registry" / "packages"
FONT = HERE / "fonts" / "GeistSerif-Regular.woff2"
SOURCE_URL = "https://github.com/The-Mettle-Project/Mettle/blob/master/website/registry/packages"
DOCS_URL = "https://github.com/The-Mettle-Project/Mettle/blob/master/docs/packages.md"
INDEX_HEADER = "mettle-registry-1"
MANIFEST = "mettle.package"
LEDGER = "versions"
TAR_BLOCK = 512
MAX_ARCHIVE = 16 << 20
MAX_FILES = 4096
NAME = re.compile(r"[a-z][a-z0-9_]{0,63}\Z")
VERSION = re.compile(r"(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\Z")
COMPONENT = re.compile(r"[A-Za-z0-9_+.-]+\Z")
RESERVED_NAMES = {"std", "mettle", "con", "prn", "aux", "nul", "v1"}
RESERVED_STEMS = {"con", "prn", "aux", "nul"}
BINARY = {".exe", ".dll", ".so", ".dylib", ".o", ".obj", ".a", ".lib", ".pdb", ".tar", ".zip", ".gz"}
SKIPPED_ROOT_FILES = {"mettle.deps", "mettle.lock"}
EMBLEM = (
    '<svg viewBox="0 0 150 150" aria-hidden="true">'
    '<polygon points="56.4 63.7 69.5 34.8 113.7 32.3 133.5 51 122.3 74.9 130.2 88.3 147.6 48 119.6 20.8 62.4 20.9 42.4 63.7"/>'
    '<polygon points="60.6 63.7 68.9 57.7 95.6 57.7 108.9 63.6"/>'
    '<polygon points="47.9 67 109.2 88.8 114.3 97.6 138 109.5 114.2 66.9"/>'
    '<path d="m140.4 114.6-94.1-45.1-2.9 6 43.7 41.8-23.6 0.1-20.8-40.3-24.2 51.9 129.7-0.1-7.8-14.3z"/>'
    '<path d="m14.2 63-12.1 5.5 1.4 2.1 23.1 32.4 13.9-29.1-26.3-10.9zm14.7 22.3-17.3-19.9 24.4 10.3-7.1 9.6z"/>'
    "</svg>"
)
STYLE = """@font-face {
  font-family: "Geist Serif";
  src: url(geist-serif.woff2) format("woff2");
  font-weight: 400;
  font-style: normal;
}
:root {
  --bg: #17181a;
  --ink: #f1efe9;
  --ink-2: #a8a69f;
  --faint: #33363a;
  --accent: #e8a468;
  --serif: "Geist Serif", "Times New Roman", Times, serif;
  --mono: "IBM Plex Mono", ui-monospace, "Cascadia Mono", Consolas, Menlo, monospace;
  --bar: 56px;
  color-scheme: dark;
}
* { box-sizing: border-box; }
html { -webkit-text-size-adjust: 100%; background: var(--bg); scroll-padding-top: var(--bar); }
body { margin: 0; background: var(--bg); color: var(--ink); font-family: var(--serif); font-synthesis: none; font-size: 20px; line-height: 1.42; -webkit-font-smoothing: antialiased; overflow-x: clip; }
a { color: inherit; text-decoration: underline; text-decoration-thickness: 1px; text-underline-offset: 0.18em; text-decoration-color: var(--faint); }
a:hover { text-decoration-color: currentColor; }
a:focus-visible, input:focus-visible, button:focus-visible { outline: 2px solid var(--accent); outline-offset: 3px; }
::selection { background: rgba(232, 164, 104, 0.32); }
.wrap { max-width: 1400px; margin: 0 auto; padding-left: 40px; padding-right: 40px; }
.top { position: sticky; top: 0; z-index: 10; background: var(--bg); box-shadow: 0 1px 0 var(--faint); }
.top .wrap { display: flex; align-items: center; gap: 36px; height: var(--bar); }
.brand { display: inline-flex; align-items: center; gap: 10px; flex: none; color: #fff; text-decoration: none; }
.brand svg { width: 24px; height: 24px; fill: currentColor; }
.top nav { display: flex; gap: 22px; font-size: 16px; white-space: nowrap; margin-left: auto; }
.top nav a { text-decoration: none; }
.top nav a:hover, .top nav a[aria-current] { text-decoration: underline; }
h1 { font-size: clamp(38px, 6.5vw, 118px); line-height: 1.02; font-weight: 400; letter-spacing: -0.028em; margin: 0; overflow-wrap: anywhere; }
.head { padding-top: clamp(40px, 9vh, 112px); padding-bottom: 48px; }
.head p { margin: 16px 0 0; max-width: 34em; font-size: clamp(20px, 2vw, 30px); line-height: 1.16; letter-spacing: -0.012em; text-wrap: pretty; }
.head .up { margin: 0 0 10px; font-size: 16px; letter-spacing: 0; color: var(--ink-2); }
h2 { margin: 0; font-size: clamp(32px, 3.6vw, 56px); line-height: 1.04; font-weight: 400; letter-spacing: -0.024em; }
.sec { display: grid; grid-template-columns: minmax(0, 4fr) minmax(0, 8fr); column-gap: 56px; padding-top: 30px; padding-bottom: 44px; border-top: 1px solid var(--faint); }
.sec > .side { position: sticky; top: calc(var(--bar) + 28px); align-self: start; }
.rows { list-style: none; margin: 0; padding: 0; min-width: 0; }
.rows > li { min-width: 0; padding-bottom: 24px; }
.rows > li:last-child { padding-bottom: 0; }
h3 { margin: 0; font-size: clamp(22px, 1.8vw, 26px); line-height: 1.16; font-weight: 400; letter-spacing: -0.012em; }
h3 a { text-decoration: none; }
h3 a:hover { text-decoration: underline; }
.rows p, .note { margin: 4px 0 0; max-width: 44em; font-size: clamp(17px, 1.25vw, 18px); line-height: 1.34; color: var(--ink-2); text-wrap: pretty; }
code { font-family: var(--mono); font-size: 0.84em; color: var(--ink); }
.facts { margin: 6px 0 0; font-family: var(--mono); font-size: 13.5px; line-height: 1.6; color: var(--ink-2); overflow-wrap: anywhere; }
.facts a { color: inherit; }
.cmd { display: flex; align-items: baseline; flex-wrap: wrap; gap: 8px 20px; }
.cmd pre { margin: 0; padding: 14px 20px; background: #0e0f10; color: #fff; font-family: var(--mono); font-size: clamp(14px, 1.08vw, 17px); line-height: 1.6; white-space: pre-wrap; overflow-wrap: anywhere; }
.cmd .ps { color: rgba(255, 255, 255, 0.6); user-select: none; }
.cmd button { padding: 0; font: inherit; font-size: 17px; color: var(--ink-2); background: none; border: 0; cursor: pointer; text-decoration: underline; text-decoration-thickness: 1px; text-underline-offset: 0.18em; text-decoration-color: var(--faint); }
.cmd button:hover { color: var(--ink); text-decoration-color: currentColor; }
.filter { display: block; width: 100%; margin-top: 18px; padding: 8px 0; font: inherit; font-size: 19px; color: var(--ink); background: none; border: 0; border-bottom: 1px solid var(--ink-2); border-radius: 0; }
.filter::placeholder { color: var(--ink-2); }
.count { margin: 12px 0 0; font-size: 16px; color: var(--ink-2); }
.snip { margin: 0; padding-left: 18px; border-left: 1px solid var(--faint); font-family: var(--mono); font-size: clamp(12.5px, 0.98vw, 14.5px); line-height: 1.6; color: var(--ink); white-space: pre-wrap; overflow-wrap: anywhere; }
.snip + .snip { margin-top: 14px; }
.file { margin: 18px 0 8px; font-family: var(--mono); font-size: 13.5px; color: var(--ink-2); }
.file:first-child { margin-top: 0; }
.ver { display: grid; grid-template-columns: 7em minmax(0, 1fr); column-gap: 20px; padding: 12px 0; border-top: 1px solid var(--faint); font-size: 18px; }
.ver:first-child { padding-top: 0; border-top: 0; }
.ver .facts { margin: 0; }
.ver s { color: var(--ink-2); }
.readme { max-width: 44em; font-size: clamp(17px, 1.25vw, 18px); line-height: 1.4; color: var(--ink-2); }
.readme h2, .readme h3, .readme h4 { margin: 28px 0 0; font-size: clamp(22px, 1.8vw, 26px); line-height: 1.16; font-weight: 400; letter-spacing: -0.012em; color: var(--ink); }
.readme > :first-child { margin-top: 0; }
.readme p, .readme ul { margin: 10px 0 0; }
.readme ul { padding-left: 1.1em; }
.readme li + li { margin-top: 6px; }
.readme pre { margin: 12px 0 0; padding-left: 18px; border-left: 1px solid var(--faint); font-family: var(--mono); font-size: clamp(12.5px, 0.98vw, 14.5px); line-height: 1.6; color: var(--ink); white-space: pre-wrap; overflow-wrap: anywhere; }
.readme pre code { font-size: inherit; }
.files { margin: 0; padding: 0; list-style: none; font-family: var(--mono); font-size: 14px; line-height: 1.7; overflow-wrap: anywhere; }
main { padding-bottom: 88px; }
.foot { padding-top: 24px; padding-bottom: 40px; border-top: 1px solid var(--faint); font-size: 16px; color: var(--ink-2); }
[hidden] { display: none !important; }
@media (max-width: 1000px) {
  .sec { display: block; padding-top: 28px; }
  .sec > .side { position: static; margin-bottom: 22px; }
}
@media (max-width: 680px) {
  :root { --bar: 52px; }
  body { font-size: 18px; }
  .wrap { padding-left: 16px; padding-right: 16px; }
  .top .wrap { gap: 18px; }
  .top nav { gap: 18px; }
  .snip, .readme pre { padding-left: 12px; }
  .ver { grid-template-columns: 5.5em minmax(0, 1fr); }
}
"""


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
            blocks.append(f"<pre><code>{text_escape(chr(10).join(code))}</code></pre>")
        elif line.startswith("#"):
            level = len(line) - len(line.lstrip("#"))
            title = line[level:].strip()
            at += 1
            if level == 1 and not title_skipped:
                title_skipped = True
                continue
            tag = f"h{min(max(level, 2), 4)}"
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


def page(title, description, depth, body, current=False):
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
<link rel="stylesheet" href="{up}assets/registry.css">
<link rel="icon" href="{up}assets/emblem.svg">
</head>
<body>
<header class="top">
  <div class="wrap">
    <a class="brand" href="{site}" aria-label="Mettle, home">{EMBLEM} Mettle</a>
    <nav aria-label="Site">
      <a href="{up}"{mark}>Packages</a>
      <a href="https://github.com/The-Mettle-Project/Mettle/blob/master/docs/README.md">Docs</a>
      <a href="https://github.com/The-Mettle-Project/Mettle">Source</a>
    </nav>
  </div>
</header>
<main>
{body}
</main>
<script>
document.addEventListener("click", function (event) {{
  var button = event.target.closest("button[data-copy]");
  if (!button || !navigator.clipboard) {{ return; }}
  navigator.clipboard.writeText(button.getAttribute("data-copy")).then(function () {{
    button.textContent = "Copied";
    setTimeout(function () {{ button.textContent = "Copy"; }}, 1600);
  }});
}});
</script>
</body>
</html>
"""


def format_size(size):
    if size < 1024:
        return f"{size} B"
    return f"{size / 1024:.1f} KB" if size < 1024 * 1024 else f"{size / 1024 / 1024:.1f} MB"


def command_block(text):
    return (
        '<div class="cmd"><pre><span class="ps">$ </span>'
        f'{text_escape(text)}</pre><button type="button" data-copy="{text_escape(text)}">Copy</button></div>'
    )


def index_page(registry):
    rows = []
    for name, versions in registry.items():
        latest = versions[latest_of(versions)]
        manifest = latest["manifest"]
        needs = ", ".join(sorted(manifest["require"]))
        requires = f" requires {text_escape(needs)}" if needs else ""
        search = text_escape(f"{name} {manifest['description']}".lower())
        rows.append(
            f'<li data-search="{search}"><h3><a href="{name}/">{name}</a></h3>'
            f"<p>{text_escape(manifest['description'])}</p>"
            f'<p class="facts">{latest["version"]} {text_escape(manifest["license"])}{requires}</p></li>'
        )
    count = len(registry)
    noun = "package" if count == 1 else "packages"
    script = """<script>
var filter = document.getElementById("filter");
var rows = document.querySelectorAll("#list > li");
var count = document.getElementById("count");
var empty = document.getElementById("empty");
filter.addEventListener("input", function () {
  var words = filter.value.toLowerCase().split(/\\s+/).filter(Boolean);
  var shown = 0;
  rows.forEach(function (row) {
    var text = row.getAttribute("data-search");
    var hit = words.every(function (word) { return text.indexOf(word) >= 0; });
    row.hidden = !hit;
    if (hit) { shown++; }
  });
  count.textContent = shown + (shown === 1 ? " package" : " packages");
  empty.hidden = shown !== 0;
});
</script>"""
    body = f"""<div class="wrap head">
  <h1>Packages</h1>
  <p>Add one with <code>mettle pkg add</code>. Every version here is pinned by its SHA-256, tested on every compiler commit, and never changes after it is published.</p>
</div>
<section class="wrap sec">
  <div class="side">
    <h2>Find</h2>
    <input id="filter" class="filter" type="search" placeholder="Name or description" aria-label="Filter packages" autocomplete="off">
    <p class="count" id="count">{count} {noun}</p>
  </div>
  <ul class="rows" id="list">
{chr(10).join(rows)}
  </ul>
</section>
<p class="wrap note" id="empty" hidden>No package matches.</p>
<section class="wrap sec">
  <h2 class="side">Use</h2>
  <ul class="rows">
    <li>{command_block("mettle pkg add sha256")}<p>Run it in your project. It writes <code>mettle.package</code>, <code>mettle.lock</code> and <code>mettle.deps</code>, and <code>import "sha256";</code> works from then on.</p></li>
    <li>{command_block("mettle pkg install --frozen")}<p>Installs exactly what <code>mettle.lock</code> names and fails if a hash differs. Use this in CI.</p></li>
  </ul>
</section>
<section class="wrap sec">
  <h2 class="side">Publish</h2>
  <ul class="rows">
    <li><h3><a href="{DOCS_URL}#publishing">Open a pull request</a></h3><p>A package is a folder in this repository. CI checks the hash, compiles the package with the current compiler and runs its <code>@test</code> functions.</p></li>
    <li><h3><a href="v1/index.json">Read the index</a></h3><p>All of it is static files: <code>v1/index.json</code> for tools, <code>v1/packages/&lt;name&gt;.txt</code> and <code>v1/archives/&lt;name&gt;-&lt;version&gt;.tar</code> for the compiler.</p></li>
  </ul>
</section>
{script}"""
    return page("Packages", "Packages for the Mettle language.", 0, body, current=True)


def required_by(registry, target):
    users = []
    for name, versions in registry.items():
        latest = versions[latest_of(versions)]
        if target in latest["manifest"]["require"]:
            users.append(name)
    return users


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
            blocks.append(f'<pre class="snip">{text_escape(chr(10).join(found))}</pre>')
        sections.append(("Exports", "".join(blocks)))
    readme_path = latest["directory"] / "README.md"
    if readme_path.is_file():
        sections.append(("Readme", f'<div class="readme">{render_readme(readme_path.read_text(encoding="utf-8"))}</div>'))
    needs = manifest["require"]
    if needs:
        items = "".join(
            f'<li><h3><a href="../{need}/">{need}</a></h3><p class="facts">{wanted} or newer</p></li>'
            for need, wanted in sorted(needs.items())
        )
        sections.append(("Requires", f'<ul class="rows">{items}</ul>'))
    users = required_by(registry, name)
    if users:
        items = "".join(f'<li><h3><a href="../{user}/">{user}</a></h3></li>' for user in users)
        sections.append(("Required by", f'<ul class="rows">{items}</ul>'))
    rows = []
    for version in sorted(versions, key=version_key, reverse=True):
        package = versions[version]
        label = f"<s>{version}</s>" if package["yanked"] else version
        state = " yanked" if package["yanked"] else ""
        rows.append(
            f'<div class="ver"><div>{label}</div><p class="facts">{package["date"]}{state} '
            f'<a href="../v1/archives/{name}-{version}.tar">{format_size(len(package["archive"]))}</a><br>'
            f'sha256 {package["sha256"]}</p></div>'
        )
    sections.append(("Versions", "".join(rows)))
    files = "".join(
        f'<li><a href="{SOURCE_URL}/{name}/{latest["version"]}/{path}">{text_escape(path)}</a></li>'
        for path in latest["files"]
    )
    sections.append(("Files", f'<ul class="files">{files}</ul>'))
    rendered = "\n".join(
        f'<section class="wrap sec"><h2 class="side">{title}</h2><div>{content}</div></section>'
        for title, content in sections
    )
    source = manifest.get("source")
    source_text = f' <a href="{text_escape(source)}">Source</a>' if source and re.match(r"https?://", source) else ""
    compiler = manifest.get("compiler")
    compiler_text = f" Needs mettle {text_escape(compiler)} or newer." if compiler else ""
    body = f"""<div class="wrap head">
  <p class="up"><a href="../">Packages</a></p>
  <h1>{name}</h1>
  <p>{text_escape(manifest['description'])}</p>
</div>
<section class="wrap sec">
  <h2 class="side">Use</h2>
  <div>
    {command_block(f"mettle pkg add {name}")}
    <p class="note"><code>import "{name}";</code></p>
    <p class="facts">{latest['version']} {text_escape(manifest['license'])}.{compiler_text}{source_text}</p>
  </div>
</section>
{rendered}"""
    return page(f"{name}: Mettle package", manifest["description"], 1, body)


def write_pages(out, registry):
    assets = out / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    (assets / "registry.css").write_bytes(STYLE.encode("utf-8"))
    (assets / "emblem.svg").write_bytes(
        EMBLEM.replace('<svg viewBox="0 0 150 150" aria-hidden="true">', '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 150 150" fill="#121211">').encode("utf-8")
    )
    shutil.copyfile(FONT, assets / "geist-serif.woff2")
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
        environment = {"METTLE_REGISTRY": str(out / "v1"), "METTLE_CACHE": str(scratch / "cache")}
        for name, versions in registry.items():
            check_latest(name, versions[latest_of(versions)], compiler, scratch, environment)


def check_latest(name, package, compiler, scratch, environment):
    project = scratch / f"use-{name}"
    project.mkdir()
    run_tool([compiler, "pkg", "add", f"{name}@{package['version']}"], project, environment)
    (project / "main.mettle").write_text(f'import "{name}";\n\nfn main() -> int32 {{\n  return 0;\n}}\n', encoding="ascii")
    run_tool([compiler, "main.mettle", "-o", "main.obj"], project)
    installed = project / ".mettle" / "packages" / f"{name}-{package['version']}"
    output = run_tool([compiler, "test", f"{name}.mettle"], installed)
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
