#!/usr/bin/env python3
"""End-to-end check for `sem_probe`: do the binaries actually still behave?

The interpreter gate compares a rewritten function against a pre-rewrite snapshot
on generated inputs. That is strong evidence and it is not the same as running
the real program. This builds each source twice -- once with `--release` alone,
once with `--release --ml-opt` plus the semantic-only COPY dispositions -- runs
both executables, and compares exit status, stdout, and stderr.

Only programs where at least one proposal actually validated are reported, since
the rest produce identical binaries by construction.

    python sem_runtime.py --sources ../../examples/*/*.mettle
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import harvest as H  # noqa: E402
import sem_probe as P  # noqa: E402

COMPILER = P.COMPILER


def run_exe(path, cwd, timeout=60):
    try:
        p = subprocess.run([path], capture_output=True, text=True,
                           timeout=timeout, cwd=cwd)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return "timeout", "", ""
    except OSError as e:
        return f"oserror:{e}", "", ""


def check_one(src):
    work = tempfile.mkdtemp(prefix="semrt_")
    try:
        base = os.path.join(work, "base.exe")
        p = subprocess.run([COMPILER, "--release", src, "-o", base],
                           capture_output=True, text=True, timeout=300, cwd=work)
        if p.returncode != 0 or not os.path.exists(base):
            return None

        P.compile_once(src, work)
        irp = os.path.join(work, "_mlopt.ir")
        if not os.path.exists(irp):
            return None
        ir = open(irp, encoding="utf-8", errors="replace").read()
        lines = []
        for fn, pairs in H.parse_ir_with_gidx(ir).items():
            if len(pairs) < 3:
                continue
            gidxs = [g for g, _ in pairs]
            body = [t for _, t in pairs]
            for gi, sn in P.semantic_only_copies(gidxs, body):
                lines.append(f"{fn} {gi} COPY? {sn}")
        if not lines:
            return None

        dpath = os.path.join(work, "sem.disp")
        with open(dpath, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        out = P.compile_once(src, work, disp=dpath)
        m = re.search(r"(\d+) model proposals?: (\d+) applied \((\d+) validated",
                      out)
        validated = int(m.group(3)) if m else 0
        if validated == 0:
            return None
        opt = os.path.join(work, "a.exe")
        if not os.path.exists(opt):
            return None

        rb = run_exe(base, work)
        ro = run_exe(opt, work)
        return dict(src=src, validated=validated, match=(rb == ro),
                    base=rb[0], opt=ro[0],
                    diff="" if rb == ro else f"base={rb[0]!r} opt={ro[0]!r}")
    except (subprocess.TimeoutExpired, OSError):
        return None
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sources", nargs="+", required=True)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    args = ap.parse_args()

    srcs = sorted({os.path.abspath(p) for pat in args.sources
                   for p in glob.glob(pat)})
    print(f"sem_runtime: {len(srcs)} sources", flush=True)
    t0 = time.time()
    nrun = nmatch = nval = 0
    bad = []
    with ProcessPoolExecutor(max_workers=args.jobs) as ex:
        for fut in as_completed({ex.submit(check_one, s): s for s in srcs}):
            r = fut.result()
            if not r:
                continue
            nrun += 1
            nval += r["validated"]
            if r["match"]:
                nmatch += 1
            else:
                bad.append(r)
    print()
    print(f"programs with >=1 validated semantic rewrite : {nrun}")
    print(f"  total validated rewrites in them           : {nval}")
    print(f"  runtime output identical to baseline       : {nmatch}/{nrun}")
    for r in bad:
        print(f"  *** MISMATCH {os.path.basename(r['src'])}: {r['diff']}")
    print(f"  ({time.time()-t0:.0f}s)")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
