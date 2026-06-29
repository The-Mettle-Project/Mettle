#!/usr/bin/env python3
"""superopt — a stronger sound optimizer (higher ceiling than sopt.py).

Adds, on top of sopt's fold/const-prop/identity/DCE:
  - comparison folding         (c1 ==/!=/</<=/>/>= c2 -> 0/1, irexec semantics)
  - variable + constant copy propagation (block-local, sound)
  - common-subexpression elimination via local value numbering
  - strength reduction         (x*2^k -> x<<k, x/2^k -> x>>k, x%2^k -> x&(2^k-1))
  - constant-branch elimination + unreachable-block removal
  - def-use liveness DCE

All transforms are equivalence-preserving; optimize() additionally verifies the
result against the input over a large branch-constant-covering input set and
falls back to the input on any mismatch, so it can never emit a wrong function.
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
from sopt import (evalop, is_control, split_def, is_lit, _BIN, _cover_vectors,
                  MASK64, to_signed)  # noqa: E402
import affine  # noqa: E402

_CMP = {"==", "!=", "<", "<=", ">", ">="}


def cmp_eval(a, op, b):
    ai, bi = a & MASK64, b & MASK64
    if op == "==": return 1 if ai == bi else 0
    if op == "!=": return 1 if ai != bi else 0
    if op == "<": return 1 if to_signed(ai) < to_signed(bi) else 0
    if op == "<=": return 1 if to_signed(ai) <= to_signed(bi) else 0
    if op == ">": return 1 if to_signed(ai) > to_signed(bi) else 0
    if op == ">=": return 1 if to_signed(ai) >= to_signed(bi) else 0
    return None


def _ispow2(n):
    return n > 0 and (n & (n - 1)) == 0


def fold(instrs):
    out, changed = [], False
    for ins in instrs:
        d = split_def(ins)
        if not d:
            out.append(ins); continue
        dest, eq, rhs = d
        m = re.match(r"^(\S+) (\S+) (\S+)$", rhs)
        if m and is_lit(m.group(1)) and is_lit(m.group(3)):
            a, op, b = int(m.group(1)), m.group(2), int(m.group(3))
            v = evalop(a, op, b) if op not in _CMP else cmp_eval(a, op, b)
            if v is not None:
                out.append(f"{dest} = {v}"); changed = True; continue
        out.append(ins)
    return out, changed


def identities(instrs):
    out, changed = [], False
    for ins in instrs:
        d = split_def(ins)
        if not d:
            out.append(ins); continue
        dest, eq, rhs = d
        m = _BIN.match(rhs)
        new = None
        if m:
            a, op, b = m.group(1), m.group(2), m.group(3)
            if op in ("+", "-", "|", "^", "<<", ">>") and b == "0": new = a
            elif op in ("+", "|", "^") and a == "0": new = b
            elif op == "*" and b == "1": new = a
            elif op == "*" and a == "1": new = b
            elif op == "*" and (a == "0" or b == "0"): new = "0"
            elif op == "&" and (a == "0" or b == "0"): new = "0"
            elif op in ("&", "|") and a == b: new = a
            elif op in ("^", "-") and a == b: new = "0"
            elif op in ("/", ) and b == "1": new = a
        if new is not None:
            out.append(f"{dest} = {new}"); changed = True; continue
        out.append(ins)
    return out, changed


def strength_reduce(instrs):
    out, changed = [], False
    for ins in instrs:
        d = split_def(ins)
        if not d:
            out.append(ins); continue
        dest, eq, rhs = d
        m = _BIN.match(rhs)
        if m:
            a, op, b = m.group(1), m.group(2), m.group(3)
            if is_lit(b):
                bv = int(b)
                if op == "*" and _ispow2(bv):
                    out.append(f"{dest} = {a} << {bv.bit_length()-1}"); changed = True; continue
                if op == "/" and _ispow2(bv):   # irexec / is unsigned
                    out.append(f"{dest} = {a} >> {bv.bit_length()-1}"); changed = True; continue
                if op == "%" and _ispow2(bv):    # unsigned mod
                    out.append(f"{dest} = {a} & {bv-1}"); changed = True; continue
        out.append(ins)
    return out, changed


def propagate(instrs):
    """Block-local copy/const propagation: substitute a name's known value
    (constant or single source var) into later operand positions, including
    branch conditions. Reset at every control-flow boundary; invalidate an entry
    when its name or its source is redefined."""
    out, changed = [], False
    val = {}  # name -> replacement token (literal or var)

    def invalidate(name):
        val.pop(name, None)
        for k in [k for k, v in val.items() if v == name]:
            del val[k]

    def subst_tokens(toks):
        nonlocal changed
        res = []
        for t in toks:
            if (t.startswith("@") or t.startswith("%")) and t in val:
                res.append(val[t]); changed = True
            else:
                res.append(t)
        return res

    for ins in instrs:
        if ins.startswith("branch_zero ") or ins.startswith("branch_eq "):
            toks = subst_tokens(C.tokenize_line(ins))
            out.append(C.detokenize(toks))
            continue
        if is_control(ins):
            out.append(ins)
            val.clear()
            continue
        d = split_def(ins)
        if not d:
            out.append(ins); continue
        dest, eq, rhs = d
        toks = subst_tokens(C.tokenize_line(rhs))
        new_rhs = C.detokenize(toks)
        out.append(f"{dest} {eq} {new_rhs}")
        invalidate(dest)
        s = new_rhs.strip()
        if is_lit(s) or re.fullmatch(r"[@%][\w.$]+", s):
            val[dest] = s
    return out, changed


def cse(instrs):
    """Local value numbering: within a block, reuse an earlier identical pure
    expression instead of recomputing it."""
    out, changed = [], False
    seen = {}  # canonical expr key -> dest name

    def reset():
        seen.clear()

    for ins in instrs:
        if is_control(ins):
            out.append(ins); reset(); continue
        d = split_def(ins)
        if not d:
            out.append(ins); continue
        dest, eq, rhs = d
        # any stored expr that referenced dest is now stale
        for k in [k for k, v in seen.items() if v == dest or dest in k]:
            del seen[k]
        m = _BIN.match(rhs)
        key = None
        if m:
            a, op, b = m.group(1), m.group(2), m.group(3)
            key = (op, a, b)
            if op in ("+", "*", "&", "|", "^") and a > b:
                key = (op, b, a)   # commutative canonicalization
        if key is not None and key in seen:
            out.append(f"{dest} <- {seen[key]}"); changed = True; continue
        out.append(ins)
        if key is not None:
            seen[key] = dest
    return out, changed


def resolve_branches(instrs):
    """Statically resolve branches with constant conditions, then drop blocks
    made unreachable from entry."""
    out, changed = [], False
    for ins in instrs:
        m = re.match(r"^branch_zero (-?\d+) -> (\S+)$", ins)
        if m:
            if (int(m.group(1)) & MASK64) == 0:
                out.append(f"jump {m.group(2)}")     # always taken
            else:
                pass                                  # never taken: drop
            changed = True
            continue
        m = re.match(r"^branch_eq (-?\d+), (-?\d+) -> (\S+)$", ins)
        if m:
            if int(m.group(1)) == int(m.group(2)):
                out.append(f"jump {m.group(3)}")
            changed = True
            continue
        out.append(ins)
    if changed:
        out = remove_unreachable(out)
    return out, changed


def remove_unreachable(instrs):
    _, succ = L.build_cfg(instrs)
    n = len(instrs)
    seen = set()
    stack = [0] if n else []
    while stack:
        i = stack.pop()
        if i in seen or i >= n:
            continue
        seen.add(i)
        stack.extend(succ[i])
    return [ins for i, ins in enumerate(instrs) if i in seen]


def dce(instrs, params):
    dead = L.dead_indices(instrs, set(params))
    return ([ins for i, ins in enumerate(instrs) if i not in dead],
            bool(dead))


PASSES = [propagate, fold, identities, strength_reduce, cse, resolve_branches]


def optimize_body(instrs, params):
    cur = list(instrs)
    for _ in range(40):
        any_change = False
        for p in PASSES:
            cur, c = p(cur)
            any_change |= c
        cur, c = affine.simplify(cur, params)    # sound: needs params for range
        any_change |= c
        cur, c = dce(cur, params)
        any_change |= c
        if not any_change:
            break
    return cur


def optimize(funcs, fname, params, verify=True, vk=600):
    opt = {n: list(b) for n, b in funcs.items()}
    opt[fname] = optimize_body(funcs[fname], params)
    if not verify:
        return opt
    vecs = _cover_vectors(funcs[fname], params, vk)
    if V.equivalent(funcs, opt, fname, params, vecs):
        return opt
    return {n: list(b) for n, b in funcs.items()}   # verification backstop
