#!/usr/bin/env python3
"""Audit how genuinely held-out the held-out set is.

Deduplicating by function-body hash removes EXACT twins across the split, which
was the bug that let the risk head score 0.997 AUC by recognizing stdlib bodies
it had already trained on. It does not remove NEAR twins: the same source
function inlined at two call sites differs only in its temp numbering and its
inlined-parameter names, and canonicalization does not always collapse that.

This measures the residual. For every held-out body it finds the most similar
training body by Jaccard similarity over instruction shapes, where a shape is the
instruction with all names and literals replaced by placeholders -- so two
inlinings of one function have identical shapes even though their text differs.

Read the output as a bound on optimism, not as a pass/fail. A held-out set where
most bodies have a >0.9 near twin in training is measuring memorization however
clean its hashes are.

    python split_audit.py _bakeoff/oracle_real_train.jsonl _bakeoff/oracle_real_held.jsonl
"""
import json
import re
import sys

_NAME = re.compile(r"[%@][A-Za-z0-9_.$]*")
_NUM = re.compile(r"(?<![\w%@])-?\d+")


def shape(ins):
    """The instruction with names and literals abstracted away."""
    s = _NAME.sub("N", ins)
    s = _NUM.sub("K", s)
    return re.sub(r"\s+", " ", s).strip()


def load(path):
    out = []
    for line in open(path, encoding="utf-8"):
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        body = r["funcs"][0]["instrs"]
        out.append((r.get("source", "?"), r["funcs"][0].get("name", "?"),
                    frozenset(shape(i) for i in body), len(body)))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    train = load(sys.argv[1])
    held = load(sys.argv[2])
    print(f"train={len(train)} held={len(held)} unique bodies")

    # Bucket training shapes by an arbitrary member to prune the comparison.
    buckets = {}
    for t in train:
        for sh in t[2]:
            buckets.setdefault(sh, []).append(t)

    sims = []
    worst = []
    for src, name, hs, n in held:
        cand = set()
        for sh in hs:
            for t in buckets.get(sh, ())[:400]:
                cand.add(id(t))
        best, bestt = 0.0, None
        seen = set()
        for sh in hs:
            for t in buckets.get(sh, ())[:400]:
                if id(t) in seen:
                    continue
                seen.add(id(t))
                inter = len(hs & t[2])
                union = len(hs | t[2])
                j = inter / union if union else 0.0
                if j > best:
                    best, bestt = j, t
        sims.append(best)
        if bestt:
            worst.append((best, name, bestt[1]))

    sims.sort()
    def pct(p):
        return sims[min(len(sims) - 1, int(len(sims) * p))] if sims else 0.0
    over = lambda th: sum(1 for s in sims if s >= th)
    print(f"  max Jaccard to any training body (instruction shapes):")
    print(f"    median {pct(0.5):.3f}   p75 {pct(0.75):.3f}   "
          f"p90 {pct(0.90):.3f}   max {max(sims) if sims else 0:.3f}")
    for th in (1.0, 0.9, 0.7, 0.5):
        print(f"    >= {th:.1f}: {over(th):4d}/{len(sims)} "
              f"({100*over(th)/max(1,len(sims)):.1f}%)")
    worst.sort(reverse=True)
    print("  closest pairs (held-out function ~ training function):")
    for s, a, b in worst[:8]:
        print(f"    {s:.3f}  {a}  ~  {b}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
