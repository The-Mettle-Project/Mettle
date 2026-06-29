#!/usr/bin/env python3
"""Minimal interpreter for Mettle optimized-IR dumps (the `--dump-ir` sidecar).

Purpose: given a divergent program, run its optimized IR through this reference
interpreter. If the interpreter's result matches the DEBUG (unoptimized) program
output, the optimized IR is semantically correct and the miscompile lives in the
backend/codegen. If it matches the (wrong) RELEASE output, an IR optimization
pass produced incorrect IR. This splits "optimizer bug" from "codegen bug"
without reading disassembly.

Supports the subset the fuzzer emits: int64/uint64 scalars, local arrays,
&addr/<<shift/load/store, binary ops, casts, conditional/unconditional branches,
labels, and direct calls into other functions in the same dump. Everything is
modeled as 64-bit two's-complement; masks in the program keep values bounded.

Usage: python irexec.py <dump.ir> [--entry main]
"""
import argparse
import re
import sys

MASK64 = (1 << 64) - 1


def to_signed(x):
    x &= MASK64
    return x - (1 << 64) if x >= (1 << 63) else x


class Func:
    def __init__(self, name):
        self.name = name
        self.insns = []          # list of (idx, raw_text)
        self.labels = {}         # label name -> position in insns


def parse_lines(lines):
    funcs = {}
    cur = None
    for line in lines:
        line = line.rstrip("\n")
        m = re.match(r"^function (\S+)", line)
        if m:
            cur = Func(m.group(1))
            funcs[cur.name] = cur
            continue
        if cur is None:
            continue
        if line.strip() == "}":
            cur = None
            continue
        m = re.match(r"^\s*(\d+):\s+(.*)$", line)
        if not m:
            continue
        text = m.group(2).strip()
        lm = re.match(r"^label (\S+)$", text)
        if lm:
            cur.labels[lm.group(1)] = len(cur.insns)
        cur.insns.append((int(m.group(1)), text))
    return funcs


def parse(path):
    with open(path) as f:
        return parse_lines(f)


def run_text(text, entry="main", max_steps=5_000_000):
    """In-process evaluation of a dump string. Returns the low byte of the
    entry's result, or None. Avoids per-call process startup for bulk use.
    A tight max_steps makes deletion probes that induce infinite loops fail
    fast (return None) instead of burning the full step budget."""
    try:
        result = Machine(parse_lines(text.splitlines()),
                         max_steps=max_steps).run(entry)
    except Exception:
        return None
    return (result & 0xFF) if result is not None else None


