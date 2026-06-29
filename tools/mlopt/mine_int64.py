#!/usr/bin/env python3
"""Mine irexec-verified (unoptimized IR -> optimized IR) pairs from int64-only
programs, for the end-to-end ML IR-opt loop.

Stronger gate than build_pairs.py: besides the real exe debug/release agreement,
both IR dumps are run through irexec.py and must match the real exit code. That
cross-validates irexec as a trusted verifier on this subset, so the model's
*generated* optimized IR can later be checked by irexec alone.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from build_pairs import build, COMPILER, ROOT  # noqa: E402

GEN = os.path.join(HERE, "gen_int64.py")
IREXEC = os.path.join(ROOT, "tools", "fuzz", "irexec.py")


def gen_program(seed):
    p = subprocess.run([sys.executable, GEN, str(seed)],
                       capture_output=True, text=True, timeout=30)
    return p.stdout if p.returncode == 0 else None


def run_exe(exe):
    try:
        return subprocess.run([exe], capture_output=True, text=True,
                              timeout=30).returncode
    except subprocess.TimeoutExpired:
        return None


def irexec_eval(ir_path):
    try:
        r = subprocess.run([sys.executable, IREXEC, ir_path, "--entry", "main"],
                           capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return None
    out = r.stdout.strip().splitlines()
    if out and out[-1].lstrip("-").isdigit():
        return int(out[-1]) & 0xFF
    return None


def read_text(p):
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--out", default=os.path.join(HERE, "int64_pairs.jsonl"))
    args = ap.parse_args()

    stats = dict(seeds=0, build_fail=0, run_fail=0, divergence=0,
                 irexec_mismatch=0, pairs=0)
    exits = {}
    tmp = tempfile.mkdtemp(prefix="mlopt_i64_")
    out_f = open(args.out, "w", encoding="utf-8")

    for seed in range(args.start, args.start + args.count):
        stats["seeds"] += 1
        src_text = gen_program(seed)
        if not src_text:
            stats["build_fail"] += 1
            continue
        src = os.path.join(tmp, f"s{seed}.mettle")
        with open(src, "w", encoding="utf-8") as f:
            f.write(src_text)
        unopt_exe = os.path.join(tmp, f"s{seed}_u.exe")
        opt_exe = os.path.join(tmp, f"s{seed}_o.exe")
        _, unopt_ir = build(src, unopt_exe, optimized=False)
        _, opt_ir = build(src, opt_exe, optimized=True)
        if unopt_ir is None or opt_ir is None:
            stats["build_fail"] += 1
            continue
        urc, orc = run_exe(unopt_exe), run_exe(opt_exe)
        if urc is None or orc is None:
            stats["run_fail"] += 1
            continue
        if urc != orc:
            stats["divergence"] += 1
            continue
        iu, io = irexec_eval(unopt_ir), irexec_eval(opt_ir)
        if iu != urc or io != orc:
            stats["irexec_mismatch"] += 1
            continue
        out_f.write(json.dumps(dict(seed=seed, exit_code=urc,
                                    input_ir=read_text(unopt_ir),
                                    target_ir=read_text(opt_ir))) + "\n")
        exits[urc] = exits.get(urc, 0) + 1
        stats["pairs"] += 1

    out_f.close()
    print(json.dumps(stats))
    print(f"distinct exit codes: {len(exits)} (non-trivial signal if >1)")
    print(f"pairs -> {args.out}")


if __name__ == "__main__":
    main()
