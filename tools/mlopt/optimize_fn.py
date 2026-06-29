#!/usr/bin/env python3
"""Usable driver: optimize a parameterized Mettle function and beat the classical
optimizer, with a multi-input correctness guarantee.

Compiles the source with Mettle's classical --release optimizer, then runs the
LEARNED optimizer (model + verifier-in-the-loop) on classical's own output,
removing dead/redundant code classical left behind. Reports the head-to-head and
prints the optimized IR. Every removal is verified equivalent over many inputs.

  python optimize_fn.py --seed 9001
  python optimize_fn.py --src prog.mettle --func f
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
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import canonicalize as C  # noqa: E402
import fn_verify as V  # noqa: E402
from build_pairs import COMPILER  # noqa: E402
from delete_common import infer_tokens  # noqa: E402
from train_delete import Labeler  # noqa: E402

GEN = os.path.join(HERE, "gen_fn.py")
VAL_K = 512


def classical_dump(src, out):
    args = [COMPILER, "--build", "--emit-obj", "--linker", "internal",
            "--dump-ir", "--release", src, "-o", out]
    p = subprocess.run(args, capture_output=True, text=True, timeout=120)
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
    ap.add_argument("--data", default=os.path.join(HERE, "data_fn"))
    ap.add_argument("--model", default=os.path.join(HERE, "fn_model.pt"))
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="mlopt_drv_")
    src = args.src or os.path.join(tmp, "p.mettle")
    if not args.src:
        open(src, "w").write(subprocess.run([sys.executable, GEN, str(args.seed)],
                                            capture_output=True, text=True).stdout)
    dump = classical_dump(src, os.path.join(tmp, "p.exe"))
    funcs_all = C.canonical_program(dump)
    fn = args.func
    instrs = funcs_all[fn]
    funcs = {fn: list(instrs)}
    params = V.func_params(funcs, fn)
    n_classical = len(instrs)

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    vocab = json.load(open(os.path.join(args.data, "vocab.json")))
    unk = vocab["<unk>"]
    ck = torch.load(args.model, map_location=dev)
    model = Labeler(**ck["cfg"]).to(dev); model.load_state_dict(ck["model"])
    model.eval()

    toks, pos, refs = infer_tokens(funcs)
    ids = torch.tensor([[vocab.get(t, unk) for t in toks]], device=dev)
    with torch.no_grad():
        prob = torch.softmax(model.cls(model(ids)[0, pos]), -1)[:, 1].tolist()
    vecs = V.arg_vectors(len(params), k=VAL_K, seed=991)

    def build(drop):
        return {fn: [ins for j, ins in enumerate(instrs) if j not in drop]}

    drop = set()
    for i in sorted(range(len(refs)), key=lambda i: -prob[i]):
        if prob[i] < 0.5:
            break
        if V.equivalent(funcs, build(drop | {refs[i][1]}), fn, params, vecs):
            drop |= {refs[i][1]}
    opt = build(drop)
    n_ml = len(opt[fn])
    assert V.equivalent(funcs, opt, fn, params, vecs)

    print(f"function:                {fn}({', '.join(params)})")
    print(f"classical --release:     {n_classical} instructions")
    print(f"learned optimizer:       {n_ml} instructions  "
          f"({100*(n_classical-n_ml)/max(1,n_classical):.0f}% smaller)")
    print(f"correctness:             VERIFIED equivalent over {VAL_K} inputs")
    print("\n--- classical --release output ---")
    print("\n".join("  " + s for s in instrs))
    print("\n--- learned-optimizer output (dead code classical left, removed) ---")
    print("\n".join("  " + s for s in opt[fn]))


if __name__ == "__main__":
    main()
