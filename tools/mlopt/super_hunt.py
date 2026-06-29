"""Hunt for GCC -O3 misses at larger expression sizes: build the optimal-form
library exhaustively up to cost 3, then randomly sample BIGGER expressions
(cost 4-7) that are secretly equivalent to a simple (<=3) form, and check whether
GCC -O3 collapses them. Anything GCC leaves bigger than the known-simple form is a
genuine frontier rewrite a learned model could propose (verifier-checked)."""
import os
import random
import subprocess
import sys
import tempfile

import super_diff as S

RNG = random.Random(20)


def rand_expr(cost):
    """Build a random expression of approximately the given cost."""
    if cost <= 0:
        return S.E("var", RNG.choice(["x", "y"]))
    r = RNG.random()
    if r < 0.18:
        return S.E("~", rand_expr(cost - 1))
    if r < 0.34:
        op = RNG.choice(["<<", ">>"])
        return S.E(op, rand_expr(cost - 1), s=RNG.choice([1, 2, 3, 4, 8, 16, 31]))
    ops = [o for o in ("+", "-", "*", "&", "|", "^") if S.COST[o] <= cost]
    op = RNG.choice(ops)
    oc = S.COST[op]
    left = RNG.randint(0, cost - oc)
    return S.E(op, rand_expr(left), rand_expr(cost - oc - left))


def main():
    nsamp = int(sys.argv[1]) if len(sys.argv) > 1 else 4000
    lib = {}                                    # fp -> cheapest expr (cost<=3)
    for e in S.enumerate_exprs(3):
        f = S.fp(e)
        if f not in lib or e.cost < lib[f].cost:
            lib[f] = e
    print(f"optimal library: {len(lib)} semantic classes (cost<=3)")
    cands, seen = [], set()
    tries = 0
    while len(cands) < nsamp and tries < nsamp * 40:
        tries += 1
        c = RNG.randint(4, 7)
        e = rand_expr(c)
        if e.cost < 4:
            continue
        f = S.fp(e)
        opt = lib.get(f)
        if opt is not None and opt.cost + 1 <= e.cost and e.txt not in seen:
            seen.add(e.txt)
            cands.append((e, opt))
    print(f"sampled {len(cands)} big exprs equivalent to a simple (<=3) form")
    misses = S.check_gcc(cands)
    print(f"\n=== GCC -O3 MISSES: {len(misses)} ===")
    for costly, opt, gc, go in misses[:30]:
        print(f"  GCC emits {gc} alu for  {costly.txt}")
        print(f"     but it equals  {opt.txt}  (optimal {go} alu)")


if __name__ == "__main__":
    main()