class Machine:
    def __init__(self, funcs, trace=False, max_steps=5_000_000):
        self.funcs = funcs
        self.trace = trace
        self.max_steps = max_steps
        self._steps = 0   # GLOBAL budget across recursive run() calls

    def param_symbols(self, fn):
        """Infer parameter @symbols: those read as operands before any local
        decl or assignment defines them. Returns them in first-seen order."""
        if hasattr(fn, "_params"):
            return fn._params
        declared = set()
        params = []
        for _, t in fn.insns:
            md = re.match(r"^local (@\S+)", t)
            if md:
                declared.add(md.group(1))
                continue
            ma = re.match(r"^(@\S+) (?:<-|=)", t)      # assignment target
            assigned_target = ma.group(1) if ma else None
            for sym in re.findall(r"@\w+", t):
                if sym not in declared and sym != assigned_target and sym not in params:
                    params.append(sym)
            if assigned_target:
                declared.add(assigned_target)
        fn._params = params
        return params

    def run(self, name, args=None):
        fn = self.funcs[name]
        env = {}            # name -> value (scalars and temps)
        if args:
            for sym, v in zip(self.param_symbols(fn), args):
                env[sym] = v
        mem = {}            # (array_name, byte_offset) -> value ; addr encoded as ('arr', name)
        # crude: arrays live in env as dict {offset:value}; &arr yields ('A', name)
        arrays = {}

        def val(tok):
            tok = tok.strip()
            if re.match(r"^-?\d+$", tok):
                return int(tok)
            if tok.startswith("@") or tok.startswith("%"):
                return env.get(tok, 0)
            return env.get(tok, 0)

        pc = 0
        insns = fn.insns
        while pc < len(insns):
            self._steps += 1
            if self._steps > self.max_steps:
                raise RuntimeError("step limit (infinite loop / recursion?)")
            _, t = insns[pc]

            # local @x : T  (decl) / local @arr : T[n]
            m = re.match(r"^local (@\S+) : (.+)$", t)
            if m:
                if "[" in m.group(2):
                    arrays[m.group(1)] = {}
                pc += 1
                continue
            if t == "nop" or t.startswith("label "):
                pc += 1
                continue
            # return X
            m = re.match(r"^return (\S+)$", t)
            if m:
                if self.trace:
                    for k in sorted(env):
                        if k.startswith("@"):
                            print(f"  {k} = {to_signed(env[k])}", file=sys.stderr)
                return to_signed(val(m.group(1)))
            # jump L
            m = re.match(r"^jump (\S+)$", t)
            if m:
                pc = fn.labels[m.group(1)]
                continue
            # branch_zero X -> L
            m = re.match(r"^branch_zero (\S+) -> (\S+)$", t)
            if m:
                pc = fn.labels[m.group(2)] if (val(m.group(1)) & MASK64) == 0 else pc + 1
                continue
            # branch_eq A, B -> L
            m = re.match(r"^branch_eq (\S+), (\S+) -> (\S+)$", t)
            if m:
                pc = fn.labels[m.group(3)] if val(m.group(1)) == val(m.group(2)) else pc + 1
                continue
            # dest <- &@arr
            m = re.match(r"^(\S+) <- &(@\S+)$", t)
            if m:
                env[m.group(1)] = ("A", m.group(2), 0)
                pc += 1
                continue
            # dest <- *ptr [size]
            m = re.match(r"^(\S+) <- \*(\S+)(?: \[\d+\])?$", t)
            if m:
                ptr = env.get(m.group(2))
                if isinstance(ptr, tuple) and ptr[0] == "A":
                    env[m.group(1)] = arrays[ptr[1]].get(ptr[2], 0)
                else:
                    env[m.group(1)] = 0
                pc += 1
                continue
            # *ptr <- val [size]   OR   *ptr = ...
            m = re.match(r"^\*(\S+) <- (\S+)(?: \[\d+\])?$", t)
            if m:
                ptr = env.get(m.group(1))
                if isinstance(ptr, tuple) and ptr[0] == "A":
                    arrays[ptr[1]][ptr[2]] = val(m.group(2)) & MASK64
                pc += 1
                continue
            # dest = base + (i<<k) style pointer arith handled as tuple offset:
            # dest = ptrtuple + offsetexpr   -> adjust tuple
            # general binary:  dest = A op B   |  dest <- A
            m = re.match(r"^(\S+) <- (\S+)$", t)
            if m:
                env[m.group(1)] = val(m.group(2))
                pc += 1
                continue
            m = re.match(r"^(\S+) = (.+)$", t)
            if m:
                dest, rhs = m.group(1), m.group(2)
                env[dest] = self.eval_rhs(rhs, val, env, arrays)
                pc += 1
                continue
            # call:  dest = f(args)
            pc += 1
        return None

    def eval_rhs(self, rhs, val, env, arrays):
        # cast: (int32)X  / (int64)X / (uint64)X
        m = re.match(r"^\((u?int\d+)\)(\S+)$", rhs)
        if m:
            v = val(m.group(2))
            ty = m.group(1)
            if ty == "int32":
                v &= 0xFFFFFFFF
                return v - (1 << 32) if v >= (1 << 31) else v
            return v
        # call: f(arg)
        m = re.match(r"^(\w+)\((.*)\)$", rhs)
        if m and m.group(1) in self.funcs:
            arg = m.group(2).strip()
            return self.run(m.group(1), [val(arg)] if arg else [])
        # pointer + scaled index: tuple + int  -> shift handled where '<<' builds offset
        # binary op
        m = re.match(r"^(\S+) (<<|>>|\+|-|\*|/|%|&|\||\^|==|!=|<|<=|>|>=) (\S+)$", rhs)
        if m:
            a = val(m.group(1)); op = m.group(2); b = val(m.group(3))
            # pointer arithmetic: a is ('A', name, off)
            if isinstance(a, tuple) and a[0] == "A":
                return ("A", a[1], a[2] + (b & MASK64))
            ai, bi = a & MASK64, b & MASK64
            if op == "+": return (ai + bi) & MASK64
            if op == "-": return (ai - bi) & MASK64
            if op == "*": return (ai * bi) & MASK64
            if op == "&": return ai & bi
            if op == "|": return ai | bi
            if op == "^": return ai ^ bi
            if op == "<<": return (ai << (bi & 63)) & MASK64
            if op == ">>": return ai >> (bi & 63)            # logical (uint context)
            if op == "/": return (ai // bi) & MASK64 if bi else 0
            if op == "%": return (ai % bi) & MASK64 if bi else 0
            if op == "==": return 1 if ai == bi else 0
            if op == "!=": return 1 if ai != bi else 0
            if op == "<": return 1 if to_signed(ai) < to_signed(bi) else 0
            if op == "<=": return 1 if to_signed(ai) <= to_signed(bi) else 0
            if op == ">": return 1 if to_signed(ai) > to_signed(bi) else 0
            if op == ">=": return 1 if to_signed(ai) >= to_signed(bi) else 0
        return val(rhs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ir")
    ap.add_argument("--entry", default="main")
    ap.add_argument("--trace", action="store_true")
    args = ap.parse_args()
    funcs = parse(args.ir)
    m = Machine(funcs, trace=args.trace)
    result = m.run(args.entry)
    # main returns int32; exit code is low byte unsigned
    print(result & 0xFF if result is not None else "None")


if __name__ == "__main__":
    sys.exit(main())
