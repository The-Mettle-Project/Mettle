#!/usr/bin/env python3
"""Affine-form simplification — the cross-instruction algebra Mettle's classical
optimizer misses, done SOUNDLY.

Each value is tracked as an affine combination of *versioned* base variables:
    value = const + Σ coeff_i·base_i
The base variables are SSA-style version tokens ("@p2#0", "@p2#1", ...): every
(re)assignment of a name mints a fresh version, so a form referring to the OLD
value of a reassigned name can never be mis-emitted as the (now different)
current name. (`(p2+p1)-p1` after `p2 := p2+p1` is NOT the current p2.)

Coefficients and constants are kept as TRUE integers (never reduced), so an
"exact" form equals the value in the full machine ring Z/2^64 and may be
re-emitted WITHOUT a trailing mask: chains collapse soundly —
(x+5)+3 → x+8, x+x+x → x*3, (x+y)-y → x, x-x → 0.

Two regimes per form:
  - exact == True: form == value in Z/2^64. Built only from variables/constants
    and +,-,*const,<<const with no lossy `& MASK` over a wide value. Safe to
    re-emit unmasked.
  - exact == False: a wide value was masked, so only value mod 2^40 is known.
    Then we emit only a copy of an existing <2^40 variable, never a recombined
    unmasked expression.

`bits40` tracks version tokens provably < 2^40 (params, masked results, copies)
so a mask-dropping copy is emitted only when sound.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from sopt import is_control, split_def, is_lit, _BIN

BITS = 40
MOD = 1 << BITS
M40 = MOD - 1
MASK64 = (1 << 64) - 1

# A form is (coeffs, const, exact):
#   coeffs: dict base_token -> TRUE integer coefficient
#   const:  TRUE integer additive constant
#   exact:  True iff the form equals the value in Z/2^64


def _is_widemask(b):
    """`& b` is identity mod 2^40 iff b has all low 40 bits set."""
    return is_lit(b) and int(b) >= M40 and (int(b) + 1) & int(b) == 0


def _add(x, y, s=1):
    cx, kx, ex = x
    cy, ky, ey = y
    d = dict(cx)
    for v, c in cy.items():
        d[v] = d.get(v, 0) + s * c
        if d[v] == 0:
            del d[v]
    return (d, kx + s * ky, ex and ey)


def _scale(x, k):
    """Scale by a TRUE integer k."""
    c, kc, e = x
    return ({v: co * k for v, co in c.items()}, kc * k, e)


def simplify(instrs, params=None):
    params = list(params or [])
    out, changed = [], False
    env = {}                                  # base token -> form
    ver = {}                                  # name -> current version int
    bits40 = set()
    floats = set()                            # names/temps of float type (persistent)

    def curtok(name):
        return f"{name}#{ver.get(name, 0)}"

    def reset():
        env.clear()
        ver.clear()
        bits40.clear()
        for p in params:
            bits40.add(curtok(p))             # params: masked-input domain (<2^40)

    def atom(tok):
        if is_lit(tok):
            return ({}, int(tok), True)
        bt = curtok(tok)
        return env.get(bt, ({bt: 1}, 0, True))

    def fits40(tok):
        if is_lit(tok):
            return 0 <= int(tok) < MOD
        return curtok(tok) in bits40

    def rhs_form(rhs):
        m = _BIN.match(rhs)
        if not m:
            s = rhs.strip()
            return atom(s) if re.fullmatch(r"[@%][\w.$]+|-?\d+", s) else None
        a, op, b = m.group(1), m.group(2), m.group(3)
        if op == "&" and _is_widemask(b):
            fa = atom(a)
            if fits40(a):
                return fa                     # mask is a no-op: value unchanged
            return (fa[0], fa[1], False)      # lossy: only value mod 2^40 known
        fa, fb = atom(a), atom(b)
        if op == "+":
            return _add(fa, fb, 1)
        if op == "-":
            return _add(fa, fb, -1)
        if op == "*":
            if not fa[0]:
                return _scale(fb, fa[1])
            if not fb[0]:
                return _scale(fa, fb[1])
            return None
        if op == "<<" and is_lit(b):
            return _scale(fa, 1 << (int(b) & 63))
        return None

    def rhs_fits40(rhs):
        s = rhs.strip()
        if is_lit(s):
            return 0 <= int(s) < MOD
        if re.fullmatch(r"[@%][\w.$]+", s):
            return fits40(s)
        m = _BIN.match(rhs)
        if m:
            a, op, b = m.group(1), m.group(2), m.group(3)
            if op == "&" and is_lit(b) and 0 <= int(b) < MOD:
                return True
            if op == "&" and _is_widemask(b):
                return fits40(a)
        return False

    def emit(dest, form, orig_is_mask):
        coeffs, c, exact = form
        vs = {v: k for v, k in coeffs.items() if k != 0}
        if not vs:
            return f"{dest} = {c & MASK64}" if exact else None
        if len(vs) != 1:
            return None
        bt, k = next(iter(vs.items()))
        name = bt.rsplit("#", 1)[0]
        if curtok(name) != bt:                # value no longer held by this name
            return None
        if k == 1 and c == 0:
            if exact:
                return f"{dest} <- {name}"     # form == value in Z/2^64
            if orig_is_mask and bt in bits40:
                return f"{dest} <- {name}"     # dest masked, value mod 2^40 == name<2^40
            return None
        if not (exact and not orig_is_mask):
            return None                       # unmasked recombination needs exact
        if k == 1 and c != 0:
            return (f"{dest} = {name} + {c}" if c > 0
                    else f"{dest} = {name} - {-c}")
        if c == 0 and k > 1:
            return f"{dest} = {name} * {k}"    # scaled collapse (e.g. x+x+x -> x*3)
        return None

    def ntoks(s):
        return len(re.findall(r"[@%][\w.$]*|\d+", s))

    _FLOATLIT = re.compile(r"\d+\.\d+")

    reset()
    for ins in instrs:
        # record float-typed locals; their type info must survive env resets
        ml = re.match(r"^local (@\S+) : .*\bfloat", ins)
        if ml:
            floats.add(ml.group(1))
        if is_control(ins) or ins.startswith("branch") or ins.startswith("*"):
            out.append(ins)
            reset()
            continue
        d = split_def(ins)
        if not d:
            out.append(ins)
            continue
        dest, eq, rhs = d
        # Type gate: never apply integer-ring algebra to a float value. A value
        # is float if the op carries a (float..) marker, has a float literal, or
        # reads a float operand. Affine reasoning is sound only in Z/2^64.
        is_float = ("(float" in rhs or _FLOATLIT.search(rhs) or
                    any(t in floats for t in re.findall(r"[@%][\w.$]+", rhs)))
        if is_float:
            floats.add(dest)
            ver[dest] = ver.get(dest, 0) + 1
            bits40.discard(curtok(dest))
            env[curtok(dest)] = ({curtok(dest): 1}, 0, True)  # opaque base var
            out.append(ins)
            continue
        mb = _BIN.match(rhs)
        orig_is_mask = bool(mb and mb.group(2) == "&" and _is_widemask(mb.group(3)))
        form = rhs_form(rhs)
        fits = rhs_fits40(rhs)
        ver[dest] = ver.get(dest, 0) + 1       # mint fresh version for dest
        nbt = curtok(dest)
        env[nbt] = form if form is not None else ({nbt: 1}, 0, True)
        bits40.discard(nbt)
        if fits:
            bits40.add(nbt)
        rendered = emit(dest, form, orig_is_mask) if form is not None else None
        if (rendered is not None and rendered != ins
                and ntoks(rendered.split(None, 2)[-1]) <= ntoks(rhs)):
            out.append(rendered)
            changed = True
        else:
            out.append(ins)
    return out, changed
