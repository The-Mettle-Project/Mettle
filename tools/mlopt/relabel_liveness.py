#!/usr/bin/env python3
"""Replace the (unsound, sampling-based) delete masks with SOUND liveness labels.

Reads function label files, recomputes each delete mask via def-use liveness
(provably dead instructions only), and writes new label files. Pure static
analysis, no irexec sampling, so the labels are correct for all inputs.
"""
import argparse
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import liveness as L  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", nargs="+", required=True)
    ap.add_argument("--suffix", default="L")
    args = ap.parse_args()

    tot_instr = tot_dead = nfn = 0
    for pat in args.inp:
        for path in glob.glob(pat):
            out = path.replace(".jsonl", f"_{args.suffix}.jsonl")
            with open(out, "w", encoding="utf-8") as w:
                for line in open(path, encoding="utf-8"):
                    r = json.loads(line)
                    fn = r["funcs"][0]
                    params = set(r["params"])
                    dead = L.dead_indices(fn["instrs"], params)
                    fn["delete"] = [1 if i in dead else 0
                                    for i in range(len(fn["instrs"]))]
                    tot_instr += len(fn["instrs"])
                    tot_dead += len(dead)
                    nfn += 1
                    w.write(json.dumps(r) + "\n")
            print(f"{path} -> {out}")
    print(json.dumps(dict(funcs=nfn, instrs=tot_instr, dead=tot_dead,
                          dead_frac=round(tot_dead / max(1, tot_instr), 3))))


if __name__ == "__main__":
    main()
