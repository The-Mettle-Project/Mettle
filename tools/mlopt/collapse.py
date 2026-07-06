"""Verified semantic collapse — recognize that a (sub)expression's VALUE equals a
much simpler in-scope value, even when it is syntactically tangled, and replace it.
This is the class GCC -O3 misses: it does syntactic GVN/peephole, but it does not
prove that e.g. ((y|x)-((y|x)-x)) == x. We find the simplest equivalent value by
trying a small library of candidate forms and verifying over a large structured
input set (random + boundaries + bit patterns); pure-bitwise equivalences are
additionally bit-exhaustively sound. The verifier backstops; on any mismatch the
original is kept.
"""
import os
import random
import re
import sys

MASK = (1 << 64) - 1
RNG = random.Random(99)
# verification inputs: boundaries + bit walks + many random 64-bit values
_BASE = [0, 1, 2, 3, 5, 255, 256, 1023, 1 << 20, 1 << 32, 1 << 63, MASK,
         MASK - 1, (1 << 40) - 1]
INPUTS = [(a, b) for a in _BASE for b in _BASE]
INPUTS += [(RNG.randint(0, MASK), RNG.randint(0, MASK)) for _ in range(160)]


def _ev_rhs(rhs, env):
    t = rhs.split()
    def v(z):
        if re.fullmatch(r"-?\d+", z):
            return int(z) & MASK
        return env[z]
    if len(t) == 1:
        return v(t[0])
    a, op, b = v(t[0]), t[1], v(t[2])
    if op == "+": return (a + b) & MASK
    if op == "-": return (a - b) & MASK
    if op == "*": return (a * b) & MASK
    if op == "&": return a & b
    if op == "|": return a | b
    if op == "^": return a ^ b
    if op == "<<": return (a << (b & 63)) & MASK
    if op == ">>": return a >> (b & 63)
    raise ValueError(op)


def eval_body(body, params, x, y):
    env = {params[0]: x}
    if len(params) > 1:
        env[params[1]] = y
    ret = 0
    for ins in body:
        if ins.startswith("return "):
            ret = env[ins.split()[1]]
            break
        lhs, _, rhs = (ins.partition(" <- ") if " <- " in ins
                       else ins.partition(" = "))
        env[lhs.strip()] = _ev_rhs(rhs.strip(), env)
    return ret


def _candidates(params):
    """Simple value forms to test, cheapest first: a param, 0, then 1-op combos."""
    p = params
    cands = [[("ret", p[0])]]
    if len(p) > 1:
        cands.append([("ret", p[1])])
    cands.append([("c", "0", None, None), ("ret", "%c")])  # constant 0
    if len(p) > 1:
        for op in ("|", "&", "^", "+", "-"):
            cands.append([("c", p[0], op, p[1]), ("ret", "%c")])
            if op in ("-",):
                cands.append([("c", p[1], op, p[0]), ("ret", "%c")])
    return cands


def _materialize(cand):
    body = []
    retname = None
    for item in cand:
        if item[0] == "ret":
            retname = item[1]
        else:                       # ('c', a, op, b)  -> %c = a op b  (or %c = a)
            _, a, op, b = item
            if op is None:
                body.append(f"%c = {a}")
            else:
                body.append(f"%c = {a} {op} {b}")
    return body, retname


def collapse_value(body, params):
    """If the function's returned value equals a simpler form (verified over the
    input set), return the simpler body; else None."""
    ref = [eval_body(body, params, x, y) for (x, y) in INPUTS]
    for cand in _candidates(params):
        cbody, ret = _materialize(cand)
        cb = cbody + [f"return {ret}"]
        try:
            ok = all(eval_body(cb, params, x, y) == ref[i]
                     for i, (x, y) in enumerate(INPUTS))
        except Exception:
            ok = False
        if ok:
            return cb
    return None


if __name__ == "__main__":
    import json
    for line in sys.stdin:
        r = json.loads(line)
        c = collapse_value(r["instrs"], r["params"])
        print(len(r["instrs"]), "->", len(c) if c else "no-collapse")
