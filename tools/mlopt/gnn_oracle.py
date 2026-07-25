#!/usr/bin/env python3
"""`gnn_oracle` -- the successor architecture to `gnn_genius`, as four nested
ablations so the overnight bake-off can attribute every point of accuracy.

    A  baseline      the shipped relational GNN, retrained (control)
    B  A + OBS       observational-equivalence node features and value edges
    C  B + PTR       a pointer head that NAMES the reuse target
    D  C + PONDER    shared-weight recurrent core with learned per-node halting

Why each exists:

OBS (`obs.py`). The baseline reasons about program text; every value-level
question it is asked -- constant? equals a leaf? recomputes a dominating temp? --
is answered from syntax. OBS evaluates each pure instruction on 8 pseudo-random
leaf assignments and hands the model a projection of the resulting 512 bits, plus
value-equality edges. `x*2`, `x+x`, and `x<<1` become the same node.

PTR. `GVN` means "reuse a dominating temp computing this same value", but the
baseline emits only the label `GVN` and the C applier re-derives the target on
its own. The head is therefore strictly less informative than the decision. PTR
scores each (node, dominating candidate) pair and picks one, or none -- turning a
classifier into a pointer network over the program graph.

PONDER. Eight fixed message-passing layers is a fixed receptive field, applied
alike to a 6-node leaf function and a 400-node loop nest. A shared-weight block
with an ACT halting scalar lets depth follow graph diameter: trivial functions
halt in two steps and compile faster, long dependence chains run to sixteen. It
also cuts the parameter count from ~10.8M to ~2.2M and the shipped blob from
42MB to ~9MB, because the per-layer message matrices are the bulk of both.

Everything here stays exportable to the flat fp32 blob that `src/ir/ml_gnn.c`
reads, and every feature stays reproducible in C -- see `obs_golden.py`.
"""
import os
import re
import sys

import torch
import torch.nn as nn

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import obs  # noqa: E402
import liveness as L  # noqa: E402
from gnn_model import KINDS, KIND_IX, OPS, OP_IX, _classify, _operand_feats  # noqa: E402
from sopt import split_def, _BIN  # noqa: E402
from gvn import _dominators, _pure_key  # noqa: E402

NFEAT_BASE = 9                       # must match gnn_model.NFEAT
NFEAT_OBS = NFEAT_BASE + obs.NOBS    # 9 + 36 = 45
NEDGE_BASE = 8
NEDGE_OBS = 12                       # + same-value fwd/rev, dominating-same-value fwd/rev
MAX_CAND = 8                         # pointer candidates considered per node


# ------------------------------------------------------------------ graph

def _same_expr_keys(instrs):
    keyof = [None] * len(instrs)
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
    return keyof


def _dominating_pairs(groups, dom):
    """For each member of each equivalence group, the nearest earlier member that
    dominates it. This is the GVN candidate relation, syntactic or semantic."""
    src, dst = [], []
    for idxs in groups.values():
        if len(idxs) < 2:
            continue
        for a_i in range(len(idxs)):
            i = idxs[a_i]
            for b_i in range(a_i - 1, -1, -1):
                j = idxs[b_i]
                if j in dom[i]:
                    src.append(j); dst.append(i)
                    break
    return src, dst


