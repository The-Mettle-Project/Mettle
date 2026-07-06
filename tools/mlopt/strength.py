#!/usr/bin/env python3
"""Constant-multiply strength superoptimizer — decompose `x * C` into the minimal
shift/add/sub chain via non-adjacent form (NAF), applied only when it is cheaper
than the multiply on the latency cost model. Classical only strength-reduces
powers of two; this handles ANY constant (x*3,*5,*7,*9,*17,*31,...), which is
what real hashing/indexing/LCG code is full of.

Soundness: x*C = Σ ±(x << k_i) exactly in Z/2^64 (shifts are *2^k, multiply
distributes over add) — sound by construction, no input sampling.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from sopt import split_def, is_lit, _BIN
from cost import OP_COST


def naf(c):
    """Non-adjacent form: c = Σ sign·2^shift with minimal nonzero digits."""
    terms = []
    i = 0
    while c > 0:
        if c & 1:
            d = 2 - (c & 3)            # +1 if c%4==1, -1 if c%4==3
            terms.append((1 if d > 0 else -1, i))
            c -= d
        c >>= 1
        i += 1
    return terms                       # ascending shift; leading (largest) is +1


def chain_cost(terms):
    shifts = sum(1 for _, k in terms if k > 0)
    adds = max(0, len(terms) - 1)
    return shifts + adds


def decompose(dest, x, C):
    """IR sequence computing dest = x*C via shifts/adds/subs, or None to skip."""
    if C < 0:
        return None
    if C == 0:
        return [f"{dest} = 0"]
    if C == 1:
        return [f"{dest} <- {x}"]
    terms = naf(C)
    pre, items = [], []
    for (s, k) in terms:
        if k == 0:
            items.append((s, x))
        else:
            t = f"{dest}s{k}"
            pre.append(f"{t} = {x} << {k}")
            items.append((s, t))
    items.sort(key=lambda it: -it[0])  # a positive term first (NAF has one)
    if items[0][0] < 0:
        return None
    if len(items) == 1:
        k = terms[0][1]
        return [f"{dest} = {x} << {k}"]
    cur = items[0][1]
    seq = []
    rest = items[1:]
    for i, (s, name) in enumerate(rest):
        op = "+" if s > 0 else "-"
        out = dest if i == len(rest) - 1 else f"{dest}c{i}"
        seq.append(f"{out} = {cur} {op} {name}")
        cur = out
    return pre + seq


def _const_mul(rhs):
    m = _BIN.match(rhs)
    if not m or m.group(2) != "*":
        return None
    a, _, b = m.group(1), m.group(2), m.group(3)
    if is_lit(b) and not is_lit(a):
        return a, int(b)
    if is_lit(a) and not is_lit(b):
        return b, int(a)
    return None


def reduce_instr(ins):
    """If `ins` is dest = x*C and a shift/add chain is cheaper, return the
    replacement instruction list; else None."""
    d = split_def(ins)
    if not d:
        return None
    dest, _, rhs = d
    cm = _const_mul(rhs)
    if not cm:
        return None
    x, C = cm
    if C < 0:
        return None
    terms = naf(C)
    if chain_cost(terms) >= OP_COST["*"]:
        return None                    # multiply is cheaper; keep it
    return decompose(dest, x, C)


def strength_reduce(instrs):
    """Rewrite every cost-reducing constant multiply. 1:1-expanding (one instr ->
    several), so returns a new instruction list."""
    out, changed = [], False
    for ins in instrs:
        rep = reduce_instr(ins)
        if rep is not None and rep != [ins]:
            out.extend(rep)
            changed = True
        else:
            out.append(ins)
    return out, changed


if __name__ == "__main__":
    for C in [3, 5, 7, 9, 17, 22, 31, 33, 260, 1000, 1000000, 1103515245]:
        terms = naf(C)
        rep = decompose("%t", "@x", C)
        print(f"x*{C}: cost {chain_cost(terms)} vs mul {OP_COST['*']} -> "
              f"{'reduce' if chain_cost(terms) < OP_COST['*'] else 'keep'}")
        if chain_cost(terms) < OP_COST["*"]:
            for r in rep:
                print("     ", r)
