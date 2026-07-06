#!/usr/bin/env python3
"""Global value numbering — sound, deterministic, NOT search.

Block-local value numbering (superopt.cse) reuses an earlier identical pure
expression only within a basic block; it resets at every control boundary. GVN
extends this across the whole control-flow graph using AVAILABLE-EXPRESSIONS
dataflow + dominance:

  I: u = a op b   is redundant if the expression `a op b` is AVAILABLE on every
  path reaching I (computed earlier and not killed since), and a dominating def
  D: t = a op b exists whose temp t still holds the value. Then  I -> u <- t.

Availability is a forward dataflow (intersection at merges, fixpoint over loops),
so an operand reassigned LATER in a loop body (e.g. `@i = @i + 1` after the use)
does not block reuse within the iteration — the kill happens after I, not between
D and I. This catches loop/cross-block redundancy classical block-local CSE
provably misses (e.g. `@a+@i` recomputed several times per loop iteration).

Soundness (no input sampling): availability => `a op b` already computed on all
paths to I and not killed since; D dominates I so its temp t is computed on every
path to I; t is a %-temp (single-assignment) so it still holds the value. Hence
`u <- t` is exactly value-equivalent. Only pure arithmetic/compare expressions
participate (no loads/calls/stores), so memory and side effects are irrelevant.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import liveness as L
from sopt import split_def, _BIN

_COMM = {"+", "*", "&", "|", "^"}
_PURE = {"+", "-", "*", "&", "|", "^", "<<", ">>", "/", "%",
         "==", "!=", "<", "<=", ">", ">="}


def _dominators(succ, n):
    preds = [[] for _ in range(n)]
    for i in range(n):
        for s in succ[i]:
            if s < n:
                preds[s].append(i)
    alln = set(range(n))
    dom = [set([0]) if i == 0 else set(alln) for i in range(n)]
    changed = True
    while changed:
        changed = False
        for i in range(1, n):
            new = set(alln)
            for p in preds[i]:
                new &= dom[p]
            new |= {i}
            if new != dom[i]:
                dom[i] = new
                changed = True
    return dom, preds


def _pure_key(rhs):
    m = _BIN.match(rhs) or re.match(r"^(\S+) (==|!=|<|<=|>|>=) (\S+)$", rhs)
    if not m:
        return None
    a, op, b = m.group(1), m.group(2), m.group(3)
    if op not in _PURE:
        return None
    if op in _COMM and a > b:
        a, b = b, a
    return (op, a, b)


def gvn(instrs, params=None):
    n = len(instrs)
    if n == 0:
        return instrs, False
    _, succ = L.build_cfg(instrs)
    dom, preds = _dominators(succ, n)

    # per-instruction expression key (pure only) and the name it defines
    keyof = [None] * n
    defname = [None] * n
    for i, ins in enumerate(instrs):
        d = split_def(ins)
        if d:
            defname[i] = d[0]
            if d[0].startswith("%"):            # only temp-defined exprs are reusable
                keyof[i] = _pure_key(d[2])

    # universe of expressions; gen/kill per node
    U = set(k for k in keyof if k)
    gen = [set() for _ in range(n)]
    kill = [set() for _ in range(n)]
    for i in range(n):
        dn = defname[i]
        if dn:
            kill[i] = {e for e in U if dn == e[1] or dn == e[2]}
        # a call or store may write a global / address-taken local through memory,
        # killing every expression that reads an @-symbol (%-temps are SSA-safe)
        if instrs[i].startswith("*") or re.search(r"[A-Za-z_]\w*\s*\(", instrs[i]):
            kill[i] |= {e for e in U if str(e[1]).startswith("@") or str(e[2]).startswith("@")}
        if keyof[i] and keyof[i] not in kill[i]:
            gen[i] = {keyof[i]}

    # forward available-expressions dataflow
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

    # for each redundant gen, reuse a dominating temp def of the same expr
    defs_by_key = {}
    for i in range(n):
        if keyof[i]:
            defs_by_key.setdefault(keyof[i], []).append(i)

    out = list(instrs)
    did = False
    for i in range(n):
        e = keyof[i]
        if not e or e not in avail_in[i]:
            continue
        for j in defs_by_key.get(e, ()):
            if j != i and j in dom[i]:
                out[i] = f"{defname[i]} <- {defname[j]}"
                did = True
                break
    return out, did


if __name__ == "__main__":
    import json
    for line in sys.stdin:
        r = json.loads(line)
        body = r["funcs"][0]["instrs"]
        o, c = gvn(body, r.get("params"))
        print("changed", c, "reused", sum(1 for a, b in zip(body, o) if a != b))
