#!/usr/bin/env python3
"""Train a `gnn_oracle` variant. Multi-task: action classification, pointer
selection, and auxiliary liveness / validator-risk heads.

    python train_oracle.py --variant C --action "div_*_act.jsonl" ... \
        --collapse "collapse_train*.jsonl" --out oracle_C.pt --epochs 40

Losses
  action   6-way per instruction, class-frequency weighted (as the baseline)
  pointer  which dominating candidate to reuse, or slot 0 = decline. Labels come
           from `gvn_targets`, a sound analysis, so the head learns the real
           relation rather than the model's own guesses.
  live     is this def dead? Free labels from `liveness.dead_indices`, and the
           exact structural fact the DELETE action depends on.
  risk     would the interpreter gate reject a speculative delete here? Labels
           only exist once `harvest.py` has run, so this term is skipped when
           absent and switched on for the DAgger rounds.
  ponder   mean halting steps, weakly penalized (PONDER variant only) so depth is
           spent where it buys accuracy -- this term is also the compile-time
           budget, since every step is real work in `ml_gnn.c`.

Built graphs are cached to disk keyed by corpus + featurization, because the
bake-off trains four variants over the same two graph sets and OBS featurization
is not free.
"""
import argparse
import glob
import hashlib
import json
import os
import random
import sys
import time

import torch
import torch.nn as nn

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import gnn_oracle as O  # noqa: E402
import liveness as L  # noqa: E402

NCLASS = 6
NAMES = ["KEEP", "DELETE", "FOLD", "AFFINE", "GVN", "COLLAPSE"]
COLLAPSE = 5
SCLASS = (1, 3, 4, 5)

CACHE = os.environ.get("ORACLE_CACHE") or os.path.join(HERE, "_graphcache")


def robust_save(obj, path):
    """Atomic save with retry: torch.save can hit a transient Windows file lock
    when antivirus rescans the just-written .pt (same guard as _train_unified)."""
    tmp = path + ".tmp"
    for attempt in range(8):
        try:
            torch.save(obj, tmp)
            os.replace(tmp, path)
            return
        except (PermissionError, OSError):
            time.sleep(0.5 * (attempt + 1))
    torch.save(obj, path)


def _expand(pats):
    out = []
    for pat in pats:
        out += sorted(glob.glob(os.path.join(HERE, pat)))
    return out


def _cache_key(files, use_obs):
    import obs
    h = hashlib.sha1()
    h.update(f"obs{obs.OBS_VERSION}".encode() if use_obs else b"base1")
    for p in files:
        st = os.stat(p)
        h.update(os.path.basename(p).encode())
        h.update(str(st.st_size).encode())
        h.update(str(int(st.st_mtime)).encode())
    return h.hexdigest()[:16]


def build_rows(files, use_obs, collapse=False, want_risk=False):
    """-> list of (graph, action, ptr_target, live, risk). ptr_target[i] is the
    candidate slot (0 = decline) for node i, or -100 where there are no
    candidates so the loss ignores it."""
    rows = []
    for p in files:
        for line in open(p, encoding="utf-8"):
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            fn = r["funcs"][0]
            instrs = fn["instrs"]
            act = list(fn.get("action") or [])
            if collapse:
                act = [COLLAPSE if a == 1 else 0 for a in act]
            if len(act) != len(instrs):
                continue
            try:
                g = O.build_oracle_graph(instrs, r.get("params") or [], use_obs)
            except Exception:
                continue
            if g["n"] != len(act):
                continue
            n = g["n"]
            # pointer labels from the sound GVN analysis
            try:
                tgt = O.gvn_targets(instrs)
            except Exception:
                tgt = {}
            cidx, cmask = g["cand"]
            ptr = [-100] * n
            for i in range(n):
                live_slots = int(cmask[i].sum().item())
                if live_slots == 0:
                    continue
                ptr[i] = 0
                j = tgt.get(i)
                if j is not None:
                    for c in range(live_slots):
                        if int(cidx[i, c].item()) == j:
                            ptr[i] = c + 1
                            break
            # liveness labels
            try:
                dead = L.dead_indices(instrs, set(r.get("params") or []))
            except Exception:
                dead = set()
            live = [1 if i in dead else 0 for i in range(n)]
            risk = fn.get("risk")
            if not want_risk or not risk or len(risk) != n:
                risk = [-100] * n
            rows.append((g, act, ptr, live, list(risk)))
    return rows


