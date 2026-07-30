#!/usr/bin/env python3
"""Cross-check the C forward pass against PyTorch on the same graphs.

`obs_golden.txt` proves the two featurizers agree. That is necessary and not
sufficient: identical features fed through a mistranscribed message-passing loop,
a wrong GRU gate order, or a misread weight blob still give different
predictions, and again nothing crashes -- the model simply behaves differently
in the compiler than it did in training.

So: run the compiler with `METTLE_ML_ACTIONS` set to dump the raw per-instruction
argmax, rebuild the same graphs in Python from the same `_mlopt.ir`, run the same
checkpoint, and compare class by class.

    python check_forward.py --model _bakeoff/oracle_C.pt --blob oracle_C.bin \\
        --source ../../examples/sort_insertion/sort_insertion.mettle

Exit status is non-zero on any disagreement.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import torch

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import gnn_oracle as O  # noqa: E402
import harvest as H  # noqa: E402

COMPILER = os.path.join(ROOT, "bin", "mettle.exe")


def run_compiler(src, blob, work):
    acts = os.path.join(work, "actions.tsv")
    env = dict(os.environ, METTLE_ML_ACTIONS=acts,
               METTLE_ML_MODEL=os.path.abspath(blob))
    subprocess.run([COMPILER, "--ml-opt", "--release", src,
                    "-o", os.path.join(work, "a.exe")],
                   capture_output=True, text=True, timeout=300, cwd=work,
                   env=env)
    irp = os.path.join(work, "_mlopt.ir")
    if not os.path.exists(irp) or not os.path.exists(acts):
        return None, None
    ir = open(irp, encoding="utf-8", errors="replace").read()
    got = {}
    for line in open(acts, encoding="utf-8", errors="replace"):
        f = line.rstrip("\n").split("\t")
        if len(f) == 3:
            got[(f[0], int(f[1]))] = int(f[2])
    return ir, got


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="the .pt checkpoint")
    ap.add_argument("--blob", required=True, help="the exported .bin")
    ap.add_argument("--source", nargs="+", required=True)
    ap.add_argument("--raw", action="store_true",
                    help="build the Python graph from the RAW dump text instead "
                         "of the canonicalized text, i.e. exactly what the "
                         "compiler featurizes")
    args = ap.parse_args()

    ck = torch.load(args.model, map_location="cpu", weights_only=False)
    cfg = dict(ck["cfg"])
    model = O.Oracle(**cfg)
    model.load_state_dict(ck["model"])
    model.eval()
    use_obs = cfg.get("use_obs", True)
    nedge = O.NEDGE_OBS if use_obs else O.NEDGE_BASE

    total = agree = missing = 0
    disagreements = []
    for src in args.source:
        work = tempfile.mkdtemp(prefix="fwdchk_")
        try:
            ir, got = run_compiler(os.path.abspath(src), args.blob, work)
        finally:
            pass
        if not ir:
            print(f"  {os.path.basename(src)}: compiler produced no dump, skipped")
            shutil.rmtree(work, ignore_errors=True)
            continue

        for fn, pairs in H.parse_ir_with_gidx(ir).items():
            if not pairs:
                continue
            if args.raw:
                gidxs = [g for g, _ in pairs]
                body = [t for _, t in pairs]
            else:
                cw = H.canonical_with_gidx(pairs)
                if not cw:
                    continue
                gidxs, body = cw
            g = O.build_oracle_graph(body, H.infer_params(body), use_obs)
            with torch.no_grad():
                batch, _ = O.collate_oracle([(g, [0] * g["n"])], "cpu", nedge)
                pred = model(batch)["logits"].argmax(-1).tolist()
            for i, gi in enumerate(gidxs):
                key = (fn, gi)
                if key not in got:
                    missing += 1
                    continue
                total += 1
                if got[key] == pred[i]:
                    agree += 1
                elif len(disagreements) < 12:
                    disagreements.append(
                        (fn, gi, body[i], pred[i], got[key]))
        shutil.rmtree(work, ignore_errors=True)

    if total == 0:
        print("no comparable nodes; is the blob a v2 model the compiler loaded?")
        return 2
    print(f"forward-pass cross-check: {agree}/{total} nodes agree "
          f"({100.0*agree/total:.2f}%)"
          + (f", {missing} nodes not reported by the compiler" if missing else ""))
    if disagreements:
        print("  first disagreements (python -> c):")
        for fn, gi, text, p, c in disagreements:
            print(f"    {fn} ir#{gi}: {text[:52]:<52} py={p} c={c}")
    return 0 if agree == total else 1


if __name__ == "__main__":
    sys.exit(main())
