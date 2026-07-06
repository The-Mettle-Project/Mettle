"""Differential superoptimizer vs GCC -O3 — find expression rewrites GCC misses.

Enumerate small expressions over 2 vars (x,y) with ops {+,-,*,&,|,^,~,<<c,>>c},
fingerprint each over many random 64-bit inputs, bucket by fingerprint (semantic
equivalence, high-confidence). For each bucket the min-cost expression is the
'optimal'. A (costly form -> optimal) pair is a candidate rewrite. We then ask
GCC -O3 to compile the costly form and see whether it reaches the optimal cost.
The ones GCC does NOT reduce are the frontier: optimizations GCC's fixed pipeline
leaves behind, which a learned model could propose (verifier-checked).
"""
import itertools
import os
import random
import subprocess
import sys
import tempfile

MASK = (1 << 64) - 1
RNG = random.Random(7)
INPUTS = [(RNG.randint(0, MASK), RNG.randint(0, MASK)) for _ in range(64)]
# include small/structured inputs too (catch shift/compare corner cases)
INPUTS += [(a, b) for a in (0, 1, 2, 3, 255, 1 << 63, MASK) for b in (0, 1, 7, MASK)]

COST = {"+": 1, "-": 1, "&": 1, "|": 1, "^": 1, "<<": 1, ">>": 1, "*": 3, "~": 1}


class E:
    __slots__ = ("op", "a", "b", "s", "cost", "txt")

    def __init__(self, op, a=None, b=None, s=None):
        self.op, self.a, self.b, self.s = op, a, b, s
        if op == "var":
            self.cost, self.txt = 0, a
        elif op == "~":
            self.cost, self.txt = a.cost + 1, f"(~{a.txt})"
        elif op in ("<<", ">>"):
            self.cost, self.txt = a.cost + 1, f"({a.txt}{op}{s})"
        else:
            self.cost = a.cost + b.cost + COST[op]
            self.txt = f"({a.txt}{op}{b.txt})"


def ev(e, x, y):
    if e.op == "var":
        return x if e.a == "x" else y
    if e.op == "~":
        return (~ev(e.a, x, y)) & MASK
    if e.op == "<<":
        return (ev(e.a, x, y) << e.s) & MASK
    if e.op == ">>":
        return ev(e.a, x, y) >> e.s
    a, b = ev(e.a, x, y), ev(e.b, x, y)
    if e.op == "+": return (a + b) & MASK
    if e.op == "-": return (a - b) & MASK
    if e.op == "*": return (a * b) & MASK
    if e.op == "&": return a & b
    if e.op == "|": return a | b
    if e.op == "^": return a ^ b


def fp(e):
    return tuple(ev(e, x, y) for x, y in INPUTS)


def enumerate_exprs(max_cost):
    base = [E("var", "x"), E("var", "y")]
    by_cost = {0: list(base)}
    alle = list(base)
    for c in range(1, max_cost + 1):
        cur = []
        # unary
        for e in alle:
            if e.cost + 1 <= c:
                if e.cost + 1 == c:
                    cur.append(E("~", e))
                    for s in (1, 2, 4, 8, 16, 31, 32, 63):
                        cur.append(E("<<", e, s=s))
                        cur.append(E(">>", e, s=s))
        # binary
        for ca in range(c):
            for cb in range(c):
                for op, oc in COST.items():
                    if op in ("~", "<<", ">>"):
                        continue
                    if ca + cb + oc != c:
                        continue
                    for ea in by_cost.get(ca, []):
                        for eb in by_cost.get(cb, []):
                            cur.append(E(op, ea, eb))
        by_cost[c] = cur
        alle += cur
    return alle


def main():
    max_cost = int(sys.argv[1]) if len(sys.argv) > 1 else 4
    exprs = enumerate_exprs(max_cost)
    buckets = {}
    for e in exprs:
        buckets.setdefault(fp(e), []).append(e)
    rewrites = []
    for f, es in buckets.items():
        es.sort(key=lambda e: e.cost)
        best = es[0]
        for e in es[1:]:
            if e.cost - best.cost >= 1 and e.cost <= max_cost:
                rewrites.append((e, best))
    # keep the most interesting: biggest cost drop, dedup by costly txt
    seen = set()
    uniq = []
    for costly, opt in sorted(rewrites, key=lambda r: r[0].cost - r[1].cost,
                              reverse=True):
        if costly.txt in seen:
            continue
        seen.add(costly.txt)
        uniq.append((costly, opt))
    print(f"enumerated {len(exprs)} exprs, {len(uniq)} candidate rewrites "
          f"(cost<= {max_cost})")
    # differential vs GCC -O3
    misses = check_gcc(uniq[:400])
    print(f"\n=== GCC -O3 MISSES: {len(misses)} rewrites GCC did not reach ===")
    for costly, opt, gcc_ops, opt_ops in misses[:25]:
        print(f"  {costly.txt}  ->  {opt.txt}   "
              f"(model cost {costly.cost}->{opt.cost}; gcc emits {gcc_ops} alu, "
              f"optimal ~{opt_ops})")


def check_gcc(cands):
    """Compile each costly form; count real ALU/mul instrs GCC emits; compare to
    the optimal form's instruction count. GCC 'misses' if it emits more."""
    src = ['#include <stdint.h>']
    names = []
    for i, (costly, opt) in enumerate(cands):
        for tag, e in (("c", costly), ("o", opt)):
            nm = f"f{i}{tag}"
            names.append(nm)
            src.append(f"uint64_t {nm}(uint64_t x,uint64_t y){{ return {e.txt}; }}")
    d = tempfile.mkdtemp()
    cpath = os.path.join(d, "s.c")
    open(cpath, "w").write("\n".join(src))
    wp = "/mnt/" + cpath[0].lower() + cpath[2:].replace("\\", "/")
    sp = wp[:-2] + ".s"
    subprocess.run(["wsl.exe", "bash", "-lc",
                    f"gcc -O3 -S -fno-asynchronous-unwind-tables "
                    f"-fno-stack-protector -masm=intel {wp} -o {sp}"],
                   capture_output=True, text=True)
    asm = subprocess.run(["wsl.exe", "bash", "-lc", f"cat {sp}"],
                         capture_output=True, text=True).stdout
    counts = _count_funcs(asm)
    misses = []
    for i, (costly, opt) in enumerate(cands):
        gc = counts.get(f"f{i}c"); go = counts.get(f"f{i}o")
        if gc is None or go is None:
            continue
        if gc > go:                    # GCC left the costly form bigger than optimal
            misses.append((costly, opt, gc, go))
    misses.sort(key=lambda m: m[2] - m[3], reverse=True)
    return misses


_ALU = ("mov", "lea", "add", "sub", "and", "or", "xor", "sal", "sar", "shl",
        "shr", "imul", "mul", "not", "neg", "rol", "ror", "movabs")


def _count_funcs(asm):
    counts = {}
    cur = None
    for line in asm.splitlines():
        line = line.strip()
        if line.endswith(":") and line[:-1].replace("_", "").isalnum() \
                and (line.startswith("f") and line[1].isdigit()):
            cur = line[:-1]
            counts[cur] = 0
        elif cur and line and not line.startswith("."):
            mn = line.split()[0]
            if mn in _ALU:
                counts[cur] += 1
            elif mn == "ret":
                cur = None
    return counts


if __name__ == "__main__":
    main()
