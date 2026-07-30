#!/usr/bin/env python3
"""One TRUE DAgger round: put the trained model in the compiler, harvest what the
validator says about its own proposals, and retrain on that.

The bake-off's `--dagger` is a warm-started refit on rows harvested with the
SHIPPED model in the loop. That is more training, not DAgger: the model never
sees the consequences of its own decisions. This does the real thing, which only
became possible once the C inference port existed:

    export the checkpoint  ->  compile the corpus with METTLE_ML_MODEL set to it
    ->  the interpreter gate adjudicates the proposals THIS model made
    ->  those verdicts become labels  ->  warm-start retrain  ->  compare

That closes the loop the whole design is built around. Every rewrite the model
proposes is executed against a pre-rewrite snapshot on generated inputs, so a
proposal that survives is a sound rewrite on real code and one that diverges is a
hard negative with a counterexample behind it. The model's own mistakes become
the next round's curriculum, and no human labels anything.

    python dagger_round.py --model _bakeoff/oracle_C.pt --work _bakeoff --round 2

The held-out set is NOT re-harvested. It stays exactly as the bake-off split it,
so round-over-round numbers are comparable and the new rows cannot leak into it.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

PY = sys.executable


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def run(cmd, logfile):
    log("$ " + " ".join(str(c) for c in cmd))
    with open(logfile, "a", encoding="utf-8") as lf:
        lf.write(f"\n=== {time.strftime('%Y-%m-%d %H:%M:%S')} :: "
                 f"{' '.join(str(c) for c in cmd)}\n")
        lf.flush()
        return subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT,
                              cwd=HERE).returncode


def held_out_shape_keys(held_path):
    """The shape keys already on the held-out side. Any newly harvested row
    matching one of them is dropped rather than trained on -- re-harvesting with
    a new model in the loop must not quietly move the boundary."""
    import re
    name_re = re.compile(r"[%@][A-Za-z0-9_.$]*")
    num_re = re.compile(r"(?<![\w%@])-?\d+")
    keys = set()
    for line in open(held_path, encoding="utf-8"):
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        shapes = sorted({
            re.sub(r"\s+", " ", num_re.sub("K", name_re.sub("N", i))).strip()
            for i in r["funcs"][0]["instrs"]})
        keys.add(hashlib.sha1("\n".join(shapes).encode()).hexdigest())
    return keys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="checkpoint to put in the loop")
    ap.add_argument("--work", default=os.path.join(HERE, "_bakeoff"))
    ap.add_argument("--round", type=int, default=2)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument("--epochs", type=int, default=20)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--budget", type=float, default=60.0)
    # Smaller than the bake-off default: this round is meant to run ALONGSIDE a
    # bake-off variant on the same GPU, and two 16M-parameter models at batch 64
    # do not comfortably share 16GB.
    ap.add_argument("--batch", type=int, default=32)
    args = ap.parse_args()

    work = os.path.abspath(args.work)
    rd = args.round
    blob = os.path.join(work, f"round{rd}_model.bin")
    raw = os.path.join(work, f"round{rd}_raw.jsonl")
    train_out = os.path.join(work, f"round{rd}_train.jsonl")
    held = os.path.join(work, "oracle_real_held.jsonl")
    base_train = os.path.join(work, "oracle_real_train.jsonl")
    out_ckpt = os.path.join(work, f"oracle_round{rd}.pt")
    logf = os.path.join(work, f"dagger{rd}.log")

    if not os.path.exists(held):
        log(f"FATAL: {held} missing; run the bake-off first")
        return 1

    # 1. export the model the compiler will run
    if not os.path.exists(blob):
        rc = run([PY, os.path.join(HERE, "export_oracle.py"), args.model, blob],
                 logf)
        if rc != 0 or not os.path.exists(blob):
            log("FATAL: export failed")
            return 1
    log(f"exported {os.path.basename(blob)}")

    # 2. harvest with THIS model in the loop
    if not os.path.exists(raw):
        rc = run([PY, os.path.join(HERE, "harvest.py"), "--roots", ROOT,
                  "--out", raw, "--jobs", str(args.jobs), "--model", blob], logf)
        if rc != 0 or not os.path.exists(raw):
            log("FATAL: harvest failed")
            return 1

    # 3. keep the held-out boundary exactly where it was
    keys = held_out_shape_keys(held)
    import re
    name_re = re.compile(r"[%@][A-Za-z0-9_.$]*")
    num_re = re.compile(r"(?<![\w%@])-?\d+")
    kept = dropped = 0
    seen = set()
    with open(raw, encoding="utf-8") as f, \
            open(train_out, "w", encoding="utf-8") as tf:
        for line in f:
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            instrs = r["funcs"][0]["instrs"]
            body = "\n".join(instrs)
            h = hashlib.sha1(body.encode()).hexdigest()
            if h in seen:
                continue
            seen.add(h)
            shapes = sorted({
                re.sub(r"\s+", " ", num_re.sub("K", name_re.sub("N", i))).strip()
                for i in instrs})
            sk = hashlib.sha1("\n".join(shapes).encode()).hexdigest()
            if sk in keys:
                dropped += 1
                continue
            tf.write(line)
            kept += 1
    log(f"round {rd} rows: {kept} kept, {dropped} dropped as held-out")

    # 4. warm-start retrain on the union of the original and new rows
    import torch
    ck = torch.load(args.model, map_location="cpu", weights_only=False)
    variant = ck.get("variant", "C")
    from bakeoff import ACTION_PATS, COLLAPSE_PATS
    rc = run([PY, os.path.join(HERE, "train_oracle.py"), "--variant", variant,
              "--action", *ACTION_PATS, base_train, train_out,
              "--collapse", *COLLAPSE_PATS, "--out", out_ckpt,
              "--epochs", str(args.epochs), "--lr", str(args.lr),
              "--batch", str(args.batch),
              "--init", args.model, "--time-budget", str(args.budget)], logf)
    if rc != 0 or not os.path.exists(out_ckpt):
        log("FATAL: retrain failed")
        return 1

    # 5. compare before and after on the untouched held-out set
    import eval_oracle
    before = eval_oracle.evaluate(args.model, [held])
    after = eval_oracle.evaluate(out_ckpt, [held])
    for tag, res in (("round 1", before), (f"round {rd}", after)):
        print()
        print(tag)
        print(eval_oracle.fmt(res))
    with open(os.path.join(work, f"dagger{rd}_result.json"), "w",
              encoding="utf-8") as f:
        json.dump(dict(before=before, after=after, kept=kept, dropped=dropped),
                  f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
