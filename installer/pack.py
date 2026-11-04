import argparse
import io
import lzma
import os
import struct
import subprocess
import zlib

from fontTools.ttLib import TTFont
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ART_WIDTH = 1680
ART_HEIGHT = 1050
RT_ICON = 3
RT_GROUP_ICON = 14
RT_VERSION = 16
RT_MANIFEST = 24
LANGUAGE = 0x0409

MANIFEST = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="TheMettleProject.Mettle.Setup" version="{version}.0" processorArchitecture="amd64"/>
  <description>Mettle Setup</description>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security><requestedPrivileges><requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security>
  </trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application><supportedOS Id="{{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}}"/></application>
  </compatibility>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
    </windowsSettings>
  </application>
  <dependency><dependentAssembly>
    <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
  </dependentAssembly></dependency>
</assembly>
"""


def payload_files():
    roots = [
        ("bin/mettle.exe", os.path.join(ROOT, "bin", "mettle.exe")),
        ("bin/mettle-build.bat", os.path.join(HERE, "mettle-build.bat")),
        ("mettle.ico", os.path.join(HERE, "art", "mettle.ico")),
        ("LICENSE", os.path.join(ROOT, "LICENSE")),
        ("README.md", os.path.join(ROOT, "README.md")),
    ]
    trees = [
        ("stdlib", os.path.join(ROOT, "stdlib")),
        ("runtime", os.path.join(ROOT, "bin", "runtime")),
        ("docs", os.path.join(ROOT, "docs")),
        ("mlopt", os.path.join(ROOT, "bin", "mlopt")),
    ]
    files = list(roots)
    for prefix, folder in trees:
        for base, dirs, names in os.walk(folder):
            dirs.sort()
            for name in sorted(names):
                full = os.path.join(base, name)
                rel = os.path.relpath(full, folder).replace("\\", "/")
                files.append((prefix + "/" + rel, full))
    return files


def build_art():
    image = Image.open(os.path.join(ROOT, "website", "media", "card-frame.jpg")).convert("YCbCr")
    w, h = image.size
    target = ART_WIDTH / ART_HEIGHT
    if w / h > target:
        crop = round(h * target)
        left = 0
        image = image.crop((left, 0, left + crop, h))
    else:
        crop = round(w / target)
        image = image.crop((0, 0, w, crop))
    image = image.resize((ART_WIDTH, ART_HEIGHT), Image.LANCZOS)
    luma, blue, red = image.split()
    luma = luma.filter(ImageFilter.UnsharpMask(radius=70, percent=90, threshold=0))
    luma = luma.filter(ImageFilter.UnsharpMask(radius=5, percent=120, threshold=0))
    luma = luma.point(lambda v: round(255 * min(1.0, max(0.0, (v - 14) / 190)) ** 1.2))
    half = (ART_WIDTH // 2, ART_HEIGHT // 2)
    planes = luma.tobytes() + blue.resize(half, Image.BOX).tobytes() + red.resize(half, Image.BOX).tobytes()
    font = io.BytesIO()
    face = TTFont(os.path.join(ROOT, "website", "fonts", "GeistSerif-Regular.woff2"))
    face.flavor = None
    face.save(font)
    font = font.getvalue()
    return struct.pack("<III", ART_WIDTH, ART_HEIGHT, len(font)) + font + lzma_pack(planes, 1 << 24)


def lzma_pack(data, dictionary):
    return lzma.compress(data, format=lzma.FORMAT_ALONE,
                         filters=[dict(id=lzma.FILTER_LZMA1, preset=9, dict_size=dictionary)])


def resource_tree(entries):
    types = {}
    for kind, name, data in entries:
        types.setdefault(kind, []).append((name, data))
    kinds = sorted(types)
    root_size = 16 + 8 * len(kinds)
    name_tables = []
    offset = root_size
    kind_offsets = []
    for kind in kinds:
        kind_offsets.append(offset)
        offset += 16 + 8 * len(types[kind])
    language_offsets = {}
    for kind in kinds:
        for name, _ in types[kind]:
            language_offsets[(kind, name)] = offset
            offset += 16 + 8
    data_entries_at = offset
    count = sum(len(types[kind]) for kind in kinds)
    blobs_at = data_entries_at + 16 * count
    return types, kinds, root_size, kind_offsets, language_offsets, data_entries_at, blobs_at


def build_rsrc(entries, rva):
    types, kinds, root_size, kind_offsets, language_offsets, data_at, blobs_at = resource_tree(entries)
    out = bytearray()
    out += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, len(kinds))
    for kind, at in zip(kinds, kind_offsets):
        out += struct.pack("<II", kind, 0x80000000 | at)
    for kind in kinds:
        names = sorted(types[kind], key=lambda item: item[0])
        out += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, len(names))
        for name, _ in names:
            out += struct.pack("<II", name, 0x80000000 | language_offsets[(kind, name)])
    index = 0
    order = []
    for kind in kinds:
        names = sorted(types[kind], key=lambda item: item[0])
        for name, data in names:
            out += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, 1)
            out += struct.pack("<II", LANGUAGE, data_at + 16 * index)
            order.append(data)
            index += 1
    blob_at = blobs_at
    records = bytearray()
    blobs = bytearray()
    for data in order:
        records += struct.pack("<IIII", rva + blob_at + len(blobs), len(data), 0, 0)
        blobs += data
        blobs += bytes((4 - len(blobs) % 4) % 4)
    return bytes(out + records + blobs)


def icon_entries(path):
    data = open(path, "rb").read()
    _, kind, count = struct.unpack_from("<HHH", data, 0)
    group = struct.pack("<HHH", 0, 1, count)
    entries = []
    for i in range(count):
        w, h, colors, zero, planes, bits, size, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
        entries.append((RT_ICON, i + 1, data[offset:offset + size]))
        group += struct.pack("<BBBBHHIH", w, h, colors, zero, planes, bits, size, i + 1)
    entries.append((RT_GROUP_ICON, 1, group))
    return entries


def wide(text):
    return (text + "\0").encode("utf-16-le")


def version_block(key, value=b"", kind=1, value_len=None, children=b""):
    head_len = 6 + len(wide(key))
    pad = bytes((4 - head_len % 4) % 4)
    body = value
    if body:
        body += bytes((4 - len(body) % 4) % 4)
    length = head_len + len(pad) + len(body) + len(children)
    return struct.pack("<HHH", length, len(value) // 2 if kind == 1 and value_len is None else (value_len or 0), kind) + \
        wide(key) + pad + body + children


def pad4(data):
    return data + bytes((4 - len(data) % 4) % 4)


def numeric_version(version):
    parts = version.split(".")
    if len(parts) == 3 and all(part.isdigit() for part in parts):
        return version
    return "0.0.0"


def version_resource(version):
    numbers = [int(part) for part in numeric_version(version).split(".")] + [0]
    high = (numbers[0] << 16) | numbers[1]
    low = (numbers[2] << 16) | numbers[3]
    fixed = struct.pack("<IIIIIIIIIIIII", 0xFEEF04BD, 0x00010000, high, low, high, low, 0x3F, 0, 0x40004, 1, 0, 0, 0)
    pairs = [
        ("CompanyName", "The Mettle Project"),
        ("FileDescription", "Mettle Setup"),
        ("FileVersion", version),
        ("LegalCopyright", "The Mettle Project. Apache 2.0."),
        ("OriginalFilename", "Mettle-Setup.exe"),
        ("ProductName", "Mettle"),
        ("ProductVersion", version),
    ]
    strings = b"".join(pad4(version_block(k, wide(v))) for k, v in pairs)
    table = version_block("040904B0", children=strings, kind=1, value_len=0)
    string_info = version_block("StringFileInfo", children=pad4(table), kind=1, value_len=0)
    var = version_block("Translation", struct.pack("<HH", LANGUAGE, 1200), kind=0, value_len=4)
    var_info = version_block("VarFileInfo", children=pad4(var), kind=1, value_len=0)
    return version_block("VS_VERSION_INFO", fixed, kind=0, value_len=len(fixed),
                         children=pad4(string_info) + var_info)


def add_resources(path, version):
    image = bytearray(open(path, "rb").read())
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    sections = struct.unpack_from("<H", image, pe + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe + 20)[0]
    optional = pe + 24
    section_alignment = struct.unpack_from("<I", image, optional + 32)[0]
    file_alignment = struct.unpack_from("<I", image, optional + 36)[0]
    size_of_headers = struct.unpack_from("<I", image, optional + 60)[0]
    table = optional + optional_size
    next_header = table + 40 * sections
    if next_header + 40 > size_of_headers:
        raise SystemExit("no room for another section header in " + path)
    end_of_raw = 0
    end_of_memory = 0
    for i in range(sections):
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", image, table + 40 * i + 8)
        end_of_raw = max(end_of_raw, raw_pointer + raw_size)
        end_of_memory = max(end_of_memory, virtual_address + virtual_size)
    if end_of_raw != len(image):
        raise SystemExit("%s carries %d bytes past its last section" % (path, len(image) - end_of_raw))
    rva = (end_of_memory + section_alignment - 1) & ~(section_alignment - 1)
    entries = icon_entries(os.path.join(HERE, "art", "mettle.ico"))
    entries.append((RT_MANIFEST, 1, MANIFEST.format(version=numeric_version(version)).encode()))
    entries.append((RT_VERSION, 1, version_resource(version)))
    rsrc = build_rsrc(entries, rva)
    pointer = (end_of_raw + file_alignment - 1) & ~(file_alignment - 1)
    aligned = (len(rsrc) + file_alignment - 1) & ~(file_alignment - 1)
    del image[end_of_raw:]
    image += bytes(pointer - len(image))
    image += rsrc + bytes(aligned - len(rsrc))
    struct.pack_into("<8sIIIIIIHHI", image, next_header, b".rsrc", len(rsrc), rva, aligned, pointer, 0, 0, 0, 0, 0x40000040)
    struct.pack_into("<H", image, pe + 6, sections + 1)
    size_of_image = (rva + len(rsrc) + section_alignment - 1) & ~(section_alignment - 1)
    struct.pack_into("<I", image, optional + 56, size_of_image)
    struct.pack_into("<II", image, optional + 112 + 2 * 8, rva, len(rsrc))
    struct.pack_into("<I", image, optional + 64, 0)
    return bytes(image)


def build_stub(compiler, out):
    command = [compiler, "--build", "--release", "--subsystem", "windows",
               "-I", os.path.join(HERE, "setup"),
               "--link-arg", "-lshell32", "--link-arg", "-lole32", "--link-arg", "-ldwmapi",
               os.path.join(HERE, "setup", "setup.mettle"), "-o", out]
    subprocess.run(command, check=True)


def compiler_version(compiler):
    text = subprocess.run([compiler, "--version"], capture_output=True, text=True, check=True).stdout
    return text.split()[1].lstrip("v")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=os.path.join(ROOT, "bin", "mettle.exe"))
    parser.add_argument("--out", default=os.path.join(HERE, "out", "Mettle-Setup.exe"))
    parser.add_argument("--fast", action="store_true")
    args = parser.parse_args()
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    stub_path = args.out + ".stub.exe"
    build_stub(args.compiler, stub_path)
    version = compiler_version(args.compiler)
    stub = add_resources(stub_path, version)
    os.remove(stub_path)
    art = build_art()
    files = payload_files()
    stream = io.BytesIO()
    index = bytearray()
    total = 0
    body = bytearray()
    for path, full in files:
        data = open(full, "rb").read()
        name = path.encode("utf-8")
        body += struct.pack("<H", len(name)) + name + struct.pack("<QI", len(data), zlib.crc32(data))
        stream.write(data)
        total += len(data)
    version_bytes = version.encode()
    index += struct.pack("<QIH", total, len(files), len(version_bytes)) + version_bytes + body
    raw = stream.getvalue()
    if args.fast:
        data = lzma.compress(raw, format=lzma.FORMAT_ALONE,
                             filters=[dict(id=lzma.FILTER_LZMA1, preset=0, dict_size=1 << 22)])
    else:
        data = lzma_pack(raw, 1 << 25)
    art_at = len(stub)
    index_at = art_at + len(art)
    data_at = index_at + len(index)
    footer = b"METTLEIN" + struct.pack("<QQQQQQQ", art_at, len(art), index_at, len(index), data_at, len(data), 0)
    with open(args.out, "wb") as f:
        f.write(stub)
        f.write(art)
        f.write(index)
        f.write(data)
        f.write(footer)
    print("%s: %d files, %.1f MB to %.1f MB, stub %.0f KB, art %.0f KB, version %s" % (
        args.out, len(files), total / 1e6, os.path.getsize(args.out) / 1e6,
        len(stub) / 1e3, len(art) / 1e3, version))


if __name__ == "__main__":
    main()
