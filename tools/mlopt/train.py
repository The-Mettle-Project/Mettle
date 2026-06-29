#!/usr/bin/env python3
"""Train a small transformer seq2seq: unoptimized-IR tokens -> optimized-IR
tokens. External (PyTorch) per the design; the trained weights are what a future
Mettle-side inference path would load. Inference/verification is in evaluate.py.
"""
import argparse
import json
import math
import os
import sys

import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader

HERE = os.path.dirname(os.path.abspath(__file__))


class PairData(Dataset):
    def __init__(self, path, vocab):
        self.rows = [json.loads(l) for l in open(path, encoding="utf-8")]
        self.vocab = vocab
        self.unk = vocab["<unk>"]

    def enc(self, toks):
        return [self.vocab.get(t, self.unk) for t in toks]

    def __len__(self):
        return len(self.rows)

    def __getitem__(self, i):
        r = self.rows[i]
        return self.enc(r["input"]), self.enc(r["target"])


def make_collate(pad, bos, eos):
    def collate(batch):
        src = [torch.tensor(s) for s, _ in batch]
        tgt = [torch.tensor([bos] + t + [eos]) for _, t in batch]
        sm = max(len(x) for x in src)
        tm = max(len(x) for x in tgt)
        S = torch.full((len(batch), sm), pad, dtype=torch.long)
        T = torch.full((len(batch), tm), pad, dtype=torch.long)
        for i, (s, t) in enumerate(zip(src, tgt)):
            S[i, :len(s)] = s
            T[i, :len(t)] = t
        return S, T
    return collate


class PositionalEncoding(nn.Module):
    def __init__(self, d_model, max_len=2048):
        super().__init__()
        pe = torch.zeros(max_len, d_model)
        pos = torch.arange(0, max_len).unsqueeze(1).float()
        div = torch.exp(torch.arange(0, d_model, 2).float() *
                        (-math.log(10000.0) / d_model))
        pe[:, 0::2] = torch.sin(pos * div)
        pe[:, 1::2] = torch.cos(pos * div)
        self.register_buffer("pe", pe.unsqueeze(0))

    def forward(self, x):
        return x + self.pe[:, :x.size(1)]


class Seq2Seq(nn.Module):
    def __init__(self, V, d_model=256, nhead=4, layers=3, ff=512, pad=0):
        super().__init__()
        self.pad = pad
        self.emb = nn.Embedding(V, d_model, padding_idx=pad)
        self.pos = PositionalEncoding(d_model)
        self.tf = nn.Transformer(d_model, nhead, layers, layers, ff,
                                 batch_first=True, dropout=0.1)
        self.out = nn.Linear(d_model, V)
        self.d = d_model

    def embed(self, x):
        return self.pos(self.emb(x) * math.sqrt(self.d))

    def forward(self, src, tgt_in):
        sm = (src == self.pad)
        tm = (tgt_in == self.pad)
        L = tgt_in.size(1)
        causal = torch.triu(torch.ones(L, L, dtype=torch.bool,
                                       device=src.device), diagonal=1)
        h = self.tf(self.embed(src), self.embed(tgt_in),
                    tgt_mask=causal, src_key_padding_mask=sm,
                    tgt_key_padding_mask=tm, memory_key_padding_mask=sm)
        return self.out(h)


def evaluate_loss(model, loader, crit, dev):
    model.eval()
    tot, ntok, correct = 0.0, 0, 0
    with torch.no_grad():
        for S, T in loader:
            S, T = S.to(dev), T.to(dev)
            logits = model(S, T[:, :-1])
            labels = T[:, 1:]
            loss = crit(logits.reshape(-1, logits.size(-1)), labels.reshape(-1))
            mask = labels != model.pad
            tot += loss.item() * mask.sum().item()
            ntok += mask.sum().item()
            correct += ((logits.argmax(-1) == labels) & mask).sum().item()
    return tot / max(1, ntok), correct / max(1, ntok)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(HERE, "data"))
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--d-model", type=int, default=256)
    ap.add_argument("--layers", type=int, default=3)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--out", default=os.path.join(HERE, "model.pt"))
    args = ap.parse_args()

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    vocab = json.load(open(os.path.join(args.data, "vocab.json")))
    pad, bos, eos = vocab["<pad>"], vocab["<bos>"], vocab["<eos>"]
    collate = make_collate(pad, bos, eos)
    tr = DataLoader(PairData(os.path.join(args.data, "train.jsonl"), vocab),
                    batch_size=args.batch, shuffle=True, collate_fn=collate)
    va = DataLoader(PairData(os.path.join(args.data, "val.jsonl"), vocab),
                    batch_size=args.batch, shuffle=False, collate_fn=collate)

    model = Seq2Seq(len(vocab), d_model=args.d_model, layers=args.layers,
                    pad=pad).to(dev)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr)
    crit = nn.CrossEntropyLoss(ignore_index=pad)
    print(f"device={dev} params={sum(p.numel() for p in model.parameters())/1e6:.1f}M "
          f"vocab={len(vocab)} train={len(tr.dataset)} val={len(va.dataset)}")

    best = 1e9
    for ep in range(1, args.epochs + 1):
        model.train()
        tl, ntok = 0.0, 0
        for S, T in tr:
            S, T = S.to(dev), T.to(dev)
            logits = model(S, T[:, :-1])
            labels = T[:, 1:]
            loss = crit(logits.reshape(-1, logits.size(-1)), labels.reshape(-1))
            opt.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            m = (labels != pad).sum().item()
            tl += loss.item() * m
            ntok += m
        vloss, vacc = evaluate_loss(model, va, crit, dev)
        if ep % 5 == 0 or ep == 1:
            print(f"epoch {ep:3d}  train_loss {tl/max(1,ntok):.4f}  "
                  f"val_loss {vloss:.4f}  val_tok_acc {vacc:.3f}")
        if vloss < best:
            best = vloss
            torch.save(dict(model=model.state_dict(),
                            cfg=dict(V=len(vocab), d_model=args.d_model,
                                     layers=args.layers, pad=pad)),
                       args.out)
    print(f"best val_loss {best:.4f} -> {args.out}")


if __name__ == "__main__":
    main()
