#!/usr/bin/env python3
"""Diverse parameterized @noinline int64 function generator for the optimizer
model. Wide structural variety so the model learns to reason about dataflow and
algebra across many shapes, not one template: 1-4 params, nested control flow
(ifs/loops to depth), deeper mixed-operator expressions, redundant subexpressions
(CSE bait), dead stores, induction-style updates, and varied sizes.

All int64, masked to 40 bits after each op (no overflow), divisors nonzero,
shift counts < 40, loop bounds small. Self-contained (no calls).
"""
import random
import sys

MASK = (1 << 40) - 1


class Gen:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.seed = seed
        self.lines = []
        self.vc = 0
        self.live = []
        self.exprs = []          # recent (expr_text) for redundancy/CSE bait

    def fresh(self):
        self.vc += 1
        return f"v{self.vc}"

    def atom(self, depth=0):
        r = self.rng.random()
        if self.live and r < 0.6:
            return self.rng.choice(self.live)
        if r < 0.8:
            return str(self.rng.randint(0, 255))
        return str(self.rng.randint(0, (1 << self.rng.randint(4, 20))))

    def expr(self, depth=2):
        if depth <= 0 or self.rng.random() < 0.3:
            return self.atom()
        op = self.rng.choice(["+", "-", "*", "&", "|", "^"])
        e = f"({self.expr(depth-1)} {op} {self.expr(depth-1)})"
        r = self.rng.random()
        if r < 0.25:
            sh = self.rng.choice([">>", "<<"])
            e = f"({e} {sh} {self.rng.randint(1, 31)})"
        elif r < 0.4:
            o = self.rng.choice(["/", "%"])
            e = f"({e} {o} {self.rng.randint(1, 97)})"
        e = f"({e} & {MASK})"
        if self.rng.random() < 0.3 and self.exprs:
            return self.rng.choice(self.exprs)        # reuse -> redundancy/CSE
        self.exprs.append(e)
        if len(self.exprs) > 6:
            self.exprs.pop(0)
        return e

    def ind(self):
        return "  " * (self.depth + 1)

    def emit(self, s):
        self.lines.append(self.ind() + s)

    def block(self, budget, depth):
        nst = self.rng.randint(1, max(1, budget))
        for _ in range(nst):
            self.stmt(budget // 2, depth)

    def stmt(self, budget, depth):
        roll = self.rng.random()
        if depth >= self.maxdepth or budget <= 0 or roll < 0.30:
            v = self.fresh()
            self.emit(f"var {v}: int64 = {self.expr(self.rng.randint(1,3))};")
            self.live.append(v)
        elif roll < 0.50:                              # dead store
            v = self.fresh()
            self.emit(f"var {v}: int64 = {self.expr(2)};")
            self.emit(f"{v} = {self.expr(2)};")
            self.live.append(v)
        elif roll < 0.66 and self.live:                # loop
            acc = self.rng.choice(self.live)
            i = self.fresh()
            n = self.rng.randint(0, 6)
            poly = self.rng.choice([f"{i}", f"({i} * {i})", f"({i} + {acc})"])
            self.emit(f"var {i}: int64 = 0;")
            self.emit(f"while ({i} < {n}) {{")
            self.depth += 1
            self.emit(f"{acc} = ({acc} + {poly}) & {MASK};")
            snap = list(self.live)                      # block-scoped decls
            if self.rng.random() < 0.4:
                self.stmt(budget // 2, depth + 1)
            self.emit(f"{i} = {i} + 1;")
            self.live = snap
            self.depth -= 1
            self.emit("}")
        elif self.live:                                # if / if-else (nestable)
            a = self.rng.choice(self.live)
            op = self.rng.choice(["<", "<=", ">", ">=", "==", "!="])
            c = self.rng.randint(0, 255)
            tgt = self.rng.choice(self.live)
            self.emit(f"if ({a} {op} {c}) {{")
            self.depth += 1
            self.emit(f"{tgt} = ({tgt} + {self.expr(2)}) & {MASK};")
            snap = list(self.live)
            if self.rng.random() < 0.4:
                self.stmt(budget // 2, depth + 1)
            self.live = snap
            self.depth -= 1
            if self.rng.random() < 0.5:
                self.emit("} else {")
                self.depth += 1
                self.emit(f"{tgt} = ({tgt} ^ {self.expr(2)}) & {MASK};")
                self.depth -= 1
            self.emit("}")
        else:
            v = self.fresh()
            self.emit(f"var {v}: int64 = {self.expr(2)};")
            self.live.append(v)

    def gen(self):
        npar = self.rng.randint(1, 4)
        self.maxdepth = self.rng.randint(1, 3)
        self.depth = 0
        params = ", ".join(f"p{i}: int64" for i in range(npar))
        self.live = [f"p{i}" for i in range(npar)]
        head = f"@noinline fn f({params}) -> int64 {{"
        for _ in range(self.rng.randint(5, 14)):
            self.stmt(3, 0)
        ret = self.rng.choice(self.live)
        if len(self.live) > 1 and self.rng.random() < 0.6:
            ret = f"({ret} + {self.rng.choice(self.live)}) & {MASK}"
        self.emit(f"return ({ret}) & {MASK};")
        body = "\n".join(self.lines)
        callargs = ", ".join(str(self.rng.randint(1, 200)) for _ in range(npar))
        main = (f"fn main() -> int64 {{\n  return f({callargs}) & 255;\n}}")
        return f"// seed={self.seed} (gen_fn v2 diverse)\n{head}\n{body}\n}}\n\n{main}\n"


if __name__ == "__main__":
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    sys.stdout.write(Gen(seed).gen())