def build_oracle_graph(instrs, params, use_obs=True):
    """The baseline graph, plus (when use_obs) OBS features and value edges.

    With use_obs=False this must produce exactly what `gnn_model.build_graph`
    produces, or variant A is not a control."""
    n = len(instrs)
    nfeat = NFEAT_OBS if use_obs else NFEAT_BASE
    nedge = NEDGE_OBS if use_obs else NEDGE_BASE
    kind = torch.zeros(n, dtype=torch.long)
    op = torch.zeros(n, dtype=torch.long)
    feat = torch.zeros(n, nfeat)
    parsed = [L.parse_instr(s) for s in instrs]

    last_def = {}
    du_src, du_dst = [], []
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
        for j, v in enumerate(_operand_feats(ins)):
            feat[i, 4 + j] = v
        for u in uses:
            if u in last_def:
                du_src.append(last_def[u]); du_dst.append(i)
        if defn:
            last_def[defn] = i

    _, succ = L.build_cfg(instrs)
    c_src, c_dst = [], []
    for i in range(n):
        for s in succ[i]:
            if s < n:
                c_src.append(i); c_dst.append(s)

    keyof = _same_expr_keys(instrs)
    se_src, se_dst = [], []
    last, by_key = {}, {}
    for i in range(n):
        k = keyof[i]
        if k is None:
            continue
        if k in last:
            se_src.append(last[k]); se_dst.append(i)
        last[k] = i
        by_key.setdefault(k, []).append(i)

    need_dom = any(len(v) > 1 for v in by_key.values())
    dom = None
    dse_src, dse_dst = [], []
    if need_dom:
        dom, _ = _dominators(succ, n)
        dse_src, dse_dst = _dominating_pairs(by_key, dom)

    def te(src, dst):
        return (torch.tensor(src, dtype=torch.long) if src else torch.zeros(0, dtype=torch.long),
                torch.tensor(dst, dtype=torch.long) if dst else torch.zeros(0, dtype=torch.long))

    edges = {0: te(du_src, du_dst), 1: te(du_dst, du_src),
             2: te(c_src, c_dst), 3: te(c_dst, c_src),
             4: te(se_src, se_dst), 5: te(se_dst, se_src),
             6: te(dse_src, dse_dst), 7: te(dse_dst, dse_src)}

    cand_src, cand_dst = list(dse_src), list(dse_dst)
    if use_obs:
        for i, row in enumerate(obs.obs_features(instrs)):
            for j, v in enumerate(row):
                feat[i, NFEAT_BASE + j] = v
        sv_src, sv_dst = obs.semantic_edges(instrs)
        fps, _, _ = obs.fingerprints(instrs)
        by_val = {}
        for i, v in enumerate(fps):
            if v is not None and obs.edge_eligible(v):
                by_val.setdefault(v, []).append(i)
        dsv_src, dsv_dst = [], []
        if any(len(v) > 1 for v in by_val.values()):
            if dom is None:
                dom, _ = _dominators(succ, n)
            dsv_src, dsv_dst = _dominating_pairs(by_val, dom)
        edges[8] = te(sv_src, sv_dst)
        edges[9] = te(sv_dst, sv_src)
        edges[10] = te(dsv_src, dsv_dst)
        edges[11] = te(dsv_dst, dsv_src)
        # Pointer candidates are the union of the syntactic and semantic
        # dominating relations: a target the model may legally name.
        have = set(zip(cand_dst, cand_src))
        for s, d in zip(dsv_src, dsv_dst):
            if (d, s) not in have:
                cand_src.append(s); cand_dst.append(d)

    return dict(kind=kind, op=op, feat=feat, edges=edges, n=n,
                nfeat=nfeat, nedge=nedge,
                cand=_pack_cands(n, cand_src, cand_dst))


def _pack_cands(n, cand_src, cand_dst):
    """-> (idx[n, MAX_CAND] long, mask[n, MAX_CAND] float). idx[i, c] is the
    source node for candidate c of node i; mask marks the live slots."""
    idx = torch.zeros(n, MAX_CAND, dtype=torch.long)
    mask = torch.zeros(n, MAX_CAND)
    fill = [0] * n
    for s, d in zip(cand_src, cand_dst):
        if fill[d] < MAX_CAND:
            idx[d, fill[d]] = s
            mask[d, fill[d]] = 1.0
            fill[d] += 1
    return idx, mask