def load_corpus(action_pats, collapse_pats, use_obs, want_risk=False):
    files_a, files_c = _expand(action_pats), _expand(collapse_pats)
    key = _cache_key(files_a + files_c, use_obs) + ("_r" if want_risk else "")
    os.makedirs(CACHE, exist_ok=True)
    cpath = os.path.join(CACHE, f"rows_{key}.pt")
    if os.path.exists(cpath):
        t0 = time.time()
        rows = torch.load(cpath, map_location="cpu", weights_only=False)
        print(f"cache hit {os.path.basename(cpath)}: {len(rows)} fns "
              f"in {time.time()-t0:.0f}s", flush=True)
        return rows
    t0 = time.time()
    rows = build_rows(files_a, use_obs, False, want_risk)
    na = len(rows)
    rows += build_rows(files_c, use_obs, True, want_risk)
    print(f"built action={na} collapse={len(rows)-na} in {time.time()-t0:.0f}s",
          flush=True)
    try:
        robust_save(rows, cpath)
    except Exception as e:
        print(f"  (cache write skipped: {e})", flush=True)
    return rows


def collate(batch, device, nedge):
    graphs = [(g, a) for g, a, _, _, _ in batch]
    feats, y = O.collate_oracle(graphs, device, nedge)
    ptr = torch.cat([torch.tensor(p, dtype=torch.long) for _, _, p, _, _ in batch]).to(device)
    live = torch.cat([torch.tensor(v, dtype=torch.long) for _, _, _, v, _ in batch]).to(device)
    risk = torch.cat([torch.tensor(v, dtype=torch.long) for _, _, _, _, v in batch]).to(device)
    return feats, y, ptr, live, risk


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", choices=sorted(O.VARIANTS), required=True)
    ap.add_argument("--action", nargs="+", required=True)
    ap.add_argument("--collapse", nargs="+", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--d", type=int, default=384)
    ap.add_argument("--layers", type=int, default=8)
    ap.add_argument("--max-steps", type=int, default=16)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--w-ptr", type=float, default=0.5)
    ap.add_argument("--w-live", type=float, default=0.2)
    ap.add_argument("--w-risk", type=float, default=0.5)
    ap.add_argument("--w-ponder", type=float, default=0.01)
    ap.add_argument("--init", default=None, help="warm-start checkpoint")
    ap.add_argument("--time-budget", type=float, default=0.0,
                    help="stop after N minutes (0 = no limit)")
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    random.seed(args.seed)
    cfgflags = O.VARIANTS[args.variant]
    use_obs = cfgflags["use_obs"]
    nedge = O.NEDGE_OBS if use_obs else O.NEDGE_BASE

    rows = load_corpus(args.action, args.collapse, use_obs,
                       want_risk=args.w_risk > 0)
    random.Random(0).shuffle(rows)          # fixed split across all variants
    val = [r for i, r in enumerate(rows) if i % 10 == 0]
    train = [r for i, r in enumerate(rows) if i % 10 != 0]
    dev = "cuda" if torch.cuda.is_available() else "cpu"

    freq = [1] * NCLASS
    for _, lab, _, _, _ in train:
        for a in lab:
            freq[a] += 1
    tot = sum(freq)
    w = torch.tensor([tot / f for f in freq], device=dev)
    w = (w / w.min()).clamp(max=30.0)

    model = O.Oracle(d_model=args.d, layers=args.layers, n_classes=NCLASS,
                     max_steps=args.max_steps, aux=True, **cfgflags).to(dev)
    if args.init and os.path.exists(args.init):
        ck0 = torch.load(args.init, map_location=dev, weights_only=False)
        try:
            model.load_state_dict(ck0["model"])
            print(f"warm-started from {args.init}", flush=True)
        except Exception as e:
            print(f"WARNING: --init incompatible ({e}); training from scratch",
                  flush=True)

    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, args.epochs)
    crit = nn.CrossEntropyLoss(weight=w)
    crit_ptr = nn.CrossEntropyLoss(ignore_index=-100)
    crit_aux = nn.CrossEntropyLoss(ignore_index=-100)
    nparam = sum(p.numel() for p in model.parameters())
    print(f"variant={args.variant} {cfgflags} device={dev} "
          f"train={len(train)} val={len(val)} params={nparam/1e6:.2f}M "
          f"nfeat={model.nfeat} nedge={nedge} freq={freq}", flush=True)

    def batches(data, shuffle):
        idx = list(range(len(data)))
        if shuffle:
            random.shuffle(idx)
        for i in range(0, len(idx), args.batch):
            yield [data[j] for j in idx[i:i + args.batch]]

    def evaluate():
        model.eval()
        import collections
        tp = collections.Counter(); fp = collections.Counter(); fn = collections.Counter()
        correct = total = 0
        pc = pt = 0
        steps = 0.0; nstep = 0
        with torch.no_grad():
            for bg in batches(val, False):
                feats, y, ptr, live, risk = collate(bg, dev, nedge)
                out = model(feats)
                pred = out["logits"].argmax(-1)
                for p, t in zip(pred.tolist(), y.tolist()):
                    total += 1; correct += (p == t)
                    if p == t: tp[t] += 1
                    else: fp[p] += 1; fn[t] += 1
                if "ptr" in out:
                    m = ptr >= 0
                    if bool(m.any()):
                        pp = out["ptr"][m].argmax(-1)
                        pc += int((pp == ptr[m]).sum().item()); pt += int(m.sum().item())
                if out.get("ponder") is not None:
                    steps += float(out["ponder"].mean().item()); nstep += 1
        acc = correct / max(1, total)
        f1 = {c: 2 * tp[c] / max(1, 2 * tp[c] + fp[c] + fn[c]) for c in SCLASS}
        return acc, f1, (pc / pt if pt else 0.0), (steps / nstep if nstep else 0.0)

    best = 0.0
    t_start = time.time()
    for ep in range(1, args.epochs + 1):
        model.train(); tl = 0.0; nb = 0; te = time.time()
        for bg in batches(train, True):
            feats, y, ptr, live, risk = collate(bg, dev, nedge)
            out = model(feats)
            loss = crit(out["logits"], y)
            if "ptr" in out and bool((ptr >= 0).any()):
                loss = loss + args.w_ptr * crit_ptr(out["ptr"], ptr)
            if "live" in out:
                loss = loss + args.w_live * crit_aux(out["live"], live)
            if "risk" in out and bool((risk >= 0).any()):
                loss = loss + args.w_risk * crit_aux(out["risk"], risk)
            if out.get("ponder") is not None:
                loss = loss + args.w_ponder * out["ponder"].mean()
            opt.zero_grad(); loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            tl += float(loss.item()); nb += 1
        sched.step()
        acc, f1, pacc, steps = evaluate()
        score = sum(f1.values()) / len(f1)
        if ep % 2 == 0 or ep == 1:
            print(f"ep {ep:3d} ({time.time()-te:.0f}s) loss {tl/max(1,nb):.3f} "
                  f"acc {acc:.3f} DEL {f1[1]:.2f} AFF {f1[3]:.2f} GVN {f1[4]:.2f} "
                  f"COL {f1[5]:.2f} PTR {pacc:.3f} steps {steps:.1f}", flush=True)
        if score >= best:
            best = score
            robust_save(dict(model=model.state_dict(),
                             cfg=dict(d_model=args.d, layers=args.layers,
                                      n_classes=NCLASS, max_steps=args.max_steps,
                                      aux=True, **cfgflags),
                             variant=args.variant,
                             metrics=dict(acc=acc, f1={str(k): v for k, v in f1.items()},
                                          ptr_acc=pacc, mean_steps=steps,
                                          score=score, epoch=ep)),
                        args.out)
        if args.time_budget and (time.time() - t_start) / 60.0 > args.time_budget:
            print(f"time budget {args.time_budget}min reached at epoch {ep}",
                  flush=True)
            break
    print(f"best mean-F1 {best:.4f} -> {args.out}", flush=True)


if __name__ == "__main__":
    main()
