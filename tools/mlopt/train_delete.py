#!/usr/bin/env python3
"""Train the Stage-1 keep/delete labeler: a transformer encoder over the program
tokens that classifies each instruction keep/delete at its <I> marker."""
import argparse
import glob
import json
import math
import os
import sys

import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from delete_common import SPECIAL, example_tokens  # noqa: E402


class LabelData(Dataset):
    def __init__(self, rows, vocab):
        self.ex = []
        for r in rows:
            toks, pos, lab = example_tokens(r["funcs"])
            self.ex.append((toks, pos, lab))
        self.vocab = vocab
        self.unk = vocab["<unk>"]

    def __len__(self):
        return len(self.ex)

    def __getitem__(self, i):
        toks, pos, lab = self.ex[i]
        ids = [self.vocab.get(t, self.unk) for t in toks]
        return ids, pos, lab


def collate(pad):
    def f(batch):
        m = max(len(ids) for ids, _, _ in batch)
        T = torch.full((len(batch), m), pad, dtype=torch.long)
        pos_list, lab_list = [], []
        for i, (ids, pos, lab) in enumerate(batch):
            T[i, :len(ids)] = torch.tensor(ids)
            pos_list.append(pos)
            lab_list.append(lab)
        return T, pos_list, lab_list
    return f


class PosEnc(nn.Module):
    def __init__(self, d, n=4096):
        super().__init__()
        pe = torch.zeros(n, d)
        p = torch.arange(n).unsqueeze(1).float()
        div = torch.exp(torch.arange(0, d, 2).float() * (-math.log(10000.0) / d))
        pe[:, 0::2] = torch.sin(p * div)
        pe[:, 1::2] = torch.cos(p * div)
        self.register_buffer("pe", pe.unsqueeze(0))

    def forward(self, x):
        return x + self.pe[:, :x.size(1)]


class Labeler(nn.Module):
    def __init__(self, V, d_model=256, nhead=4, layers=4, ff=512, pad=0,
                 n_classes=2):
        super().__init__()
        self.pad = pad
        self.emb = nn.Embedding(V, d_model, padding_idx=pad)
        self.pos = PosEnc(d_model)
        enc = nn.TransformerEncoderLayer(d_model, nhead, ff, batch_first=True,
                                         dropout=0.1)
        self.enc = nn.TransformerEncoder(enc, layers)
        self.cls = nn.Linear(d_model, n_classes)
        self.d = d_model

    def forward(self, T):
        mask = (T == self.pad)
        h = self.enc(self.pos(self.emb(T) * math.sqrt(self.d)),
                     src_key_padding_mask=mask)
        return h  # [B, L, d]


def run(model, T, pos_list, dev):
    h = model(T.to(dev))
    logits, idx = [], []
    for b, pos in enumerate(pos_list):
        if pos:
            logits.append(model.cls(h[b, pos]))
            idx.append(len(pos))
    return torch.cat(logits, 0) if logits else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--labels", nargs="+", required=True)
    ap.add_argument("--out", default=os.path.join(HERE, "delete_model.pt"))
    ap.add_argument("--data-out", default=os.path.join(HERE, "data_del"))
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--batch", type=int, default=32)
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
        seen.add(r["seed"])
        uniq.append(r)
    rows = sorted(uniq, key=lambda r: r["seed"])

    val = [r for i, r in enumerate(rows) if i % 10 == 0]
    train = [r for i, r in enumerate(rows) if i % 10 != 0]

    vocab = dict.fromkeys(SPECIAL)
    vocab = {t: i for i, t in enumerate(SPECIAL)}
    for r in train:
        toks, _, _ = example_tokens(r["funcs"])
        for t in toks:
            if t not in vocab:
                vocab[t] = len(vocab)
    os.makedirs(args.data_out, exist_ok=True)
    json.dump(vocab, open(os.path.join(args.data_out, "vocab.json"), "w"))
    json.dump([r["seed"] for r in val],
              open(os.path.join(args.data_out, "val_seeds.json"), "w"))

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    pad = vocab["<pad>"]
    tr = DataLoader(LabelData(train, vocab), batch_size=args.batch, shuffle=True,
                    collate_fn=collate(pad))
    va = DataLoader(LabelData(val, vocab), batch_size=args.batch, shuffle=False,
                    collate_fn=collate(pad))
    # class weight: deletes are the minority
    ndel = sum(sum(l) for _, _, l in tr.dataset.ex)
    ntot = sum(len(l) for _, _, l in tr.dataset.ex)
    w = torch.tensor([1.0, max(1.0, (ntot - ndel) / max(1, ndel))], device=dev)
    model = Labeler(len(vocab), pad=pad).to(dev)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr)
    crit = nn.CrossEntropyLoss(weight=w)
    print(f"device={dev} vocab={len(vocab)} train={len(train)} val={len(val)} "
          f"params={sum(p.numel() for p in model.parameters())/1e6:.1f}M "
          f"del_frac={ndel/max(1,ntot):.3f}")

    def evaluate():
        model.eval()
        tp = fp = fn = tn = 0
        with torch.no_grad():
            for T, pos, labs in va:
                logits = run(model, T, pos, dev)
                preds = logits.argmax(-1).tolist()
                flat = [x for l in labs for x in l]
                for p, y in zip(preds, flat):
                    if y == 1 and p == 1: tp += 1
                    elif y == 0 and p == 1: fp += 1
                    elif y == 1 and p == 0: fn += 1
                    else: tn += 1
        acc = (tp + tn) / max(1, tp + tn + fp + fn)
        prec = tp / max(1, tp + fp)
        rec = tp / max(1, tp + fn)
        return acc, prec, rec

    best = 0.0
    for ep in range(1, args.epochs + 1):
        model.train()
        tot = 0.0
        for T, pos, labs in tr:
            logits = run(model, T, pos, dev)
            y = torch.tensor([x for l in labs for x in l], device=dev)
            loss = crit(logits, y)
            opt.zero_grad(); loss.backward(); opt.step()
            tot += loss.item()
        acc, prec, rec = evaluate()
        f1 = 2 * prec * rec / max(1e-9, prec + rec)
        if ep % 5 == 0 or ep == 1:
            print(f"epoch {ep:3d} loss {tot/len(tr):.4f} del_acc {acc:.3f} "
                  f"prec {prec:.3f} rec {rec:.3f} f1 {f1:.3f}")
        if f1 >= best:
            best = f1
            torch.save(dict(model=model.state_dict(),
                            cfg=dict(V=len(vocab), pad=pad)), args.out)
    print(f"best del_f1 {best:.3f} -> {args.out}")


if __name__ == "__main__":
    main()