def gvn_targets(instrs):
    """-> {node i: node j} where a sound GVN would rewrite i to reuse j's temp.

    Mirrors `gvn.gvn`'s selection (available expressions + dominance) but returns
    the INDEX rather than the rewritten text, which is what the pointer head must
    learn. Labels derived from a sound analysis, never from sampling."""
    n = len(instrs)
    if n == 0:
        return {}
    _, succ = L.build_cfg(instrs)
    dom, preds = _dominators(succ, n)
    keyof = [None] * n
    defname = [None] * n
    for i, ins in enumerate(instrs):
        d = split_def(ins)
        if d:
            defname[i] = d[0]
            if d[0].startswith("%"):
                keyof[i] = _pure_key(d[2])

    U = set(k for k in keyof if k)
    gen = [set() for _ in range(n)]
    kill = [set() for _ in range(n)]
    for i in range(n):
        dn = defname[i]
        if dn:
            kill[i] = {e for e in U if dn == e[1] or dn == e[2]}
        if instrs[i].startswith("*") or re.search(r"[A-Za-z_]\w*\s*\(", instrs[i]):
            kill[i] |= {e for e in U if str(e[1]).startswith("@") or str(e[2]).startswith("@")}
        if keyof[i] and keyof[i] not in kill[i]:
            gen[i] = {keyof[i]}

    avail_in = [set() for _ in range(n)]
    avail_out = [set(U) for _ in range(n)]
    changed = True
    while changed:
        changed = False
        for i in range(n):
            ain = set() if not preds[i] else set(U)
            for p in preds[i]:
                ain &= avail_out[p]
            if i == 0:
                ain = set()
            aout = (ain - kill[i]) | gen[i]
            if ain != avail_in[i] or aout != avail_out[i]:
                avail_in[i], avail_out[i] = ain, aout
                changed = True

    defs_by_key = {}
    for i in range(n):
        if keyof[i]:
            defs_by_key.setdefault(keyof[i], []).append(i)
    out = {}
    for i in range(n):
        e = keyof[i]
        if not e or e not in avail_in[i]:
            continue
        for j in defs_by_key.get(e, ()):
            if j != i and j in dom[i]:
                out[i] = j
                break
    return out


# ------------------------------------------------------------------ model

class MPBlock(nn.Module):
    """One relational message-passing step: mean-aggregate per edge type, add a
    self transform. Identical maths to the baseline's inner loop, factored out so
    PONDER can apply the same weights repeatedly."""

    def __init__(self, d, nedge):
        super().__init__()
        self.msg = nn.ModuleList([nn.Linear(d, d) for _ in range(nedge)])
        self.selfw = nn.Linear(d, d)
        self.d = d

    def forward(self, h, edges):
        agg = self.selfw(h)
        N = h.size(0)
        for t, (src, dst) in edges.items():
            if src.numel() == 0 or t >= len(self.msg):
                continue
            m = self.msg[t](h.index_select(0, src))
            acc = torch.zeros(N, self.d, device=h.device, dtype=h.dtype)
            acc.index_add_(0, dst, m)
            deg = torch.zeros(N, 1, device=h.device, dtype=h.dtype)
            deg.index_add_(0, dst, torch.ones(dst.size(0), 1, device=h.device, dtype=h.dtype))
            agg = agg + acc / deg.clamp(min=1.0)
        return agg


