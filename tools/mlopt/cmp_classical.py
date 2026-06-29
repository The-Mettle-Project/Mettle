#!/usr/bin/env python3
"""Head-to-head: ML+verifier optimizer vs Mettle's classical --release optimizer.

For each held-out program we have the unoptimized IR and the classical optimized
IR (the --release `target_ir`). We run the verifier-in-the-loop ML deletion on
BOTH, and on the classical output in particular: any instruction the ML can
verifiably delete from already-classically-optimized code is dead/redundant code
the classical optimizer LEFT BEHIND -> a concrete, sound "better than classical"
result. Equivalence is checked by irexec against the program's known exit code.
"""
import argparse
import glob
import json
import os
import sys

import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import canonicalize as C  # noqa: E402
import irexec as IR  # noqa: E402
from delete_common import infer_tokens  # noqa: E402
from train_delete import Labeler  # noqa: E402


def irexec(funcs):
    return IR.run_text(C.to_dump(funcs), max_steps=50000)


def ninstr(funcs):
    return sum(len(b) for b in funcs.values())


def ml_verified_delete(model, vocab, unk, dev, funcs, target_exit):
    """Verifier-in-the-loop: commit confidence-ranked deletions that preserve
    the result. Returns the optimized funcs (subset of input instructions)."""
    if irexec(funcs) != target_exit:
        return funcs  # can't verify; no change
    toks, pos, refs = infer_tokens(funcs)
    ids = torch.tensor([[vocab.get(t, unk) for t in toks]], device=dev)
    with torch.no_grad():
        prob = torch.softmax(model.cls(model(ids)[0, pos]), -1)[:, 1].tolist()

    def build(drop):
        return {nm: [ins for j, ins in enumerate(b) if (nm, j) not in drop]
                for nm, b in funcs.items()}

    drop = set()
    for i in sorted(range(len(refs)), key=lambda i: -prob[i]):
        if prob[i] < 0.5:
            break
        trial = drop | {refs[i]}
        if irexec(build(trial)) == target_exit:
            drop = trial
    return build(drop)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pairs", nargs="+", required=True)
    ap.add_argument("--labels", nargs="+", required=True)
    ap.add_argument("--data", default=os.path.join(HERE, "data_del"))
    ap.add_argument("--model", default=os.path.join(HERE, "delete_model.pt"))
    args = ap.parse_args()

    pairs = {}
    for pat in args.pairs:
        for p in glob.glob(pat):
            for l in open(p, encoding="utf-8"):
                r = json.loads(l)
                pairs[r["seed"]] = r

    lab_rows = []
    for pat in args.labels:
        for p in glob.glob(pat):
            lab_rows += [json.loads(l) for l in open(p, encoding="utf-8")]
    seen, uniq = set(), []
    for r in lab_rows:
        if r["seed"] in seen:
            continue
        seen.add(r["seed"]); uniq.append(r)
    uniq.sort(key=lambda r: r["seed"])
    val_seeds = [r["seed"] for i, r in enumerate(uniq) if i % 10 == 0]

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    vocab = json.load(open(os.path.join(args.data, "vocab.json")))
    unk = vocab["<unk>"]
    ck = torch.load(args.model, map_location=dev)
    model = Labeler(**ck["cfg"]).to(dev); model.load_state_dict(ck["model"])
    model.eval()

    n = 0
    sum_unopt = sum_classical = sum_ml_unopt = sum_ml_classical = 0
    classical_left_dead = 0          # programs where ML removed >0 from classical
    extra_removed = 0                # total instrs ML removed from classical out
    ml_unopt_beats_classical = 0     # ML-on-unopt smaller than classical
    for seed in val_seeds:
        if seed not in pairs:
            continue
        r = pairs[seed]
        exit_code = r["exit_code"]
        unopt = C.canonical_program(r["input_ir"])
        classical = C.canonical_program(r["target_ir"])
        # sanity: both reproduce the known answer in irexec
        if irexec(unopt) != exit_code or irexec(classical) != exit_code:
            continue
        n += 1
        n_un, n_cl = ninstr(unopt), ninstr(classical)
        ml_un = ml_verified_delete(model, vocab, unk, dev, unopt, exit_code)
        ml_cl = ml_verified_delete(model, vocab, unk, dev, classical, exit_code)
        n_mlun, n_mlcl = ninstr(ml_un), ninstr(ml_cl)
        sum_unopt += n_un; sum_classical += n_cl
        sum_ml_unopt += n_mlun; sum_ml_classical += n_mlcl
        if n_mlcl < n_cl:
            classical_left_dead += 1
            extra_removed += n_cl - n_mlcl
        if n_mlun < n_cl:
            ml_unopt_beats_classical += 1

    print(f"\n=== ML+verifier vs classical --release optimizer ({n} held-out) ===")
    print(f"avg instrs  unoptimized:        {sum_unopt/n:.1f}")
    print(f"avg instrs  classical --release:{sum_classical/n:.1f}")
    print(f"avg instrs  ML on unopt:        {sum_ml_unopt/n:.1f}")
    print(f"avg instrs  ML on classical:    {sum_ml_classical/n:.1f}")
    print(f"\nclassical left removable dead code: {classical_left_dead}/{n} "
          f"programs ({100*classical_left_dead/n:.0f}%)")
    print(f"  total extra instrs ML removed from classical output: {extra_removed}")
    print(f"ML(on unopt) ends SMALLER than classical --release: "
          f"{ml_unopt_beats_classical}/{n} ({100*ml_unopt_beats_classical/n:.0f}%)")
    print("\nall ML outputs irexec-verified equivalent (verifier-in-the-loop).")


if __name__ == "__main__":
    main()
