#!/usr/bin/env python3
"""Evaluate a `gnn_oracle` checkpoint on held-out data. Apples-to-apples across
variants: same files, same batching, same metrics.

    python eval_oracle.py oracle_C.pt --files real_held.jsonl [--json out.json]

Reports
  action  per-class precision / recall / F1 (the baseline's metric, kept so old
          and new numbers are comparable)
  ptr     accuracy of the reuse target, and separately the accuracy restricted to
          sites that genuinely have a target. On real IR most candidate sites
          should DECLINE, so overall accuracy is dominated by the decline class
          and would flatter a model that never points at anything.
  risk    can the model predict which of its own proposals the gate will reject?
          Reported as ROC-AUC plus the precision/recall of a 0.5 threshold, and
          the rejection rate that survives filtering at several thresholds --
          which is the number the speculative-mode rejection problem turns on.
  steps   mean ACT halting steps (PONDER only); the compile-time budget.
"""
import argparse
import json
import os
import sys

import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import gnn_oracle as O  # noqa: E402
import train_oracle as T  # noqa: E402

NAMES = ["KEEP", "DELETE", "FOLD", "AFFINE", "GVN", "COLLAPSE"]


def roc_auc(scores, labels):
    """Rank-based AUC; ties get average rank. No sklearn dependency."""
    pairs = sorted(zip(scores, labels))
    n = len(pairs)
    if n == 0:
        return float("nan")
    npos = sum(labels)
    nneg = n - npos
    if npos == 0 or nneg == 0:
        return float("nan")
    ranks = [0.0] * n
    i = 0
    while i < n:
        j = i
        while j + 1 < n and pairs[j + 1][0] == pairs[i][0]:
            j += 1
        avg = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            ranks[k] = avg
        i = j + 1
    srank = sum(r for r, (_, l) in zip(ranks, pairs) if l == 1)
    return (srank - npos * (npos + 1) / 2.0) / (npos * nneg)


