"""Context-rich COLLAPSE corpus: teach the model to LOCALIZE a superoptimizable /
collapsible bitwise-shift tangle inside a realistic function body, not just as an
isolated return expression.

Each function mixes ordinary arithmetic distractors with one bitwise-shift tangle.
The tangle is a POSITIVE when its value has a strictly-cheaper equivalent (exactly
the condition the in-compiler bitwise/xor-shift superoptimizer rewrites under) --
its root is labelled COLLAPSE; it is a hard NEGATIVE when it is already optimal
(no cheaper form) -- all KEEP. Reducibility/optimality is decided against a cost<=4
optimal library (cached), so both labels are correct. The rest of the body is
labelled by the SOUND oracles (make_action_labels), so every label is right and
the data strengthens all classes, COLLAPSE most.

Gaps this closes vs gen_collapse.py: volume, criterion (strictly-cheaper, not
just collapse-to-<=2), realistic context (distractors + downstream use), and hard
optimal-tangle negatives (precision).
"""
import json
import os
import pickle
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import super_diff as S
import super_hunt as H
import make_action_labels as A

COLLAPSE = 5
LIB_COST = 4
LIB_CACHE = os.path.join(HERE, "_lib4.pkl")
DISTRACT_OPS = ["+", "-", "*", "&", "|", "^"]


def load_lib():
    """fp-hash -> minimal cost, over all exprs of cost <= LIB_COST (cached)."""
    if os.path.exists(LIB_CACHE):
        with open(LIB_CACHE, "rb") as f:
            return pickle.load(f)
    lib = {}
    for e in S.enumerate_exprs(LIB_COST):
        k = hash(S.fp(e))
        if k not in lib or e.cost < lib[k]:
            lib[k] = e.cost
    with open(LIB_CACHE, "wb") as f:
        pickle.dump(lib, f, protocol=pickle.HIGHEST_PROTOCOL)
    return lib


def render_tangle(e, vx, vy, ctr, body):
    root_idx = [None]

    def go(n):
        if n.op == "var":
            return vx if n.a == "x" else vy
        if n.op == "~":
            a = go(n.a); t = f"%t{ctr[0]}"; ctr[0] += 1
            body.append(f"{t} = {a} ^ -1"); root_idx[0] = len(body) - 1
            return t
        if n.op in ("<<", ">>"):
            a = go(n.a); t = f"%t{ctr[0]}"; ctr[0] += 1
            body.append(f"{t} = {a} {n.op} {n.s}"); root_idx[0] = len(body) - 1
            return t
        a = go(n.a); b = go(n.b); t = f"%t{ctr[0]}"; ctr[0] += 1
        body.append(f"{t} = {a} {n.op} {b}"); root_idx[0] = len(body) - 1
        return t

    return go(e), root_idx[0]


def gen_one(rng, lib, want_pos):
    nparams = rng.randint(2, 4)
    pnames = [f"@p{i}" for i in range(nparams)]
    body = []
    ctr = [0]
    live = list(pnames)

    for _ in range(rng.randint(1, 4)):
        op = rng.choice(DISTRACT_OPS)
        t = f"%d{ctr[0]}"; ctr[0] += 1
        body.append(f"{t} = {rng.choice(live)} {op} {rng.choice(live)}")
        live.append(t)

    vx, vy = rng.sample(pnames, 2)
    if want_pos:
        # span small (full cancellation -> leaf, cost 3-4) through large (cost 7)
        # so simple collapses like (x^a)^a -> x are as well represented as complex
        # multi-op reductions; both are what the realizer rewrites.
        e = H.rand_expr(rng.randint(3, 7))
        opt = lib.get(hash(S.fp(e)))
        if opt is None or e.cost - opt < 1:
            return None
    else:
        e = H.rand_expr(rng.randint(2, 4))
        opt = lib.get(hash(S.fp(e)))
        if opt is None or opt != e.cost:        # not provably optimal -> skip
            return None
    if e.cost < 2:
        return None

    root, root_idx = render_tangle(e, vx, vy, ctr, body)
    if root_idx is None:
        return None

    if rng.random() < 0.5:
        t = f"%u{ctr[0]}"; ctr[0] += 1
        body.append(f"{t} = {root} {rng.choice(DISTRACT_OPS)} {rng.choice(live)}")
        ret = t
    else:
        ret = root
    body.append(f"return {ret}")
    return pnames, body, root_idx, want_pos


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 24000
    out_path = sys.argv[2] if len(sys.argv) > 2 else "super_train.jsonl"
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 101
    rng = random.Random(seed)
    H.RNG = random.Random(seed + 1)

    import time
    t0 = time.time()
    lib = load_lib()
    print(f"optimal library: {len(lib)} classes (cost<={LIB_COST}) "
          f"in {time.time()-t0:.0f}s", flush=True)

    w = open(os.path.join(HERE, out_path), "w", encoding="utf-8")
    npos = nneg = sid = 0
    counts = [0] * 6
    tries = 0
    while (npos < n // 2 or nneg < n // 2) and tries < n * 200:
        tries += 1
        want_pos = npos <= nneg if (npos < n // 2 and nneg < n // 2) else (npos < n // 2)
        r = gen_one(rng, lib, want_pos)
        if r is None:
            continue
        params, body, root_idx, is_pos = r
        acts = A.action_labels(body, params)
        if is_pos:
            acts[root_idx] = COLLAPSE
            npos += 1
        else:
            nneg += 1
        for a in acts:
            counts[a] += 1
        w.write(json.dumps({"seed": sid, "params": params,
                            "funcs": [{"instrs": body, "action": acts}]}) + "\n")
        sid += 1
        if sid % 4000 == 0:
            print(f"  {sid} ({npos} pos / {nneg} neg)", flush=True)
    w.close()
    names = ["KEEP", "DELETE", "FOLD", "AFFINE", "GVN", "COLLAPSE"]
    print(f"wrote {sid} fns ({npos} pos / {nneg} neg) -> {out_path}")
    print({names[i]: counts[i] for i in range(6)})


if __name__ == "__main__":
    main()
