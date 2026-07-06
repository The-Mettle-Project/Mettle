#!/usr/bin/env python3
"""Generate keep/delete labels for the Stage-1 verified-action model.

For each mined program we work on the canonical INPUT instruction list (the
irexec-faithful form) and greedily find instructions that can be removed without
changing the program's result: a reverse pass tries deleting each instruction,
re-runs irexec, and commits the deletion iff the result is unchanged. The
resulting per-instruction delete mask is a sound dead/redundant-code labeling
(every committed deletion is individually verified equivalence-preserving).

The model trained on these never hallucinates: its output is a subsequence of
the input's own instructions, so constants and ops are copied exactly. Output
record: {seed, exit_code, funcs:[{name, instrs:[...], delete:[0/1,...]}]}.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import canonicalize as C  # noqa: E402
import irexec as IR  # noqa: E402


def irexec(funcs):
    # Tight step cap: legit tiny programs run well under it; a deletion that
    # induces an infinite loop fails fast (None) instead of burning 5M steps.
    return IR.run_text(C.to_dump(funcs), max_steps=50000)


def label_program(input_ir, exit_code):
    funcs = C.canonical_program(input_ir)
    if irexec(funcs) != exit_code:
        return None  # canonical input must reproduce the known answer
    # work on a mutable copy; track deletions against original indices
    orig = {n: list(b) for n, b in funcs.items()}
    cur = {n: list(b) for n, b in funcs.items()}
    deleted = {n: [False] * len(b) for n, b in orig.items()}

    def render():
        return {n: [orig[n][i] for i in range(len(orig[n]))
                    if not deleted[n][i]] for n in orig}

    for name in orig:
        for i in range(len(orig[name]) - 1, -1, -1):
            deleted[name][i] = True
            if irexec(render()) != exit_code:
                deleted[name][i] = False  # needed; keep it
    return dict(funcs=[dict(name=n, instrs=orig[n],
                            delete=[int(x) for x in deleted[n]])
                       for n in orig])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pairs", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--count", type=int, default=10**9)
    args = ap.parse_args()

    rows = [json.loads(l) for l in open(args.pairs, encoding="utf-8")]
    rows = rows[args.start:args.start + args.count]
    out_f = open(args.out, "w", encoding="utf-8")
    n = ok = tot_instr = tot_del = 0
    for r in rows:
        n += 1
        lab = label_program(r["input_ir"], r["exit_code"])
        if lab is None:
            continue
        ok += 1
        for fn in lab["funcs"]:
            tot_instr += len(fn["instrs"])
            tot_del += sum(fn["delete"])
        out_f.write(json.dumps(dict(seed=r["seed"], exit_code=r["exit_code"],
                                    **lab)) + "\n")
    out_f.close()
    frac = tot_del / max(1, tot_instr)
    print(json.dumps(dict(n=n, labeled=ok, instrs=tot_instr, deletable=tot_del,
                          deletable_frac=round(frac, 3))))


if __name__ == "__main__":
    main()
