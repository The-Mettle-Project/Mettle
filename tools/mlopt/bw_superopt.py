#!/usr/bin/env python3
# Bitwise superoptimizer. Enumerate &|^~ exprs, key by exact truth-table
# fingerprint (leaves set to the 2^k column constants), keep cheapest per fp ->
# bw_lib.txt: "k fp postfix" (RPN over L0..L5, Z=0, O=~0, ~ & | ^).
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MASK = (1 << 64) - 1

def col(j, k):
    v = 0
    for r in range(1 << k):
        if (r >> j) & 1:
            v |= (1 << r)
    return v

def fpmask(k):
    return (1 << (1 << k)) - 1

class E:
    __slots__ = ("tok", "a", "b", "cost", "fp", "post")

    def __init__(self, tok, a=None, b=None, cost=0, fp=0, post=""):
        self.tok, self.a, self.b, self.cost, self.fp, self.post = tok, a, b, cost, fp, post

def build_k(k, max_cost):
    m = fpmask(k)
    base = []
    for j in range(k):
        base.append(E(f"L{j}", cost=0, fp=col(j, k) & m, post=f"L{j}"))
    base.append(E("Z", cost=0, fp=0, post="Z"))
    base.append(E("O", cost=0, fp=MASK & m, post="O"))
    best = {}
    for e in base:
        if e.fp not in best or e.cost < best[e.fp].cost:
            best[e.fp] = e
    by_cost = {0: list(best.values())}
    for c in range(1, max_cost + 1):
        cur = []
        alle = [e for cc in by_cost for e in by_cost[cc]]
        for e in alle:
            if e.cost + 1 == c:
                fp = (~e.fp) & m
                cur.append(E("~", e, cost=c, fp=fp, post=e.post + "~"))
        for ca in range(c):
            cb = c - 1 - ca
            if cb < 0 or cb > ca:
                continue
            for ea in by_cost.get(ca, []):
                for eb in by_cost.get(cb, []):
                    for op in ("&", "|", "^"):
                        fp = {"&": ea.fp & eb.fp, "|": ea.fp | eb.fp,
                              "^": ea.fp ^ eb.fp}[op] & m
                        cur.append(E(op, ea, eb, cost=c, fp=fp,
                                     post=ea.post + eb.post + op))
        promoted = []
        for e in cur:
            if e.fp not in best or e.cost < best[e.fp].cost:
                best[e.fp] = e
                promoted.append(e)
        by_cost[c] = promoted
    return best

def main():
    max_cost = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    out = open(os.path.join(HERE, "bw_lib.txt"), "w", encoding="utf-8")
    total = 0
    for k in range(1, 5):
        best = build_k(k, max_cost)
        for fp, e in best.items():
            out.write(f"{k} {fp} {e.post}\n")
            total += 1
        cov = len(best)
        print(f"k={k}: {cov} distinct functions covered (of {1 << (1 << k)} possible), "
              f"max_cost={max_cost}")
    out.close()
    print(f"wrote bw_lib.txt: {total} (k,fp)->optimal entries")

if __name__ == "__main__":
    main()
