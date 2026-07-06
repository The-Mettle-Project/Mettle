#!/usr/bin/env python3
"""Comprehensive head-to-head on held-out parameterized functions:

  classical --release   |   liveness DCE   |   sound multi-pass optimizer
                        |                  |   |   + learned model (gated)

All non-classical columns are verified equivalent (liveness is sound; the
multi-pass optimizer self-verifies over branch-covering inputs). Reports average
sizes, % smaller than classical, and win rates.
"""
import argparse
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import liveness as L  # noqa: E402
import sopt  # noqa: E402


def load_model(data, model_path):
    import torch
    from train_delete import Labeler
    if not os.path.exists(model_path):
        return None
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    vocab = json.load(open(os.path.join(data, "vocab.json")))
    ck = torch.load(model_path, map_location=dev)
    m = Labeler(**ck["cfg"]).to(dev); m.load_state_dict(ck["model"]); m.eval()
    return (m, vocab, dev)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--labels", nargs="+", required=True)
    ap.add_argument("--data", default=os.path.join(HERE, "data_fl"))
    ap.add_argument("--model", default=os.path.join(HERE, "fl_model.pt"))
    ap.add_argument("--limit", type=int, default=120)
    args = ap.parse_args()

    rows = []
    for pat in args.labels:
        for p in glob.glob(pat):
            rows += [json.loads(l) for l in open(p, encoding="utf-8")]
    seen, uniq = set(), []
    for r in rows:
        if r["seed"] in seen:
            continue
        seen.add(r["seed"]); uniq.append(r)
    uniq.sort(key=lambda r: r["seed"])
    val = [r for i, r in enumerate(uniq) if i % 10 == 0][:args.limit]

    model = load_model(args.data, args.model)
    if model:
        import torch
        from delete_common import infer_tokens
        m, vocab, dev = model
        unk = vocab["<unk>"]

    n = 0
    s_cls = s_dce = s_sopt = s_mlg = 0
    w_sopt = w_mlg = 0
    for r in val:
        instrs = r["funcs"][0]["instrs"]; params = r["params"]
        funcs = {"f": list(instrs)}
        n_cls = len(instrs)
        dead = L.dead_indices(instrs, set(params))
        n_dce = n_cls - len(dead)
        n_sopt = len(sopt.optimize(funcs, "f", params, verify=True)["f"])
        n += 1
        s_cls += n_cls; s_dce += n_dce; s_sopt += n_sopt
        if n_sopt < n_cls:
            w_sopt += 1
        if model:
            toks, pos, refs = infer_tokens(funcs)
            ids = torch.tensor([[vocab.get(t, unk) for t in toks]], device=dev)
            with torch.no_grad():
                prob = torch.softmax(m.cls(m(ids)[0, pos]), -1)[:, 1].tolist()
            pred = set(refs[i][1] for i in range(len(refs)) if prob[i] > 0.5)
            gated = pred & set(dead)               # sound gate
            n_mlg = n_cls - len(gated)
            s_mlg += n_mlg
            if n_mlg < n_cls:
                w_mlg += 1

    def line(name, s, wins=None):
        extra = f"  beats classical {wins}/{n} ({100*wins/n:.0f}%)" if wins is not None else ""
        pct = f"  ({100*(s_cls-s)/s_cls:.0f}% smaller)" if name != "classical --release" else ""
        print(f"  {name:<28} {s/n:5.1f}{pct}{extra}")

    print(f"\n=== Comprehensive comparison ({n} held-out parameterized functions) ===")
    line("classical --release", s_cls)
    line("liveness DCE (sound)", s_dce)
    line("sound multi-pass optimizer", s_sopt, w_sopt)
    if model:
        line("learned model + sound gate", s_mlg, w_mlg)
    print("\nnon-classical columns verified equivalent (no sampling for DCE; "
          "multi-pass self-verified over branch-covering inputs).")


if __name__ == "__main__":
    main()
