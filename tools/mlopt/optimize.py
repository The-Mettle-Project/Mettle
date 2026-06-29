#!/usr/bin/env python3
"""End-to-end ML IR optimizer driver.

Takes a Mettle source file (or a generated seed), compiles it to UNOPTIMIZED IR,
then optimizes that IR with the trained keep/delete model under verifier-in-the-
loop control: the model ranks instructions by delete-confidence, and each
deletion is committed only if irexec confirms the program's result is unchanged.
The output is therefore guaranteed equivalent and (usually) shorter. Equivalence
is double-checked against the real compiled executable's exit code.

  python optimize.py --seed 9001
  python optimize.py --src path/to/prog.mettle
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
import irexec as IR  # noqa: E402
from build_pairs import build  # noqa: E402
from delete_common import infer_tokens  # noqa: E402
from train_delete import Labeler  # noqa: E402

GEN = os.path.join(HERE, "gen_int64.py")


def irexec(funcs):
    return IR.run_text(C.to_dump(funcs), max_steps=50000)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int)
    ap.add_argument("--src")
    ap.add_argument("--data", default=os.path.join(HERE, "data_del"))
    ap.add_argument("--model", default=os.path.join(HERE, "delete_model.pt"))
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="mlopt_drive_")
    if args.src:
        src = args.src
    else:
        src = os.path.join(tmp, "p.mettle")
        with open(src, "w") as f:
            f.write(subprocess.run([sys.executable, GEN, str(args.seed)],
                                   capture_output=True, text=True).stdout)

    # unoptimized IR (release-shaped, optimizer disabled) + real exit code
    exe = os.path.join(tmp, "p.exe")
    _, ir_path = build(src, exe, optimized=False)
    true_exit = subprocess.run([exe], capture_output=True).returncode
    input_ir = open(ir_path, encoding="utf-8", errors="replace").read()

    funcs = C.canonical_program(input_ir)
    n_in = sum(len(b) for b in funcs.values())
    if irexec(funcs) != true_exit:
        print("irexec disagrees with the executable on this program; abort.")
        return

    dev = "cuda" if torch.cuda.is_available() else "cpu"
    vocab = json.load(open(os.path.join(args.data, "vocab.json")))
    unk = vocab["<unk>"]
    ck = torch.load(args.model, map_location=dev)
    model = Labeler(**ck["cfg"]).to(dev)
    model.load_state_dict(ck["model"])
    model.eval()

    toks, pos, refs = infer_tokens(funcs)
    ids = torch.tensor([[vocab.get(t, unk) for t in toks]], device=dev)
    with torch.no_grad():
        prob_del = torch.softmax(model.cls(model(ids)[0, pos]), -1)[:, 1].tolist()

    def build_kept(drop):
        return {nm: [ins for j, ins in enumerate(b) if (nm, j) not in drop]
                for nm, b in funcs.items()}

    # verifier-in-the-loop: commit confidence-ranked deletions that preserve result
    drop = set()
    for i in sorted(range(len(refs)), key=lambda i: -prob_del[i]):
        if prob_del[i] < 0.5:
            break
        trial = drop | {refs[i]}
        if irexec(build_kept(trial)) == true_exit:
            drop = trial
    opt = build_kept(drop)
    n_out = sum(len(b) for b in opt.values())

    # final guarantees
    assert irexec(opt) == true_exit, "optimized IR must match the real exit"

    print(f"program:            {os.path.basename(src)}")
    print(f"real exit code:     {true_exit}")
    print(f"instructions:       {n_in} -> {n_out}  "
          f"({n_in - n_out} removed, {100*(n_in-n_out)/max(1,n_in):.0f}% smaller)")
    print(f"equivalence:        VERIFIED (irexec == executable exit {true_exit})")
    print("\n--- optimized IR ---")
    print(C.to_dump(opt))


if __name__ == "__main__":
    main()
