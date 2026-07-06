#!/usr/bin/env python3
"""Per-class P/R/F1 of a model on fixed held-out files (apples-to-apples old vs
new). Usage: python _cmp_eval.py MODEL.pt [--collapse] file1.jsonl file2.jsonl ..."""
import collections, glob, json, os, sys
import torch
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
from gnn_model import GNN, build_graph, collate

NAMES = ["KEEP", "DELETE", "FOLD", "AFFINE", "GVN", "COLLAPSE"]
COLLAPSE = 5
args = sys.argv[1:]
model_path = args[0]; args = args[1:]
remap = False
if args and args[0] == "--collapse":
    remap = True; args = args[1:]

rows = []
for pat in args:
    for p in glob.glob(os.path.join(HERE, pat)):
        for line in open(p, encoding="utf-8"):
            r = json.loads(line); fn = r["funcs"][0]
            act = list(fn["action"])
            if remap:
                act = [COLLAPSE if a == 1 else 0 for a in act]
            try:
                g = build_graph(fn["instrs"], r["params"])
            except Exception:
                continue
            if g["n"] == len(act):
                rows.append((g, act))

dev = "cuda" if torch.cuda.is_available() else "cpu"
ck = torch.load(model_path, map_location=dev)
model = GNN(**ck["cfg"]).to(dev); model.load_state_dict(ck["model"]); model.eval()

tp = collections.Counter(); fp = collections.Counter(); fn = collections.Counter()
correct = total = 0
with torch.no_grad():
    for i in range(0, len(rows), 64):
        feats, y = collate(rows[i:i+64], dev)
        pred = model(feats).argmax(-1)
        for p, t in zip(pred.tolist(), y.tolist()):
            total += 1; correct += (p == t)
            if p == t: tp[t] += 1
            else: fp[p] += 1; fn[t] += 1
print(f"{os.path.basename(model_path)} on {len(rows)} fns: acc {correct/max(1,total):.4f}")
print(f"{'class':10s} {'prec':>7s} {'recall':>7s} {'f1':>7s} {'support':>9s}")
for c in range(6):
    sup = tp[c] + fn[c]
    if sup == 0 and fp[c] == 0:
        continue
    pr = tp[c]/max(1, tp[c]+fp[c]); rc = tp[c]/max(1, tp[c]+fn[c])
    f1 = 2*pr*rc/max(1e-9, pr+rc)
    print(f"{NAMES[c]:10s} {pr:7.3f} {rc:7.3f} {f1:7.3f} {sup:9d}")
