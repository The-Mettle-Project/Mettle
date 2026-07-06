#!/usr/bin/env python3
"""Sound def-use liveness / dead-code analysis for the int64 IR.

Differential testing over sampled inputs is UNSOUND for branch-dependent code: a
deletion can look safe only because no test input exercised the relevant branch
(observed: deleting an `if (p0 != 110)` guard passed because 110 wasn't sampled).
This computes liveness over the control-flow graph, so an instruction is removed
ONLY if its defined value is provably unused on every path — sound for all
inputs, no sampling. Control-flow instructions (label/jump/branch) and stores are
never removed. Used both to generate sound labels and as the inference-time gate.

Scope: scalar int64 IR (no pointer stores / calls), which is what gen_fn emits.
"""
import re

_TOK = re.compile(r"[%@][A-Za-z0-9_.$]*")


def _names(s):
    return [t for t in _TOK.findall(s)]


def parse_instr(ins):
    """-> (kind, def_name|None, uses[list], is_removable_if_dead)."""
    if ins.startswith("label ") or ins.startswith("jump "):
        return ("control", None, [], False)
    if ins.startswith("branch_zero "):
        m = re.match(r"branch_zero (\S+) ->", ins)
        return ("control", None, _names(m.group(1)) if m else [], False)
    if ins.startswith("branch_eq "):
        m = re.match(r"branch_eq (\S+), (\S+) ->", ins)
        u = _names(m.group(1)) + _names(m.group(2)) if m else []
        return ("control", None, u, False)
    if ins.startswith("return "):
        return ("return", None, _names(ins[len("return "):]), False)
    if ins.startswith("local "):
        m = re.match(r"local (@\S+)", ins)
        return ("decl", m.group(1) if m else None, [], True)
    if ins.startswith("*"):                       # pointer store: side effect
        return ("store", None, _names(ins), False)
    # value def:  DEST = rhs   |   DEST <- src   |   DEST += rhs
    m = re.match(r"^(\S+)\s*(?:=|<-|\+=)\s*(.*)$", ins)
    if m:
        dest = m.group(1)
        rhs = m.group(2)
        dnames = _names(dest)
        if not dnames:
            return ("other", None, _names(ins), False)
        defn = dnames[0]
        uses = _names(rhs)
        if "+=" in ins.split("//")[0][:len(dest) + 4]:
            uses = [defn] + uses
        # A call on the RHS may have side effects (I/O, global writes); never
        # remove it even if its result is unused. Pure-fn analysis could relax
        # this, but conservative keep is the sound default.
        removable = re.search(r"[A-Za-z_]\w*\s*\(", rhs) is None
        return ("def", defn, uses, removable)
    return ("other", None, _names(ins), False)


def build_cfg(body):
    parsed = [parse_instr(s) for s in body]
    n = len(body)
    label_at = {}
    for i, s in enumerate(body):
        if s.startswith("label "):
            label_at[s.split()[1]] = i
    succ = [[] for _ in range(n)]
    for i, s in enumerate(body):
        if s.startswith("jump "):
            t = s.split()[1]
            if t in label_at:
                succ[i].append(label_at[t])
        elif s.startswith("branch_zero ") or s.startswith("branch_eq "):
            m = re.search(r"-> (\S+)$", s)
            if m and m.group(1) in label_at:
                succ[i].append(label_at[m.group(1)])
            if i + 1 < n:
                succ[i].append(i + 1)
        elif s.startswith("return "):
            pass
        else:
            if i + 1 < n:
                succ[i].append(i + 1)
    return parsed, succ


def dead_indices(body, params):
    """Iteratively remove instructions whose defined value is dead, to fixpoint.
    Returns the set of original indices that are dead (removable)."""
    alive = list(range(len(body)))  # surviving original indices
    removed = set()
    while True:
        idxs = [i for i in range(len(body)) if i not in removed]
        sub = [body[i] for i in idxs]
        parsed, succ = build_cfg(sub)
        m = len(sub)
        live_out = [set() for _ in range(m)]
        live_in = [set() for _ in range(m)]
        changed = True
        while changed:
            changed = False
            for j in range(m - 1, -1, -1):
                lo = set()
                for s in succ[j]:
                    lo |= live_in[s]
                kind, defn, uses, _ = parsed[j]
                li = set(uses) | (lo - ({defn} if defn else set()))
                if lo != live_out[j] or li != live_in[j]:
                    live_out[j], live_in[j] = lo, li
                    changed = True
        newly = set()
        for j in range(m):
            kind, defn, uses, removable = parsed[j]
            if removable and defn and defn not in live_out[j] and defn not in params:
                newly.add(idxs[j])
        if not newly:
            break
        removed |= newly
    return removed
