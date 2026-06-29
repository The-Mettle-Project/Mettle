#!/usr/bin/env python3
"""Mine + label parameterized functions for the fair optimizer comparison.

Per seed: generate an @noinline function, compile it with the CLASSICAL
--release optimizer, take that optimized function body as the baseline, then run
the multi-input deletion oracle to find instructions the classical optimizer
LEFT BEHIND (dead/redundant for all inputs). Emits a label record (compatible
with train_delete) whose input is classical's output and whose delete mask is
what classical missed. Also reports how much classical left, the headline
"better than classical" statistic.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import canonicalize as C  # noqa: E402
import fn_verify as V  # noqa: E402
from build_pairs import COMPILER  # noqa: E402

GEN = os.path.join(HERE, "gen_fn.py")


def classical_optimized_f(src, out):
    args = [COMPILER, "--build", "--emit-obj", "--linker", "internal",
            "--dump-ir", "--release", src, "-o", out]
    try:
        p = subprocess.run(args, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return None
    if p.returncode != 0:
        return None
    stem = os.path.splitext(out)[0]
    for cand in (stem + ".obj.ir", out + ".ir", stem + ".ir"):
        if os.path.exists(cand):
            return open(cand, encoding="utf-8", errors="replace").read()
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--count", type=int, default=500)
    ap.add_argument("--k", type=int, default=48, help="input vectors for oracle")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="mlopt_fn_")
    out_f = open(args.out, "w", encoding="utf-8")
    n = ok = c_instr = left = 0
    for seed in range(args.start, args.start + args.count):
        n += 1
        src = os.path.join(tmp, f"f{seed}.mettle")
        with open(src, "w") as f:
            f.write(subprocess.run([sys.executable, GEN, str(seed)],
                                   capture_output=True, text=True).stdout)
        dump = classical_optimized_f(src, os.path.join(tmp, f"f{seed}.exe"))
        if not dump:
            continue
        funcs = C.canonical_program(dump)
        if "f" not in funcs or not funcs["f"]:
            continue
        params = V.func_params(funcs, "f")
        if not params:
            continue
        vecs = V.arg_vectors(len(params), k=args.k, seed=seed)
        _, mask = V.oracle_delete(funcs, "f", vecs, params)
        ok += 1
        c_instr += len(funcs["f"])
        left += sum(mask)
        out_f.write(json.dumps(dict(
            seed=seed, params=params,
            funcs=[dict(name="f", instrs=funcs["f"], delete=mask)])) + "\n")
    out_f.close()
    print(json.dumps(dict(n=n, labeled=ok, classical_instrs=c_instr,
                          classical_left_removable=left,
                          frac=round(left / max(1, c_instr), 3))))


if __name__ == "__main__":
    main()
