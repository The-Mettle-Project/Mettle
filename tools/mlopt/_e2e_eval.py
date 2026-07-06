"""End-to-end optimizer eval over ALL functions in a labeled set (no i%10
sampling): model predicts actions -> sound applier (DELETE+AFFINE+propagate/fold/
DCE) to fixpoint -> verify in-process (parse once, many vectors) -> fall back on
mismatch. Reports size vs classical, vs delete-only, vs sound-superopt, plus
soundness. Works for any distribution; functions must be self-contained for
verification (filter calls upstream)."""
import glob
import json
import os
import sys

import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import liveness as L
import superopt
from gnn_model import GNN, build_graph, collate
from eval_action import apply_actions
from _aff_sound import cover_vectors, dump, run_once

dev = "cuda" if torch.cuda.is_available() else "cpu"
model_path = os.environ.get("MODEL", os.path.join(HERE, "gnn_uni.pt"))
ck = torch.load(model_path, map_location=dev)
model = GNN(**ck["cfg"]).to(dev); model.load_state_dict(ck["model"]); model.eval()

rows = []
for pat in sys.argv[1:]:
    for p in glob.glob(os.path.join(HERE, pat)):
        rows += [json.loads(l) for l in open(p, encoding="utf-8")]

n = s_cls = s_del = s_mdl = s_sup = unsound = 0
for r in rows:
    body = r["funcs"][0]["instrs"]; params = r["params"]
    cur = list(body)
    for _ in range(6):
        g = build_graph(cur, params)
        with torch.no_grad():
            feats, _ = collate([(g, [0] * g["n"])], dev)
            pred = model(feats).argmax(-1).tolist()
        if os.environ.get("NOGVN"):
            pred = [0 if p == 4 else p for p in pred]   # ablate GVN -> KEEP
        nxt = apply_actions(cur, params, pred)
        if nxt == cur:
            break
        cur = nxt
    out = cur
    if os.environ.get("VERIFY") and len(body) <= 45:
        fo, fm = dump(body), dump(out)
        ok = all(run_once(fo, params, a) == run_once(fm, params, a)
                 for a in cover_vectors(body, len(params), k=24))
        if not ok:
            unsound += 1
            print(f"  *** UNSOUND {r.get('seed')}", flush=True)
            out = [s for i, s in enumerate(body)
                   if i not in L.dead_indices(body, set(params))]
    n += 1
    s_cls += len(body)
    s_del += len(body) - len(L.dead_indices(body, set(params)))
    s_mdl += len(out)
    s_sup += len(superopt.optimize_body(body, params))

print(f"model={os.path.basename(model_path)}  {n} functions (all, no sampling)")
print(f"classical --release: {s_cls/n:.1f}")
print(f"delete-only (DCE):   {s_del/n:.1f}  ({100*(s_cls-s_del)/s_cls:.0f}% smaller)")
print(f"GNN model:           {s_mdl/n:.1f}  ({100*(s_cls-s_mdl)/s_cls:.0f}% smaller)")
print(f"sound ceiling:       {s_sup/n:.1f}  ({100*(s_cls-s_sup)/s_cls:.0f}% smaller)")
print(f"unsound (caught):    {unsound}/{n}")
