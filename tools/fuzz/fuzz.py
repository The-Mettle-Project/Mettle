#!/usr/bin/env python3
"""Differential miscompile fuzzer for the Mettle compiler.

For each seed it generates a self-contained program (genprog.py), builds it at
debug and at release, runs both, and compares exit codes. Debug is the trusted
oracle: the optimizer only runs at -O/--release, so a debug-vs-release exit-code
divergence is a silent miscompile. A build failure at exactly one level, or a
crash (exit code indicating a fault) at one level only, is also flagged.

Repros are written to tools/fuzz/repros/<seed>.mettle for replay. The seed is
embedded in each program header, so any failing case is fully reproducible with
`python genprog.py <seed>`.

Usage:
  python fuzz.py [--count N] [--start S] [--compiler PATH] [--keep]
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import genprog  # noqa: E402

REPRO_DIR = os.path.join(HERE, "repros")
DEFAULT_COMPILER = os.path.join(HERE, "..", "..", "bin", "mettle.exe")

# Pass lists for failure-time attribution (METTLE_SKIP_PASS bisection).
# Names must match src/ir/optimizer (fixpoint list + named-sequence stages).
SIMD_PASSES = [
    "simd_sum_i32", "simd_sum_u8", "simd_byte_map", "simd_dot_i32",
    "simd_dot_i8", "simd_slp_mac_i32", "simd_slp_mac_i8",
    "simd_insertion_sort_i32", "simd_minmax_i32", "simd_affine_map_float",
    "simd_exp_f32", "simd_i2f_reduce", "simd_dot_float", "simd_sum_float",
    "auto_vectorize", "outer_vectorize", "simd_memory_map",
]
OTHER_PASSES = [
    "inline_small_functions",
    "reduction_unroll", "copy_and_constant_propagation",
    "unroll_small_const_bound_loops", "coalesce_single_use_temp_assign",
    "eliminate_single_use_float_symbol_copies", "sroa",
    "common_subexpression_elimination", "constant_and_branch_simplify",
    "eliminate_dead_temp_writes", "thread_jump_targets", "null_check_licm",
    "hoist_pure_calls", "induction_pointer", "prefix_sum_i32",
    "lower_bound_i32", "detect_shift_loops", "eliminate_congruent_ivs",
    "positive_loop_div2_to_shift", "memcpy_inline",
    "eliminate_load_symbol_copy", "fuse_rotate_add",
    "strength_reduce_rotate_loops",
]


def build(compiler, src, out, release, skip=None, no_mir=False):
    args = [compiler, "--build", "--emit-obj", "--linker", "internal"]
    if release:
        args.append("--release")
    args += [src, "-o", out]
    env = dict(os.environ)
    if skip:
        env["METTLE_SKIP_PASS"] = ",".join(skip)
    else:
        env.pop("METTLE_SKIP_PASS", None)
    if no_mir:
        env["METTLE_MIR"] = "0"
    else:
        env.pop("METTLE_MIR", None)
    p = subprocess.run(args, capture_output=True, text=True, timeout=120,
                       env=env)
    return p.returncode, (p.stdout + p.stderr)


def run(exe):
    try:
        p = subprocess.run([exe], capture_output=True, text=True, timeout=30)
        return p.returncode, (p.stdout + p.stderr)
    except subprocess.TimeoutExpired:
        return "TIMEOUT", ""


def is_crash(code):
    # Windows fault exit codes are large negatives / 0xC0000005-style values.
    return isinstance(code, int) and (code < 0 or code > 255)


def attribute(compiler, src, rel_exe, debug_rc):
    """Bisect a divergence to an optimizer pass via METTLE_SKIP_PASS.
    Returns a short human-readable attribution string."""
    def rc_with(skip):
        bc, _ = build(compiler, src, rel_exe, release=True, skip=skip)
        if bc != 0:
            return None
        rc, _ = run(rel_exe)
        return rc

    for group_name, group in (("SIMD", SIMD_PASSES), ("other", OTHER_PASSES)):
        if rc_with(SIMD_PASSES if group_name == "SIMD"
                   else SIMD_PASSES + OTHER_PASSES) == debug_rc:
            for name in group:
                if rc_with([name]) == debug_rc:
                    return f"pass={name}"
            return f"pass-group={group_name} (no single pass; interaction?)"
    return "backend/codegen (persists with all IR passes skipped)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--compiler", default=DEFAULT_COMPILER)
    ap.add_argument("--keep", action="store_true",
                    help="keep generated source for every seed, not just failures")
    args = ap.parse_args()

    os.makedirs(REPRO_DIR, exist_ok=True)
    tmp = os.environ.get("TEMP", "/tmp")
    # PID-unique so multiple fuzz shards can run concurrently without
    # clobbering each other's source/executables.
    tag = os.getpid()
    src = os.path.join(tmp, f"mettle_fuzz_{tag}.mettle")
    dbg = os.path.join(tmp, f"mettle_fuzz_{tag}_dbg.exe")
    rel = os.path.join(tmp, f"mettle_fuzz_{tag}_rel.exe")

    failures = []
    for seed in range(args.start, args.start + args.count):
        prog = genprog.generate(seed)
        with open(src, "w") as f:
            f.write(prog)

        def save_repro(reason):
            path = os.path.join(REPRO_DIR, f"{seed}.mettle")
            with open(path, "w") as f:
                f.write(prog)
            failures.append((seed, reason))
            print(f"  [FAIL seed={seed}] {reason}  -> {path}")

        dbg_bc, dbg_blog = build(args.compiler, src, dbg, release=False)
        rel_bc, rel_blog = build(args.compiler, src, rel, release=True)

        if dbg_bc != 0 and rel_bc != 0:
            # Both reject -> likely a generator bug, not a miscompile. Skip noisily.
            print(f"  [skip seed={seed}] both builds failed (generator issue?)")
            continue
        if (dbg_bc == 0) != (rel_bc == 0):
            save_repro(f"build divergence: debug rc={dbg_bc}, release rc={rel_bc}")
            continue

        dbg_rc, _ = run(dbg)
        rel_rc, _ = run(rel)

        # Backend-vs-backend oracle: the same unoptimized IR through the MIR
        # and fallback backends must agree. A mismatch is a definite codegen
        # bug in one of them, independent of (and invisible to) the
        # debug-vs-release comparison when the buggy backend wins both.
        fbk_bc, _ = build(args.compiler, src, dbg, release=False, no_mir=True)
        fbk_rc = run(dbg)[0] if fbk_bc == 0 else None

        if dbg_rc != rel_rc:
            attr = attribute(args.compiler, src, rel, dbg_rc)
            save_repro(f"EXIT DIVERGENCE: debug={dbg_rc}, release={rel_rc}"
                       f" [{attr}]")
        elif fbk_rc is not None and fbk_rc != dbg_rc:
            save_repro(f"BACKEND DIVERGENCE at -O0: mir={dbg_rc},"
                       f" fallback={fbk_rc}")
        elif is_crash(dbg_rc) and is_crash(rel_rc):
            save_repro(f"both crash (rc={dbg_rc}) -- possible UB in generator")
        elif args.keep:
            with open(os.path.join(REPRO_DIR, f"ok_{seed}.mettle"), "w") as f:
                f.write(prog)

        if (seed - args.start + 1) % 25 == 0:
            print(f"  ...{seed - args.start + 1}/{args.count} done, "
                  f"{len(failures)} failures")

    print(f"\nDone. {len(failures)} failure(s) out of {args.count} seeds.")
    for seed, reason in failures:
        print(f"  seed {seed}: {reason}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
