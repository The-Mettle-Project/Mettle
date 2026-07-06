#!/usr/bin/env python3
"""Int64-only Mettle program generator for the ML IR-opt end-to-end loop.

Restricted to the subset `tools/fuzz/irexec.py` interprets (int64 scalars,
binary ops, masks, shifts, div/mod, comparisons, branches, bounded loops, direct
calls) so the model's *generated* optimized IR can be executed and verified
without a C-side IR parser. Still exercises rich optimization surface: constant
folding, copy/const propagation, CSE, dead-store elimination, small-loop
unrolling, strength reduction, reassociation.

Determinism (so debug==release==irexec, and a divergence is a real bug):
  - every value masked to 40 bits after each op -> no signed overflow
  - divisors/mod are nonzero constants; shift counts < 64
  - main returns `acc & 255`, exactly the low byte irexec and the exe agree on
"""
import random
import sys

MASK = (1 << 40) - 1


class Gen:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.seed = seed
        self.lines = []
        self.indent = 1
        self.vc = 0
        self.live = []
        self.helpers = []  # (name, nparams)

    def fresh(self):
        self.vc += 1
        return f"v{self.vc}"

    def emit(self, s):
        self.lines.append("  " * self.indent + s)

    def atom(self):
        if self.live and self.rng.random() < 0.6:
            return self.rng.choice(self.live)
        return str(self.rng.randint(0, 255))

    def expr(self):
        op = self.rng.choice(["+", "-", "*", "&", "|", "^"])
        e = f"({self.atom()} {op} {self.atom()})"
        if self.rng.random() < 0.4:
            op2 = self.rng.choice(["+", "-", "*"])
            e = f"({e} {op2} {self.atom()})"
        # strength-reduction / div-mod surface
        if self.rng.random() < 0.3:
            op3 = self.rng.choice([">>", "/", "%"])
            rhs = self.rng.randint(1, 31) if op3 == ">>" else self.rng.randint(1, 97)
            e = f"({e} {op3} {rhs})"
        return f"({e} & {MASK})"

    def stmt(self, budget):
        roll = self.rng.random()
        if roll < 0.40 or budget <= 0:
            self.stmt_assign()
        elif roll < 0.55:
            self.stmt_dead()
        elif roll < 0.72:
            self.stmt_loop()
        elif roll < 0.87:
            self.stmt_if(budget)
        else:
            self.stmt_call()

    def stmt_assign(self):
        v = self.fresh()
        self.emit(f"var {v}: int64 = {self.expr()};")
        self.live.append(v)

    def stmt_dead(self):
        # a value computed then overwritten before use -> dead-store elimination
        v = self.fresh()
        self.emit(f"var {v}: int64 = {self.expr()};")
        self.emit(f"{v} = {self.expr()};")
        self.live.append(v)

    def stmt_loop(self):
        if not self.live:
            self.stmt_assign()
            return
        acc = self.rng.choice(self.live)
        n = self.rng.randint(0, 6)
        i = self.fresh()
        poly = self.rng.choice([f"{i}", f"({i} * {i})", f"({i} + {acc})"])
        self.emit(f"var {i}: int64 = 0;")
        self.emit(f"while ({i} < {n}) {{")
        self.indent += 1
        self.emit(f"{acc} = ({acc} + {poly}) & {MASK};")
        self.emit(f"{i} = {i} + 1;")
        self.indent -= 1
        self.emit("}")

    def stmt_if(self, budget):
        if not self.live:
            self.stmt_assign()
            return
        a = self.rng.choice(self.live)
        op = self.rng.choice(["<", "<=", ">", ">=", "==", "!="])
        c = self.rng.randint(0, 255)
        tgt = self.rng.choice(self.live)
        self.emit(f"if ({a} {op} {c}) {{")
        self.indent += 1
        self.emit(f"{tgt} = ({tgt} + {self.expr()}) & {MASK};")
        self.indent -= 1
        if self.rng.random() < 0.5:
            self.emit("} else {")
            self.indent += 1
            self.emit(f"{tgt} = ({tgt} ^ {self.expr()}) & {MASK};")
            self.indent -= 1
        self.emit("}")

    def stmt_call(self):
        if not self.helpers:
            self.stmt_assign()
            return
        name, npar = self.rng.choice(self.helpers)
        args = ", ".join(self.atom() for _ in range(npar))
        v = self.fresh()
        self.emit(f"var {v}: int64 = {name}({args});")
        self.live.append(v)

    def gen_helper(self):
        name = f"h{len(self.helpers)}"
        npar = self.rng.randint(1, 3)
        params = ", ".join(f"p{i}: int64" for i in range(npar))
        self.lines.append(f"fn {name}({params}) -> int64 {{")
        self.indent = 1
        self.live = [f"p{i}" for i in range(npar)]
        self.vc = 0
        for _ in range(self.rng.randint(1, 2)):
            self.stmt_assign()
        if self.rng.random() < 0.3:
            self.stmt_loop()
        self.emit(f"return {self.rng.choice(self.live)} & {MASK};")
        self.lines.append("}")
        self.lines.append("")
        self.helpers.append((name, npar))

    def gen(self):
        for _ in range(self.rng.randint(0, 1)):
            self.gen_helper()
        self.lines.append("fn main() -> int64 {")
        self.indent = 1
        self.live = []
        self.vc = 0
        self.emit("var acc: int64 = 1;")
        self.live.append("acc")
        for _ in range(self.rng.randint(3, 6)):
            self.stmt(budget=2)
        # fold everything live into acc so nothing is trivially dead
        for v in list(self.live):
            if v != "acc":
                self.emit(f"acc = (acc + {v}) & {MASK};")
        self.emit("return acc & 255;")
        self.lines.append("}")
        return "// seed=%d (gen_int64)\n" % self.seed + "\n".join(self.lines) + "\n"


if __name__ == "__main__":
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    sys.stdout.write(Gen(seed).gen())
