#!/usr/bin/env python3
"""Let the model NAME its own reuse targets, and ask the validator what it thinks.

`sem_probe.py` shows that semantic candidate generation finds sound rewrites, but
it picks targets with a script. This is the version where the pointer head picks
them: `METTLE_ML_PTR=1` makes `ml_gnn.c` score every (node, dominating candidate)
pair with the trained bilinear head and emit the winner.

Any target the head chooses that the sound analysis did not is emitted as `COPY?`
-- model-sourced, no construction-time proof -- so it must clear the interpreter
differential even on functions the gate would otherwise let through unchecked.
This script counts what happens to exactly those proposals.

    python ptr_probe.py --blob _bakeoff/oracle_C.bin --sources ../../examples/*/*.mettle

A `COPY?` that validates is a reuse target a neural network chose and the
compiler's reference interpreter then confirmed is behaviour-preserving.
"""
import argparse
import collections
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

COMPILER = os.path.join(ROOT, "bin", "mettle.exe")
_BLOB = None


def probe_one(src):
    work = tempfile.mkdtemp(prefix="ptrprobe_")
    try:
        trace = os.path.join(work, "t.tsv")
        env = dict(os.environ, METTLE_ML_TRACE=trace, METTLE_ML_PTR="1",
                   METTLE_ML_MODEL=_BLOB)
        subprocess.run([COMPILER, "--ml-opt", "--release", src,
                        "-o", os.path.join(work, "a.exe")],
                       capture_output=True, text=True, timeout=300, cwd=work,
                       env=env)
        if not os.path.exists(trace):
            return None
        c = collections.Counter()
        for line in open(trace, encoding="utf-8", errors="replace"):
            f = line.rstrip("\n").split("\t")
            if len(f) == 5:
                c[(f[3], f[4])] += 1
        return c
    except (subprocess.TimeoutExpired, OSError):
        return None
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _init(blob):
    global _BLOB
    _BLOB = blob


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--blob", required=True)
    ap.add_argument("--sources", nargs="+", required=True)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    args = ap.parse_args()
    blob = os.path.abspath(args.blob)
    srcs = sorted({os.path.abspath(p) for pat in args.sources
                   for p in glob.glob(pat)})
    print(f"ptr_probe: {len(srcs)} sources, model={os.path.basename(blob)}",
          flush=True)

    t0 = time.time()
    tot = collections.Counter()
    nsrc = 0
    with ProcessPoolExecutor(max_workers=args.jobs, initializer=_init,
                             initargs=(blob,)) as ex:
        for fut in as_completed({ex.submit(probe_one, s): s for s in srcs}):
            c = fut.result()
            if c is None:
                continue
            nsrc += 1
            tot.update(c)

    kinds = sorted({k for k, _ in tot})
    print()
    print(f"dispositions by kind and verdict ({nsrc} sources, "
          f"{time.time()-t0:.0f}s)")
    print(f"  {'kind':10s} {'validated':>10s} {'rejected':>9s} {'proven':>7s} "
          f"{'skipped':>8s}")
    for k in kinds:
        print(f"  {k:10s} {tot[(k,'validated')]:10d} {tot[(k,'rejected')]:9d} "
              f"{tot[(k,'proven')]:7d} {tot[(k,'skipped')]:8d}")

    v, r = tot[("COPY?", "validated")], tot[("COPY?", "rejected")]
    print()
    if v + r:
        print(f"MODEL-CHOSEN reuse targets adjudicated by the gate: "
              f"{v} validated, {r} rejected ({100.0*v/(v+r):.1f}% sound)")
    else:
        print("the pointer head proposed no target the sound analysis "
              "had not already found")
    print("(`COPY` rows are the sound analysis's own choices, unchanged.)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
