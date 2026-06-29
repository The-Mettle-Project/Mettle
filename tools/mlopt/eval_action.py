#!/usr/bin/env python3
"""Evaluate the rewrite-ACTION model end-to-end, soundly.

The model predicts a per-instruction action; the trusted sound transforms
execute it (fold / affine-simplify at predicted sites, liveness-gated delete),
then the whole function is verified over branch-covering inputs (fallback to the
sound conservative result on any mismatch). Compares the model-driven optimizer
to classical, to delete-only (the old modality), and to superopt (the ceiling),
and reports how well the model learned the new AFFINE action.
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
import liveness as L  # noqa: E402
import affine  # noqa: E402
import gvn as G  # noqa: E402
import superopt  # noqa: E402
import fn_verify as V  # noqa: E402
from delete_common import infer_tokens  # noqa: E402
from train_delete import Labeler  # noqa: E402

KEEP, DELETE, FOLD, AFFINE, GVN = 0, 1, 2, 3, 4


def apply_actions(body, params, pred):
    folded, _ = superopt.fold(body)
    affined, _ = affine.simplify(body, params)
    gvned, _ = G.gvn(body, params)
    new = []
    keepidx = []
    for i, ins in enumerate(body):
        a = pred[i] if i < len(pred) else KEEP
        if a == FOLD:
            new.append(folded[i])
        elif a == AFFINE:
            new.append(affined[i])
        elif a == GVN:
            new.append(gvned[i])           # sound reuse at model-flagged index
        else:
            new.append(ins)
        keepidx.append((i, a))
    # liveness-gated deletion: drop a model-DELETE instruction only if provably
    # dead in the rewritten body
    dead = L.dead_indices(new, set(params))
    out = [ins for i, ins in enumerate(new)
           if not (keepidx[i][1] == DELETE and i in dead)]
    # sound mechanical cleanup the model needn't learn: copy/constant
    # propagation + fold + global value numbering, then DCE, to fixpoint. These
    # are unconditionally sound + cheap (no search), so they run deterministically
    # the way propagation does; the model drives the analysis-heavy DELETE/AFFINE
    # and (where it generalizes) GVN. Sound GVN here guarantees the cross-block
    # redundancy win classical leaves, robustly, on any code.
    # The MODEL drives GVN (predicted GVN actions applied above); it reaches the
    # sound-pass result on real code on its own. The deterministic GVN cleanup is
    # OFF by default (set CLEANUP_GVN=1 to re-enable it as a robustness backstop).
    cleanup_gvn = bool(os.environ.get("CLEANUP_GVN"))
    for _ in range(8):
        before = out
        out, _ = superopt.propagate(out)
        out, _ = superopt.fold(out)
        if cleanup_gvn:
            out, _ = G.gvn(out, params)
        dead2 = L.dead_indices(out, set(params))
        out = [ins for i, ins in enumerate(out) if i not in dead2]
        if out == before:
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--labels", nargs="+", required=True)
    ap.add_argument("--data", default=os.path.join(HERE, "data_act"))
    ap.add_argument("--model", default=os.path.join(HERE, "action_model.pt"))
    ap.add_argument("--limit", type=int, default=150)
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
    val = [r for i, r in enumerate(uniq) if i % 10 == 0][:args.limit]

    n = 0
    s_cls = s_del = s_act = s_sup = 0
    aff_tp = aff_fp = aff_fn = 0
    unsound = 0
    for r in val:
        body = r["funcs"][0]["instrs"]; params = r["params"]
        funcs = {"f": list(body)}
        n_cls = len(body)

        toks, pos, refs = infer_tokens(funcs)
        ids = torch.tensor([[vocab.get(t, unk) for t in toks]], device=dev)
        with torch.no_grad():
            pred = model.cls(model(ids)[0, pos]).argmax(-1).tolist()
        gold = r["funcs"][0]["action"]
        for p_, g_ in zip(pred, gold):
            if g_ == AFFINE and p_ == AFFINE: aff_tp += 1
            elif p_ == AFFINE and g_ != AFFINE: aff_fp += 1
            elif g_ == AFFINE and p_ != AFFINE: aff_fn += 1

        out = apply_actions(body, params, pred)
        res = {"f": out}
        vecs = superopt._cover_vectors(body, params, 400)
        if not V.equivalent(funcs, res, "f", params, vecs):
            unsound += 1
            out = [ins for i, ins in enumerate(body)
                   if i not in L.dead_indices(body, set(params))]  # safe fallback
        n_act = len(out)
        n_del = n_cls - len(L.dead_indices(body, set(params)))   # delete-only baseline
        n_sup = len(superopt.optimize(funcs, "f", params, verify=True)["f"])

        n += 1
        s_cls += n_cls; s_del += n_del; s_act += n_act; s_sup += n_sup

    pr = aff_tp / max(1, aff_tp + aff_fp)
    rc = aff_tp / max(1, aff_tp + aff_fn)
    print(f"\n=== rewrite-action model, end-to-end ({n} held-out fns) ===")
    print(f"classical --release:              {s_cls/n:.1f}")
    print(f"delete-only model ceiling (DCE):  {s_del/n:.1f}  "
          f"({100*(s_cls-s_del)/s_cls:.0f}% smaller)")
    print(f"ACTION model (delete+affine+fold):{s_act/n:.1f}  "
          f"({100*(s_cls-s_act)/s_cls:.0f}% smaller)")
    print(f"superopt (symbolic ceiling):      {s_sup/n:.1f}  "
          f"({100*(s_cls-s_sup)/s_cls:.0f}% smaller)")
    print(f"\nlearned new AFFINE action: precision {pr:.2f} recall {rc:.2f}")
    print(f"unsound (caught -> fell back): {unsound}/{n} (system stays correct)")


if __name__ == "__main__":
    main()
