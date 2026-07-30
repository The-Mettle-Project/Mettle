import sys
# Shape B: many small functions + call graph (inliner / per-function optimizer volume)
lines = []
n = int(sys.argv[1]) if len(sys.argv) > 1 else 4000
for i in range(n):
    lines.append(f"fn f{i}(x: int32) -> int32 {{")
    lines.append(f"    var t: int32 = x + {i};")
    lines.append(f"    t = t * 3;")
    lines.append(f"    t = t ^ {i % 7};")
    lines.append(f"    return t;")
    lines.append("}")
lines.append("fn main() -> int32 {")
lines.append("    var s: int32 = 0;")
for i in range(0, n, 4):
    lines.append(f"    s = s + f{i}(s);")
lines.append("    return s - s;")
lines.append("}")
open("tests/cs_funcs.mettle", "w").write("\n".join(lines) + "\n")
print("funcs:", len(lines))
