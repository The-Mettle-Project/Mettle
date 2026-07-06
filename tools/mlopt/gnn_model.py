#!/usr/bin/env python3
"""Graph construction + a relational GNN for per-instruction action prediction.

A flat token transformer has to rediscover def-use structure through attention;
a GNN is handed it. We build a graph per function — one node per instruction,
with typed edges:
  0 def->use   1 use->def   (dataflow, both directions for backward liveness)
  2 ctrl->next 3 next->ctrl (control flow, from the CFG)
Message passing over these edges lets a node learn "is my value used by any
reachable successor" (liveness) and recognize operand-cancellation patterns
(affine) — the structure the per-instruction action labels depend on.
"""
import os
import re
import sys

import torch
import torch.nn as nn

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import liveness as L  # noqa: E402
from sopt import split_def, is_lit, _BIN  # noqa: E402
from gvn import _dominators  # noqa: E402

KINDS = ["label", "jump", "branch", "return", "local", "store",
         "def_bin", "def_copy", "def_call", "other"]
KIND_IX = {k: i for i, k in enumerate(KINDS)}
OPS = ["none", "+", "-", "*", "&", "|", "^", "<<", ">>", "/", "%",
       "==", "!=", "<", "<=", ">", ">="]
OP_IX = {o: i for i, o in enumerate(OPS)}
NFEAT = 9          # scalar node features (must match src/ir/ml_gnn.c)


def _operand_feats(ins):
    """5 binary-operand features (op-embedding can't express them): operands
    equal (a^a, a&a, (a^b)^b cancellation), and whether an operand is the literal
    -1 (NOT mask), 0, 1, or a power of two. Computed identically in the C
    inference (src/ir/ml_gnn.c) so train- and compile-time features match."""
    d = split_def(ins)
    if not d:
        return (0.0, 0.0, 0.0, 0.0, 0.0)
    m = _BIN.match(d[2]) or re.match(r"^(\S+) (==|!=|<|<=|>|>=) (\S+)$", d[2])
    if not m:
        return (0.0, 0.0, 0.0, 0.0, 0.0)
    a, b = m.group(1), m.group(3)

    def pow2(s):
        try:
            v = int(s)
        except ValueError:
            return False
        return v >= 2 and (v & (v - 1)) == 0
    return (1.0 if a == b else 0.0,
            1.0 if (a == "-1" or b == "-1") else 0.0,
            1.0 if (a == "0" or b == "0") else 0.0,
            1.0 if (a == "1" or b == "1") else 0.0,
            1.0 if (pow2(a) or pow2(b)) else 0.0)
# edge types: 0 def->use 1 use->def 2 ctrl->next 3 next->ctrl
#             4 same-expr (earlier->later)        5 same-expr (later->earlier)
#             6 dominating-same-expr (dom->dominated)  7 reverse
# Same-expr edges link instructions computing an identical pure expression. The
# DOMINATING-same-expr edge additionally requires the earlier def to dominate the
# later one (computed via the CFG) — the exact GVN candidate structure. GNNs are
# weak at deriving global dominance from message passing, so we hand it to the
# model as an edge (as with def-use); it still must learn kill-safety/profit.
NEDGE = 8


def _classify(ins):
    if ins.startswith("label "):
        return "label", "none"
    if ins.startswith("jump "):
        return "jump", "none"
    if ins.startswith("branch"):
        return "branch", "none"
    if ins.startswith("return "):
        return "return", "none"
    if ins.startswith("local "):
        return "local", "none"
    if ins.startswith("*"):
        return "store", "none"
    d = split_def(ins)
    if not d:
        return "other", "none"
    rhs = d[2]
    if re.search(r"[A-Za-z_]\w*\s*\(", rhs):
        return "def_call", "none"
    m = _BIN.match(rhs) or re.match(r"^(\S+) (==|!=|<|<=|>|>=) (\S+)$", rhs)
    if m:
        return "def_bin", m.group(2)
    return "def_copy", "none"


