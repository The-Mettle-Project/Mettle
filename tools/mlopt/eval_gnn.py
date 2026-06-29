#!/usr/bin/env python3
"""End-to-end eval of the GNN action model: predict per-instruction actions on
the IR graph, apply them with the sound applier (fold/affine + liveness-gated
delete), verify, and compare to classical, delete-only, and the superopt
ceiling."""
import argparse
import glob
import json
import os
import sys

import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import liveness as L  # noqa: E402
import superopt  # noqa: E402
import fn_verify as V  # noqa: E402
from gnn_model import GNN, build_graph, collate  # noqa: E402
from eval_action import apply_actions, AFFINE  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--labels", nargs="+", required=True)
    ap.add_argument("--model", default=os.path.join(HERE, "gnn_model.pt"))
    ap.add_argument("--limit", type=int, default=150)
    args = ap.parse_args()

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    ck = torch.load(args.model, map_location=dev)
    model = GNN(**ck["cfg"]).to(dev); model.load_state_dict(ck["model"]); model.eval()

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
    val = [r for i, r in enumerate(uniq) if i % 10 == 0][:args.limit]

    n = s_cls = s_del = s_gnn = s_sup = 0
    aff_tp = aff_fp = aff_fn = 0
    unsound = 0
    for r in val:
        body = r["funcs"][0]["instrs"]; params = r["params"]
        funcs = {"f": list(body)}
        g = build_graph(body, params)
        with torch.no_grad():
            feats, _ = collate([(g, [0] * g["n"])], dev)
            pred = model(feats).argmax(-1).tolist()
        gold = r["funcs"][0]["action"]
        for p_, t_ in zip(pred, gold):
            if t_ == AFFINE and p_ == AFFINE: aff_tp += 1
            elif p_ == AFFINE and t_ != AFFINE: aff_fp += 1
            elif t_ == AFFINE and p_ != AFFINE: aff_fn += 1

        # ITERATIVE application: predict -> apply -> re-predict, to fixpoint, so
        # the model's one-shot actions compound (cascades) toward the symbolic
        # optimizer's fixpoint result.
        cur = list(body)
        for _ in range(6):
            gg = build_graph(cur, params)
            with torch.no_grad():
                ff, _ = collate([(gg, [0] * gg["n"])], dev)
                pp = model(ff).argmax(-1).tolist()
            nxt = apply_actions(cur, params, pp)
            if nxt == cur:
                break
            cur = nxt
        out = cur
        res = {"f": out}
        vecs = superopt._cover_vectors(body, params, 400)
        if not V.equivalent(funcs, res, "f", params, vecs):
            unsound += 1
            out = [s for i, s in enumerate(body)
                   if i not in L.dead_indices(body, set(params))]
        n += 1
        s_cls += len(body)
        s_del += len(body) - len(L.dead_indices(body, set(params)))
        s_gnn += len(out)
        s_sup += len(superopt.optimize(funcs, "f", params, verify=True)["f"])

    pr = aff_tp / max(1, aff_tp + aff_fp); rc = aff_tp / max(1, aff_tp + aff_fn)
    print(f"\n=== GNN action model, end-to-end ({n} held-out fns) ===")
    print(f"classical --release:              {s_cls/n:.1f}")
    print(f"delete-only (old keep/delete):    {s_del/n:.1f}  ({100*(s_cls-s_del)/s_cls:.0f}% smaller)")
    print(f"GNN action model:                 {s_gnn/n:.1f}  ({100*(s_cls-s_gnn)/s_cls:.0f}% smaller)")
    print(f"superopt (symbolic ceiling):      {s_sup/n:.1f}  ({100*(s_cls-s_sup)/s_cls:.0f}% smaller)")
    print(f"\nAFFINE action: precision {pr:.2f} recall {rc:.2f}")
    print(f"unsound (caught -> fell back): {unsound}/{n} (system stays correct)")


if __name__ == "__main__":
    main()
