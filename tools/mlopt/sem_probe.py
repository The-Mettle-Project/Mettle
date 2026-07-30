#!/usr/bin/env python3
"""Does semantic candidate generation find SOUND optimizations the compiler
currently misses? Ask the validator, with no model in the loop.

`obs_gap.py` counts dominating pairs that are value-equal but not
expression-equal: redundancy the shipped model has no edge for. That is a count
of *candidates*, and a candidate is not a rewrite. Eight probes agreeing is
evidence of value equality, not proof, so no sound static analysis in this
codebase will license acting on them.

The interpreter gate will. This isolates the claim end to end:

  1. compile with `--ml-opt` to get the post-classical IR the pass sees
  2. compute the semantic-only dominating pairs on that exact IR
  3. emit each as a `COPY` disposition into a file
  4. recompile with `METTLE_ML_DISP` pointing at it, so those proposals -- and
     nothing else -- go through the interpreter differential

Whatever validates is a sound rewrite that survives the classical optimizer, has
no syntactic GVN edge, and would change the compiled binary. Whatever is rejected
comes back with a counterexample. No network is involved, so the result measures
the FEATURE rather than any model's ability to use it.

    python sem_probe.py --sources ../../examples/*/*.mettle

The pairs are restricted to cases the COPY applier can realize: both sides must
define a `%` temp, since `ml_opt.c` rewrites `dest <- src` and requires an SSA
temp destination.
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
import obs  # noqa: E402
import liveness as L  # noqa: E402
import gnn_oracle as G  # noqa: E402
import harvest as H  # noqa: E402
from gvn import _dominators  # noqa: E402

COMPILER = os.path.join(ROOT, "bin", "mettle.exe")
_DEFNAME = re.compile(r"^(%[A-Za-z0-9_.$]*)\s*(?:=|<-)")


def semantic_only_copies(gidxs, body):
    """-> [(dest_gidx, src_name)] for dominating value-equal pairs that have no
    syntactic expression-key edge and that the COPY applier can realize."""
    n = len(body)
    if n < 2:
        return []
    _, succ = L.build_cfg(body)
    fps, _, _ = obs.fingerprints(body)
    eligible = [v is not None and obs.edge_eligible(v) for v in fps]
    if sum(eligible) < 2:
        return []

    keyof = G._same_expr_keys(body)
    by_key, by_val = {}, {}
    for i in range(n):
        if keyof[i] is not None and eligible[i]:
            by_key.setdefault(keyof[i], []).append(i)
        if eligible[i]:
            by_val.setdefault(fps[i], []).append(i)
    if not any(len(v) > 1 for v in by_val.values()):
        return []

    dom, _ = _dominators(succ, n)
    syn = set(zip(*G._dominating_pairs(by_key, dom))) if by_key else set()
    vs, vd = G._dominating_pairs(by_val, dom)

    out = []
    for j, i in zip(vs, vd):
        if (j, i) in syn:
            continue
        md, ms = _DEFNAME.match(body[i]), _DEFNAME.match(body[j])
        if not md or not ms:
            continue                     # applier needs an SSA temp on both ends
        if md.group(1) == ms.group(1):
            continue
        out.append((gidxs[i], ms.group(1)))
    return out


def compile_once(src, work, disp=None, speculative=False, trace=None):
    env = dict(os.environ)
    if disp:
        env["METTLE_ML_DISP"] = disp
    if trace:
        env["METTLE_ML_TRACE"] = trace
    flag = "--ml-opt-speculative" if speculative else "--ml-opt"
    p = subprocess.run([COMPILER, flag, "--release", src,
                        "-o", os.path.join(work, "a.exe")],
                       capture_output=True, text=True, timeout=300, cwd=work,
                       env=env)
    return p.stdout + p.stderr


def read_verdicts(trace):
    """Exact per-disposition verdicts. The summary line is written for humans and
    varies with pluralization and which clauses are present; this does not."""
    counts = {"validated": 0, "proven": 0, "rejected": 0, "skipped": 0}
    if not os.path.exists(trace):
        return counts, 0
    total = 0
    for line in open(trace, encoding="utf-8", errors="replace"):
        f = line.rstrip("\n").split("\t")
        if len(f) == 5:
            total += 1
            counts[f[4]] = counts.get(f[4], 0) + 1
    return counts, total


def probe_one(src):
    work = tempfile.mkdtemp(prefix="semprobe_")
    try:
        compile_once(src, work)
        irp = os.path.join(work, "_mlopt.ir")
        if not os.path.exists(irp):
            return None
        ir = open(irp, encoding="utf-8", errors="replace").read()

        # What the shipped pass proposes on this same program, so the claim
        # "these are rewrites the compiler does not find" is verified rather
        # than inferred from the absence of a syntactic edge.
        own = set()
        dpath0 = os.path.join(work, "_mlopt.disp")
        if os.path.exists(dpath0):
            for line in open(dpath0, encoding="utf-8", errors="replace"):
                f = line.split()
                if len(f) >= 3:
                    own.add((f[0], f[1]))

        lines, npairs, overlap = [], 0, 0
        for fn, pairs in H.parse_ir_with_gidx(ir).items():
            if len(pairs) < 3:
                continue
            gidxs = [g for g, _ in pairs]
            body = [t for _, t in pairs]
            for gi, srcname in semantic_only_copies(gidxs, body):
                if (fn, str(gi)) in own:
                    overlap += 1          # the pass already proposes here
                    continue
                lines.append(f"{fn} {gi} COPY? {srcname}")
                npairs += 1
        if not lines:
            return dict(src=src, proposed=0, adjudicated=0, validated=0,
                        proven=0, rejected=0, skipped=0, overlap=overlap,
                        examples=[])

        dpath = os.path.join(work, "sem.disp")
        with open(dpath, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        tpath = os.path.join(work, "sem.trace")
        out = compile_once(src, work, disp=dpath, trace=tpath)
        counts, adjudicated = read_verdicts(tpath)
        ex = []
        for line in out.splitlines():
            if "PROPOSAL REJECTED" in line and len(ex) < 2:
                ex.append(line.strip())
        return dict(src=src, proposed=npairs, adjudicated=adjudicated,
                    validated=counts["validated"], proven=counts["proven"],
                    rejected=counts["rejected"], skipped=counts["skipped"],
                    overlap=overlap, examples=ex, lines=lines[:4])
    except (subprocess.TimeoutExpired, OSError):
        return None
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sources", nargs="+", required=True)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    srcs = []
    for pat in args.sources:
        srcs += [os.path.abspath(p) for p in glob.glob(pat)]
    srcs = sorted(set(srcs))
    if args.limit:
        srcs = srcs[:args.limit]
    print(f"sem_probe: {len(srcs)} sources, {args.jobs} jobs", flush=True)

    t0 = time.time()
    tot = dict(proposed=0, adjudicated=0, validated=0, proven=0,
               rejected=0, skipped=0, overlap=0)
    nsrc = 0
    shown = 0
    with ProcessPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(probe_one, s): s for s in srcs}
        for fut in as_completed(futs):
            r = fut.result()
            if not r:
                continue
            nsrc += 1
            for k in tot:
                tot[k] += r[k]
            if r["proposed"] and shown < 5 and r.get("lines"):
                shown += 1
                print(f"  {os.path.basename(r['src'])}: {r['proposed']} proposed,"
                      f" {r['validated']} validated, {r['rejected']} rejected",
                      flush=True)
                for l in r["lines"][:2]:
                    print(f"      {l}", flush=True)

    print()
    print(f"semantic-only reuse candidates, adjudicated by the interpreter gate")
    print(f"  sources compiled     : {nsrc}")
    print(f"  dropped, the shipped pass already proposes them: {tot['overlap']}")
    print(f"  proposed             : {tot['proposed']}")
    print(f"  reached the gate     : {tot['adjudicated']}")
    print(f"  VALIDATED equivalent : {tot['validated']}")
    print(f"  proven-only          : {tot['proven']}")
    print(f"  REJECTED             : {tot['rejected']}")
    print(f"  skipped (applier declined / function unverifiable): "
          f"{tot['skipped']}")
    decided = tot['validated'] + tot['rejected']
    if decided:
        print(f"  of the {decided} the gate actually ruled on: "
              f"{100.0*tot['validated']/decided:.1f}% validated")
    print(f"  ({time.time()-t0:.0f}s)")
    print()
    print("Every validated row is a sound rewrite that survived the classical")
    print("optimizer and has no syntactic GVN edge -- redundancy the shipped")
    print("model's graph cannot represent, found with no model involved.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
