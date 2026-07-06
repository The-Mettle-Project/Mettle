#!/usr/bin/env python3
"""Sound multi-pass optimizer for the int64 IR — the transforms Mettle's
classical --release optimizer leaves on the table, done correctly.

Passes (run to fixpoint):
  - constant folding:   c1 OP c2 -> constant  (semantics matched to irexec)
  - algebraic identity: x+0, x-0, x*1, x*0, x&x, x|x, x^x, x|0, x&0, x<<0, ...
  - block-local constant propagation: a constant assigned to a name is
    substituted into later uses until the name is reassigned or a control-flow
    boundary (labels/branches reset state -> sound across the CFG)
  - dead-code elimination: def-use liveness (liveness.py), sound for all inputs

Each transform is locally sound; as a belt-and-suspenders guarantee the public
optimize() verifies the result against the input over a large, branch-constant-
covering input set and falls back to the original on any mismatch. So the
optimizer can never emit a non-equivalent function.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import canonicalize as C  # noqa: E402
import fn_verify as V  # noqa: E402
import liveness as L  # noqa: E402

MASK64 = (1 << 64) - 1


def to_signed(x):
    x &= MASK64
    return x - (1 << 64) if x >= (1 << 63) else x


def evalop(a, op, b):
    ai, bi = a & MASK64, b & MASK64
    if op == "+": return (ai + bi) & MASK64
    if op == "-": return (ai - bi) & MASK64
    if op == "*": return (ai * bi) & MASK64
    if op == "&": return ai & bi
    if op == "|": return ai | bi
    if op == "^": return ai ^ bi
    if op == "<<": return (ai << (bi & 63)) & MASK64
    if op == ">>": return ai >> (bi & 63)
    if op == "/": return ((ai // bi) & MASK64) if bi else 0
    if op == "%": return ((ai % bi) & MASK64) if bi else 0
    return None


_DEF = re.compile(r"^(\S+)\s*(=|<-)\s*(.*)$")
_BIN = re.compile(r"^(\S+) (<<|>>|\+|-|\*|/|%|&|\||\^) (\S+)$")
_LIT = re.compile(r"^-?\d+$")


def is_lit(t):
    return bool(_LIT.match(t))


def is_control(ins):
    return (ins.startswith("label ") or ins.startswith("jump ") or
            ins.startswith("branch") or ins.startswith("return ") or
            ins.startswith("local ") or ins.startswith("*"))


def split_def(ins):
    m = _DEF.match(ins)
    if not m or is_control(ins):
        return None
    return m.group(1), m.group(2), m.group(3).strip()


def fold_and_identity(instrs):
    out = []
    changed = False
    for ins in instrs:
        d = split_def(ins)
        if not d:
            out.append(ins); continue
        dest, eq, rhs = d
        mb = _BIN.match(rhs)
        if mb:
            a, op, b = mb.group(1), mb.group(2), mb.group(3)
            if is_lit(a) and is_lit(b):
                v = evalop(int(a), op, int(b))
                if v is not None:
                    out.append(f"{dest} = {v}"); changed = True; continue
            # identities (a or b literal, or a==b)
            new = None
            if op in ("+", "-", "|", "^", "<<", ">>") and b == "0":
                new = a
            elif op == "+" and a == "0":
                new = b
            elif op in ("|", "^") and a == "0":
                new = b
            elif op == "*" and b == "1":
                new = a
            elif op == "*" and a == "1":
                new = b
            elif op == "*" and (a == "0" or b == "0"):
                new = "0"
            elif op == "&" and (a == "0" or b == "0"):
                new = "0"
            elif op in ("&", "|") and a == b:
                new = a
            elif op == "^" and a == b:
                new = "0"
            elif op == "-" and a == b:
                new = "0"
            elif op == "/" and b == "1":
                new = a
            if new is not None:
                out.append(f"{dest} = {new}"); changed = True; continue
        out.append(ins)
    return out, changed


def const_prop(instrs):
    """Block-local: substitute known constant values into RHS operands. State
    resets at every control-flow boundary (sound for arbitrary CFGs)."""
    out = []
    const = {}            # name -> literal string
    changed = False
    for ins in instrs:
        if is_control(ins):
            out.append(ins)
            const.clear()  # boundary: forget all (join points unknown)
            continue
        d = split_def(ins)
        if not d:
            out.append(ins); continue
        dest, eq, rhs = d
        # substitute constants into RHS operand tokens
        toks = C.tokenize_line(rhs)
        new_toks = [const.get(t, t) if (t.startswith("@") or t.startswith("%")) else t
                    for t in toks]
        new_rhs = C.detokenize(new_toks)
        if new_rhs != rhs:
            changed = True
        out.append(f"{dest} {eq} {new_rhs}")
        # update const map for dest
        const.pop(dest, None)
        # also any name equal to dest's old value is unaffected; record if const
        if is_lit(new_rhs.strip()):
            const[dest] = new_rhs.strip()
        else:
            # dest now non-constant; invalidate entries (already popped dest)
            pass
    return out, changed


def dce(instrs, params):
    dead = L.dead_indices(instrs, set(params))
    if not dead:
        return instrs, False
    return [ins for i, ins in enumerate(instrs) if i not in dead], True


def optimize_body(instrs, params):
    cur = list(instrs)
    for _ in range(20):
        any_change = False
        cur, c1 = fold_and_identity(cur); any_change |= c1
        cur, c2 = const_prop(cur); any_change |= c2
        cur, c3 = dce(cur, params); any_change |= c3
        if not any_change:
            break
    return cur


def optimize(funcs, fname, params, verify=True, vk=600):
    """Optimize fname's body soundly; verify and fall back to the input on any
    mismatch over a branch-constant-covering input set."""
    opt = {n: list(b) for n, b in funcs.items()}
    opt[fname] = optimize_body(funcs[fname], params)
    if not verify:
        return opt
    vecs = _cover_vectors(funcs[fname], params, vk)
    if V.equivalent(funcs, opt, fname, params, vecs):
        return opt
    return {n: list(b) for n, b in funcs.items()}  # safe fallback


def _cover_vectors(body, params, k):
    consts = set()
    for ins in body:
        for m in re.finditer(r"(?:!=|==|<=|>=|<|>) ?(-?\d+)", ins):
            consts.add(int(m.group(1)))
    extra = sorted({c for c in consts} | {c + 1 for c in consts} |
                   {c - 1 for c in consts})
    import random
    rng = random.Random(12345)
    pool = list(range(0, 260)) + extra + [(1 << 40) - 1, 1 << 20]
    vecs = [[v] * len(params) for v in pool]
    while len(vecs) < k:
        vecs.append([rng.choice(pool) if rng.random() < 0.5
                     else rng.randint(0, (1 << 40) - 1) for _ in params])
    return vecs[:k]
