#!/usr/bin/env python3
"""Generate a large (~200k LOC) Mettle program to profile compiler phase timings.

Produces varied, valid Mettle: structs, forward-declared functions, arithmetic
chains, loops, conditionals, and struct field access -- so every compiler phase
(parse, typecheck, opt, codegen) gets real work, not one giant trivial function.

Usage: python gen_profiler_test.py [target_loc] [out.mettle]
"""
import sys

target_loc = int(sys.argv[1]) if len(sys.argv) > 1 else 200_000
out_path = sys.argv[2] if len(sys.argv) > 2 else "tests/profiler_test.mettle"

lines = []
emit = lines.append

# A handful of struct types so typecheck has real type tables to walk.
NUM_STRUCTS = 16
for s in range(NUM_STRUCTS):
    emit(f"struct S{s} {{")
    emit("  a: int64;")
    emit("  b: int64;")
    emit("  c: int64;")
    emit("}")
    emit("")

# Forward declarations (exercise forward-decl resolution at scale).
# We size the function count to hit the target LOC; each function is ~14 lines.
LINES_PER_FN = 14
header_lines = len(lines)
# Each function costs ~LINES_PER_FN (body) + 1 (forward decl). main() adds
# ~one call line per (step) functions; we cap main at ~2000 lines, so reserve that.
MAIN_RESERVE = 2100
num_fns = max(1, (target_loc - header_lines - MAIN_RESERVE) // (LINES_PER_FN + 1))

for i in range(num_fns):
    emit(f"fn compute{i}(x: int64, y: int64) -> int64;")
emit("")

# Function bodies: each does loops, conditionals, struct use, and calls a
# previously-defined function so the call graph is non-trivial.
for i in range(num_fns):
    si = i % NUM_STRUCTS
    callee = i // 2  # creates a real (acyclic) call graph
    emit(f"fn compute{i}(x: int64, y: int64) -> int64 {{")
    emit(f"  var v: S{si};")
    emit("  v.a = x + y;")
    emit(f"  v.b = x * (int64){(i % 7) + 1};")
    emit("  v.c = (int64)0;")
    emit("  var acc: int64 = (int64)0;")
    emit(f"  for (var k: int64 = (int64)0; k < (int64){(i % 5) + 3}; k = k + (int64)1) {{")
    emit("    if (k > v.a) {")
    emit("      acc = acc + v.b - k;")
    emit("    } else {")
    emit(f"      acc = acc + compute{callee}(k, v.a) % (int64){(i % 13) + 2};")
    emit("    }")
    emit("  }")
    emit("  return acc + v.a + v.b + v.c;")
    emit("}")
    emit("")

# main() drives a subset of the functions and returns a checksum (mod 256).
emit("fn main() -> int32 {")
emit("  var total: int64 = (int64)0;")
step = max(1, num_fns // 2000)  # ~2000 call sites in main
for i in range(0, num_fns, step):
    emit(f"  total = total + compute{i}((int64){i % 97}, (int64){i % 53});")
emit("  return (int32)(total % (int64)256);")
emit("}")

with open(out_path, "w") as f:
    f.write("\n".join(lines) + "\n")

print(f"wrote {out_path}: {len(lines)} lines, {num_fns} functions, {NUM_STRUCTS} structs")