def build_graph(instrs, params):
    n = len(instrs)
    kind = torch.zeros(n, dtype=torch.long)
    op = torch.zeros(n, dtype=torch.long)
    feat = torch.zeros(n, NFEAT)
    parsed = [L.parse_instr(s) for s in instrs]
    last_def = {}                       # name -> instr index (reaching def, linear)
    du_src, du_dst = [], []             # def -> use
    for i, ins in enumerate(instrs):
        k, o = _classify(ins)
        kind[i] = KIND_IX[k]
        op[i] = OP_IX.get(o, 0)
        _, defn, uses, _ = parsed[i]
        nconst = len(re.findall(r"(?<![\w%@])-?\d+", ins))
        feat[i, 0] = 1.0 if defn and defn.startswith("%") else 0.0
        feat[i, 1] = 1.0 if defn and defn.startswith("@") else 0.0
        feat[i, 2] = float(min(nconst, 3))
        feat[i, 3] = float(min(len(uses), 4))
        of = _operand_feats(ins)
        for j in range(5):
            feat[i, 4 + j] = of[j]
        for u in uses:
            if u in last_def:
                du_src.append(last_def[u]); du_dst.append(i)
        if defn:
            last_def[defn] = i
    # control edges from the CFG
    _, succ = L.build_cfg(instrs)
    c_src, c_dst = [], []
    for i in range(n):
        for s in succ[i]:
            if s < n:
                c_src.append(i); c_dst.append(s)
    # same-expression keys per instruction (pure ops)
    keyof = [None] * n
    for i, ins in enumerate(instrs):
        d = split_def(ins)
        if not d:
            continue
        m = _BIN.match(d[2]) or re.match(r"^(\S+) (==|!=|<|<=|>|>=) (\S+)$", d[2])
        if not m:
            continue
        a, o2, b = m.group(1), m.group(2), m.group(3)
        if o2 in ("+", "*", "&", "|", "^") and a > b:
            a, b = b, a
        keyof[i] = (o2, a, b)
    # 4/5: same-expr chain (nearest earlier same expr)
    se_src, se_dst = [], []
    last = {}
    by_key = {}
    for i in range(n):
        k = keyof[i]
        if k is None:
            continue
        if k in last:
            se_src.append(last[k]); se_dst.append(i)
        last[k] = i
        by_key.setdefault(k, []).append(i)
    # 6/7: DOMINATING same-expr (earlier def dominates the later use)
    dse_src, dse_dst = [], []
    if any(by_key.get(k) and len(v) > 1 for k, v in by_key.items()):
        dom, _ = _dominators(succ, n)
        for k, idxs in by_key.items():
            if len(idxs) < 2:
                continue
            for a_i in range(len(idxs)):
                i = idxs[a_i]
                for b_i in range(a_i - 1, -1, -1):
                    j = idxs[b_i]
                    if j in dom[i]:
                        dse_src.append(j); dse_dst.append(i)
                        break          # nearest dominating def

    def te(src, dst):
        return (torch.tensor(src, dtype=torch.long) if src else torch.zeros(0, dtype=torch.long),
                torch.tensor(dst, dtype=torch.long) if dst else torch.zeros(0, dtype=torch.long))
    edges = {0: te(du_src, du_dst), 1: te(du_dst, du_src),
             2: te(c_src, c_dst), 3: te(c_dst, c_src),
             4: te(se_src, se_dst), 5: te(se_dst, se_src),
             6: te(dse_src, dse_dst), 7: te(dse_dst, dse_src)}
    return dict(kind=kind, op=op, feat=feat, edges=edges, n=n)


class GNN(nn.Module):
    def __init__(self, d_model=192, layers=5, n_classes=4):
        super().__init__()
        self.kind_emb = nn.Embedding(len(KINDS), d_model)
        self.op_emb = nn.Embedding(len(OPS), d_model)
        self.feat_lin = nn.Linear(NFEAT, d_model)
        self.layers = layers
        self.msg = nn.ModuleList(
            [nn.ModuleList([nn.Linear(d_model, d_model) for _ in range(NEDGE)])
             for _ in range(layers)])
        self.selfw = nn.ModuleList([nn.Linear(d_model, d_model) for _ in range(layers)])
        self.norm = nn.ModuleList([nn.LayerNorm(d_model) for _ in range(layers)])
        self.head = nn.Sequential(nn.Linear(d_model, d_model), nn.ReLU(),
                                  nn.Linear(d_model, n_classes))
        self.d = d_model

    def forward(self, batch):
        h = (self.kind_emb(batch["kind"]) + self.op_emb(batch["op"]) +
             self.feat_lin(batch["feat"]))
        N = h.size(0)
        for li in range(self.layers):
            agg = self.selfw[li](h)
            for t, (src, dst) in batch["edges"].items():
                if src.numel() == 0:
                    continue
                m = self.msg[li][t](h.index_select(0, src))
                acc = torch.zeros(N, self.d, device=h.device)
                acc.index_add_(0, dst, m)
                deg = torch.zeros(N, 1, device=h.device)
                deg.index_add_(0, dst, torch.ones(dst.size(0), 1, device=h.device))
                agg = agg + acc / deg.clamp(min=1.0)
            h = self.norm[li](h + torch.relu(agg))
        return self.head(h)


def collate(graphs, device):
    """Disjoint-union batch of graphs into one big graph."""
    kinds, ops, feats, labels = [], [], [], []
    edges = {t: ([], []) for t in range(NEDGE)}
    off = 0
    for g, lab in graphs:
        kinds.append(g["kind"]); ops.append(g["op"]); feats.append(g["feat"])
        labels.append(torch.tensor(lab, dtype=torch.long))
        for t, (s, d) in g["edges"].items():
            if s.numel():
                edges[t][0].append(s + off); edges[t][1].append(d + off)
        off += g["n"]
    E = {t: (torch.cat(edges[t][0]).to(device) if edges[t][0] else torch.zeros(0, dtype=torch.long, device=device),
             torch.cat(edges[t][1]).to(device) if edges[t][1] else torch.zeros(0, dtype=torch.long, device=device))
         for t in range(NEDGE)}
    return (dict(kind=torch.cat(kinds).to(device), op=torch.cat(ops).to(device),
                 feat=torch.cat(feats).to(device), edges=E),
            torch.cat(labels).to(device))
