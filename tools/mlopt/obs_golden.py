#!/usr/bin/env python3
"""Emit the OBS cross-check vectors the C port must reproduce exactly.

`tools/mlopt/README.md` already warns that the node features must be computed
identically in Python (training) and C (inference), or the model reads different
inputs at compile time than it trained on. OBS widens that surface from 5 boolean
flags to a 64-bit PRNG, a name hash, a 32x512 projection matrix, and full uint64
expression evaluation -- far too much to keep in sync by reading both listings.

So: this writes `obs_golden.txt`, and `src/ir/ml_gnn.c`'s self-test reads it back
and compares. Any divergence is a hard build failure rather than a model that
quietly underperforms at compile time.

    python obs_golden.py            # -> obs_golden.txt
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import obs  # noqa: E402

# Bodies chosen to exercise every evaluation path: commuted operands, the
# identity family, literal folding, a load, a call, a mutable loop counter,
# division by zero, shift masking, and signed comparison.
BODIES = [
    ["%a = @x + @y", "%b = @y + @x", "return %a"],
    ["%c = @x - 0", "%d = @x", "%e = @x * 0", "%f = @x ^ @x", "return %f"],
    ["%g = @x | @x", "%h = @x & @x", "%i = @x ^ -1", "return %i"],
    ["%j = 7 / 0", "%k = 7 % 0", "%l = @x << 65", "%m = @x >> 64", "return %m"],
    ["%n = @x < @y", "%o = @x <= -1", "%p = -1 < @x", "return %p"],
    ["local @i : int64", "@i <- 0", "label L", "%t = @i + 1", "@i <- %t",
     "%u = @i < @n", "branch_zero %u -> E", "jump L", "label E", "return @i"],
    ["%q = lower_bound_i32(arr = @arr, n = @n, key = @k)", "%r = @arr + %q",
     "%s <- * %r [ 4 ]", "*%r [ 4 ] = %s", "return %s"],
    ["%v = @x * 2", "%w = @x + @x", "%y = @x << 1", "return %y"],
]


def real_bodies(path, limit=40, max_len=60):
    """Sample real harvested function bodies.

    The hand-written bodies above cover the evaluation paths deliberately, which
    is exactly why they miss things: they contain the constructs I thought to
    test. Real IR contains the ones I did not -- a trained-model cross-check
    disagreed on `@i <- 0` inside a real loop, a shape no synthetic body here
    exercised. Sampling actual harvested bodies makes the parity gate cover the
    distribution the compiler really sees.

    Deterministic: sorted by body hash, first `limit` taken, so regenerating
    picks the same bodies as long as the corpus is the same."""
    import hashlib
    import json
    if not os.path.exists(path):
        return []
    seen, cand = set(), []
    for line in open(path, encoding="utf-8"):
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        body = r["funcs"][0]["instrs"]
        if not (3 <= len(body) <= max_len):
            continue
        h = hashlib.sha1("\n".join(body).encode()).hexdigest()
        if h in seen:
            continue
        seen.add(h)
        cand.append((h, body))
    cand.sort()
    return [b for _, b in cand[:limit]]


def main():
    dst = os.path.join(HERE, "obs_golden.txt")
    with open(dst, "w", encoding="utf-8") as f:
        f.write(f"# OBS golden vectors -- regenerate with obs_golden.py\n")
        f.write(f"NPROBE {obs.NPROBE}\nNPROJ {obs.NPROJ}\n"
                f"NSEM {obs.NSEM}\nNOBS {obs.NOBS}\n")

        f.write("\n[splitmix64]\n")
        for x in (0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x9E3779B97F4A7C15,
                  0xDEADBEEFCAFEBABE):
            f.write(f"{x:016x} {obs.splitmix64(x):016x}\n")

        f.write("\n[fnv1a64]\n")
        for s in ("@x", "@y", "%t0", "@arr", "@n", "", "a_very_long_name_1234"):
            f.write(f"{s!r} {obs.fnv1a64(s):016x}\n")

        f.write("\n[leaf_values]\n")
        for s in ("@x", "@y", "@n"):
            f.write(f"{s} " + " ".join(f"{v:016x}" for v in obs.leaf_values(s)) + "\n")

        f.write("\n[opaque_values]\n")
        for s, i in (("%q", 0), ("%q", 3), ("%s", 2)):
            f.write(f"{s} {i} " +
                    " ".join(f"{v:016x}" for v in obs.opaque_values(s, i)) + "\n")

        f.write("\n[projection]\n")   # each row as NPROBE 64-bit words, low first
        for r, row in enumerate(obs.projection()):
            words = [(row >> (64 * w)) & obs.MASK64 for w in range(obs.NPROBE)]
            f.write(f"{r} " + " ".join(f"{w:016x}" for w in words) + "\n")

        bodies = list(BODIES) + real_bodies(
            os.path.join(HERE, "_bakeoff", "real_raw.jsonl"))
        for bi, body in enumerate(bodies):
            f.write(f"\n[body {bi}] {len(body)}\n")
            for ins in body:
                f.write(f"| {ins}\n")
            fps, _, leaves = obs.fingerprints(body)
            f.write(f"leaves {' '.join(sorted(leaves))}\n")
            for i, v in enumerate(fps):
                f.write(f"fp {i} " +
                        ("none" if v is None
                         else " ".join(f"{x:016x}" for x in v)) + "\n")
            for i, row in enumerate(obs.obs_features(body)):
                f.write(f"ft {i} " + " ".join(f"{x:+.6f}" for x in row) + "\n")
            s, d = obs.semantic_edges(body)
            f.write("sedge " + " ".join(f"{a}->{b}" for a, b in zip(s, d)) + "\n")

    print(f"wrote {dst} ({os.path.getsize(dst)} bytes)")


if __name__ == "__main__":
    main()
