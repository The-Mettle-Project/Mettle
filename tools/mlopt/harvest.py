#!/usr/bin/env python3
"""Validator-in-the-loop corpus harvest: turn the interpreter gate into a
labelling oracle over REAL IR.

The shipped model was trained almost entirely on synthetic functions. Real IR is
the distribution it actually runs on, and hand-labelling it is infeasible -- but
the `--ml-opt` pass already contains an oracle. Every speculative proposal is
executed through the reference interpreter against a pre-rewrite snapshot: the
ones that survive are sound rewrites on real code, and the ones that diverge are
hard negatives with a counterexample behind them. Nothing about that signal was
being kept.

This harness compiles a corpus under `--ml-opt-speculative` with
`METTLE_ML_TRACE` set, pairs each proposal with its verdict, and emits training
rows in the same schema as the synthetic corpus plus a `risk` vector:

    action[i]  validator-certified rewrite class at node i (KEEP elsewhere)
    risk[i]    1 = the gate REJECTED a proposal here, 0 = it validated one,
               -100 = no adjudicated proposal (loss ignores it)

Because each round re-collects with the current model in the loop, this is
DAgger: round N+1 trains on the mistakes round N actually made, rather than on a
fixed synthetic distribution. The `risk` head is what closes the loop -- a model
that predicts its own rejections can decline to propose them.

    python harvest.py --roots ../.. --out real_wave0.jsonl --jobs 8

Compiles run in per-worker directories because the pass writes `_mlopt.ir`,
`_mlopt.disp`, and `_mlopt.explain` relative to the working directory.
"""
import argparse
import json
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
import canonicalize as C  # noqa: E402

COMPILER = os.path.join(ROOT, "bin", "mettle.exe")

# Disposition kind -> action class. The trace records the DISPOSITION the applier
# received, not the model's raw argmax, so this mapping is approximate where two
# actions lower to the same disposition: AFFINE and GVN both emit COPY, and
# COLLAPSE shares REWRITE with the superoptimizers. Treating COPY as GVN and
# REWRITE as COLLAPSE keeps the label honest about what was VALIDATED, which is
# what these rows are for.
KIND_ACTION = {"NOP": 1, "CONST": 2, "COPY": 4, "REWRITE": 5}

_INSTR_RE = re.compile(r"^\s*(\d+):\s+(.*)$")
_FUNC_RE = re.compile(r"^function (\S+)\s*\{")


def parse_ir_with_gidx(text):
    """{func: [(gidx, raw_line)]} with `nop` lines dropped.

    `ml_gnn.c` strips dead nops before building the graph but keeps each
    surviving instruction's ORIGINAL index, and dispositions are addressed by
    that index. So the rows here must drop nops the same way and carry the
    original index alongside, or every label lands on the wrong instruction."""
    funcs, cur = {}, None
    for line in text.splitlines():
        m = _FUNC_RE.match(line)
        if m:
            cur = m.group(1)
            funcs[cur] = []
            continue
        if cur is None:
            continue
        if line.strip() == "}":
            cur = None
            continue
        im = _INSTR_RE.match(line)
        if im:
            body = im.group(2).strip()
            if body != "nop":
                funcs[cur].append((int(im.group(1)), body))
    return funcs


def canonical_with_gidx(pairs):
    """Apply the corpus canonicalization to (gidx, raw) pairs, keeping alignment.
    `canonicalize_fn` drops nops (already gone here) and renames temps/labels, so
    it is order-preserving and the gidx list stays valid.

    NOT used for harvesting -- see `rows_for`. Kept because it is the right thing
    for corpora that will never be featurized by the compiler."""
    raw = [t for _, t in pairs]
    canon = C.canonicalize_fn(raw)
    if len(canon) != len(raw):
        return None
    return [g for g, _ in pairs], canon


def infer_params(body):
    """Names used before any definition: the function's inputs. Mirrors the
    `infer_params` heuristic in ml_gnn.c rather than importing irexec's, so the
    graph the trainer builds matches the graph the compiler built."""
    defined, params = set(), []
    for ins in body:
        m = re.match(r"^(\S+)\s*(?:=|<-|\+=)\s*(.*)$", ins)
        lhs, rhs = (m.group(1), m.group(2)) if m else (None, ins)
        if ins.startswith(("label ", "jump ")):
            continue
        for tok in re.findall(r"@[A-Za-z0-9_.$]*", rhs if m else ins):
            base = tok.split(".", 1)[0]
            if base not in defined and base not in params:
                params.append(base)
        if ins.startswith("local "):
            mm = re.match(r"local (@\S+)", ins)
            if mm:
                defined.add(mm.group(1).split(".", 1)[0])
        elif lhs and lhs.startswith("@"):
            defined.add(lhs.split(".", 1)[0])
    return params


