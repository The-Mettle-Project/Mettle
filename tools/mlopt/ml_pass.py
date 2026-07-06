#!/usr/bin/env python3
# Legacy Python reference for the in-compiler pass (authoritative version is the C
# port in src/ir/ml_gnn.c). Reads the IR dump, runs the GNN, emits per-index
# dispositions: "<fn> <idx> NOP|COPY <src>|CONST <int>".
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import liveness as L
import affine
import gvn as G
from sopt import split_def, _BIN, is_lit

KEEP, DELETE, FOLD, AFFINE, GVN = 0, 1, 2, 3, 4
_IDX = re.compile(r"^\s*(\d+):\s+(.*\S)\s*$")

def parse_dump(text):
    funcs, name, cur = [], None, []
    for line in text.splitlines():
        m = re.match(r"^function (\S+) \{", line)
        if m:
            name, cur = m.group(1), []
            continue
        if line.startswith("}") and name:
            funcs.append((name, cur)); name, cur = None, []
            continue
        if name is None:
            continue
        mi = _IDX.match(line)
        if mi and not line.lstrip().startswith("block "):
            cur.append((int(mi.group(1)), mi.group(2)))
    return funcs

def infer_params(body):
    declared, params = set(), []
    for ins in body:
        md = re.match(r"^local (@\S+)", ins)
        if md:
            declared.add(md.group(1)); continue
        ma = re.match(r"^(@\S+) (?:<-|=)", ins)
        tgt = ma.group(1) if ma else None
        for sym in re.findall(r"@\w+", ins):
            if sym not in declared and sym != tgt and sym not in params:
                params.append(sym)
        if tgt:
            declared.add(tgt)
    return params

def main():
    text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    funcs = parse_dump(text)

    model = predict = None
    try:
        import torch
        from gnn_model import GNN, build_graph, collate
        dev = "cpu"
        ck = torch.load(os.path.join(HERE, "gnn_genius.pt"), map_location=dev)
        model = GNN(**ck["cfg"]).to(dev); model.load_state_dict(ck["model"]); model.eval()

        def predict(body, params):
            g = build_graph(body, params)
            with torch.no_grad():
                return model(collate([(g, [0] * g["n"])], dev)[0]).argmax(-1).tolist()
    except Exception as e:
        sys.stderr.write(f"ml_pass: model unavailable ({e}); teacher-gated\n")

    out = []
    for name, items in funcs:
        if not items:
            continue
        idxs = [gi for gi, _ in items]
        body = [t for _, t in items]
        params = infer_params(body)
        try:
            dead = L.dead_indices(body, set(params))
            gvned, _ = G.gvn(body, params)
            affined, _ = affine.simplify(body, params)
        except Exception:
            continue
        pred = predict(body, params) if predict else [None] * len(body)
        # only model-confirmed GVN COPY (dominance-sound); pure arithmetic and
        # compares both, even in memory-bearing functions (loads aren't numbered).
        for j, ins in enumerate(body):
            gi = idxs[j]
            p = pred[j] if j < len(pred) else None
            if gvned[j] == ins or p not in (None, GVN):
                continue
            od, cd = split_def(ins), split_def(gvned[j])
            is_expr = od and (_BIN.match(od[2].strip()) or
                              re.match(r"^(\S+) (==|!=|<|<=|>|>=) (\S+)$", od[2].strip()))
            if not od or not cd or not is_expr:
                continue
            rhs = cd[2].strip()
            if re.fullmatch(r"[@%][\w.$]+", rhs):
                out.append(f"{name} {gi} COPY {rhs}")
    open(sys.argv[2], "w", encoding="utf-8").write("\n".join(out) + "\n")
    sys.stderr.write(f"ml_pass: {len(out)} dispositions "
                     f"({'model-gated' if predict else 'teacher'})\n")

if __name__ == "__main__":
    main()
