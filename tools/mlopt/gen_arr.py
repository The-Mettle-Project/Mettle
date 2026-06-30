#!/usr/bin/env python3
"""Array/string-scan generator — produces the real-code GVN pattern the model was
blind to: an index expression (`arr + i`) recomputed across branches inside a
loop, each recomputation feeding a LOAD (`arr[i]`). Diagnosis showed the model
missed 14/14 real GVN nodes BECAUSE their result feeds a load (def_copy) while
synthetic redundant temps feed arithmetic. This closes that consumer-kind gap.

cstring params, read-only indexing (loads), accumulate the bytes. Labels come
from the sound static teacher (gvn/affine/liveness), so the code need not run.
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

    def idx(self, arr, iv):
        # index expression: arr[i] or arr[i+k] (recomputed across blocks -> GVN
        # on the address arr+i(+k); the load is the consumer)
        if self.r.random() < 0.7:
            return f"{arr}[{iv}]"
        return f"{arr}[{iv} + {self.r.randint(1, 4)}]"

    def scan_body(self, arrs, iv, acc, depth):
        a = self.r.choice(arrs)
        ix = self.idx(a, iv)
        c = self.r.randint(1, 254)
        cmp = self.r.choice(["<", ">", "<=", ">=", "==", "!="])
        self.emit(f"if ({ix} {cmp} {c}) {{")          # load #1 of a[i]
        self.d += 1
        b = self.r.choice(arrs)
        self.emit(f"{acc} = ({acc} + {self.idx(b, iv)}) & {MASK};")
        if depth > 0 and self.r.random() < 0.5:
            self.scan_body(arrs, iv, acc, depth - 1)
        self.d -= 1
        if self.r.random() < 0.6:
            self.emit("} else {")
            self.d += 1
            self.emit(f"{acc} = ({acc} ^ {self.idx(self.r.choice(arrs), iv)}) & {MASK};")
            self.d -= 1
        self.emit("}")
        # recompute a[i] after the merge -> dominating-same-expr address, load use
        t = self.fresh()
        self.emit(f"var {t}: int64 = {ix};")          # load #2 of the same a[i]
        self.emit(f"{acc} = ({acc} + {t}) & {MASK};")

    def gen(self):
        npar = self.r.randint(1, 3)
        arrs = [f"p{k}" for k in range(npar)]
        params = ", ".join(f"{a}: cstring" for a in arrs)
        self.emit("var acc: int64 = 0;")
        self.emit("var i: int64 = 0;")
        n = self.r.randint(2, 6)
        self.emit(f"while (i < {n}) {{")
        self.d += 1
        for _ in range(self.r.randint(1, 3)):
            self.scan_body(arrs, "i", "acc", self.r.randint(0, 2))
        self.emit("i = i + 1;")
        self.d -= 1
        self.emit("}")
        body = "\n".join("  " + x for x in self.L)
        head = f"@noinline fn f({params}) -> int64 {{"
        # main passes literal strings so it type-checks / compiles
        callargs = ", ".join('"abcdefgh"' for _ in arrs)
        main = (f"fn main() -> int64 {{\n  return f({callargs}) & 255;\n}}")
        return (f"// seed={self.seed} (gen_arr scan/load)\n{head}\n{body}\n"
                f"  return (acc) & {MASK};\n}}\n\n{main}\n")


if __name__ == "__main__":
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    sys.stdout.write(Gen(seed).gen())
