#!/usr/bin/env python3
"""Random Mettle program generator for differential miscompile testing (v2).

Emits a self-contained program whose `main` deterministically computes an
int64 accumulator and returns the low byte (xor-folded) as its exit code. The
program contains no I/O and no UB-prone constructs, so debug and release builds
MUST agree on the exit code. A divergence is a silent miscompile.

v1 only generated int64 shapes; every historical float/struct/narrow-int/byte
miscompile was invisible to it. v2 adds the blind-spot shapes, each one keyed
to a real past bug class or a SIMD recognizer:

  - float scalar chains + helpers with float32/float64 params called with
    float binary-op temps (the float-temp-call-arg miscompile shape) and
    float32-returning helpers (the float32-narrowing-return shape)
  - float32/float64 arrays with fill + affine-map / div / sum / dot loops
    (simd_affine_map_float, simd_sum_float, simd_dot_float, auto_vectorize)
  - counter->float64 chain->(int64) trunc reductions (simd_i2f_reduce)
  - int32 arrays with sum (incl. the cast-free int32->int64 reduction),
    and int32 dot loops (simd_sum_i32, simd_dot_i32)
  - uint8 arrays with const-op byte map chains, byte sums, and
    byte-load->int32 dot loops (simd_byte_map, simd_sum_u8, simd_dot_i8)
  - narrow scalars: uint32/int32 mul/div/mod/shift with constant and
    variable divisors, zero-extending folds (the uint32-as-signed and
    magic-number div classes)
  - uint32 accumulators that genuinely WRAP mod 2^32 (no re-masking):
    unsigned sub-64-bit wrap is defined language semantics, and the homes
    must stay zero-extend-canonical in scalar code (the promoted-register
    uint32 canonicalization class)
  - small structs by value: struct-returning makers, struct-param folders,
    whole-struct copies + field mutation (SROA / struct-copy / MIR
    struct-chain classes)
  - globals written in loops around @pure helper calls that read them
    (the pure-call-LICM no-hoist classes)
  - @inline/@noinline/@pure decorated helpers, multi-param helpers (>4
    params exercises stack-arg passing), range-for loops (the ir_for_cond_
    vectorizer gate), and occasional jumbo functions (MIR size-bail ->
    fallback backend coverage)

DETERMINISM RULES (why a divergence is always a real miscompile):
  - the int64 accumulator is masked to 40 bits after every step, so signed
    overflow never occurs
  - every float value is an exact binary number: integer-valued floats with
    bounded magnitude (f64 < 2^50, f32 < 2^24 in any f32 context), float
    division only by powers of two, no transcendentals. All float ops are
    therefore IEEE-exact, so vectorized reduction reordering cannot change
    the result.
  - narrow ints are masked before multiplication so they never wrap; shift
    counts < width; divisors are nonzero (constants, or `| 1`-forced).
    Exception: uint32 in stmt_u32wrap wraps freely -- unsigned wrap mod
    2^width is defined semantics, so it is still deterministic
  - struct fields and arrays are always fully written before any read
"""

import random

MASK = 1099511627775  # (1 << 40) - 1


