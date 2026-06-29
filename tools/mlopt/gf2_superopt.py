#!/usr/bin/env python3
# GF(2)-affine superoptimizer (handles shifts, unlike the bitwise truth-table).
# {^,~,<<c,>>c} exprs are affine over GF(2): f(x)=M.x^b, exact via 64k basis evals.
# Enumerate, key by (b, columns), keep cheapest -> gf2_lib.txt:
# "k b col0..col_{64k-1} post" (RPN over L0.., Z, O, ~, ^, <a=shl, >a=shr).
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MASK = (1 << 64) - 1
SHIFTS = [1, 2, 3, 4, 5, 6, 7, 8, 11, 13, 16, 17, 23, 32]

class E:
    __slots__ = ("post", "cost", "key")

    def __init__(self, post, cost, key):
        self.post, self.cost, self.key = post, cost, key

def signature(evalfn, k):
    b = evalfn([0] * k)
    cols = []
    for i in range(k):
        for p in range(64):
            x = [0] * k
            x[i] = 1 << p
            cols.append(evalfn(x) ^ b)
    return (b, tuple(cols))

def mk_eval(post):
    def ev(x):
        st = []
        i = 0
        while i < len(post):
            t = post[i]
            if t[0] == "L":
                st.append(x[int(t[1:])])
            elif t == "Z":
                st.append(0)
            elif t == "O":
                st.append(MASK)
            elif t == "~":
                st.append((~st.pop()) & MASK)
            elif t[0] == "<":
                st.append((st.pop() << int(t[1:])) & MASK)
            elif t[0] == ">":
                st.append(st.pop() >> int(t[1:]))
            elif t == "^":
                b = st.pop(); a = st.pop(); st.append(a ^ b)
            i += 1
        return st[0]
    return ev

def build(k, max_cost):
    base = [E([f"L{j}"], 0, None) for j in range(k)] + [E(["Z"], 0, None), E(["O"], 0, None)]
    best = {}
    for e in base:
        e.key = signature(mk_eval(e.post), k)
        if e.key not in best or e.cost < best[e.key].cost:
            best[e.key] = e
    by_cost = {0: list({x.key: x for x in base}.values())}
    for c in range(1, max_cost + 1):
        cur = {}

        def add(post):
            e = E(post, c, None)
            e.key = signature(mk_eval(post), k)
            if e.key not in best or c < best[e.key].cost:
                best[e.key] = e; cur[e.key] = e

        for e in [x for cc in by_cost for x in by_cost[cc]]:
            if e.cost + 1 == c:
                add(e.post + ["~"])
                for s in SHIFTS:
                    add(e.post + [f"<{s}"])
                    add(e.post + [f">{s}"])
        for ca in range(c):
            cb = c - 1 - ca
            if cb < 0 or cb > ca:
                continue
            for ea in by_cost.get(ca, []):
                for eb in by_cost.get(cb, []):
                    add(ea.post + eb.post + ["^"])
        by_cost[c] = list(cur.values())
    return best

def main():
    max_cost = int(sys.argv[1]) if len(sys.argv) > 1 else 4
    out = open(os.path.join(HERE, "gf2_lib.txt"), "w", encoding="utf-8")
    total = 0
    for k in (1, 2):
        best = build(k, max_cost)
        for (b, cols), e in best.items():
            out.write(f"{k} {b} {' '.join(str(c) for c in cols)} {''.join(e.post)}\n")
            total += 1
        print(f"k={k}: {len(best)} distinct GF(2)-affine functions, max_cost={max_cost}")
    out.close()
    print(f"wrote gf2_lib.txt: {total} entries")

if __name__ == "__main__":
    main()