class Oracle(nn.Module):
    def __init__(self, d_model=384, layers=8, n_classes=6,
                 use_obs=True, use_ptr=True, use_ponder=False,
                 max_steps=16, aux=True):
        super().__init__()
        self.d = d_model
        self.layers = layers
        self.n_classes = n_classes
        self.use_obs = use_obs
        self.use_ptr = use_ptr
        self.use_ponder = use_ponder
        self.max_steps = max_steps
        self.aux = aux
        nfeat = NFEAT_OBS if use_obs else NFEAT_BASE
        nedge = NEDGE_OBS if use_obs else NEDGE_BASE
        self.nfeat, self.nedge = nfeat, nedge

        self.kind_emb = nn.Embedding(len(KINDS), d_model)
        self.op_emb = nn.Embedding(len(OPS), d_model)
        self.feat_lin = nn.Linear(nfeat, d_model)

        if use_ponder:
            self.block = MPBlock(d_model, nedge)
            self.upd = nn.GRUCell(d_model, d_model)
            self.norm = nn.LayerNorm(d_model)
            self.halt = nn.Linear(d_model, 1)
        else:
            self.blocks = nn.ModuleList([MPBlock(d_model, nedge) for _ in range(layers)])
            self.norms = nn.ModuleList([nn.LayerNorm(d_model) for _ in range(layers)])

        self.head = nn.Sequential(nn.Linear(d_model, d_model), nn.ReLU(),
                                  nn.Linear(d_model, n_classes))
        if use_ptr:
            # Bilinear scoring of (destination, candidate source), plus a learned
            # "none of these" logit so the head can decline to point.
            self.ptr_q = nn.Linear(d_model, d_model)
            self.ptr_k = nn.Linear(d_model, d_model)
            self.ptr_none = nn.Parameter(torch.zeros(1))
        if aux:
            self.aux_live = nn.Linear(d_model, 2)     # is this def dead?
            self.aux_risk = nn.Linear(d_model, 2)     # would the validator reject?

    # -- core ---------------------------------------------------------------

    def encode(self, batch):
        h = (self.kind_emb(batch["kind"]) + self.op_emb(batch["op"]) +
             self.feat_lin(batch["feat"]))
        edges = batch["edges"]
        if not self.use_ponder:
            for li in range(self.layers):
                agg = self.blocks[li](h, edges)
                h = self.norms[li](h + torch.relu(agg))
            return h, None

        # ACT: accumulate a halting-probability-weighted state. A node that has
        # halted FREEZES (it still sends messages, but stops updating), which is
        # what lets the C port skip recomputing it.
        N = h.size(0)
        cum = torch.zeros(N, device=h.device, dtype=h.dtype)
        y = torch.zeros_like(h)
        ponder = torch.zeros(N, device=h.device, dtype=h.dtype)
        eps = 0.01
        for step in range(self.max_steps):
            running = (cum < 1.0 - eps)
            if not bool(running.any()):
                break
            agg = self.block(h, edges)
            h_new = self.norm(self.upd(torch.relu(agg), h))
            h = torch.where(running.unsqueeze(-1), h_new, h)
            p = torch.sigmoid(self.halt(h)).squeeze(-1)
            last = (cum + p >= 1.0 - eps) | (step == self.max_steps - 1)
            w = torch.where(last, (1.0 - cum).clamp(min=0.0), p) * running.to(h.dtype)
            y = y + w.unsqueeze(-1) * h
            cum = cum + p * running.to(h.dtype)
            ponder = ponder + running.to(h.dtype)
        return y, ponder

    def forward(self, batch):
        h, ponder = self.encode(batch)
        out = {"logits": self.head(h), "ponder": ponder}
        if self.use_ptr:
            idx, mask = batch["cand"]
            q = self.ptr_q(h)                                  # [N, d]
            k = self.ptr_k(h)                                  # [N, d]
            csrc = k.index_select(0, idx.reshape(-1)).reshape(idx.size(0), MAX_CAND, -1)
            scores = (csrc * q.unsqueeze(1)).sum(-1) / (self.d ** 0.5)
            scores = scores.masked_fill(mask < 0.5, float("-inf"))
            none = self.ptr_none.expand(idx.size(0), 1)
            out["ptr"] = torch.cat([none, scores], dim=1)      # slot 0 = decline
        if self.aux:
            out["live"] = self.aux_live(h)
            out["risk"] = self.aux_risk(h)
        return out


def collate_oracle(graphs, device, nedge):
    """Disjoint-union batch. Candidate indices are offset with their graph."""
    kinds, ops, feats, labels = [], [], [], []
    cidx, cmask = [], []
    edges = {t: ([], []) for t in range(nedge)}
    off = 0
    for g, lab in graphs:
        kinds.append(g["kind"]); ops.append(g["op"]); feats.append(g["feat"])
        labels.append(torch.tensor(lab, dtype=torch.long))
        for t, (s, d) in g["edges"].items():
            if t < nedge and s.numel():
                edges[t][0].append(s + off); edges[t][1].append(d + off)
        ci, cm = g["cand"]
        cidx.append(ci + off); cmask.append(cm)
        off += g["n"]
    E = {}
    for t in range(nedge):
        if edges[t][0]:
            E[t] = (torch.cat(edges[t][0]).to(device), torch.cat(edges[t][1]).to(device))
        else:
            z = torch.zeros(0, dtype=torch.long, device=device)
            E[t] = (z, z)
    batch = dict(kind=torch.cat(kinds).to(device), op=torch.cat(ops).to(device),
                 feat=torch.cat(feats).to(device), edges=E,
                 cand=(torch.cat(cidx).to(device), torch.cat(cmask).to(device)))
    return batch, torch.cat(labels).to(device)


VARIANTS = {
    "A": dict(use_obs=False, use_ptr=False, use_ponder=False),
    "B": dict(use_obs=True,  use_ptr=False, use_ponder=False),
    "C": dict(use_obs=True,  use_ptr=True,  use_ponder=False),
    "D": dict(use_obs=True,  use_ptr=True,  use_ponder=True),
}
