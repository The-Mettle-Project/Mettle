#!/usr/bin/env python3
"""Diverse global-redundancy generator — forces the model to learn the ABSTRACT
GVN condition (a dominating same-expression def with operands unmodified since)
instead of surface features.

Diagnosis showed the model overfit to gen_scan's single shape (`param + i`): it
fired on `@a+@i` but ignored `@out+@read`, distant chains, non-`+` ops, etc.
This generator varies EVERYTHING that shouldn't matter:
  * operands: any mix of params, loop-invariant locals, updated locals, counters
  * operator: + - * ^ | &  (any pure binary)
  * recomputation distance: immediate, across a branch, across the loop back-edge,
    in nested blocks, after the merge
  * chain reuse: reuse from the original def AND from an intermediate reuse
  * NEGATIVES: sometimes an operand is reassigned between the two computations, so
    the expression is NOT redundant and must be kept (teaches the kill boundary)
All int64, masked to 40 bits, self-contained -> irexec-verifiable.
"""
import random
import sys

MASK = (1 << 40) - 1
OPS = ["+", "-", "*", "^", "|", "&"]


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.seed = seed
        self.L = []
        self.vc = 0
        self.d = 1
        self.names = []          # currently-live value names
        self.invariant = set()   # names not reassigned inside loops

    def fresh(self, invariant=False):
        self.vc += 1
        n = f"v{self.vc}"
        self.names.append(n)
        if invariant:
            self.invariant.add(n)
        return n

    def emit(self, s):
        self.L.append("  " * self.d + s)

    def operand(self):
        return self.r.choice(self.names)

    def masked(self, e):
        return f"(({e}) & {MASK})"

    def rand_expr(self):
        a, b = self.operand(), self.operand()
        op = self.r.choice(OPS)
        return (op, a, b)

    def expr_str(self, e, masked=True):
        op, a, b = e
        return self.masked(f"{a} {op} {b}") if masked else f"{a} {op} {b}"

    def compute(self, e, masked=True):
        v = self.fresh()
        self.emit(f"var {v}: int64 = {self.expr_str(e, masked)};")
        return v

    def use(self, acc, v):
        # mask the operand when folding into acc, so unmasked redundant temps
        # (real-code address arithmetic) are safe to accumulate
        op = self.r.choice(["+", "^", "-", "|"])
        self.emit(f"{acc} = ({acc} {op} ({v} & {MASK})) & {MASK};")

    def redundancy(self, acc, depth):
        """Compute an expression, then RECOMPUTE it across a control-flow boundary
        (the hard, real-code GVN case block-local CSE cannot see)."""
        e = self.rand_expr()
        masked = self.r.random() < 0.4          # 60% UNMASKED (real address style)
        v1 = self.compute(e, masked)
        self.use(acc, v1)
        kill = self.r.random() < 0.22 and not (set(e[1:]) & self.invariant)
        # ALWAYS separate the two computations by control flow -> cross-block
        c = self.r.randint(0, 255)
        cmp = self.r.choice(["<", ">", "<=", ">=", "==", "!="])
        self.emit(f"if ({self.operand()} {cmp} {c}) {{")
        snap = list(self.names)                    # block-local decls don't escape
        self.d += 1
        if depth > 0 and self.r.random() < 0.55:
            self.redundancy(acc, depth - 1)        # nested cross-block redundancy
        else:
            self.use(acc, self.compute(self.rand_expr()))
        self.d -= 1
        self.names = list(snap)
        if self.r.random() < 0.5:
            self.emit("} else {")
            self.d += 1
            self.use(acc, self.compute(self.rand_expr()))
            self.d -= 1
            self.names = list(snap)
        self.emit("}")
        if kill:                                   # reassign an operand -> NOT redundant
            tgt = e[self.r.choice([1, 2])]
            self.emit(f"{tgt} = ({tgt} + {self.r.randint(1, 9)}) & {MASK};")
        v2 = self.compute(e, masked)               # recompute after the merge
        self.use(acc, v2)

    def gen(self):
        npar = self.r.randint(1, 4)
        for k in range(npar):
            self.names.append(f"p{k}")
            self.invariant.add(f"p{k}")
        params = ", ".join(f"p{k}: int64" for k in range(npar))
        acc = self.fresh(invariant=False)
        self.emit(f"var {acc}: int64 = 0;")
        # some loop-invariant locals and some updated locals
        for _ in range(self.r.randint(1, 3)):
            e = self.rand_expr()                  # build expr BEFORE declaring the name
            inv = self.r.random() < 0.5
            v = self.fresh(invariant=inv)
            self.emit(f"var {v}: int64 = {self.expr_str(e)};")
        ctr = self.fresh(invariant=False)
        self.emit(f"var {ctr}: int64 = 0;")
        n = self.r.randint(2, 6)
        self.emit(f"while ({ctr} < {n}) {{")
        self.d += 1
        snap = list(self.names)
        for _ in range(self.r.randint(1, 3)):
            self.redundancy(acc, self.r.randint(0, 2))
        self.emit(f"{ctr} = {ctr} + 1;")
        self.names = snap
        self.d -= 1
        self.emit("}")
        # a few straight-line recomputations too (cross sequential blocks)
        for _ in range(self.r.randint(0, 2)):
            self.redundancy(acc, self.r.randint(0, 1))
        body = "\n".join("  " + x for x in self.L)
        head = f"@noinline fn f({params}) -> int64 {{"
        args = ", ".join(str(self.r.randint(1, 200)) for _ in range(npar))
        main = f"fn main() -> int64 {{\n  return f({args}) & 255;\n}}"
        return (f"// seed={self.seed} (gen_gvn diverse)\n{head}\n{body}\n"
                f"  return ({acc}) & {MASK};\n}}\n\n{main}\n")


if __name__ == "__main__":
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    sys.stdout.write(Gen(seed).gen())
