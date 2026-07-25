#!/usr/bin/env python3
"""Measure the syntactic/semantic gap on real IR: how much redundancy is
invisible to a model that reasons about program text?

The baseline GNN links two instructions as equivalent only when their expression
strings match after sorting commutative operands. OBS links them when they
evaluate identically on 8 pseudo-random leaf assignments. This counts the
difference on real IR, independent of any model:

  syntactic pairs   dominating (j, i) with the SAME expression key -- what the
                    shipped model gets handed as edge type 6
  semantic pairs    dominating (j, i) with the same OBS fingerprint
  gap               semantic pairs that are NOT syntactic pairs: real redundancy
                    the current architecture has no edge for

Every reported pair is a *candidate*, not a rewrite. Fingerprint agreement over 8
probes is evidence of value equality, and the interpreter gate remains the
authority on whether any of it is sound.

    python obs_gap.py _bakeoff/real_raw.jsonl
"""
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import obs  # noqa: E402
import liveness as L  # noqa: E402
import gnn_oracle as G  # noqa: E402
from gvn import _dominators  # noqa: E402


def analyze(instrs):
    n = len(instrs)
    if n < 2:
        return None
    _, succ = L.build_cfg(instrs)

    fps, _, _ = obs.fingerprints(instrs)
    eligible = [v is not None and obs.edge_eligible(v) for v in fps]
    nconst = sum(1 for v in fps if v is not None and not obs.edge_eligible(v))

    keyof = G._same_expr_keys(instrs)
    by_key, by_key_el = {}, {}
    for i, k in enumerate(keyof):
        if k is None:
            continue
        by_key.setdefault(k, []).append(i)
        if eligible[i]:
            by_key_el.setdefault(k, []).append(i)

    by_val = {}
    for i, v in enumerate(fps):
        if eligible[i]:
            by_val.setdefault(v, []).append(i)

    groups = (by_key, by_key_el, by_val)
    if not any(any(len(g) > 1 for g in grp.values()) for grp in groups):
        return dict(n=n, syn=0, syn_el=0, sem=0, gap=0, const=nconst,
                    gap_examples=[])

    dom, _ = _dominators(succ, n)
    ss, sd = G._dominating_pairs(by_key, dom)
    es, ed = G._dominating_pairs(by_key_el, dom)
    vs, vd = G._dominating_pairs(by_val, dom)
    syn = set(zip(ss, sd))
    syn_el = set(zip(es, ed))
    sem = set(zip(vs, vd))
    # The gap is computed on the eligible population only, so both sides are
    # measured over the same nodes. Comparing the semantic count against the
    # UNFILTERED syntactic count would be meaningless: the semantic side drops
    # constants and booleans, the shipped model's syntactic edge does not.
    gap = sem - syn_el
    ex = []
    for j, i in list(gap)[:3]:
        ex.append((instrs[j], instrs[i]))
    return dict(n=n, syn=len(syn), syn_el=len(syn_el), sem=len(sem),
                gap=len(gap), const=nconst, gap_examples=ex)


def main():
    paths = sys.argv[1:] or [os.path.join(HERE, "_bakeoff", "real_raw.jsonl")]
    tot = collections.Counter()
    nfn = 0
    fn_with_gap = 0
    examples = []
    seen_bodies = set()
    for p in paths:
        for line in open(p, encoding="utf-8"):
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            instrs = r["funcs"][0]["instrs"]
            key = "\n".join(instrs)
            if key in seen_bodies:            # stdlib repeats in every binary
                continue
            seen_bodies.add(key)
            a = analyze(instrs)
            if not a:
                continue
            nfn += 1
            tot["n"] += a["n"]; tot["syn"] += a["syn"]
            tot["sem"] += a["sem"]; tot["gap"] += a["gap"]
            tot["syn_el"] += a.get("syn_el", 0)
            tot["const"] += a.get("const", 0)
            if a["gap"]:
                fn_with_gap += 1
                if len(examples) < 12:
                    examples += a["gap_examples"]

    print(f"real IR: {nfn} unique function bodies, {tot['n']} instructions")
    print(f"  syntactic pairs, all nodes       : {tot['syn']}"
          f"   (what the shipped model is handed)")
    print(f"  --- like for like, on the nodes eligible for a value edge ---")
    print(f"  syntactic pairs                  : {tot['syn_el']}")
    print(f"  semantic  pairs                  : {tot['sem']}")
    print(f"  semantic-only (the gap)          : {tot['gap']}"
          f"  ({100*tot['gap']/max(1,tot['sem']):.1f}% of semantic pairs)")
    print(f"  functions containing a gap pair  : {fn_with_gap}/{nfn} "
          f"({100*fn_with_gap/max(1,nfn):.1f}%)")
    print(f"  (constant- or boolean-valued nodes, edge-excluded: {tot['const']})")
    if examples:
        print("\n  examples (earlier def -> later redundant def, "
              "textually different, same value):")
        for a, b in examples[:10]:
            print(f"    {a:<44} ~= {b}")


if __name__ == "__main__":
    main()
