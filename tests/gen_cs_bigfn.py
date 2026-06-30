import sys
# Shape C: ONE huge over-budget function with many loops and many calls
# (stresses the over-budget caller gate incl. per-site loop membership).
lines = []
nloops = int(sys.argv[1]) if len(sys.argv) > 1 else 300
lines.append("fn helper(x: int32) -> int32 {")
for i in range(24):
    lines.append(f"    x = x + {i};")
lines.append("    return x;")
lines.append("}")
lines.append("fn main() -> int32 {")
lines.append("    var s: int32 = 0;")
for i in range(nloops):
    lines.append(f"    var i{i}: int32 = 0;")
    lines.append(f"    while (i{i} < 10) {{")
    lines.append(f"        s = s + helper(i{i});")
    lines.append(f"        i{i} = i{i} + 1;")
    lines.append("    }")
    lines.append(f"    s = s + helper({i});")
lines.append("    return s - s;")
open("tests/cs_bigfn.mettle", "w").write("\n".join(lines) + "\n")
lines.append("}")
print("bigfn:", len(lines))