def evaluate(model_path, files, collapse=False, batch=64, device=None):
    dev = device or ("cuda" if torch.cuda.is_available() else "cpu")
    ck = torch.load(model_path, map_location=dev, weights_only=False)
    cfg = dict(ck["cfg"])
    model = O.Oracle(**cfg).to(dev)
    model.load_state_dict(ck["model"])
    model.eval()
    use_obs = cfg.get("use_obs", True)
    nedge = O.NEDGE_OBS if use_obs else O.NEDGE_BASE

    paths = []
    for f in files:
        paths += T._expand([f])
    rows = T.build_rows(paths, use_obs, collapse=collapse, want_risk=True)
    if not rows:
        return dict(model=os.path.basename(model_path), n_functions=0)

    import collections
    tp = collections.Counter(); fp = collections.Counter(); fn = collections.Counter()
    correct = total = 0
    ptr_c = ptr_t = ptr_pos_c = ptr_pos_t = 0
    risk_scores, risk_labels = [], []
    steps_sum = 0.0
    steps_n = 0

    with torch.no_grad():
        for i in range(0, len(rows), batch):
            bg = rows[i:i + batch]
            feats, y, ptr, live, risk = T.collate(bg, dev, nedge)
            out = model(feats)
            pred = out["logits"].argmax(-1)
            for p, t in zip(pred.tolist(), y.tolist()):
                total += 1; correct += (p == t)
                if p == t: tp[t] += 1
                else: fp[p] += 1; fn[t] += 1
            if "ptr" in out:
                m = ptr >= 0
                if bool(m.any()):
                    pp = out["ptr"][m].argmax(-1)
                    tt = ptr[m]
                    # m.sum(), NOT m.numel(): the denominator is the number of
                    # nodes that HAVE candidates, not every node in the batch.
                    ptr_c += int((pp == tt).sum().item())
                    ptr_t += int(m.sum().item())
                    pm = tt > 0
                    if bool(pm.any()):
                        ptr_pos_c += int((pp[pm] == tt[pm]).sum().item())
                        ptr_pos_t += int(pm.sum().item())
            if "risk" in out:
                m = risk >= 0
                if bool(m.any()):
                    pr = torch.softmax(out["risk"][m], -1)[:, 1]
                    risk_scores += pr.tolist()
                    risk_labels += risk[m].tolist()
            if out.get("ponder") is not None:
                steps_sum += float(out["ponder"].mean().item()); steps_n += 1

    res = dict(model=os.path.basename(model_path),
               variant=ck.get("variant"),
               train_epoch=(ck.get("metrics") or {}).get("epoch"),
               params=sum(p.numel() for p in model.parameters()),
               n_functions=len(rows), n_nodes=total,
               acc=correct / max(1, total))
    per = {}
    for c in range(6):
        sup = tp[c] + fn[c]
        if sup == 0 and fp[c] == 0:
            continue
        pr = tp[c] / max(1, tp[c] + fp[c])
        rc = tp[c] / max(1, tp[c] + fn[c])
        per[NAMES[c]] = dict(precision=pr, recall=rc,
                             f1=2 * pr * rc / max(1e-9, pr + rc), support=sup)
    res["per_class"] = per
    res["ptr_acc"] = ptr_c / ptr_t if ptr_t else None
    res["ptr_acc_pointing"] = ptr_pos_c / ptr_pos_t if ptr_pos_t else None
    res["ptr_sites"] = ptr_t
    res["ptr_pointing_sites"] = ptr_pos_t

    if risk_labels:
        res["risk_auc"] = roc_auc(risk_scores, risk_labels)
        res["risk_n"] = len(risk_labels)
        res["risk_base_reject_rate"] = sum(risk_labels) / len(risk_labels)
        # If we only proposed where predicted risk < threshold, what happens to
        # the rejection rate, and how many good proposals do we lose?
        sweep = {}
        for th in (0.3, 0.5, 0.7):
            kept = [(s, l) for s, l in zip(risk_scores, risk_labels) if s < th]
            if kept:
                sweep[str(th)] = dict(
                    kept_frac=len(kept) / len(risk_labels),
                    reject_rate=sum(l for _, l in kept) / len(kept),
                    good_kept=sum(1 for _, l in kept if l == 0),
                    good_total=sum(1 for l in risk_labels if l == 0))
        res["risk_threshold_sweep"] = sweep
    res["mean_steps"] = steps_sum / steps_n if steps_n else None
    return res


def fmt(res):
    out = [f"{res['model']} (variant {res.get('variant')}, "
           f"{res.get('params',0)/1e6:.2f}M params)",
           f"  {res['n_functions']} functions / {res.get('n_nodes',0)} nodes  "
           f"acc {res.get('acc',0):.4f}"]
    out.append(f"  {'class':10s} {'prec':>7s} {'recall':>7s} {'f1':>7s} {'support':>9s}")
    for name, d in res.get("per_class", {}).items():
        out.append(f"  {name:10s} {d['precision']:7.3f} {d['recall']:7.3f} "
                   f"{d['f1']:7.3f} {d['support']:9d}")
    if res.get("ptr_acc") is not None:
        out.append(f"  ptr: {res['ptr_acc']:.4f} over {res['ptr_sites']} sites; "
                   f"on the {res['ptr_pointing_sites']} that DO have a target: "
                   f"{(res['ptr_acc_pointing'] or 0):.4f}")
    if res.get("risk_auc") is not None:
        out.append(f"  risk: AUC {res['risk_auc']:.4f} over {res['risk_n']} "
                   f"adjudicated proposals (base reject rate "
                   f"{res['risk_base_reject_rate']:.3f})")
        for th, d in res.get("risk_threshold_sweep", {}).items():
            out.append(f"    filter <{th}: keep {d['kept_frac']:.2f} of "
                       f"proposals, reject rate {d['reject_rate']:.3f}, "
                       f"retains {d['good_kept']}/{d['good_total']} good ones")
    if res.get("mean_steps") is not None:
        out.append(f"  mean ACT steps: {res['mean_steps']:.2f}")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("--files", nargs="+", required=True)
    ap.add_argument("--collapse", action="store_true")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()
    res = evaluate(args.model, args.files, collapse=args.collapse)
    print(fmt(res))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(res, f, indent=2)


if __name__ == "__main__":
    main()
