#!/usr/bin/env python3
"""Generate a large (~200k LOC) flat Mettle program for parse/typecheck stress.

Module-level globals only — two struct types, a few scalars, then ~200k
one-line arithmetic initializers. Matches the Haste profiler fixture pattern:
parse + typecheck volume without functions, control flow, or call graphs.

Usage: python gen_parse_stress_test.py [target_loc] [out.mettle]
"""
import sys

target_loc = int(sys.argv[1]) if len(sys.argv) > 1 else 200_000
out_path = sys.argv[2] if len(sys.argv) > 2 else "tests/parse_stress_test.mettle"

lines = []
emit = lines.append

emit("struct Vec2 {")
emit("  x: float64;")
emit("  y: float64;")
emit("}")
emit("")
emit("struct Point {")
emit("  x: float64;")
emit("  y: float64;")
emit("}")
emit("")
emit("var a: float64 = 7.0;")
emit("var b: float64 = 6.0;")
emit("var c: float64 = 7.0;")
emit("")

HEADER_LINES = len(lines)
# One global per line, same shape as Haste's const loop (i = 1 .. n-1).
num_globals = max(0, target_loc - HEADER_LINES - 4)  # reserve main() + trailing newline

for i in range(1, num_globals + 1):
    emit(f"var a{i}: float64 = {i} + a / b * c;")

emit("")
emit("fn main() -> int32 {")
emit("  return 0;")
emit("}")

with open(out_path, "w") as f:
    f.write("\n".join(lines) + "\n")

print(f"wrote {out_path}: {len(lines)} lines, {num_globals} globals")
