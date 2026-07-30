#!/usr/bin/env python3
"""Trivial baselines for "will the validator reject this proposal?".

The risk head is the most immediately useful thing the oracle learns, so it needs
a floor to beat. Random-initialized networks are not that floor: four untrained
checkpoints scored 0.88, 0.47, 0.91, and 0.06 AUC on this task, which says only
that a random projection of the graph retains a lot of coarse structure, and that
one draw tells you nothing.

These baselines are deterministic and interpretable:

  kind        the empirical rejection rate of each instruction KIND (10 classes),
              fitted on train and applied to held-out. Ten numbers, no learning.
  kind+op     the same, keyed by (kind, operator).
  feats       logistic regression on the nine original scalar node features plus
              a one-hot kind -- no message passing, no graph at all.

If the GNN cannot clear these by a wide margin, the head is reading instruction
type rather than program structure, and should be described that way.

    python risk_baseline.py _bakeoff/oracle_real_train.jsonl \\
                            _bakeoff/oracle_real_held.jsonl
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
from gnn_model import KIND_IX, OP_IX, _classify, _operand_feats  # noqa: E402
import liveness as L  # noqa: E402
import re  # noqa: E402
from eval_oracle import roc_auc  # noqa: E402


def rows(path):
    """-> list of (kind, op, feats[9], risk) for every adjudicated node."""
    out = []
    for line in open(path, encoding="utf-8"):
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        fn = r["funcs"][0]
        instrs = fn["instrs"]
        risk = fn.get("risk") or []
        if len(risk) != len(instrs):
            continue
        parsed = [L.parse_instr(s) for s in instrs]
        for i, ins in enumerate(instrs):
            if risk[i] < 0:
                continue
            k, o = _classify(ins)
            _, defn, uses, _ = parsed[i]
            nconst = len(re.findall(r"(?<![\w%@])-?\d+", ins))
            f = [1.0 if defn and defn.startswith("%") else 0.0,
                 1.0 if defn and defn.startswith("@") else 0.0,
                 float(min(nconst, 3)), float(min(len(uses), 4))]
            f += list(_operand_feats(ins))
            out.append((KIND_IX[k], OP_IX.get(o, 0), f, risk[i]))
    return out


def sweep(scores, labels):
    """The operational question, not the ranking question: if we only propose
    where predicted risk is below a threshold, how much validator work do we
    avoid and how many sound rewrites do we lose? AUC does not answer this --
    two models with the same AUC can behave very differently at a usable
    threshold."""
    rows = []
    ngood = sum(1 for y in labels if y == 0)
    for th in (0.3, 0.5, 0.7):
        kept = [(s, y) for s, y in zip(scores, labels) if s < th]
        if not kept:
            continue
        rows.append((th, len(kept) / len(labels),
                     sum(y for _, y in kept) / len(kept),
                     sum(1 for _, y in kept if y == 0), ngood))
    return rows


def rate_baseline(train, held, key):
    num, den = {}, {}
    gnum = gden = 0
    for k, o, f, y in train:
        kk = key(k, o)
        num[kk] = num.get(kk, 0) + y
        den[kk] = den.get(kk, 0) + 1
        gnum += y; gden += 1
    prior = gnum / max(1, gden)
    scores = [num.get(key(k, o), 0) / den[key(k, o)]
              if key(k, o) in den else prior for k, o, f, y in held]
    labels = [y for _, _, _, y in held]
    return roc_auc(scores, labels), sweep(scores, labels)


def logistic(train, held, epochs=300):
    import torch
    import torch.nn as nn
    nk = len(KIND_IX)

    def mat(rs):
        X = torch.zeros(len(rs), 9 + nk)
        y = torch.zeros(len(rs), dtype=torch.long)
        for i, (k, o, f, lab) in enumerate(rs):
            X[i, :9] = torch.tensor(f)
            X[i, 9 + k] = 1.0
            y[i] = lab
        return X, y

    Xtr, ytr = mat(train)
    Xhe, yhe = mat(held)
    mu, sd = Xtr.mean(0, keepdim=True), Xtr.std(0, keepdim=True).clamp(min=1e-6)
    Xtr = (Xtr - mu) / sd
    Xhe = (Xhe - mu) / sd
    m = nn.Linear(Xtr.size(1), 2)
    opt = torch.optim.Adam(m.parameters(), lr=0.05)
    w = torch.tensor([1.0, max(1.0, float((ytr == 0).sum()) / max(1, float((ytr == 1).sum())))])
    crit = nn.CrossEntropyLoss(weight=w)
    for _ in range(epochs):
        opt.zero_grad()
        loss = crit(m(Xtr), ytr)
        loss.backward()
        opt.step()
    with torch.no_grad():
        s = torch.softmax(m(Xhe), -1)[:, 1].tolist()
    return roc_auc(s, yhe.tolist()), sweep(s, yhe.tolist())


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    train, held = rows(sys.argv[1]), rows(sys.argv[2])
    base = sum(y for *_, y in held) / max(1, len(held))
    print(f"adjudicated nodes: train={len(train)} held={len(held)}  "
          f"held rejection rate={base:.3f}")
    results = [
        ("kind", rate_baseline(train, held, lambda k, o: k)),
        ("kind+op", rate_baseline(train, held, lambda k, o: (k, o))),
        ("feats+LR", logistic(train, held)),
    ]
    for name, (auc, sw) in results:
        print(f"  baseline {name:9s}: AUC {auc:.4f}")
        for th, keptf, rej, good, ngood in sw:
            print(f"      filter <{th}: keep {keptf:.2f} of proposals, "
                  f"reject rate {rej:.3f}, retains {good}/{ngood} good ones")
    print("  (the GNN risk head must clear these to be reading structure "
          "rather than instruction type)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
