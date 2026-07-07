import sys
lines = []
n = 4000
for i in range(n):
    lines.append(f"fn f{i}(x: int32) -> int32 {{")
    lines.append(f"    var t: int32 = x + {i};")
    lines.append(f"    t = t * 3;")
    lines.append(f"    t = t ^ {i % 7};")
    lines.append(f"    return t;")
    lines.append("}")
for d in range(10):
    lines.append(f"fn driver{d}(s0: int32) -> int32 {{")
    lines.append("    var s: int32 = s0;")
    for i in range(d * 100, (d + 1) * 100):
        lines.append(f"    s = s + f{i * 4 % n}(s);")
    lines.append("    return s;")
    lines.append("}")
lines.append("fn main() -> int32 {")
lines.append("    var s: int32 = 0;")
for d in range(10):
    lines.append(f"    s = s + driver{d}(s);")
lines.append("    return s - s;")
lines.append("}")
open("tests/cs_split.mettle", "w").write("\n".join(lines) + "\n")
print("split:", len(lines))
