#!/usr/bin/env python3
"""End-to-end evaluation of the ML IR-opt model.

For each held-out program: feed the unoptimized-IR tokens, greedily decode the
model's optimized-IR tokens, reconstruct an irexec-parseable dump, and run it.
Report:
  valid       - reconstructs + runs in irexec
  equivalent  - irexec result == the pair's known-correct exit code
  optimized   - generated program has FEWER instructions than the input
  win         - equivalent AND optimized (a real, verified optimization)
  exact       - generated tokens == the optimizer's actual target
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import canonicalize as C  # noqa: E402
from train import Seq2Seq  # noqa: E402

IREXEC = os.path.join(HERE, "..", "fuzz", "irexec.py")


def irexec(dump_text):
    f = tempfile.NamedTemporaryFile("w", suffix=".ir", delete=False,
                                    encoding="utf-8")
    f.write(dump_text)
    f.close()
    try:
        r = subprocess.run([sys.executable, IREXEC, f.name, "--entry", "main"],
                           capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        os.unlink(f.name)
        return None
    os.unlink(f.name)
    o = r.stdout.strip().splitlines()
    return (int(o[-1]) & 0xFF) if o and o[-1].lstrip("-").isdigit() else None


def n_instr(tokens):
    # instruction count = number of '\n' line markers between function bodies
    return sum(1 for t in tokens if t == "\\n")


@torch.no_grad()
def greedy(model, src_ids, bos, eos, dev, max_len=512):
    model.eval()
    src = torch.tensor([src_ids], device=dev)
    out = [bos]
    for _ in range(max_len):
        tgt = torch.tensor([out], device=dev)
        logits = model(src, tgt)
        nxt = int(logits[0, -1].argmax())
        if nxt == eos:
            break
        out.append(nxt)
    return out[1:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(HERE, "data"))
    ap.add_argument("--model", default=os.path.join(HERE, "model.pt"))
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--show", type=int, default=0, help="print N decoded samples")
    args = ap.parse_args()

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    vocab = json.load(open(os.path.join(args.data, "vocab.json")))
    inv = {i: t for t, i in vocab.items()}
    pad, bos, eos, unk = (vocab["<pad>"], vocab["<bos>"], vocab["<eos>"],
                          vocab["<unk>"])
    ckpt = torch.load(args.model, map_location=dev)
    model = Seq2Seq(**ckpt["cfg"]).to(dev)
    model.load_state_dict(ckpt["model"])

    val = [json.loads(l) for l in
           open(os.path.join(args.data, "val.jsonl"), encoding="utf-8")]
    if args.limit:
        val = val[:args.limit]

    st = dict(n=0, valid=0, equiv=0, opt=0, win=0, exact=0,
              input_ok=0, copy_would_win=0)
    for r in val:
        st["n"] += 1
        src_ids = [vocab.get(t, unk) for t in r["input"]]
        gen_ids = greedy(model, src_ids, bos, eos, dev)
        gen_toks = [inv[i] for i in gen_ids if i not in (pad, bos, eos)]

        # sanity: the input itself runs to the known answer
        in_dump = C.tokens_to_dump(C.undigitize(r["input"]))
        if irexec(in_dump) == r["exit_code"]:
            st["input_ok"] += 1
        n_in = n_instr(r["input"])

        try:
            dump = C.tokens_to_dump(C.undigitize(gen_toks))
        except Exception:
            dump = None
        res = irexec(dump) if dump else None
        if res is not None:
            st["valid"] += 1
        equiv = res is not None and res == r["exit_code"]
        n_gen = n_instr(gen_toks)
        opt = n_gen < n_in
        if equiv:
            st["equiv"] += 1
        if opt:
            st["opt"] += 1
        if equiv and opt:
            st["win"] += 1
        if gen_toks == r["target"]:
            st["exact"] += 1
        # reference: the real optimizer's target is both equiv and smaller
        if n_instr(r["target"]) < n_in:
            st["copy_would_win"] += 1

        if args.show and st["n"] <= args.show:
            print(f"\n--- seed {r['seed']} exit={r['exit_code']} "
                  f"in_instr={n_in} gen_instr={n_gen} "
                  f"valid={res is not None} equiv={equiv} opt={opt} ---")
            print("GEN:", " ".join(C.undigitize(gen_toks))[:300])

    n = st["n"]
    print(f"\n=== end-to-end eval over {n} held-out programs ===")
    print(f"input runs to known answer (sanity): {st['input_ok']}/{n}")
    print(f"valid (reconstructs + runs):          {st['valid']}/{n}")
    print(f"equivalent (correct result):          {st['equiv']}/{n}")
    print(f"optimized (fewer instrs than input):  {st['opt']}/{n}")
    print(f"WIN (equivalent AND optimized):       {st['win']}/{n}")
    print(f"exact match to optimizer target:      {st['exact']}/{n}")
    print(f"(reference: optimizer target is a win on {st['copy_would_win']}/{n})")


if __name__ == "__main__":
    main()
