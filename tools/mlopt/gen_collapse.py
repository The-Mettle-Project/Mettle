"""Generate a dataset teaching the model to RECOGNIZE collapsible expressions —
tangled multi-op expressions that are secretly a simple value (the class GCC
misses). Positives: random cost-4-7 expressions whose value equals a simple
(cost<=2) form; the root instruction is labelled COLLAPSE. Negatives: random
expressions that are already near-optimal (no simpler equivalent); labelled KEEP.

The model must learn the SEMANTIC pattern (this graph computes something trivial)
from structure alone — true intellect, not syntactic matching. The verifier
backstops every collapse, so the model only needs to point; soundness is separate.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import super_diff as S
import super_hunt as H

KEEP, COLLAPSE = 0, 1


def render(e):
    body, ctr = [], [0]

    def go(n):
        if n.op == "var":
            return "@" + n.a
        if n.op == "~":
            a = go(n.a); t = f"%t{ctr[0]}"; ctr[0] += 1
            body.append(f"{t} = {a} ^ -1"); return t
        if n.op in ("<<", ">>"):
            a = go(n.a); t = f"%t{ctr[0]}"; ctr[0] += 1
            body.append(f"{t} = {a} {n.op} {n.s}"); return t
        a = go(n.a); b = go(n.b); t = f"%t{ctr[0]}"; ctr[0] += 1
        body.append(f"{t} = {a} {n.op} {b}"); return t

    root = go(e)
    return body, root


def main():
    import random
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 6000
    H.RNG = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 20)
    out = open(os.path.join(HERE, sys.argv[2] if len(sys.argv) > 2
                            else "collapse.jsonl"), "w", encoding="utf-8")
    lib = {}
    for e in S.enumerate_exprs(3):
        f = S.fp(e)
        if f not in lib or e.cost < lib[f].cost:
            lib[f] = e
    npos = nneg = sid = 0
    tries = 0
    while (npos < n // 2 or nneg < n // 2) and tries < n * 60:
        tries += 1
        e = H.rand_expr(H.RNG.randint(4, 7))
        if e.cost < 4:
            continue
        opt = lib.get(S.fp(e))
        if opt is None:
            continue
        body, root = render(e)
        if len(body) < 4:
            continue
        params = sorted({"@" + t for ins in body
                         for t in __import__("re").findall(r"@(\w+)", ins)})
        if not params:
            continue
        collaps = opt.cost <= 2 and e.cost - opt.cost >= 2
        if collaps and npos >= n // 2:
            continue
        if not collaps and nneg >= n // 2:
            continue
        # action: root COLLAPSE iff collapsible, else all KEEP
        acts = [KEEP] * len(body)
        if collaps:
            acts[-1] = COLLAPSE; npos += 1
        else:
            nneg += 1
        instrs = body + [f"return {root}"]
        acts = acts + [KEEP]
        out.write(json.dumps({"seed": sid, "params": params,
                              "funcs": [{"instrs": instrs, "action": acts}],
                              "simple": opt.txt if collaps else None}) + "\n")
        sid += 1
    out.close()
    print(f"wrote {sid} records: {npos} collapsible / {nneg} not")


if __name__ == "__main__":
    main()