class Gen:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.seed = seed
        self.lines = []
        self.indent = 0
        self.var_counter = 0
        self.live_vars = []          # int64 scalars currently in scope
        self.helpers = []            # emitted helper function source blocks
        self.helper_sigs = []        # (name, nparams) int64 helpers
        self.float_helpers = {}      # kind -> name
        self.kernels = {}            # kind -> name (pointer-param SIMD bait)
        self.structs = []            # dicts: name, fields, make, fold
        self.globals = []            # (name, pure_reader_name_or_None)
        self.struct_defs = []        # struct type source blocks

    # ---- emission helpers -------------------------------------------------
    def emit(self, s):
        self.lines.append("  " * self.indent + s)

    def fresh(self, prefix="v"):
        self.var_counter += 1
        return f"{prefix}{self.var_counter}"

    def pick_var(self):
        return self.rng.choice(self.live_vars) if self.live_vars else None

    def ensure_acc(self):
        acc = self.pick_var()
        if acc is None:
            self.stmt_decl()
            acc = self.live_vars[-1]
        return acc
    
    def counted_loop(self, lo, hi, body, fuzz_start=False):
        """Emit a counted loop over [lo, hi); body(ivar) emits the body.
        Randomly uses range-for (the ir_for_cond_ vectorizer gate) or while.
        fuzz_start=True (only safe for loops that READ already-initialized
        data or map it in place -- never for array-init fills) sometimes bumps
        the start to a small nonzero index: the SIMD recognizers replay
        counted loops as 0..bound, so a nonzero start that they fail to
        refuse is a silent miscompile (the iv-start-zero bug class)."""
        i = self.fresh("i")
        if fuzz_start and self.rng.random() < 0.35:
            lo = lo + self.rng.randint(1, 3)
        if self.rng.random() < 0.4:
            self.emit(f"for {i}: int64 in {lo}..{hi} {{")
            self.indent += 1
            body(i)
            self.indent -= 1
            self.emit("}")
        else:
            self.emit(f"var {i}: int64 = {lo};")
            self.emit(f"while ({i} < {hi}) {{")
            self.indent += 1
            body(i)
            self.emit(f"{i} = {i} + 1;")
            self.indent -= 1
            self.emit("}")
        return i

    # ---- expression generation -------------------------------------------
    def const(self):
        return self.rng.randint(0, 9999)

    def atom(self, depth):
        """A non-dividing int64 expression usable anywhere."""
        choices = ["const"]
        if self.live_vars:
            choices += ["var", "var"]
        if depth > 0:
            choices += ["binop"]
        kind = self.rng.choice(choices)
        if kind == "const":
            return str(self.const())
        if kind == "var":
            return self.pick_var()
        a = self.atom(depth - 1)
        b = self.atom(depth - 1)
        op = self.rng.choice(["+", "-", "*", "&", "|", "^"])
        return f"({a} {op} {b})"

    def masked_assign(self, target):
        """Assign a fresh value to `target`, keeping it in [0, 1<<40)."""
        expr = self.atom(self.rng.randint(1, 3))
        if self.rng.random() < 0.35:
            ut = self.fresh("u")
            base = self.rng.randint(1, 1 << 30)
            op = self.rng.choice([">>", "/", "%"])
            rhs = self.rng.randint(1, 31) if op == ">>" else self.rng.randint(1, 97)
            self.emit(f"var {ut}: uint64 = (uint64){base};")
            self.emit(f"{ut} = {ut} {op} {rhs};")
            expr = f"({expr} + (int64){ut})"
        # `(bareident) & ...` used to be mis-parsed as a cast of an address-of
        # (parser fix 2026-06-09, regression tests/test_paren_ident_binop.mettle);
        # deliberately emit that shape sometimes so the fuzzer keeps it covered.
        elif self.live_vars and self.rng.random() < 0.30:
            expr = f"({self.pick_var()})"
        self.emit(f"{target} = {expr} & {MASK};")

    # ---- statement dispatch -----------------------------------------------
    def stmt(self, budget):
        if budget <= 0:
            return
        kinds = [
            (12, self.stmt_decl),
            (12, lambda: self.stmt_loop(budget)),
            (10, lambda: self.stmt_if(budget)),
            (8,  lambda: self.stmt_array(budget)),
            (7,  self.stmt_call),
            (7,  self.stmt_float_call),
            (6,  self.stmt_float_scalar),
            (9,  self.stmt_float_array),
            (9,  self.stmt_i32_array),
            (8,  self.stmt_byte_array),
            (8,  self.stmt_narrow),
            (6,  self.stmt_u32wrap),
            (8,  self.stmt_struct),
            (6,  self.stmt_global),
            (5,  self.stmt_matmul),
        ]
        total = sum(w for w, _ in kinds)
        roll = self.rng.uniform(0, total)
        for w, fn in kinds:
            roll -= w
            if roll <= 0:
                fn()
                return
        kinds[-1][1]()

    # ---- v1 statements (int64 core) ---------------------------------------
    def stmt_decl(self):
        v = self.fresh()
        self.emit(f"var {v}: int64 = {self.atom(2)} & {MASK};")
        self.live_vars.append(v)

    def stmt_loop(self, budget):
        acc = self.ensure_acc()
        n = self.rng.randint(0, 40)
        i = self.fresh("i")
        poly = self.rng.choice(["{i}", "{i}*{i}", "{i}*{i}*{i}",
                                "(2*{i}+3)", "7"]).replace("{i}", i)
        self.emit(f"var {i}: int64 = 1;")
        self.emit(f"while ({i} <= {n}) {{")
        self.indent += 1
        self.emit(f"{acc} = ({acc} + {poly}) & {MASK};")
        self.emit(f"{i} = {i} + 1;")
        self.indent -= 1
        self.emit("}")

    def block(self, budget):
        saved = list(self.live_vars)
        self.stmt(budget - 1)
        self.live_vars = saved

    def stmt_if(self, budget):
        a = self.atom(2)
        b = self.atom(1)
        op = self.rng.choice(["<", "<=", ">", ">=", "==", "!="])
        self.emit(f"if ({a} {op} {b}) {{")
        self.indent += 1
        self.block(budget)
        self.indent -= 1
        if self.rng.random() < 0.5:
            self.emit("} else {")
            self.indent += 1
            self.block(budget)
            self.indent -= 1
        self.emit("}")

    def stmt_array(self, budget):
        """int64 arr[i] = f(i) write loop, then read-back fold."""
        arr = self.fresh("arr")
        n = self.rng.randint(2, 16)
        acc = self.ensure_acc()
        c, d = self.rng.randint(1, 50), self.rng.randint(0, 99)
        self.emit(f"var {arr}: int64[{n}];")
        self.counted_loop(0, n, lambda i: self.emit(
            f"{arr}[{i}] = ({i} * {c} + {d}) & {MASK};"))
        self.counted_loop(0, n, lambda j: self.emit(
            f"{acc} = ({acc} + {arr}[{j}]) & {MASK};"), fuzz_start=True)

    def stmt_call(self):
        if not self.helper_sigs:
            return
        name, np = self.rng.choice(self.helper_sigs)
        acc = self.ensure_acc()
        args = ", ".join(self.atom(1) for _ in range(np))
        self.emit(f"{acc} = ({acc} + {name}({args})) & {MASK};")

    # ---- float statements --------------------------------------------------
    def stmt_float_call(self):
        """Float helpers called with float binary-op temps: the
        float-temp-call-arg and float32-narrowing-return bug shapes."""
        if not self.float_helpers:
            return
        acc = self.ensure_acc()
        a = self.fresh("fx")
        k = self.rng.randint(1, 9)
        self.emit(f"var {a}: float32 = (float32)({self.atom(1)} & 4095);")
        did = False
        if "f32f32_f64" in self.float_helpers and self.rng.random() < 0.7:
            fn = self.float_helpers["f32f32_f64"]
            c = self.rng.randint(0, 9)
            self.emit(f"{acc} = ({acc} + (int64){fn}({a} * {k}.0 + {c}.0,"
                      f" {a})) & {MASK};")
            did = True
        if "f32_f32" in self.float_helpers and self.rng.random() < 0.6:
            fn = self.float_helpers["f32_f32"]
            fz = self.fresh("fz")
            self.emit(f"var {fz}: float32 = {fn}({a} / 2.0);")
            self.emit(f"{acc} = ({acc} + (int64){fz}) & {MASK};")
            did = True
        if "f64_i64" in self.float_helpers and (not did or
                                                self.rng.random() < 0.4):
            fn = self.float_helpers["f64_i64"]
            self.emit(f"{acc} = ({acc} + {fn}((float64){a} * 3.0)) & {MASK};")

    def stmt_float_scalar(self):
        acc = self.ensure_acc()
        if self.rng.random() < 0.5:
            # counter -> float64 chain -> (int64) trunc reduction
            # (the simd_i2f_reduce shape; needs a constant bound)
            f = self.fresh("fs")
            n = self.rng.randint(4, 40)
            k = self.rng.randint(1, 9)
            c = self.rng.randint(0, 9)
            self.emit(f"var {f}: float64 = 0.0;")
            self.counted_loop(1, n + 1, lambda i: self.emit(
                f"{f} = {f} + (float64){i} * {k}.0 + {c}.0;"), fuzz_start=True)
            self.emit(f"{acc} = ({acc} + (int64){f}) & {MASK};")
        else:
            g = self.fresh("fg")
            self.emit(f"var {g}: float64 = (float64)({self.atom(1)}"
                      f" & 65535);")
            for _ in range(self.rng.randint(1, 3)):
                if self.rng.random() < 0.7:
                    m = self.rng.randint(2, 9)
                    add = self.rng.randint(0, 99)
                    self.emit(f"{g} = {g} * {m}.0 + {add}.0;")
                else:
                    d = self.rng.choice([2, 4, 8])  # exact divisions only
                    self.emit(f"{g} = {g} / {d}.0;")
            if self.rng.random() < 0.4:
                t = self.rng.randint(0, 9999)
                self.emit(f"if ({g} > {t}.0) {{")
                self.indent += 1
                self.emit(f"{acc} = ({acc} + 13) & {MASK};")
                self.indent -= 1
                self.emit("}")
            self.emit(f"{acc} = ({acc} + (int64){g}) & {MASK};")

    def stmt_float_array(self):
        """Float array fill + map/div/sum/dot loops: the float vectorizer
        shapes (affine map, vloop, sum_float, dot_float)."""
        acc = self.ensure_acc()
        f32 = self.rng.random() < 0.5
        ty = "float32" if f32 else "float64"
        # bounds chosen so every value stays integer-or-halves and exact:
        # f32 ctx < 2^24 even after map + dot + n-wide sum
        n = self.rng.randint(8, 32 if f32 else 48)
        cmax, dmax = (5, 9) if f32 else (999, 999)
        arr = self.fresh("fa")
        c, d = self.rng.randint(1, cmax), self.rng.randint(0, dmax)
        self.emit(f"var {arr}: {ty}[{n}];")
        self.counted_loop(0, n, lambda i: self.emit(
            f"{arr}[{i}] = ({ty})({i} * {c} + {d});"))
        if self.rng.random() < 0.5:  # optional in-place map
            kmap = self.kernels.get("fmap_" + ty)
            if kmap and self.rng.random() < 0.5:
                self.emit(f"{kmap}(&{arr}[0], {n});")
            elif self.rng.random() < 0.75:
                m = 2 if f32 else self.rng.randint(2, 3)
                a = self.rng.randint(0, dmax)
                self.counted_loop(0, n, lambda i: self.emit(
                    f"{arr}[{i}] = {arr}[{i}] * {m}.0 + {a}.0;"), fuzz_start=True)
            else:
                self.counted_loop(0, n, lambda i: self.emit(
                    f"{arr}[{i}] = {arr}[{i}] / 2.0;"), fuzz_start=True)
        fs = self.fresh("fr")
        self.emit(f"var {fs}: {ty} = 0.0;")
        ksum = self.kernels.get("fsum_" + ty)
        kdot = self.kernels.get("fdot_" + ty)
        if self.rng.random() < 0.5:  # sum reduction
            if ksum and self.rng.random() < 0.5:
                self.emit(f"{fs} = {ksum}(&{arr}[0], {n});")
            else:
                self.counted_loop(0, n, lambda i: self.emit(
                    f"{fs} = {fs} + {arr}[{i}];"), fuzz_start=True)
        else:                        # dot reduction
            arr2 = self.fresh("fb")
            c2, d2 = self.rng.randint(1, cmax), self.rng.randint(0, dmax)
            self.emit(f"var {arr2}: {ty}[{n}];")
            self.counted_loop(0, n, lambda i: self.emit(
                f"{arr2}[{i}] = ({ty})({i} * {c2} + {d2});"))
            if kdot and self.rng.random() < 0.5:
                self.emit(f"{fs} = {kdot}(&{arr}[0], &{arr2}[0], {n});")
            else:
                self.counted_loop(0, n, lambda i: self.emit(
                    f"{fs} = {fs} + {arr}[{i}] * {arr2}[{i}];"), fuzz_start=True)
        self.emit(f"{acc} = ({acc} + (int64){fs}) & {MASK};")

    # ---- int32 / byte array statements -------------------------------------
    def stmt_i32_array(self):
        """int32 arrays: sum (incl. the cast-free int32->int64 reduction)
        and int32 dot (simd_sum_i32 / simd_dot_i32 shapes)."""
        acc = self.ensure_acc()
        n = self.rng.randint(8, 64)
        arr = self.fresh("na")
        c, d = self.rng.randint(1, 30), self.rng.randint(0, 99)
        self.emit(f"var {arr}: int32[{n}];")
        self.counted_loop(0, n, lambda i: self.emit(
            f"{arr}[{i}] = (int32)(({i} * {c} + {d}) & 1023);"))
        s = self.fresh("ns")
        roll = self.rng.random()
        if roll < 0.35:
            # cast-free widening reduction: s64 += a[i]
            self.emit(f"var {s}: int64 = 0;")
            if "isum" in self.kernels and self.rng.random() < 0.5:
                self.emit(f"{s} = {self.kernels['isum']}(&{arr}[0], {n});")
            else:
                self.counted_loop(0, n, lambda i: self.emit(
                    f"{s} = {s} + {arr}[{i}];"), fuzz_start=True)
            self.emit(f"{acc} = ({acc} + {s}) & {MASK};")
        elif roll < 0.65:
            self.emit(f"var {s}: int64 = 0;")
            self.counted_loop(0, n, lambda i: self.emit(
                f"{s} = {s} + (int64){arr}[{i}];"), fuzz_start=True)
            self.emit(f"{acc} = ({acc} + {s}) & {MASK};")
        else:
            arr2 = self.fresh("nb")
            c2 = self.rng.randint(1, 30)
            self.emit(f"var {arr2}: int32[{n}];")
            self.counted_loop(0, n, lambda i: self.emit(
                f"{arr2}[{i}] = (int32)(({i} * {c2} + 1) & 1023);"))
            # values <= 1023 so a 64-wide int32 dot cannot overflow
            if "idot" in self.kernels and self.rng.random() < 0.5:
                self.emit(f"var {s}: int64 = {self.kernels['idot']}"
                          f"(&{arr}[0], &{arr2}[0], {n});")
                self.emit(f"{acc} = ({acc} + {s}) & {MASK};")
            else:
                self.emit(f"var {s}: int32 = 0;")
                self.counted_loop(0, n, lambda i: self.emit(
                    f"{s} = {s} + {arr}[{i}] * {arr2}[{i}];"), fuzz_start=True)
                self.emit(f"{acc} = ({acc} + (int64){s}) & {MASK};")

    def stmt_byte_array(self):
        """uint8 arrays: const-op byte map chains, byte sums, and byte dot
        (simd_byte_map / simd_sum_u8 / simd_dot_i8 shapes). Byte stores
        truncate mod 256 identically at both levels, so wrapping is fine."""
        acc = self.ensure_acc()
        n = self.rng.randint(8, 64)
        arr = self.fresh("ba")
        c, d = self.rng.randint(1, 30), self.rng.randint(0, 99)
        self.emit(f"var {arr}: uint8[{n}];")
        self.counted_loop(0, n, lambda i: self.emit(
            f"{arr}[{i}] = (uint8)(({i} * {c} + {d}) & 255);"))
        if self.rng.random() < 0.6:  # in-place byte map chain
            if "bmap" in self.kernels and self.rng.random() < 0.5:
                self.emit(f"{self.kernels['bmap']}(&{arr}[0], {n});")
            else:
                def map_body(i):
                    for _ in range(self.rng.randint(1, 3)):
                        op = self.rng.choice(["*", "+", "-", "^", "&", "|"])
                        k = self.rng.randint(1, 255)
                        self.emit(f"{arr}[{i}] = {arr}[{i}] {op} {k};")
                self.counted_loop(0, n, map_body, fuzz_start=True)
        if self.rng.random() < 0.5:  # byte sum
            s = self.fresh("bs")
            self.emit(f"var {s}: int64 = 0;")
            if "bsum" in self.kernels and self.rng.random() < 0.5:
                self.emit(f"{s} = {self.kernels['bsum']}(&{arr}[0], {n});")
            else:
                self.counted_loop(0, n, lambda i: self.emit(
                    f"{s} = {s} + (int64){arr}[{i}];"), fuzz_start=True)
            self.emit(f"{acc} = ({acc} + {s}) & {MASK};")
        else:                        # byte dot -> int32 accumulator
            arr2 = self.fresh("bb")
            c2 = self.rng.randint(1, 30)
            self.emit(f"var {arr2}: uint8[{n}];")
            self.counted_loop(0, n, lambda i: self.emit(
                f"{arr2}[{i}] = (uint8)(({i} * {c2} + 3) & 255);"))
            s = self.fresh("bd")
            self.emit(f"var {s}: int32 = 0;")
            if "bdot" in self.kernels and self.rng.random() < 0.5:
                self.emit(f"{s} = {self.kernels['bdot']}(&{arr}[0],"
                          f" &{arr2}[0], {n});")
            else:
                self.counted_loop(0, n, lambda i: self.emit(
                    f"{s} = {s} + (int32){arr}[{i}] * (int32){arr2}[{i}];"), fuzz_start=True)
            self.emit(f"{acc} = ({acc} + (int64){s}) & {MASK};")

    def stmt_matmul(self):
        """Small int32 matmul through the kernel: SLP MAC / MIR-SIMD-inline
        shapes. Values <= 1023 with n <= 8 keep every dot in int32 range."""
        if "matmul" not in self.kernels:
            self.stmt_i32_array()
            return
        acc = self.ensure_acc()
        n = self.rng.choice([4, 8])
        nn = n * n
        a, b, cm = self.fresh("ma"), self.fresh("mb"), self.fresh("mc")
        ca, da = self.rng.randint(1, 15), self.rng.randint(0, 63)
        cb = self.rng.randint(1, 15)
        self.emit(f"var {a}: int32[{nn}];")
        self.emit(f"var {b}: int32[{nn}];")
        self.emit(f"var {cm}: int32[{nn}];")
        self.counted_loop(0, nn, lambda i: (
            self.emit(f"{a}[{i}] = (int32)(({i} * {ca} + {da}) & 1023);"),
            self.emit(f"{b}[{i}] = (int32)(({i} * {cb} + 1) & 1023);")))
        self.emit(f"{self.kernels['matmul']}(&{a}[0], &{b}[0],"
                  f" &{cm}[0], {n});")
        s = self.fresh("ms")
        self.emit(f"var {s}: int64 = 0;")
        self.counted_loop(0, nn, lambda i: self.emit(
            f"{s} = {s} + (int64){cm}[{i}];"), fuzz_start=True)
        self.emit(f"{acc} = ({acc} + {s}) & {MASK};")

    # ---- narrow scalar statement -------------------------------------------
    def stmt_narrow(self):
        """uint32/int32 arithmetic with div/mod/shift: the uint32-as-signed
        and magic-number-division bug classes. Values are re-masked after
        every multiply so they never wrap."""
        acc = self.ensure_acc()
        u = self.fresh("nu")
        z = self.fresh("nz")
        self.emit(f"var {u}: uint32 = (uint32)({self.atom(1)} & 65535);")
        self.emit(f"var {z}: int32 = (int32)({self.atom(1)} & 32767);")
        for _ in range(self.rng.randint(1, 3)):
            kind = self.rng.choice(["umul", "udiv", "umod", "ushr",
                                    "zmul", "zdiv", "zmod", "zshr", "uvardiv"])
            if kind == "umul":
                k, j = self.rng.randint(2, 50), self.rng.randint(0, 99)
                self.emit(f"{u} = ({u} * {k} + {j}) & 65535;")
            elif kind == "udiv":
                self.emit(f"{u} = {u} / {self.rng.randint(2, 97)};")
            elif kind == "umod":
                self.emit(f"{u} = {u} % {self.rng.randint(2, 97)};")
            elif kind == "ushr":
                self.emit(f"{u} = {u} >> {self.rng.randint(1, 15)};")
            elif kind == "zmul":
                k, j = self.rng.randint(2, 21), self.rng.randint(0, 99)
                self.emit(f"{z} = ({z} * {k} + {j}) & 32767;")
            elif kind == "zdiv":
                self.emit(f"{z} = {z} / {self.rng.randint(2, 97)};")
            elif kind == "zmod":
                self.emit(f"{z} = {z} % {self.rng.randint(2, 97)};")
            elif kind == "zshr":
                self.emit(f"{z} = {z} >> {self.rng.randint(1, 14)};")
            else:  # divide by a runtime variable, forced nonzero
                self.emit(f"{u} = {u} / (uint32)(({z} & 15) | 1);")
        self.emit(f"{acc} = ({acc} + (int64){u} + (int64){z}) & {MASK};")

    # ---- wrapping uint32 accumulator statement ------------------------------
    def stmt_u32wrap(self):
        """uint32 accumulator that genuinely WRAPS mod 2^32 (no re-masking):
        unsigned sub-64-bit wrap is defined language semantics, so the home
        (stack slot or promoted register) must always hold the zero-extended
        canonical value. Mixes homomorphic wrap ops (+, *, ^) in a loop with
        width-sensitive consumers (/, >>, !=) reading the wrapped local --
        the promoted-register canonicalization bug class."""
        acc = self.ensure_acc()
        u = self.fresh("uw")
        self.emit(f"var {u}: uint32 = (uint32)({self.atom(1)} & {MASK});")
        n = self.rng.randint(1, 12)
        k = self.rng.choice([2654435761, 2246822519, 3266489917, 668265263])
        j = self.rng.randint(1, 9999)
        shift = self.rng.randint(1, 15)
        use_xor = self.rng.random() < 0.5

        def body(i):
            self.emit(f"{u} = {u} * {k} + {j};")
            if use_xor:
                self.emit(f"{u} = {u} ^ ({u} >> {shift});")
        self.counted_loop(0, n, body)
        roll = self.rng.random()
        if roll < 0.35:
            self.emit(f"{u} = {u} / {self.rng.randint(2, 97)};")
        elif roll < 0.6:
            self.emit(f"{u} = {u} >> {self.rng.randint(1, 15)};")
        self.emit(f"{acc} = ({acc} + (int64){u}) & {MASK};")

    # ---- struct statement ----------------------------------------------------
    def stmt_struct(self):
        """Struct return + by-value param + whole-struct copy + field
        mutation: the struct-copy / SROA / MIR-struct-chain bug classes."""
        if not self.structs:
            self.stmt_decl()
            return
        st = self.rng.choice(self.structs)
        acc = self.ensure_acc()
        sv = self.fresh("sv")
        self.emit(f"var {sv}: {st['name']} = {st['make']}({self.atom(1)}"
                  f" & 1048575);")
        self.emit(f"{acc} = ({acc} + {st['fold']}({sv})) & {MASK};")
        if self.rng.random() < 0.6:
            sc = self.fresh("sc")
            self.emit(f"var {sc}: {st['name']} = {sv};")
            fname, fty = self.rng.choice(st["fields"])
            if fty == "int64":
                self.emit(f"{sc}.{fname} = {sc}.{fname} + 7;")
            elif fty == "float64":
                self.emit(f"{sc}.{fname} = {sc}.{fname} + 3.0;")
            elif fty == "uint8":
                self.emit(f"{sc}.{fname} = (uint8){self.rng.randint(0, 255)};")
            else:
                self.emit(f"{sc}.{fname} = ({fty}){self.rng.randint(0, 9999)};")
            # fold both: the copy must have changed, the original must not
            self.emit(f"{acc} = ({acc} + {st['fold']}({sc})"
                      f" + {st['fold']}({sv})) & {MASK};")
        elif self.rng.random() < 0.5:
            fname, fty = self.rng.choice(st["fields"])
            cast = "" if fty == "int64" else "(int64)"
            self.emit(f"{acc} = ({acc} + {cast}{sv}.{fname} * 3) & {MASK};")

    # ---- global statement ------------------------------------------------------
    def stmt_global(self):
        """Global read/write loops around @pure helper calls that read the
        global: the pure-call-LICM hoist/no-hoist classes."""
        if not self.globals:
            self.stmt_decl()
            return
        gname, reader = self.rng.choice(self.globals)
        acc = self.ensure_acc()
        roll = self.rng.random()
        if roll < 0.4 or reader is None:
            self.emit(f"{gname} = ({gname} + {self.atom(1)}) & {MASK};")
            self.emit(f"{acc} = ({acc} + {gname}) & {MASK};")
        elif roll < 0.75:
            # body mutates the global -> the pure call must NOT be hoisted
            n = self.rng.randint(1, 8)
            k = self.rng.randint(0, 99)

            def body(i):
                self.emit(f"{gname} = ({gname} + 1) & {MASK};")
                self.emit(f"{acc} = ({acc} + {reader}({k})) & {MASK};")
            self.counted_loop(0, n, body)
        else:
            # body does not touch the global -> hoisting is legal
            n = self.rng.randint(1, 8)
            k = self.rng.randint(0, 99)
            self.counted_loop(0, n, lambda i: self.emit(
                f"{acc} = ({acc} + {reader}({k})) & {MASK};"))

    # ---- program-level generators ---------------------------------------------
    def gen_structs(self, k):
        types = ["int32", "int64", "float64", "uint32", "uint8"]
        for _ in range(k):
            name = self.fresh("St")
            nf = self.rng.randint(2, 4)
            fields = [(f"f{j}", self.rng.choice(types)) for j in range(nf)]
            lines = [f"struct {name} {{"]
            for fn_, ft in fields:
                lines.append(f"  {fn_}: {ft};")
            lines.append("}")
            self.struct_defs.append("\n".join(lines))

            make = f"{name}_make"
            fold = f"{name}_fold"
            deco = self.rng.choice(["", "", "@noinline ", "@inline "])
            mk = [f"{deco}fn {make}(x: int64) -> {name} {{",
                  f"  var s: {name};"]
            for fn_, ft in fields:
                kc = self.rng.randint(1, 9)
                if ft == "int32":
                    mk.append(f"  s.{fn_} = (int32)((x + {kc}) & 16383);")
                elif ft == "uint32":
                    mk.append(f"  s.{fn_} = (uint32)((x * {kc}) & 65535);")
                elif ft == "int64":
                    mk.append(f"  s.{fn_} = (x * {kc}) & {MASK};")
                elif ft == "float64":
                    mk.append(f"  s.{fn_} = (float64)((x + {kc}) & 65535);")
                else:  # uint8
                    mk.append(f"  s.{fn_} = (uint8)((x + {kc}) & 255);")
            mk.append("  return s;")
            mk.append("}")
            self.helpers.append("\n".join(mk))

            deco2 = self.rng.choice(["", "", "@noinline "])
            terms = []
            for fn_, ft in fields:
                cast = "" if ft == "int64" else "(int64)"
                terms.append(f"{cast}s.{fn_}")
            fl = [f"{deco2}fn {fold}(s: {name}) -> int64 {{",
                  f"  return ({' + '.join(terms)}) & {MASK};",
                  "}"]
            self.helpers.append("\n".join(fl))
            self.structs.append({"name": name, "fields": fields,
                                 "make": make, "fold": fold})

    def gen_globals(self, k):
        for _ in range(k):
            gname = self.fresh("g")
            self.struct_defs.append(
                f"var {gname}: int64 = {self.rng.randint(0, 999)};")
            reader = None
            if self.rng.random() < 0.7:
                reader = f"pr_{gname}"
                self.helpers.append(
                    f"@pure @noinline fn {reader}(x: int64) -> int64 {{\n"
                    f"  return (x + {gname}) & {MASK};\n"
                    f"}}")
            self.globals.append((gname, reader))

    def gen_helpers(self, k):
        for _ in range(k):
            name = self.fresh("h")
            np = self.rng.choice([1, 1, 1, 2, 6])
            deco = self.rng.choice(["", "", "@inline ", "@noinline ",
                                    "@pure @noinline ", "@pure "])
            params = ", ".join(f"p{j}: int64" for j in range(np))
            body = [f"{deco}fn {name}({params}) -> int64 {{"]
            mix = " + ".join(f"p{j} * {2 * j + 1}" for j in range(np))
            body.append(f"  var r: int64 = ({mix}) & {MASK};")
            m = self.rng.randint(0, 20)
            body.append(f"  var k: int64 = 1;")
            body.append(f"  while (k <= {m}) {{")
            poly = self.rng.choice(["k", "k*k", "(k+r)"])
            body.append(f"    r = (r + {poly}) & {MASK};")
            body.append(f"    k = k + 1;")
            body.append(f"  }}")
            body.append(f"  return r & {MASK};")
            body.append("}")
            self.helpers.append("\n".join(body))
            self.helper_sigs.append((name, np))

    def gen_float_helpers(self):
        # (float32, float32) -> float64 : f32 binop temps cross the call ABI
        if self.rng.random() < 0.8:
            name = self.fresh("fha")
            k = self.rng.randint(1, 9)
            l = self.rng.randint(1, 9)
            c = self.rng.randint(0, 9)
            self.helpers.append(
                f"fn {name}(x: float32, y: float32) -> float64 {{\n"
                f"  var r: float64 = (float64)x * {k}.0 + (float64)y * {l}.0;\n"
                f"  return r + {c}.0;\n"
                f"}}")
            self.float_helpers["f32f32_f64"] = name
        # (float32) -> float32 : the narrowing-return shape
        if self.rng.random() < 0.8:
            name = self.fresh("fhb")
            k = self.rng.randint(1, 9)
            c = self.rng.randint(0, 9)
            self.helpers.append(
                f"fn {name}(x: float32) -> float32 {{\n"
                f"  return x * {k}.0 + {c}.0;\n"
                f"}}")
            self.float_helpers["f32_f32"] = name
        # (float64) -> int64 : trunc on the way out
        if self.rng.random() < 0.8:
            name = self.fresh("fhc")
            k = self.rng.randint(1, 9)
            c = self.rng.randint(0, 9)
            self.helpers.append(
                f"fn {name}(x: float64) -> int64 {{\n"
                f"  return (int64)(x * {k}.0 + {c}.0);\n"
                f"}}")
            self.float_helpers["f64_i64"] = name

    def kernel_loop(self, body_lines, n_expr="n"):
        """Render a counted kernel loop in while or range-for style."""
        ind = "  "
        if self.rng.random() < 0.4:
            out = [f"{ind}for i: int64 in 0..{n_expr} {{"]
            out += [f"{ind}  {b}" for b in body_lines]
            out.append(f"{ind}}}")
        else:
            out = [f"{ind}var i: int64 = 0;",
                   f"{ind}while (i < {n_expr}) {{"]
            out += [f"{ind}  {b}" for b in body_lines]
            out.append(f"{ind}  i = i + 1;")
            out.append(f"{ind}}}")
        return "\n".join(out)

    def gen_kernels(self):
        """Pointer-param kernel helpers: the SIMD recognizers key on loads
        through pointer params, so local-array inline loops never engage
        them. Each kernel is the canonical recognizer shape."""
        r = self.rng
        if r.random() < 0.85:  # float sum
            name = self.fresh("kfs")
            ty = r.choice(["float32", "float64"])
            self.helpers.append(
                f"fn {name}(a: {ty}*, n: int64) -> {ty} {{\n"
                f"  var s: {ty} = 0.0;\n"
                + self.kernel_loop(["s = s + a[i];"]) + "\n"
                f"  return s;\n}}")
            self.kernels["fsum_" + ty] = name
        if r.random() < 0.85:  # float dot
            name = self.fresh("kfd")
            ty = r.choice(["float32", "float64"])
            self.helpers.append(
                f"fn {name}(a: {ty}*, b: {ty}*, n: int64) -> {ty} {{\n"
                f"  var s: {ty} = 0.0;\n"
                + self.kernel_loop(["s = s + a[i] * b[i];"]) + "\n"
                f"  return s;\n}}")
            self.kernels["fdot_" + ty] = name
        if r.random() < 0.7:   # float in-place affine map
            name = self.fresh("kfm")
            ty = r.choice(["float32", "float64"])
            m = 2 if ty == "float32" else r.randint(2, 3)
            add = r.randint(0, 9)
            self.helpers.append(
                f"fn {name}(a: {ty}*, n: int64) {{\n"
                + self.kernel_loop([f"a[i] = a[i] * {m}.0 + {add}.0;"])
                + "\n}")
            self.kernels["fmap_" + ty] = name
        if r.random() < 0.8:   # int32 sum (cast-free widening or cast)
            name = self.fresh("kis")
            body = ("s = s + a[i];" if r.random() < 0.5
                    else "s = s + (int64)a[i];")
            self.helpers.append(
                f"fn {name}(a: int32*, n: int64) -> int64 {{\n"
                f"  var s: int64 = 0;\n"
                + self.kernel_loop([body]) + "\n"
                f"  return s;\n}}")
            self.kernels["isum"] = name
        if r.random() < 0.7:   # int32 dot (int64 acc is the dot_i32 shape)
            name = self.fresh("kid")
            self.helpers.append(
                f"fn {name}(a: int32*, b: int32*, n: int64) -> int64 {{\n"
                f"  var s: int64 = 0;\n"
                + self.kernel_loop(["s = s + a[i] * b[i];"]) + "\n"
                f"  return s;\n}}")
            self.kernels["idot"] = name
        if r.random() < 0.7:   # byte sum
            name = self.fresh("kbs")
            self.helpers.append(
                f"fn {name}(a: uint8*, n: int64) -> int64 {{\n"
                f"  var s: int64 = 0;\n"
                + self.kernel_loop(["s = s + (int64)a[i];"]) + "\n"
                f"  return s;\n}}")
            self.kernels["bsum"] = name
        if r.random() < 0.7:   # byte in-place map chain
            name = self.fresh("kbm")
            ops = []
            for _ in range(r.randint(1, 3)):
                op = r.choice(["*", "+", "-", "^", "&", "|"])
                ops.append(f"a[i] = a[i] {op} {r.randint(1, 255)};")
            self.helpers.append(
                f"fn {name}(a: uint8*, n: int64) {{\n"
                + self.kernel_loop(ops) + "\n}")
            self.kernels["bmap"] = name
        if r.random() < 0.7:   # byte dot -> int32
            name = self.fresh("kbd")
            self.helpers.append(
                f"fn {name}(a: uint8*, b: uint8*, n: int64) -> int32 {{\n"
                f"  var s: int32 = 0;\n"
                + self.kernel_loop(["s = s + (int32)a[i] * (int32)b[i];"])
                + "\n"
                f"  return s;\n}}")
            self.kernels["bdot"] = name
        if r.random() < 0.5:   # int32 matmul (SLP MAC / MIR-SIMD shape)
            name = self.fresh("kmm")
            self.helpers.append(
                f"fn {name}(a: int32*, b: int32*, c: int32*,"
                f" n: int64) {{\n"
                f"  var i: int64 = 0;\n"
                f"  while (i < n) {{\n"
                f"    var j: int64 = 0;\n"
                f"    while (j < n) {{\n"
                f"      var s: int32 = 0;\n"
                f"      var k: int64 = 0;\n"
                f"      while (k < n) {{\n"
                f"        s = s + a[i * n + k] * b[k * n + j];\n"
                f"        k = k + 1;\n"
                f"      }}\n"
                f"      c[i * n + j] = s;\n"
                f"      j = j + 1;\n"
                f"    }}\n"
                f"    i = i + 1;\n"
                f"  }}\n"
                f"}}")
            self.kernels["matmul"] = name

    # ---- top-level program ------------------------------------------------
    def generate(self):
        if self.rng.random() < 0.75:
            self.gen_structs(self.rng.randint(1, 2))
        if self.rng.random() < 0.6:
            self.gen_globals(self.rng.randint(1, 2))
        self.gen_helpers(self.rng.randint(1, 3))
        self.gen_float_helpers()
        self.gen_kernels()

        self.emit("fn main() -> int32 {")
        self.indent += 1
        self.emit("var acc: int64 = 1;")
        self.live_vars = ["acc"]
        # ~8% jumbo mains: enough statements to trip the MIR size bail so
        # the fallback backend sees every shape too
        if self.rng.random() < 0.08:
            nstmts = self.rng.randint(40, 80)
        else:
            nstmts = self.rng.randint(6, 14)
        for _ in range(nstmts):
            self.stmt(self.rng.randint(2, 4))
        for v in self.live_vars:
            if v != "acc":
                self.emit(f"acc = (acc + {v}) & {MASK};")
        self.emit("return (int32)(acc & 255);")
        self.indent -= 1
        self.emit("}")

        header = [f"// seed={self.seed} (genprog v2)", ""]
        return "\n".join(header + self.struct_defs + self.helpers +
                         [""] + self.lines) + "\n"


def generate(seed):
    return Gen(seed).generate()


if __name__ == "__main__":
    import sys
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    sys.stdout.write(generate(seed))
