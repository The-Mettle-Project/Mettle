#!/usr/bin/env python3
"""Verified end-to-end eval of the Stage-1 keep/delete model.

For each held-out program: predict keep/delete per instruction, reconstruct the
kept subsequence (input instruction STRINGS, so values are exact), run irexec.
The verifier-gated system accepts the model's output iff it is equivalent AND
shorter, else falls back to the input (always correct). We report acceptance
(= verified wins) plus the oracle ceiling (dropping every truly-deletable instr).
"""
import argparse
import glob
import json
import os
import sys

import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import canonicalize as C  # noqa: E402
import irexec as IR  # noqa: E402
from delete_common import infer_tokens  # noqa: E402
from train_delete import Labeler  # noqa: E402


def irexec(funcs):
    return IR.run_text(C.to_dump(funcs), max_steps=50000)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--labels", nargs="+", required=True)
    ap.add_argument("--data", default=os.path.join(HERE, "data_del"))
    ap.add_argument("--model", default=os.path.join(HERE, "delete_model.pt"))
    args = ap.parse_args()

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    vocab = json.load(open(os.path.join(args.data, "vocab.json")))
    unk = vocab["<unk>"]
    ck = torch.load(args.model, map_location=dev)
    model = Labeler(**ck["cfg"]).to(dev)
    model.load_state_dict(ck["model"])
    model.eval()

    rows = []
    for pat in args.labels:
        for p in glob.glob(pat):
            rows += [json.loads(l) for l in open(p, encoding="utf-8")]
    seen, uniq = set(), []
    for r in rows:
        if r["seed"] in seen:
            continue
        seen.add(r["seed"])
        uniq.append(r)
    rows = sorted(uniq, key=lambda r: r["seed"])
    val = [r for i, r in enumerate(rows) if i % 10 == 0]

    thresholds = [0.5, 0.7, 0.9, 0.95, 0.99]
    # plain threshold sweep + a verifier-in-the-loop greedy variant
    wins = {t: 0 for t in thresholds}
    greedy_win = 0
    oracle_win = 0
    n = len(val)
    for r in val:
        funcs = {fn["name"]: list(fn["instrs"]) for fn in r["funcs"]}
        n_in = sum(len(b) for b in funcs.values())
        flat = [(name, j) for name in funcs for j in range(len(funcs[name]))]

        toks, pos, refs = infer_tokens(funcs)
        ids = torch.tensor([[vocab.get(t, unk) for t in toks]], device=dev)
        with torch.no_grad():
            h = model(ids)
            prob_del = torch.softmax(model.cls(h[0, pos]), -1)[:, 1].tolist()

        def build(drop):
            return {nm: [ins for j, ins in enumerate(b) if (nm, j) not in drop]
                    for nm, b in funcs.items()}

        # plain thresholds: delete where P(delete) > t
        for t in thresholds:
            drop = {refs[i] for i, p in enumerate(prob_del) if p > t}
            if not drop:
                continue
            kept = build(drop)
            if irexec(kept) == r["exit_code"] and \
               sum(len(b) for b in kept.values()) < n_in:
                wins[t] += 1

        # verifier-in-the-loop: try deletions in confidence order, committing
        # each only if it preserves the result. Always equivalence-safe.
        order = sorted(range(len(refs)), key=lambda i: -prob_del[i])
        drop = set()
        for i in order:
            if prob_del[i] < 0.5:
                break
            trial = drop | {refs[i]}
            if irexec(build(trial)) == r["exit_code"]:
                drop = trial
        if drop and irexec(build(drop)) == r["exit_code"]:
            greedy_win += 1

        ofuncs = {fn["name"]: [ins for ins, d in zip(fn["instrs"], fn["delete"])
                               if not d] for fn in r["funcs"]}
        if irexec(ofuncs) == r["exit_code"] and \
           sum(len(b) for b in ofuncs.values()) < n_in:
            oracle_win += 1

    print(f"\n=== Stage-1 keep/delete verified eval over {n} held-out programs ===")
    print("verified WIN rate (equivalent AND shorter), by delete threshold:")
    for t in thresholds:
        print(f"  P(delete) > {t:<4}: {wins[t]}/{n}  ({100*wins[t]/n:.0f}%)")
    print(f"verifier-in-the-loop greedy:   {greedy_win}/{n}  "
          f"({100*greedy_win/n:.0f}%)")
    print(f"oracle ceiling:                {oracle_win}/{n}  "
          f"({100*oracle_win/n:.0f}%)")
    print("\nverifier-gated system is correct by construction: accepted "
          "proposals are irexec-verified equivalent; all else falls back to input.")


if __name__ == "__main__":
    main()
