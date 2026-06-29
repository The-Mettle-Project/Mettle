#!/usr/bin/env python3
# Train the unified 6-class GNN: action corpus (classes 0-4) + collapse corpus
# (binary root -> class 5). Export with export_gnn.py.
import argparse
import glob
import json
import os
import random
import sys
import time

import torch
import torch.nn as nn

def robust_save(obj, path):
    """Save atomically with retry: torch.save can hit a transient Windows file
    lock (antivirus rescans the just-written .pt). Write a temp then os.replace,
    retrying briefly on PermissionError/OSError."""
    tmp = path + ".tmp"
    for attempt in range(8):
        try:
            torch.save(obj, tmp)
            os.replace(tmp, path)
            return
        except (PermissionError, OSError):
            time.sleep(0.5 * (attempt + 1))
    torch.save(obj, path)  # last resort: let it raise if still broken

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
from gnn_model import GNN, build_graph, collate

NCLASS = 6
NAMES = ["KEEP", "DELETE", "FOLD", "AFFINE", "GVN", "COLLAPSE"]
COLLAPSE = 5
SCLASS = (1, 3, 4, 5)

def load_rows(paths, collapse=False):
    rows = []
    for pat in paths:
        for p in glob.glob(pat):
            for line in open(p, encoding="utf-8"):
                r = json.loads(line)
                fn = r["funcs"][0]
                act = list(fn["action"])
                if collapse:
                    act = [COLLAPSE if a == 1 else 0 for a in act]
                try:
                    g = build_graph(fn["instrs"], r["params"])
                except Exception:
                    continue
                if g["n"] == len(act):
                    rows.append((g, act))
    return rows

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--action", nargs="+", required=True)
    ap.add_argument("--collapse", nargs="+", required=True)
    ap.add_argument("--out", default=os.path.join(HERE, "gnn_genius6.pt"))
    ap.add_argument("--epochs", type=int, default=45)
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--d", type=int, default=384)
    ap.add_argument("--layers", type=int, default=8)
    ap.add_argument("--lr", type=float, default=1e-3)
    # Warm-start: keep the current model instead of training from scratch. Loads
    # weights from this checkpoint (arch must match) before training, so a retrain
    # refines the shipped model rather than relearning from zero. For incremental
    # fine-tuning on added data use a gentler --lr (e.g. 3e-4) and fewer --epochs.
    ap.add_argument("--init", default=None,
                    help="warm-start from this .pt checkpoint (e.g. gnn_genius.pt)")
    args = ap.parse_args()

    t0 = time.time()
    act_rows = load_rows(args.action)
    col_rows = load_rows(args.collapse, collapse=True)
    print(f"loaded action={len(act_rows)} collapse={len(col_rows)} "
          f"in {time.time()-t0:.0f}s", flush=True)
    rows = act_rows + col_rows
    random.Random(0).shuffle(rows)
    val = [g for i, g in enumerate(rows) if i % 10 == 0]
    train = [g for i, g in enumerate(rows) if i % 10 != 0]
    dev = "cuda" if torch.cuda.is_available() else "cpu"

    freq = [1] * NCLASS
    for _, lab in train:
        for a in lab:
            freq[a] += 1
    tot = sum(freq)
    w = torch.tensor([tot / f for f in freq], device=dev)
    w = (w / w.min()).clamp(max=30.0)
    model = GNN(d_model=args.d, layers=args.layers, n_classes=NCLASS).to(dev)
    if args.init and os.path.exists(args.init):
        ck0 = torch.load(args.init, map_location=dev)
        want = dict(d_model=args.d, layers=args.layers, n_classes=NCLASS)
        if ck0.get("cfg") == want:
            model.load_state_dict(ck0["model"])
            print(f"warm-started from {args.init} (keeping current model)", flush=True)
        else:
            print(f"WARNING: --init arch {ck0.get('cfg')} != {want}; training "
                  f"from scratch", flush=True)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, args.epochs)
    crit = nn.CrossEntropyLoss(weight=w)
    print(f"device={dev} train={len(train)} val={len(val)} "
          f"params={sum(p.numel() for p in model.parameters())/1e6:.2f}M "
          f"freq={freq} w={[round(x,1) for x in w.tolist()]}", flush=True)

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
        f1 = {c: 2 * tp[c] / max(1, 2 * tp[c] + fp[c] + fn[c]) for c in SCLASS}
        return acc, f1

    best = 0.0
    for ep in range(1, args.epochs + 1):
        model.train(); tl = 0; nb = 0; te = time.time()
        for bg in batches(train, True):
            feats, y = collate(bg, dev)
            loss = crit(model(feats), y)
            opt.zero_grad(); loss.backward(); opt.step()
            tl += loss.item(); nb += 1
        sched.step()
        acc, f1 = evaluate()
        score = sum(f1.values()) / len(f1)
        if ep % 2 == 0 or ep == 1:
            print(f"ep {ep:3d} ({time.time()-te:.0f}s) loss {tl/nb:.3f} acc {acc:.3f} "
                  f"DEL {f1[1]:.2f} AFF {f1[3]:.2f} GVN {f1[4]:.2f} COL {f1[5]:.2f}",
                  flush=True)
        if score >= best:
            best = score
            robust_save(dict(model=model.state_dict(),
                             cfg=dict(d_model=args.d, layers=args.layers,
                                      n_classes=NCLASS)), args.out)
    print(f"best mean-F1 {best:.3f} -> {args.out}", flush=True)

if __name__ == "__main__":
    main()
