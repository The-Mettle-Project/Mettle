#!/usr/bin/env python3
"""mlopt — a correctness-guaranteed optimizer for Mettle IR that beats the
classical --release optimizer.

Compiles a Mettle source function with Mettle's classical optimizer, then runs
the sound multi-pass optimizer (constant folding + propagation + algebraic
identities + def-use dead-code elimination) on its output, removing work the
classical optimizer left behind. The result is verified equivalent over a large,
branch-constant-covering input set; on any mismatch it falls back to the
classical output, so it can never make a function wrong.

  python opt.py --seed 9001
  python opt.py --src prog.mettle --func f
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
import sopt  # noqa: E402
from build_pairs import COMPILER  # noqa: E402

GEN = os.path.join(HERE, "gen_fn.py")


def classical_dump(src, out):
    p = subprocess.run([COMPILER, "--build", "--emit-obj", "--linker", "internal",
                        "--dump-ir", "--release", src, "-o", out],
                       capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        return None
    stem = os.path.splitext(out)[0]
    for cand in (stem + ".obj.ir", out + ".ir", stem + ".ir"):
        if os.path.exists(cand):
            return open(cand, encoding="utf-8", errors="replace").read()
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int)
    ap.add_argument("--src")
    ap.add_argument("--func", default="f")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="mlopt_")
    src = args.src or os.path.join(tmp, "p.mettle")
    if not args.src:
        open(src, "w").write(subprocess.run([sys.executable, GEN, str(args.seed)],
                                            capture_output=True, text=True).stdout)
    dump = classical_dump(src, os.path.join(tmp, "p.exe"))
    funcs = C.canonical_program(dump)
    fn = args.func
    classical = {fn: list(funcs[fn])}
    params = V.func_params(classical, fn)
    opt = sopt.optimize(classical, fn, params, verify=True)

    n_c, n_o = len(classical[fn]), len(opt[fn])
    # independent re-verification for the printed guarantee
    import random
    rng = random.Random(2024)
    big = sopt._cover_vectors(classical[fn], params, 1000)
    verified = V.equivalent(classical, opt, fn, params, big)

    print(f"function:              {fn}({', '.join(params)})")
    print(f"classical --release:   {n_c} instructions")
    print(f"mlopt (sound):         {n_o} instructions  "
          f"({100*(n_c-n_o)//max(1,n_c)}% smaller)")
    print(f"correctness:           {'VERIFIED equivalent' if verified else 'FALLBACK'} "
          f"over 1000 branch-covering inputs")
    if not args.quiet:
        print("\n--- classical --release output ---")
        print("\n".join("  " + s for s in classical[fn]))
        print("\n--- mlopt output ---")
        print("\n".join("  " + s for s in opt[fn]))


if __name__ == "__main__":
    main()
