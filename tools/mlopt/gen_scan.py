#!/usr/bin/env python3
"""Generator of REAL-CODE-LIKE scan loops, to teach the model the global-value-
numbering pattern that actually occurs in hand-written code: an index/address
expression (`base + i`) recomputed at several points ACROSS branches inside a
loop body (as in cstr_compare, memcmp, markdown scanners, insertion sort).

gen_fn's GVN redundancy is generator-specific CSE bait; the model overfit to it
and failed to recognize real-code GVN. These functions put `pK + i` style
expressions before a branch and recompute them after the merge / in branches, so
block-local CSE cannot remove them but dominance-based GVN can. Pure int64,
masked, self-contained (the "load" is a pure surrogate), so irexec can verify.
"""
import random
import sys

MASK = (1 << 40) - 1


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.seed = seed
        self.L = []
        self.vc = 0
        self.d = 1

    def fresh(self):
        self.vc += 1
        return f"v{self.vc}"

    def emit(self, s):
        self.L.append("  " * self.d + s)

    def idx_expr(self, base, i):
        return f"(({base} + {i}) & {MASK})"

    def loadish(self, base, i):
        # pure surrogate for a memory load at address base+i (recomputes base+i)
        k = self.r.choice([2, 3, 5, 7, 9, 11, 13])
        return f"((({base} + {i}) * {k}) & {MASK})"

    def body(self, bases, i, acc, depth):
        b0 = self.r.choice(bases)
        # address computed up front; then a branch; then RECOMPUTE base+i after
        # the merge -> cross-block redundancy that block-local CSE can't see.
        a = self.fresh()
        self.emit(f"var {a}: int64 = {self.loadish(b0, i)};")
        c = self.r.randint(0, 255)
        op = self.r.choice(["<", ">", "==", "!=", "<=", ">="])
        self.emit(f"if ({a} {op} {c}) {{")
        self.d += 1
        b1 = self.r.choice(bases)
        self.emit(f"{acc} = ({acc} + {self.idx_expr(b1, i)}) & {MASK};")
        if depth > 0 and self.r.random() < 0.5:
            self.body(bases, i, acc, depth - 1)
        self.d -= 1
        if self.r.random() < 0.6:
            self.emit("} else {")
            self.d += 1
            b2 = self.r.choice(bases)
            self.emit(f"{acc} = ({acc} ^ {self.loadish(b2, i)}) & {MASK};")
            self.d -= 1
        self.emit("}")
        # recompute base+i AFTER the merge (dominated by the up-front compute,
        # not killed since i is unchanged until the loop step) -> GVN target
        t = self.fresh()
        self.emit(f"var {t}: int64 = {self.idx_expr(b0, i)};")
        self.emit(f"{acc} = ({acc} + {t}) & {MASK};")

    def gen(self):
        npar = self.r.randint(1, 3)
        bases = [f"p{k}" for k in range(npar)]
        params = ", ".join(f"{b}: int64" for b in bases)
        self.emit("var acc: int64 = 0;")
        self.emit("var i: int64 = 0;")
        n = self.r.randint(2, 7)
        self.emit(f"while (i < {n}) {{")
        self.d += 1
        nb = self.r.randint(1, 3)
        for _ in range(nb):
            self.body(bases, "i", "acc", self.r.randint(0, 2))
        self.emit("i = i + 1;")
        self.d -= 1
        self.emit("}")
        # a tail that also recomputes pK + (n-1)-ish via a fresh index
        if self.r.random() < 0.5:
            self.emit(f"var j: int64 = {self.r.randint(0, 5)};")
            b = self.r.choice(bases)
            self.emit(f"acc = (acc + {self.idx_expr(b, 'j')}) & {MASK};")
            self.emit(f"acc = (acc ^ {self.idx_expr(b, 'j')}) & {MASK};")
        body = "\n".join("  " + x for x in self.L)
        head = f"@noinline fn f({params}) -> int64 {{"
        args = ", ".join(str(self.r.randint(1, 200)) for _ in bases)
        main = f"fn main() -> int64 {{\n  return f({args}) & 255;\n}}"
        return (f"// seed={self.seed} (gen_scan)\n{head}\n{body}\n"
                f"  return (acc) & {MASK};\n}}\n\n{main}\n")


if __name__ == "__main__":
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    sys.stdout.write(Gen(seed).gen())
