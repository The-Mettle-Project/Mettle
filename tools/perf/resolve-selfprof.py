import bisect
import struct
import subprocess
import sys
from collections import Counter


def image_base(path):
    data = open(path, "rb").read(1024)
    lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    return struct.unpack_from("<Q", data, lfanew + 0x18 + 0x18)[0]


def load_symbols(path):
    entries = []
    listing = subprocess.run(["nm", "-n", path], capture_output=True, text=True).stdout
    for line in listing.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        address, kind, name = parts
        if kind not in "tTwW" or name.startswith("."):
            continue
        try:
            entries.append((int(address, 16), name))
        except ValueError:
            continue
    entries.sort()
    return entries


exe = sys.argv[1] if len(sys.argv) > 1 else "bin/mettle.exe"
samples = sys.argv[2] if len(sys.argv) > 2 else "mettle.selfprof"
rows = int(sys.argv[3]) if len(sys.argv) > 3 else 40
callers_of = sys.argv[4] if len(sys.argv) > 4 else None

symbols = load_symbols(exe)
addresses = [a for a, _ in symbols]
preferred = image_base(exe)


def name_of(value, module):
    relative = value - module
    if not 0 <= relative < 0x10000000:
        return None
    index = bisect.bisect_right(addresses, preferred + relative) - 1
    return symbols[index][1] if index >= 0 else None


module = None
flat = Counter()
callers = Counter()
total = 0
foreign = 0
for line in open(samples):
    line = line.split()
    if not line:
        continue
    if line[0] == "module":
        module = int(line[1], 16)
        continue
    if module is None:
        continue
    total += 1
    here = name_of(int(line[0], 16), module)
    if here is None:
        foreign += 1
        continue
    flat[here] += 1
    if callers_of and here == callers_of and len(line) > 1:
        callers[name_of(int(line[1], 16), module) or "<unknown>"] += 1

print(f"{total} samples ({foreign} outside the compiler image)")
for name, count in flat.most_common(rows):
    print(f"  {count * 100.0 / total:6.2f}%  {count:8d}  {name}")
if callers_of:
    print(f"\ncallers of {callers_of}:")
    for name, count in callers.most_common(20):
        print(f"  {count:8d}  {name}")
