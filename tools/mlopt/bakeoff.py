#!/usr/bin/env python3
"""The overnight bake-off: harvest, train four ablations, evaluate, report.

    python bakeoff.py --work _bakeoff --budget 50

Phases (each restartable; `--skip` any that already produced its output):
  harvest   compile the real corpus under the validator gate -> labelled rows
  split     dedup by body, then hold out by INSTRUCTION SHAPE so no function
            appears on both sides under a different temp numbering (see
            split_audit.py).
  train     A / B / C / D on identical data, seed, split, and time budget
  eval      every checkpoint on the held-out real IR
  report    a markdown table

Nothing here touches `gnn_genius.bin` or the shipped pipeline. Checkpoints land
in the work directory under new names; the compiler keeps loading the model it
loads today until someone deliberately exports a new one.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

PY = sys.executable

# The synthetic corpus the shipped model was trained on, kept in the mix so the
# real-IR rounds refine rather than overwrite what it already knows.
ACTION_PATS = ["div_*_act.jsonl", "arr_*_act.jsonl", "xgen_train_act.jsonl",
               "super_train*.jsonl", "ident_train.jsonl"]
COLLAPSE_PATS = ["collapse_train.jsonl", "collapse_train2.jsonl"]

VARIANTS = ["A", "B", "C", "D"]


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def run(cmd, logfile, timeout=None):
    log("$ " + " ".join(str(c) for c in cmd))
    with open(logfile, "a", encoding="utf-8") as lf:
        lf.write(f"\n=== {time.strftime('%Y-%m-%d %H:%M:%S')} :: "
                 f"{' '.join(str(c) for c in cmd)}\n")
        lf.flush()
        try:
            p = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT,
                               timeout=timeout, cwd=HERE)
            return p.returncode
        except subprocess.TimeoutExpired:
            lf.write("\n*** TIMEOUT ***\n")
            return -1


def phase_harvest(work, roots, jobs, out):
    if os.path.exists(out) and os.path.getsize(out) > 0:
        log(f"harvest: {os.path.basename(out)} exists, skipping")
        return 0
    return run([PY, os.path.join(HERE, "harvest.py"), "--roots", *roots,
                "--out", out, "--jobs", str(jobs)],
               os.path.join(work, "harvest.log"))


def phase_split(raw, train_out, held_out, frac=0.2):
    """Deduplicate by FUNCTION BODY, then split on the body hash.

    Splitting by source file is not enough and the first rehearsal proved it.
    Every Mettle binary links the same stdlib, so `mem_copy`, `strncmp`, and
    friends are compiled into program after program with byte-identical bodies:
    72% of held-out function instances had an exact twin in training, and the
    risk head scored 0.997 AUC by recognizing bodies it had already seen.

    Hashing the body and splitting on that hash makes an exact duplicate
    impossible across the boundary, and collapsing duplicates first stops the
    stdlib from dominating the training signal by sheer repetition. Near-
    duplicates (same function inlined into different call sites) can still
    straddle the split, and `split_audit.py` showed exact hashing is not enough
    on its own: after it, 32.5% of held-out bodies still had a training twin with
    an IDENTICAL instruction shape. That is the same source function inlined at a
    second call site, differing only in temp numbering and inlined-parameter
    names, so its canonical text differs and its hash does not collide.
    `write_i32_le` appeared on both sides of the split under its own name.

    So the side is decided by a SHAPE key: the sorted SET of distinct
    instructions with every name and literal replaced by a placeholder. Two
    inlinings of one function share a shape key and therefore a side. Unrelated
    functions that happen to share a skeleton also share a side, which is
    conservative and costs only a little training data.

    The key is a set rather than a multiset deliberately. A multiset key still
    let bodies through that use exactly the same instruction shapes in different
    quantities -- a loop peeled a different number of times, say -- and those are
    the cases hardest to argue are not the same function. What remains is partial
    overlap, which is ordinary generalization rather than leakage."""
    if os.path.exists(held_out) and os.path.getsize(held_out) > 0:
        log("split: exists, skipping")
        return
    import hashlib
    import re
    name_re = re.compile(r"[%@][A-Za-z0-9_.$]*")
    num_re = re.compile(r"(?<![\w%@])-?\d+")

    def shape_key(instrs):
        shapes = sorted({
            re.sub(r"\s+", " ", num_re.sub("K", name_re.sub("N", i))).strip()
            for i in instrs})
        return hashlib.sha1("\n".join(shapes).encode()).hexdigest()

    seen = set()
    ntr = nhe = ndup = 0
    with open(raw, encoding="utf-8") as f, \
            open(train_out, "w", encoding="utf-8") as tf, \
            open(held_out, "w", encoding="utf-8") as hf:
        for line in f:
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            instrs = r["funcs"][0]["instrs"]
            hx = hashlib.sha1("\n".join(instrs).encode()).hexdigest()
            if hx in seen:
                ndup += 1
                continue
            seen.add(hx)
            sk = shape_key(instrs)
            if int(sk[:8], 16) / 0xFFFFFFFF < frac:
                hf.write(line); nhe += 1
            else:
                tf.write(line); ntr += 1
    log(f"split: {ntr} train / {nhe} held-out unique bodies "
        f"({ndup} exact duplicates dropped; sides decided by shape key)")


def phase_train(work, variant, budget, epochs, real_train, seed):
    out = os.path.join(work, f"oracle_{variant}.pt")
    if os.path.exists(out):
        log(f"train {variant}: checkpoint exists, skipping")
        return out
    cmd = [PY, os.path.join(HERE, "train_oracle.py"), "--variant", variant,
           "--action", *ACTION_PATS, real_train,
           "--collapse", *COLLAPSE_PATS,
           "--out", out, "--epochs", str(epochs), "--seed", str(seed),
           "--time-budget", str(budget)]
    rc = run(cmd, os.path.join(work, f"train_{variant}.log"))
    log(f"train {variant}: rc={rc}")
    return out if os.path.exists(out) else None


def phase_eval(work, ckpt, held):
    import eval_oracle
    name = os.path.splitext(os.path.basename(ckpt))[0]
    jpath = os.path.join(work, f"eval_{name}.json")
    if os.path.exists(jpath):
        return json.load(open(jpath, encoding="utf-8"))
    try:
        res = eval_oracle.evaluate(ckpt, [held])
    except Exception as e:
        log(f"eval {name}: FAILED {type(e).__name__}: {e}")
        return dict(model=name, error=f"{type(e).__name__}: {e}")
    with open(jpath, "w", encoding="utf-8") as f:
        json.dump(res, f, indent=2)
    log(f"eval {name}: acc {res.get('acc', 0):.4f}")
    return res


def report(work, results, extra=""):
    lines = ["# gnn_oracle bake-off", "",
             f"Generated {time.strftime('%Y-%m-%d %H:%M:%S')}.", "",
             "Held-out set is real IR. Exact duplicate bodies are dropped and "
             "the side is decided by INSTRUCTION SHAPE, so the same function "
             "cannot appear on both sides under a different temp numbering. "
             "Labels on the real rows are the interpreter gate's own verdicts, "
             "so `DELETE` support here means *the validator confirmed a delete "
             "was sound*.", "",
             "| variant | params | ep | acc | DELETE F1 | GVN F1 | COLLAPSE F1 | "
             "ptr (pointing) | risk AUC | steps |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for r in results:
        if not r or r.get("error"):
            lines.append(f"| {r.get('model','?')} | FAILED: "
                         f"{r.get('error','')} | | | | | | | |")
            continue
        pc = r.get("per_class", {})

        def f1(n):
            return f"{pc[n]['f1']:.3f}" if n in pc else "-"
        ptr = ("-" if r.get("ptr_acc") is None else
               f"{r['ptr_acc']:.3f} ({(r.get('ptr_acc_pointing') or 0):.3f})")
        risk = "-" if r.get("risk_auc") is None else f"{r['risk_auc']:.3f}"
        steps = "-" if r.get("mean_steps") is None else f"{r['mean_steps']:.1f}"
        lines.append(
            f"| {r.get('variant') or r['model']} | {r.get('params',0)/1e6:.2f}M "
            f"| {r.get('train_epoch') or '-'} | {r.get('acc',0):.4f} | "
            f"{f1('DELETE')} | {f1('GVN')} | "
            f"{f1('COLLAPSE')} | {ptr} | {risk} | {steps} |")
    lines += ["", "## Risk-head threshold sweep", "",
              "Only propose where predicted rejection risk is below the "
              "threshold. `reject rate` is what the validator would still throw "
              "out; `retains` is how many sound rewrites survive the filter.", ""]
    for r in results:
        if not r or not r.get("risk_threshold_sweep"):
            continue
        lines.append(f"**{r.get('variant') or r['model']}** "
                     f"(base reject rate {r.get('risk_base_reject_rate',0):.3f}, "
                     f"n={r.get('risk_n',0)})")
        lines.append("")
        lines.append("| threshold | proposals kept | reject rate | sound kept |")
        lines.append("|---|---|---|---|")
        for th, d in r["risk_threshold_sweep"].items():
            lines.append(f"| <{th} | {d['kept_frac']:.2f} | "
                         f"{d['reject_rate']:.3f} | "
                         f"{d['good_kept']}/{d['good_total']} |")
        lines.append("")
    if extra:
        lines += ["## Notes", "", extra]
    path = os.path.join(work, "REPORT.md")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    log(f"report -> {path}")
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=os.path.join(HERE, "_bakeoff"))
    ap.add_argument("--roots", nargs="+", default=[ROOT])
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument("--budget", type=float, default=50.0,
                    help="minutes per variant")
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--variants", nargs="+", default=VARIANTS)
    ap.add_argument("--dagger", action="store_true",
                    help="after the bake-off, re-harvest with the winner and "
                         "warm-start a second round")
    args = ap.parse_args()

    work = os.path.abspath(args.work)
    os.makedirs(work, exist_ok=True)
    raw = os.path.join(work, "real_raw.jsonl")
    rtrain = os.path.join(work, "oracle_real_train.jsonl")
    rheld = os.path.join(work, "oracle_real_held.jsonl")

    t0 = time.time()
    log(f"bake-off starting; work={work}")
    phase_harvest(work, args.roots, args.jobs, raw)
    if not os.path.exists(raw) or os.path.getsize(raw) == 0:
        log("FATAL: harvest produced nothing; aborting")
        return 1
    phase_split(raw, rtrain, rheld)

    results = []
    for v in args.variants:
        log(f"--- variant {v} ---")
        ck = phase_train(work, v, args.budget, args.epochs, rtrain, args.seed)
        if ck:
            results.append(phase_eval(work, ck, rheld))
        else:
            results.append(dict(model=f"oracle_{v}", variant=v,
                                error="training produced no checkpoint"))
        report(work, results)          # rewrite after each variant, so an
                                       # interrupted night still leaves a table

    notes = ""
    if args.dagger:
        ranked = [r for r in results if r and not r.get("error")]
        ranked.sort(key=lambda r: -(r.get("acc") or 0))
        if ranked:
            winner = ranked[0]
            wv = winner.get("variant")
            log(f"--- DAgger round 2 on winner {wv} ---")
            # NOTE: re-harvesting with the winner IN THE LOOP needs the C port of
            # this architecture, which does not exist yet. Until then round 2 is
            # a warm-started refit on the same real rows, which is honest
            # additional training but NOT yet true DAgger.
            ck2 = os.path.join(work, f"oracle_{wv}_r2.pt")
            if not os.path.exists(ck2):
                run([PY, os.path.join(HERE, "train_oracle.py"), "--variant", wv,
                     "--action", *ACTION_PATS, rtrain,
                     "--collapse", *COLLAPSE_PATS, "--out", ck2,
                     "--epochs", "25", "--lr", "3e-4", "--seed", str(args.seed),
                     "--init", os.path.join(work, f"oracle_{wv}.pt"),
                     "--time-budget", str(args.budget)],
                    os.path.join(work, f"train_{wv}_r2.log"))
            if os.path.exists(ck2):
                r2 = phase_eval(work, ck2, rheld)
                r2["variant"] = f"{wv} (round 2)"
                results.append(r2)
            notes = ("Round 2 is a warm-started refit on the same harvested "
                     "rows. True DAgger -- re-collecting with the new model in "
                     "the loop -- needs the C inference port, which is the next "
                     "step, not an overnight one.")

    report(work, results, notes)
    log(f"bake-off done in {(time.time()-t0)/60:.0f} min")
    return 0


if __name__ == "__main__":
    sys.exit(main())
