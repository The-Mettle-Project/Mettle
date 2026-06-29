#!/usr/bin/env python3
"""Train the relational GNN per-instruction action classifier (KEEP/DELETE/
FOLD/AFFINE). Operates on the IR dataflow+control graph, so it can reason about
liveness and operand cancellation structurally — aiming well past the flat
transformer's ~0.48 per-instruction accuracy."""
import argparse
import glob
import json
import os
import random
import sys
import time

import torch
import torch.nn as nn

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gnn_model import GNN, build_graph, collate  # noqa: E402

NCLASS = 5
NAMES = ["KEEP", "DELETE", "FOLD", "AFFINE", "GVN"]
SCLASS = (1, 3, 4)                     # scored classes: DELETE, AFFINE, GVN


def load(paths):
    rows = []
    for pat in paths:
        for p in glob.glob(pat):
            rows += [json.loads(l) for l in open(p, encoding="utf-8")]
    seen, uniq = set(), []
    for r in rows:
        if r["seed"] in seen:
            continue
        seen.add(r["seed"]); uniq.append(r)
    return sorted(uniq, key=lambda r: r["seed"])


def to_graph(r):
    fn = r["funcs"][0]
    g = build_graph(fn["instrs"], r["params"])
    return g, fn["action"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--labels", nargs="+", required=True)
    ap.add_argument("--out", default=os.path.join(HERE, "gnn_model.pt"))
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--d", type=int, default=192)
    ap.add_argument("--layers", type=int, default=5)
    ap.add_argument("--lr", type=float, default=1e-3)
    args = ap.parse_args()

    rows = load(args.labels)
    graphs = [to_graph(r) for r in rows]
    val = [g for i, g in enumerate(graphs) if i % 10 == 0]
    train = [g for i, g in enumerate(graphs) if i % 10 != 0]
    dev = "cuda" if torch.cuda.is_available() else "cpu"

    freq = [1] * NCLASS
    for _, lab in train:
        for a in lab:
            freq[a] += 1
    tot = sum(freq)
    w = torch.tensor([tot / f for f in freq], device=dev)
    w = (w / w.min()).clamp(max=20.0)
    model = GNN(d_model=args.d, layers=args.layers, n_classes=NCLASS).to(dev)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, args.epochs)
    crit = nn.CrossEntropyLoss(weight=w)
    print(f"device={dev} train={len(train)} val={len(val)} params="
          f"{sum(p.numel() for p in model.parameters())/1e6:.2f}M "
          f"weights={[round(x,1) for x in w.tolist()]}", flush=True)

    def batches(data, shuffle):
        idx = list(range(len(data)))
        if shuffle:
            random.shuffle(idx)
        for i in range(0, len(idx), args.batch):
            yield [data[j] for j in idx[i:i + args.batch]]

    def evaluate():
        model.eval()
        import collections
        tp = collections.Counter(); fp = collections.Counter(); fn = collections.Counter()
        correct = total = 0
        with torch.no_grad():
            for bg in batches(val, False):
                feats, y = collate(bg, dev)
                pred = model(feats).argmax(-1)
                for p, t in zip(pred.tolist(), y.tolist()):
                    total += 1; correct += (p == t)
                    if p == t: tp[t] += 1
                    else: fp[p] += 1; fn[t] += 1
        acc = correct / max(1, total)
        pr = {c: tp[c] / max(1, tp[c] + fp[c]) for c in SCLASS}
        rc = {c: tp[c] / max(1, tp[c] + fn[c]) for c in SCLASS}
        f1 = {c: 2 * pr[c] * rc[c] / max(1e-9, pr[c] + rc[c]) for c in SCLASS}
        return acc, pr, rc, f1

    best = 0.0
    for ep in range(1, args.epochs + 1):
        model.train(); t0 = time.time(); tl = 0; nb = 0
        for bg in batches(train, True):
            feats, y = collate(bg, dev)
            loss = crit(model(feats), y)
            opt.zero_grad(); loss.backward(); opt.step()
            tl += loss.item(); nb += 1
        sched.step()
        acc, pr, rc, f1 = evaluate()
        score = sum(f1[c] for c in SCLASS) / len(SCLASS)   # mean F1 over DEL/AFF/GVN
        if ep % 2 == 0 or ep == 1:
            print(f"ep {ep:3d} ({time.time()-t0:.0f}s) loss {tl/nb:.3f} acc {acc:.3f} "
                  f"DEL F{f1[1]:.2f} AFF p{pr[3]:.2f}/r{rc[3]:.2f}/F{f1[3]:.2f} "
                  f"GVN p{pr[4]:.2f}/r{rc[4]:.2f}/F{f1[4]:.2f}",
                  flush=True)
        if score >= best:
            best = score
            torch.save(dict(model=model.state_dict(),
                            cfg=dict(d_model=args.d, layers=args.layers,
                                     n_classes=NCLASS)), args.out)
    print(f"best mean-F1(DEL,AFF) {best:.3f} -> {args.out}", flush=True)


if __name__ == "__main__":
    main()
