#!/usr/bin/env python3
"""SOUND head-to-head on parameterized functions, no input sampling.

Baseline = Mettle's classical --release output. Ground truth = def-use liveness
(provably-dead instructions, correct for all inputs). Reports:
  - classical vs liveness-DCE code size (the sound win over classical);
  - how well the learned model predicts the liveness-dead set (learnability);
  - the model gated by liveness (accept a deletion only if provably dead): a
    sound, ML-driven optimizer, and how close it gets to the DCE optimum.
"""
import argparse
import glob
import json
import os
import sys

import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import liveness as L  # noqa: E402
from delete_common import infer_tokens  # noqa: E402
from train_delete import Labeler  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--labels", nargs="+", required=True)
    ap.add_argument("--data", default=os.path.join(HERE, "data_fnL"))
    ap.add_argument("--model", default=os.path.join(HERE, "fnL_model.pt"))
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
    s_cls = s_live = s_mlgate = 0
    tp = fp = fn_ = 0
    live_beats = mlgate_beats = 0
    for r in val:
        fnrec = r["funcs"][0]
        instrs = fnrec["instrs"]
        gt = set(i for i, d in enumerate(fnrec["delete"]) if d)  # liveness-dead
        n_cls = len(instrs)
        n_live = n_cls - len(gt)

        funcs = {"f": list(instrs)}
        toks, pos, refs = infer_tokens(funcs)
        ids = torch.tensor([[vocab.get(t, unk) for t in toks]], device=dev)
        with torch.no_grad():
            prob = torch.softmax(model.cls(model(ids)[0, pos]), -1)[:, 1].tolist()
        pred = set(refs[i][1] for i in range(len(refs)) if prob[i] > 0.5)
        gated = pred & gt              # sound: only provably-dead removals
        n_mlgate = n_cls - len(gated)

        tp += len(pred & gt); fp += len(pred - gt); fn_ += len(gt - pred)
        n += 1
        s_cls += n_cls; s_live += n_live; s_mlgate += n_mlgate
        if n_live < n_cls:
            live_beats += 1
        if n_mlgate < n_cls:
            mlgate_beats += 1

    prec = tp / max(1, tp + fp); rec = tp / max(1, tp + fn_)
    print(f"\n=== SOUND comparison vs classical --release ({n} held-out funcs) ===")
    print(f"avg instrs  classical --release:     {s_cls/n:.1f}")
    print(f"avg instrs  liveness DCE (sound):    {s_live/n:.1f}  "
          f"({100*(s_cls-s_live)/s_cls:.0f}% smaller than classical)")
    print(f"avg instrs  learned model + gate:    {s_mlgate/n:.1f}  "
          f"({100*(s_cls-s_mlgate)/s_cls:.0f}% smaller than classical)")
    print(f"\nliveness DCE beats classical:        {live_beats}/{n} "
          f"({100*live_beats/n:.0f}%)")
    print(f"model+gate beats classical:          {mlgate_beats}/{n} "
          f"({100*mlgate_beats/n:.0f}%)")
    print(f"model predicting provably-dead: precision {prec:.2f} recall {rec:.2f}")
    print("\nall removals provably dead (def-use liveness) -> correct for ALL "
          "inputs, no sampling.")


if __name__ == "__main__":
    main()
