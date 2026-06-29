#!/usr/bin/env python3
"""Build the seq2seq training dataset for the ML IR-opt model.

Each mined (unoptimized IR, optimized IR) pair becomes a token sequence pair:
  input  = digitize(program_tokens(canonical(unoptimized IR)))
  target = digitize(program_tokens(canonical(optimized IR)))
Both are whole-program, irexec-reconstructable (verified 88/88). Pairs longer
than --max-len (either side) are dropped to keep training tractable. Vocab is
built from the train split only; val holds out unseen seeds.
"""
import argparse
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import canonicalize as C  # noqa: E402

SPECIAL = ["<pad>", "<bos>", "<eos>", "<unk>"]


def pair_tokens(ir_text):
    return C.digitize(C.program_tokens(C.canonical_program(ir_text)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pairs", nargs="+",
                    default=[os.path.join(HERE, "int64_pairs.jsonl")])
    ap.add_argument("--out", default=os.path.join(HERE, "data"))
    ap.add_argument("--max-len", type=int, default=384)
    ap.add_argument("--val-frac", type=float, default=0.1)
    args = ap.parse_args()

    rows = []
    for pat in args.pairs:
        for path in glob.glob(pat):
            for line in open(path, encoding="utf-8"):
                rows.append(json.loads(line))
    # de-dup by seed (parallel mines may overlap ranges)
    seen, uniq = set(), []
    for r in rows:
        if r["seed"] in seen:
            continue
        seen.add(r["seed"])
        uniq.append(r)
    rows = uniq

    samples = []
    dropped = 0
    for r in rows:
        inp = pair_tokens(r["input_ir"])
        tgt = pair_tokens(r["target_ir"])
        if len(inp) > args.max_len or len(tgt) > args.max_len:
            dropped += 1
            continue
        samples.append(dict(seed=r["seed"], exit_code=r["exit_code"],
                            input=inp, target=tgt))

    samples.sort(key=lambda s: s["seed"])
    n_val = max(1, int(len(samples) * args.val_frac))
    # deterministic split: every 1/val_frac-th sample to val
    val, train = [], []
    step = max(2, int(1 / args.val_frac))
    for i, s in enumerate(samples):
        (val if i % step == 0 else train).append(s)

    vocab = list(SPECIAL)
    seen = set(SPECIAL)
    for s in train:
        for t in s["input"] + s["target"]:
            if t not in seen:
                seen.add(t)
                vocab.append(t)

    os.makedirs(args.out, exist_ok=True)
    json.dump({t: i for i, t in enumerate(vocab)},
              open(os.path.join(args.out, "vocab.json"), "w"))
    for name, split in (("train", train), ("val", val)):
        with open(os.path.join(args.out, f"{name}.jsonl"), "w") as f:
            for s in split:
                f.write(json.dumps(s) + "\n")

    in_lens = [len(s["input"]) for s in samples]
    out_lens = [len(s["target"]) for s in samples]
    print(f"pairs in:        {len(rows)}")
    print(f"kept (<= {args.max_len}): {len(samples)}  (dropped {dropped} too long)")
    print(f"train/val:       {len(train)}/{len(val)}")
    print(f"vocab size:      {len(vocab)}")
    print(f"input len  med/max: {sorted(in_lens)[len(in_lens)//2]}/{max(in_lens)}")
    print(f"target len med/max: {sorted(out_lens)[len(out_lens)//2]}/{max(out_lens)}")
    print(f"-> {args.out}/")


if __name__ == "__main__":
    main()