def compile_one(src, timeout):
    """Compile one source with the gate armed; -> (ir_text, [(fn, gidx, kind,
    verdict)]) or None. Runs in its own directory: the pass writes its scratch
    files relative to the working directory, so parallel workers would clobber
    each other."""
    work = tempfile.mkdtemp(prefix="harvest_")
    trace = os.path.join(work, "trace.tsv")
    try:
        env = dict(os.environ, METTLE_ML_TRACE=trace)
        out = os.path.join(work, "a.exe")
        p = subprocess.run([COMPILER, "--ml-opt-speculative", "--release",
                            src, "-o", out],
                           capture_output=True, text=True, timeout=timeout,
                           cwd=work, env=env)
        irp = os.path.join(work, "_mlopt.ir")
        if not os.path.exists(irp):
            return None
        ir = open(irp, encoding="utf-8", errors="replace").read()
        recs = []
        if os.path.exists(trace):
            for line in open(trace, encoding="utf-8", errors="replace"):
                f = line.rstrip("\n").split("\t")
                if len(f) == 5:
                    recs.append((f[1], int(f[2]), f[3], f[4]))
        return ir, recs, p.returncode
    except (subprocess.TimeoutExpired, OSError):
        return None
    finally:
        shutil.rmtree(work, ignore_errors=True)


def rows_for(src, timeout):
    got = compile_one(src, timeout)
    if not got:
        return []
    ir, recs, _rc = got
    by_fn = {}
    for fn, gidx, kind, verdict in recs:
        by_fn.setdefault(fn, []).append((gidx, kind, verdict))

    rows = []
    for fn, pairs in parse_ir_with_gidx(ir).items():
        if not pairs or len(pairs) < 3:
            continue
        # RAW dump text, deliberately not canonicalized. `ml_gnn.c` featurizes
        # the dump exactly as written, so canonicalizing here would train the
        # model on text the compiler never shows it. Measured with
        # `check_forward.py`, that skew alone moved Python/C prediction agreement
        # from 99.3% to 97.8% -- small, invisible, and entirely self-inflicted.
        # Nops are already dropped by parse_ir_with_gidx, matching what the
        # compiler strips.
        gidxs = [g for g, _ in pairs]
        body = [t for _, t in pairs]
        pos = {g: i for i, g in enumerate(gidxs)}
        action = [0] * len(body)
        risk = [-100] * len(body)
        adjudicated = 0
        for gidx, kind, verdict in by_fn.get(fn, ()):
            i = pos.get(gidx)
            if i is None:
                continue
            if verdict in ("validated", "proven"):
                action[i] = KIND_ACTION.get(kind, 0)
                risk[i] = 0
                adjudicated += 1
            elif verdict == "rejected":
                # The proposal was WRONG here: the action label stays KEEP and
                # the risk head learns that this site is a trap.
                risk[i] = 1
                adjudicated += 1
        rows.append(dict(seed=f"{os.path.basename(src)}::{fn}",
                         source=os.path.relpath(src, ROOT).replace("\\", "/"),
                         params=infer_params(body),
                         adjudicated=adjudicated,
                         funcs=[dict(name=fn, instrs=body, action=action,
                                     risk=risk)]))
    return rows


def _work(a):
    src, timeout = a
    try:
        return src, rows_for(src, timeout)
    except Exception as e:                     # one bad source must not kill the run
        return src, [{"__error__": f"{type(e).__name__}: {e}"}]


def discover(roots, limit=0):
    """Absolute paths only. Each compile runs in its own temporary directory (the
    pass writes `_mlopt.ir` relative to the working directory), so a relative
    source path would be resolved against that temp directory and silently find
    nothing -- the run reports sources discovered and zero functions harvested."""
    srcs = []
    for root in roots:
        for dirpath, dirnames, files in os.walk(os.path.abspath(root)):
            dirnames[:] = [d for d in dirnames
                           if d not in (".git", "obj", "obj-linux", "obj_c99m",
                                        "obj_lnx", "bin", ".vs", "node_modules",
                                        ".elfwork", ".tmp")]
            for f in files:
                if f.endswith(".mettle"):
                    srcs.append(os.path.join(dirpath, f))
    srcs = sorted(set(srcs))
    return srcs[:limit] if limit else srcs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roots", nargs="+", default=[ROOT])
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--model", default=None,
                    help="METTLE_ML_MODEL for this round (DAgger: the model "
                         "currently in the loop)")
    args = ap.parse_args()

    if args.model:
        os.environ["METTLE_ML_MODEL"] = os.path.abspath(args.model)
    srcs = discover(args.roots, args.limit)
    print(f"harvest: {len(srcs)} sources, {args.jobs} jobs, "
          f"model={args.model or 'default'}", flush=True)

    t0 = time.time()
    nfn = nadj = nrej = nval = nerr = 0
    done = 0
    with open(args.out, "w", encoding="utf-8") as f, \
            ProcessPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(_work, (s, args.timeout)): s for s in srcs}
        for fut in as_completed(futs):
            src, rows = fut.result()
            done += 1
            for r in rows:
                if "__error__" in r:
                    nerr += 1
                    continue
                f.write(json.dumps(r) + "\n")
                nfn += 1
                nadj += r["adjudicated"]
                nrej += sum(1 for v in r["funcs"][0]["risk"] if v == 1)
                nval += sum(1 for v in r["funcs"][0]["risk"] if v == 0)
            if done % 50 == 0 or done == len(srcs):
                el = time.time() - t0
                print(f"  {done}/{len(srcs)} src  {nfn} fns  {nadj} adjudicated "
                      f"({nval} validated / {nrej} rejected)  {el:.0f}s",
                      flush=True)
    print(f"harvest -> {args.out}: {nfn} functions, {nadj} adjudicated "
          f"({nval} validated, {nrej} rejected), {nerr} errors, "
          f"{time.time()-t0:.0f}s", flush=True)


if __name__ == "__main__":
    main()
