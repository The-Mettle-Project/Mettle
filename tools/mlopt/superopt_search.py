#!/usr/bin/env python3
"""Enumerative superoptimizer — finds 'black magic' equivalences by SEARCH, not
rules.

Given a target function over its params, it searches the space of expression DAGs
for the cheapest one that computes the same function, under a cost model where
multiply/divide are expensive. It works in value space: each candidate value is
fingerprinted by its outputs on a fixed input set; programs are dedup'd by
fingerprint keeping the cheapest, and expanded cheapest-first (Dijkstra over
cost). This rediscovers non-obvious rewrites no peephole encodes —
`x*15 → (x<<4)-x`, `x*7 → (x<<3)-x`, constant-divide → multiply-shift, etc.

A fingerprint match over the input set is necessary, not sufficient; the winner
is re-verified over a large fresh input set (high-confidence; a proof needs SMT).
"""
import ast
import heapq
import random

MASK = (1 << 40) - 1


def masked_eval(expr, env):
    """Evaluate a fully-parenthesized expr with per-operation 40-bit masking,
    matching the search's (and irexec's) semantics exactly."""
    node = ast.parse(expr, mode="eval").body

    def ev(n):
        if isinstance(n, ast.BinOp):
            a, b = ev(n.left), ev(n.right)
            op = n.op
            if isinstance(op, ast.Add): r = a + b
            elif isinstance(op, ast.Sub): r = a - b
            elif isinstance(op, ast.Mult): r = a * b
            elif isinstance(op, ast.BitAnd): r = a & b
            elif isinstance(op, ast.BitOr): r = a | b
            elif isinstance(op, ast.BitXor): r = a ^ b
            elif isinstance(op, ast.LShift): r = a << (b & 63)
            elif isinstance(op, ast.RShift): r = a >> (b & 63)
            else: raise ValueError(op)
            return r & MASK
        if isinstance(n, ast.Constant):
            return n.value & MASK
        if isinstance(n, ast.Name):
            return env[n.id] & MASK
        raise ValueError(ast.dump(n))
    return ev(node)

# cost model: cheap ALU ops vs expensive multiply/divide (latency-ish)
BIN_OPS = {
    "+": (1, lambda a, b: (a + b) & MASK),
    "-": (1, lambda a, b: (a - b) & MASK),
    "&": (1, lambda a, b: a & b),
    "|": (1, lambda a, b: a | b),
    "^": (1, lambda a, b: a ^ b),
    "*": (4, lambda a, b: (a * b) & MASK),
}
SHIFTS = {"<<": (1, lambda a, k: (a << k) & MASK),
          ">>": (1, lambda a, k: a >> k)}


def fp(vals):
    return tuple(v & MASK for v in vals)


class _Found(Exception):
    pass


def superoptimize(target_fp, seeds, max_cost=4, max_shift=20, cap=6000):
    """Cost-layered BFS over expression DAGs (dedup by output fingerprint, keep
    cheapest). Returns (cost, expr) for the cheapest program matching target_fp,
    or None. Returns early as soon as the target is reached, so cheap rewrites
    are fast; a value cap bounds the (exponential) space — the place where a
    learned proposal policy would prune."""
    best = {}                                   # fp -> (cost, expr)
    by_cost = [[] for _ in range(max_cost + 1)]
    result = {}

    def add(f, cost, expr):
        if cost > max_cost:
            return
        if f not in best or cost < best[f][0]:
            best[f] = (cost, expr)
            by_cost[cost].append(f)
            if f == target_fp:
                result["hit"] = (cost, expr)
                raise _Found()

    try:
        for f, e, c in seeds:
            add(f, c, e)
        for ca in range(max_cost + 1):
            j = 0
            while j < len(by_cost[ca]):
                fa = by_cost[ca][j]; j += 1
                if best[fa][0] != ca:
                    continue
                ea = best[fa][1]
                if ca + 1 <= max_cost:
                    for sym, (_, fn) in SHIFTS.items():
                        for k in range(1, max_shift + 1):
                            add(fp([fn(x, k) for x in fa]), ca + 1,
                                f"({ea} {sym} {k})")
                for cb in range(max_cost - ca):          # need ca+cb+1 <= max_cost
                    for fb in by_cost[cb]:
                        if best[fb][0] != cb:
                            continue
                        eb = best[fb][1]
                        for sym, (oc, fn) in BIN_OPS.items():
                            if ca + cb + oc > max_cost:
                                continue
                            add(fp([fn(x, y) for x, y in zip(fa, fb)]),
                                ca + cb + oc, f"({ea} {sym} {eb})")
                if len(best) > cap:
                    break
    except _Found:
        return result["hit"]
    return best.get(target_fp)


def make_inputs(nparams, k=24, seed=0):
    rng = random.Random(seed)
    base = [0, 1, 2, 3, 5, 7, 11, 255, 1023, 1 << 20, (1 << 40) - 1]
    rows = [[b] * nparams for b in base]
    while len(rows) < k:
        rows.append([rng.randint(0, (1 << 40) - 1) for _ in range(nparams)])
    return rows


def demo(expr_fn, nparams, label, orig_cost, orig_expr, max_cost=6):
    inputs = make_inputs(nparams)
    pnames = [f"p{i}" for i in range(nparams)]
    seeds = []
    for i, nm in enumerate(pnames):
        seeds.append((fp([row[i] for row in inputs]), nm, 0))
    for c in range(0, 4):                          # tiny literal seeds
        seeds.append((fp([c] * len(inputs)), str(c), 0))
    target = fp([expr_fn(*row) & MASK for row in inputs])
    res = superoptimize(target, seeds, max_cost=max_cost)
    # verify winner over a large fresh input set
    ok = "n/a"
    if res:
        fresh = make_inputs(nparams, k=4000, seed=999)
        good = all((expr_fn(*row) & MASK) ==
                   masked_eval(res[1], {pn: row[i] for i, pn in enumerate(pnames)})
                   for row in fresh)
        ok = "VERIFIED" if good else "FALSE-MATCH"
    print(f"{label}")
    print(f"   baseline: {orig_expr}   (cost {orig_cost})")
    if res:
        print(f"   FOUND:    {res[1]}   (cost {res[0]})  [{ok}]  "
              f"{'<-- cheaper!' if res[0] < orig_cost else ''}")
    else:
        print(f"   (no equivalent within cost {max_cost})")
    print()


def main():
    # x*C: superoptimizer should find shift-add decompositions cheaper than mul
    demo(lambda p0: p0 * 15, 1, "p0 * 15", 4, "p0 * 15")
    demo(lambda p0: p0 * 7, 1, "p0 * 7", 4, "p0 * 7")
    demo(lambda p0: p0 * 100, 1, "p0 * 100", 4, "p0 * 100", max_cost=5)
    demo(lambda p0: (p0 * 2 + p0) & MASK, 1, "p0*2 + p0 (=p0*3)", 5, "(p0*2)+p0")
    demo(lambda p0, p1: ((p0 + p1) * (p0 + p1)) & MASK, 2,
         "(p0+p1)*(p0+p1)", 5, "(p0+p1)*(p0+p1)")


if __name__ == "__main__":
    main()
