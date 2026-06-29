#!/usr/bin/env python3
"""Mine (unoptimized IR -> optimized IR) training pairs for the ML IR-opt model.

For each seed: generate a UB-free program with the fuzzer's genprog, build it
DEBUG (optimizer off -> input IR) and RELEASE (optimizer on -> target IR), each
with --dump-ir to capture the IR sidecar. Run both binaries; the debug build is
the trusted oracle (the optimizer only runs at release). A pair is emitted only
when both builds compile, both run, and their exit codes AGREE -- i.e. the
release IR is a verified semantics-preserving optimization of the debug IR.
Divergences are quarantined: those are miscompiles, not training data.

This is the Milestone-1 go/no-go: does a clean verified pair dataset exist at
scale, and at what yield?

Usage:
  python tools/mlopt/build_pairs.py --count 100 [--start 1] [--out pairs.jsonl]
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
COMPILER = os.path.join(ROOT, "bin", "mettle.exe")
GENPROG = os.path.join(ROOT, "tools", "fuzz", "genprog.py")

# Every optimizer pass name (fixpoint + named + program-level). Disabling all of
# them in a --release build yields release-SHAPED but UNOPTIMIZED IR: the same
# function set and no debug crash-trap instrumentation as the optimized target,
# so the two differ by ONLY the optimizer. This is also the exact IR the
# deployed --ml-opt model sees at the pre-optimization point of a release build,
# so training input matches serving input.
SKIP_ALL_PASSES = ",".join([
    # fixpoint passes (IR_OPT_PASS_LIST)
    "reduction_unroll", "copy_and_constant_propagation", "fuse_rotate_add",
    "strength_reduce_rotate_loops", "unroll_small_const_bound_loops",
    "positive_loop_div2_to_shift", "fold_popcount_byte_loop",
    "fuse_popcount_buffer_loop", "collatz_odd_step_fold",
    "coalesce_single_use_temp_assign",
    "eliminate_single_use_float_symbol_copies",
    "common_subexpression_elimination", "constant_and_branch_simplify",
    "reassociate_constants", "count_word_starts", "eliminate_dead_temp_writes",
    "thread_jump_targets", "null_check_licm",
    "remove_empty_conditional_diamonds",
    "remove_redundant_fallthrough_branches", "remove_redundant_jumps",
    "eliminate_unreachable_straightline", "eliminate_unreachable_blocks",
    "remove_unused_labels", "memcpy_inline", "memcmp_byte_loop",
    "eliminate_load_symbol_copy", "simd_sum_i32", "simd_sum_u8",
    "simd_byte_map", "simd_dot_i32", "simd_dot_i8", "simd_slp_mac_i32",
    "simd_slp_mac_i8", "simd_insertion_sort_i32", "sroa",
    # post-fixpoint / pre-inline named passes
    "simd_minmax_i32", "prefix_sum_i32", "induction_pointer", "lower_bound_i32",
    "simd_fill", "simd_affine_map_float", "simd_exp_f32", "simd_silu_f32",
    "simd_lcg", "simd_i2f_reduce", "simd_dot_float", "simd_sum_float",
    "auto_vectorize", "auto_vectorize_int", "auto_vectorize_find",
    "outer_vectorize", "simd_memory_map", "detect_shift_loops",
    "eliminate_congruent_ivs",
    # program-level passes
    "inline_small_functions", "inline_self_recursion", "hoist_pure_calls",
])


def gen_program(seed):
    p = subprocess.run([sys.executable, GENPROG, str(seed)],
                       capture_output=True, text=True, timeout=30)
    if p.returncode != 0:
        return None
    return p.stdout


def build(src, out, optimized):
    """Build src to an exe with an IR sidecar. Both modes build --release so the
    function set and (lack of) instrumentation match; the unoptimized input is
    produced by disabling every pass. Returns (rc, ir_path|None)."""
    args = [COMPILER, "--build", "--emit-obj", "--linker", "internal",
            "--dump-ir", "--release"]
    args += [src, "-o", out]
    env = dict(os.environ)
    if optimized:
        env.pop("METTLE_SKIP_PASS", None)
    else:
        env["METTLE_SKIP_PASS"] = SKIP_ALL_PASSES
    try:
        p = subprocess.run(args, capture_output=True, text=True, timeout=120,
                           env=env)
    except subprocess.TimeoutExpired:
        return None, None
    if p.returncode != 0:
        return p.returncode, None
    # The .ir sidecar is named after the OBJECT file the compiler emits, which
    # with --build is the -o path with its extension swapped to .obj. Probe the
    # known candidates rather than assume.
    stem = os.path.splitext(out)[0]
    for cand in (stem + ".obj.ir", out + ".ir", stem + ".ir"):
        if os.path.exists(cand):
            return 0, cand
    return 0, None


def run_exe(exe):
    try:
        p = subprocess.run([exe], capture_output=True, text=True, timeout=30)
        return p.returncode
    except subprocess.TimeoutExpired:
        return None


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--out", default=os.path.join(HERE, "pairs.jsonl"))
    ap.add_argument("--quarantine", default=os.path.join(HERE, "quarantine.jsonl"))
    args = ap.parse_args()

    stats = dict(seeds=0, gen_fail=0, unopt_build_fail=0, opt_build_fail=0,
                 run_fail=0, divergence=0, pairs=0)
    tmp = tempfile.mkdtemp(prefix="mlopt_pairs_")
    out_f = open(args.out, "w", encoding="utf-8")
    quar_f = open(args.quarantine, "w", encoding="utf-8")

    for seed in range(args.start, args.start + args.count):
        stats["seeds"] += 1
        src_text = gen_program(seed)
        if src_text is None:
            stats["gen_fail"] += 1
            continue
        src = os.path.join(tmp, f"s{seed}.mettle")
        with open(src, "w", encoding="utf-8") as f:
            f.write(src_text)

        unopt_exe = os.path.join(tmp, f"s{seed}_unopt.exe")
        opt_exe = os.path.join(tmp, f"s{seed}_opt.exe")
        rc, unopt_ir = build(src, unopt_exe, optimized=False)
        if unopt_ir is None:
            stats["unopt_build_fail"] += 1
            continue
        rc, opt_ir = build(src, opt_exe, optimized=True)
        if opt_ir is None:
            stats["opt_build_fail"] += 1
            continue

        # The unoptimized build is the trusted oracle (no optimizer to introduce
        # bugs); agreement means the optimizer was semantics-preserving here.
        unopt_rc = run_exe(unopt_exe)
        opt_rc = run_exe(opt_exe)
        if unopt_rc is None or opt_rc is None:
            stats["run_fail"] += 1
            continue
        if unopt_rc != opt_rc:
            stats["divergence"] += 1
            quar_f.write(json.dumps(dict(seed=seed, unopt_rc=unopt_rc,
                                         opt_rc=opt_rc)) + "\n")
            continue

        rec = dict(seed=seed, exit_code=unopt_rc,
                   input_ir=read_text(unopt_ir), target_ir=read_text(opt_ir))
        out_f.write(json.dumps(rec) + "\n")
        stats["pairs"] += 1

    out_f.close()
    quar_f.close()
    yield_pct = 100.0 * stats["pairs"] / max(1, stats["seeds"])
    print(json.dumps(stats))
    print(f"yield: {stats['pairs']}/{stats['seeds']} = {yield_pct:.1f}%")
    print(f"pairs -> {args.out}")
    if stats["divergence"]:
        print(f"quarantined {stats['divergence']} divergences -> {args.quarantine}")


if __name__ == "__main__":
    main()
