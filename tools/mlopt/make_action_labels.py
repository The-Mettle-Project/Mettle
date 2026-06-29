#!/usr/bin/env python3
"""Derive per-instruction ACTION labels (richer modality than keep/delete) from
the sound transforms, for training the rewrite-action model.

Action classes (per instruction of the classical --release output):
  0 KEEP    - unchanged
  1 DELETE  - provably dead (def-use liveness)
  2 FOLD    - RHS is two-constant foldable -> a constant
  3 AFFINE  - collapses via linear-form cancellation ((x+y)-y, 2x-x, ...)

Priority DELETE > AFFINE > FOLD > KEEP (no point folding a dead value). All
sources are sound static analyses, so the labels are correct by construction.
"""
import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import liveness as L  # noqa: E402
import affine  # noqa: E402
import gvn as G  # noqa: E402
from sopt import split_def, is_lit, _BIN  # noqa: E402

KEEP, DELETE, FOLD, AFFINE, GVN = 0, 1, 2, 3, 4
NAMES = ["KEEP", "DELETE", "FOLD", "AFFINE", "GVN"]


def foldable(ins):
    d = split_def(ins)
    if not d:
        return False
    m = _BIN.match(d[2]) or re.match(r"^(\S+) (==|!=|<|<=|>|>=) (\S+)$", d[2])
    return bool(m and is_lit(m.group(1)) and is_lit(m.group(3)))


def action_labels(body, params):
    dead = L.dead_indices(body, set(params))
    affined, _ = affine.simplify(body, params)   # sound; per-index rewrite
    gvned, _ = G.gvn(body, params)               # sound global redundancy
    acts = []
    for i, ins in enumerate(body):
        if i in dead:
            acts.append(DELETE)
        elif affined[i] != ins:
            acts.append(AFFINE)
        elif gvned[i] != ins:
            acts.append(GVN)
        elif foldable(ins):
            acts.append(FOLD)
        else:
            acts.append(KEEP)
    return acts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", nargs="+", required=True)
    ap.add_argument("--suffix", default="act")
    args = ap.parse_args()
    counts = [0] * len(NAMES)
    nfn = 0
    for pat in args.inp:
        for path in glob.glob(pat):
            out = path.replace(".jsonl", f"_{args.suffix}.jsonl")
            with open(out, "w", encoding="utf-8") as w:
                for line in open(path, encoding="utf-8"):
                    r = json.loads(line)
                    fn = r["funcs"][0]
                    acts = action_labels(fn["instrs"], r["params"])
                    fn["action"] = acts
                    for a in acts:
                        counts[a] += 1
                    nfn += 1
                    w.write(json.dumps(r) + "\n")
            print(f"{path} -> {out}")
    tot = sum(counts)
    print(json.dumps({NAMES[i]: f"{counts[i]} ({100*counts[i]//max(1,tot)}%)"
                      for i in range(len(NAMES))} | {"funcs": nfn}))


if __name__ == "__main__":
    main()
