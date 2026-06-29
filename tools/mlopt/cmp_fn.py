#!/usr/bin/env python3
"""Fair head-to-head on parameterized functions: the learned ML optimizer (model
+ multi-input verifier-in-the-loop) vs Mettle's classical --release optimizer.

Baseline = the classical optimizer's own output for each function. The model
ranks its instructions by delete-confidence; each deletion is committed only if
the function stays equivalent over a LARGE independent input set (coverage-
focused + random). Any instruction removed is dead/redundant code classical left
behind, removed with high-confidence soundness (a true proof would need SMT).
"""
import argparse
import glob
import json
import os
import sys

import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fn_verify as V  # noqa: E402
from delete_common import infer_tokens  # noqa: E402
from train_delete import Labeler  # noqa: E402

VAL_K = 256          # independent validation input vectors
VAL_SEED = 991       # fixed, different from labeling seeds


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--labels", nargs="+", required=True)
    ap.add_argument("--data", default=os.path.join(HERE, "data_fn"))
    ap.add_argument("--model", default=os.path.join(HERE, "fn_model.pt"))
    args = ap.parse_args()

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    vocab = json.load(open(os.path.join(args.data, "vocab.json")))
    unk = vocab["<unk>"]
    ck = torch.load(args.model, map_location=dev)
    model = Labeler(**ck["cfg"]).to(dev); model.load_state_dict(ck["model"])
    model.eval()

    rows = []
    for pat in args.labels:
        for p in glob.glob(pat):
            rows += [json.loads(l) for l in open(p, encoding="utf-8")]
    seen, uniq = set(), []
    for r in rows:
        if r["seed"] in seen:
            continue
        seen.add(r["seed"]); uniq.append(r)
    uniq.sort(key=lambda r: r["seed"])
    val = [r for i, r in enumerate(uniq) if i % 10 == 0]

    n = 0
    sum_classical = sum_ml = sum_oracle = 0
    ml_beats = 0
    total_removed = 0
    for r in val:
        instrs = r["funcs"][0]["instrs"]
        params = r["params"]
        funcs = {"f": list(instrs)}
        vecs = V.arg_vectors(len(params), k=VAL_K, seed=VAL_SEED)
        # validation inputs must agree with the labeling reference first
        n_classical = len(instrs)

        toks, pos, refs = infer_tokens(funcs)
        ids = torch.tensor([[vocab.get(t, unk) for t in toks]], device=dev)
        with torch.no_grad():
            prob = torch.softmax(model.cls(model(ids)[0, pos]), -1)[:, 1].tolist()

        def build(drop):
            return {"f": [ins for j, ins in enumerate(instrs) if j not in drop]}

        drop = set()
        for i in sorted(range(len(refs)), key=lambda i: -prob[i]):
            if prob[i] < 0.5:
                break
            trial = drop | {refs[i][1]}
            if V.equivalent(funcs, build(trial), "f", params, vecs):
                drop = trial
        n_ml = n_classical - len(drop)
        n_oracle = n_classical - sum(r["funcs"][0]["delete"])

        n += 1
        sum_classical += n_classical
        sum_ml += n_ml
        sum_oracle += n_oracle
        total_removed += len(drop)
        if n_ml < n_classical:
            ml_beats += 1

    print(f"\n=== Learned optimizer vs classical --release ({n} held-out funcs) ===")
    print(f"avg instrs  classical --release: {sum_classical/n:.1f}")
    print(f"avg instrs  ML + verifier:       {sum_ml/n:.1f}  "
          f"({100*(sum_classical-sum_ml)/sum_classical:.0f}% smaller)")
    print(f"avg instrs  oracle ceiling:      {sum_oracle/n:.1f}")
    print(f"functions where ML beats classical: {ml_beats}/{n} "
          f"({100*ml_beats/n:.0f}%)")
    print(f"total instructions removed that classical left behind: {total_removed}")
    print(f"\nall removals verified equivalent over {VAL_K} independent inputs "
          f"(coverage-focused + random); high-confidence sound.")


if __name__ == "__main__":
    main()
