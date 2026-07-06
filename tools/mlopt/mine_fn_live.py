#!/usr/bin/env python3
"""Fast miner: gen_fn -> classical --release -> sound liveness labels.

No irexec sampling (liveness is static), so this is limited only by compile
time. Produces (classical-optimized function, liveness-dead mask) records for
training the learned dead-code model at scale.
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
import liveness as L  # noqa: E402
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
    ap.add_argument("--out", required=True)
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--count", type=int, default=1000)
    args = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="mlopt_fl_")
    out_f = open(args.out, "w", encoding="utf-8")
    n = ok = ti = td = 0
    for seed in range(args.start, args.start + args.count):
        n += 1
        src = os.path.join(tmp, f"f{seed}.mettle")
        open(src, "w").write(subprocess.run([sys.executable, GEN, str(seed)],
                                            capture_output=True, text=True).stdout)
        dump = classical_dump(src, os.path.join(tmp, f"f{seed}.exe"))
        if not dump:
            continue
        funcs = C.canonical_program(dump)
        if "f" not in funcs or not funcs["f"]:
            continue
        params = V.func_params(funcs, "f")
        instrs = funcs["f"]
        dead = L.dead_indices(instrs, set(params))
        ok += 1
        ti += len(instrs); td += len(dead)
        out_f.write(json.dumps(dict(
            seed=seed, params=params,
            funcs=[dict(name="f", instrs=instrs,
                        delete=[1 if i in dead else 0 for i in range(len(instrs))])]
        )) + "\n")
    out_f.close()
    print(json.dumps(dict(n=n, labeled=ok, instrs=ti, dead=td,
                          frac=round(td / max(1, ti), 3))))


if __name__ == "__main__":
    main()
