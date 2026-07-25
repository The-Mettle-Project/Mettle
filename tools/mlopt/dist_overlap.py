#!/usr/bin/env python3
"""How far outside the training distribution is a codebase, really?

The held-out split in `bakeoff.py` separates function BODIES by instruction
shape. That stops the same function appearing on both sides, and it does not stop
the model learning one project's idioms and being scored on the same project's
idioms. Warband exposed the difference: a delete-precision result that looked
like a twentyfold win on the toolchain's own corpus did not survive contact with
a codebase from outside it.

So a held-out number needs a companion number: how much of the evaluation set the
training set actually covers. This reports, for the functions of a compiled
program, what fraction share a shape key with the training corpus and how similar
the rest are.

    python dist_overlap.py --ir <_mlopt.ir dump> --train _bakeoff/oracle_real_train.jsonl
"""
import argparse
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import harvest as H  # noqa: E402

_NAME = re.compile(r"[%@][A-Za-z0-9_.$]*")
_NUM = re.compile(r"(?<![\w%@])-?\d+")


def shapes(instrs):
    return {re.sub(r"\s+", " ", _NUM.sub("K", _NAME.sub("N", i))).strip()
            for i in instrs}


def shape_key(instrs):
    return hashlib.sha1("\n".join(sorted(shapes(instrs))).encode()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ir", required=True, help="an _mlopt.ir dump")
    ap.add_argument("--train", required=True, nargs="+")
    args = ap.parse_args()

    tkeys, tshapes = set(), []
    for p in args.train:
        for line in open(p, encoding="utf-8"):
            try:
                body = json.loads(line)["funcs"][0]["instrs"]
            except (json.JSONDecodeError, KeyError, IndexError):
                continue
            tkeys.add(shape_key(body))
            tshapes.append(shapes(body))

    ir = open(args.ir, encoding="utf-8", errors="replace").read()
    funcs = [(fn, [t for _, t in pairs])
             for fn, pairs in H.parse_ir_with_gidx(ir).items() if len(pairs) >= 3]

    exact = 0
    sims = []
    for _, body in funcs:
        if shape_key(body) in tkeys:
            exact += 1
        s = shapes(body)
        best = 0.0
        for ts in tshapes:
            inter = len(s & ts)
            if not inter:
                continue
            j = inter / len(s | ts)
            if j > best:
                best = j
        sims.append(best)

    sims.sort()
    n = len(sims)

    def pct(p):
        return sims[min(n - 1, int(n * p))] if n else 0.0

    print(f"evaluation program: {n} functions in the post-classical IR")
    print(f"training corpus   : {len(tkeys)} distinct shape keys")
    print(f"  exact shape-key match in training : {exact}/{n} "
          f"({100*exact/max(1,n):.1f}%)")
    print(f"  max shape similarity to training  : median {pct(.5):.3f}  "
          f"p90 {pct(.9):.3f}  max {sims[-1] if n else 0:.3f}")
    for th in (0.9, 0.7, 0.5):
        k = sum(1 for s in sims if s >= th)
        print(f"    >= {th:.1f}: {k}/{n} ({100*k/max(1,n):.1f}%)")


if __name__ == "__main__":
    sys.exit(main())
