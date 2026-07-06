#!/usr/bin/env python3
"""Train the rewrite-ACTION model: per-instruction 4-class
(KEEP/DELETE/FOLD/AFFINE) over the program tokens. Richer modality than the
keep/delete model, so it can learn the cross-instruction algebra, not just
deletion."""
import argparse
import glob
import json
import os
import sys

import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from delete_common import SPECIAL, example_tokens  # noqa: E402
from train_delete import Labeler, PosEnc, collate  # noqa: E402

NCLASS = 4
NAMES = ["KEEP", "DELETE", "FOLD", "AFFINE"]


class ActData(Dataset):
    def __init__(self, rows, vocab):
        self.ex = []
        for r in rows:
            toks, pos, _ = example_tokens(r["funcs"])
            acts = r["funcs"][0]["action"]
            self.ex.append((toks, pos, acts))
        self.vocab = vocab
        self.unk = vocab["<unk>"]

    def __len__(self):
        return len(self.ex)

    def __getitem__(self, i):
        toks, pos, acts = self.ex[i]
        return [self.vocab.get(t, self.unk) for t in toks], pos, acts


def run(model, T, pos_list, dev):
    h = model(T.to(dev))
    logits = []
    for b, pos in enumerate(pos_list):
        if pos:
            logits.append(model.cls(h[b, pos]))
    return torch.cat(logits, 0) if logits else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--labels", nargs="+", required=True)
    ap.add_argument("--out", default=os.path.join(HERE, "action_model.pt"))
    ap.add_argument("--data-out", default=os.path.join(HERE, "data_act"))
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--batch", type=int, default=48)
    ap.add_argument("--lr", type=float, default=3e-4)
    args = ap.parse_args()

    rows = []
    for pat in args.labels:
        for p in glob.glob(pat):
            rows += [json.loads(l) for l in open(p, encoding="utf-8")]
    seen, uniq = set(), []
    for r in rows:
        if r["seed"] in seen:
            continue
        seen.add(r["seed"]); uniq.append(r)
    rows = sorted(uniq, key=lambda r: r["seed"])
    val = [r for i, r in enumerate(rows) if i % 10 == 0]
    train = [r for i, r in enumerate(rows) if i % 10 != 0]

    vocab = {t: i for i, t in enumerate(SPECIAL)}
    for r in train:
        toks, _, _ = example_tokens(r["funcs"])
        for t in toks:
            if t not in vocab:
                vocab[t] = len(vocab)
    os.makedirs(args.data_out, exist_ok=True)
    json.dump(vocab, open(os.path.join(args.data_out, "vocab.json"), "w"))

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    pad = vocab["<pad>"]
    tr = DataLoader(ActData(train, vocab), batch_size=args.batch, shuffle=True,
                    collate_fn=collate(pad))
    va = DataLoader(ActData(val, vocab), batch_size=args.batch, shuffle=False,
                    collate_fn=collate(pad))
    # class weights: inverse frequency so DELETE/AFFINE aren't swamped by KEEP
    freq = [1, 1, 1, 1]
    for _, _, acts in tr.dataset.ex:
        for a in acts:
            freq[a] += 1
    tot = sum(freq)
    w = torch.tensor([tot / f for f in freq], device=dev)
    w = w / w.min()

    model = Labeler(len(vocab), pad=pad, n_classes=NCLASS).to(dev)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr)
    crit = nn.CrossEntropyLoss(weight=w)
    print(f"device={dev} vocab={len(vocab)} train={len(train)} val={len(val)} "
          f"classweights={[round(x,1) for x in w.tolist()]}")

    def evaluate():
        model.eval()
        import collections
        tp = collections.Counter(); fp = collections.Counter()
        fn = collections.Counter(); correct = 0; total = 0
        with torch.no_grad():
            for T, pos, labs in va:
                logits = run(model, T, pos, dev)
                preds = logits.argmax(-1).tolist()
                flat = [x for l in labs for x in l]
                for p, y in zip(preds, flat):
                    total += 1; correct += (p == y)
                    if p == y: tp[y] += 1
                    else: fp[p] += 1; fn[y] += 1
        acc = correct / max(1, total)
        pr = {c: tp[c] / max(1, tp[c] + fp[c]) for c in (1, 3)}
        rc = {c: tp[c] / max(1, tp[c] + fn[c]) for c in (1, 3)}
        return acc, pr, rc

    best = 0.0
    for ep in range(1, args.epochs + 1):
        model.train(); tot_l = 0
        for T, pos, labs in tr:
            logits = run(model, T, pos, dev)
            y = torch.tensor([x for l in labs for x in l], device=dev)
            loss = crit(logits, y)
            opt.zero_grad(); loss.backward(); opt.step()
            tot_l += loss.item()
        acc, pr, rc = evaluate()
        score = (rc[1] + rc[3]) / 2
        if ep % 5 == 0 or ep == 1:
            print(f"ep {ep:3d} loss {tot_l/len(tr):.3f} acc {acc:.3f} "
                  f"DEL p{pr[1]:.2f}/r{rc[1]:.2f} AFF p{pr[3]:.2f}/r{rc[3]:.2f}")
        if score >= best:
            best = score
            torch.save(dict(model=model.state_dict(),
                            cfg=dict(V=len(vocab), pad=pad, n_classes=NCLASS)),
                       args.out)
    print(f"best mean-recall(DEL,AFF) {best:.3f} -> {args.out}")


if __name__ == "__main__":
    main()
