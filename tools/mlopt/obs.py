#!/usr/bin/env python3
"""Observational-equivalence node features (OBS) for the IR GNN.

The relational GNN in `gnn_model.py` sees program TEXT: a node's identity is its
kind, its operator, and a handful of syntactic flags. Two instructions computing
the same value are linked only when their expression strings match after operand
sorting, so `(a + b)` and `(b + a)` link but `(a - (0 - b))` does not. Every
value-level question the model is asked -- is this a constant (FOLD), does this
equal an in-scope leaf (COLLAPSE), does this recompute a dominating temp (GVN) --
is therefore being answered from syntax.

OBS hands the model semantics instead. Each pure instruction is evaluated on
NPROBE deterministic pseudo-random assignments to the function's leaves; the
resulting NPROBE x 64 bits are the node's FINGERPRINT. Nodes computing the same
function of the same leaves get bit-identical fingerprints regardless of how they
are written; nodes computing nearly the same value get fingerprints at small
Hamming distance. A frozen +-1 random projection (SimHash) turns those 512 bits
into NPROJ bounded floats, so the projection preserves that structure and the
learned `feat_lin` sees a continuous semantic space.

This is a FEATURE, not a proof. Fingerprint agreement is evidence of value
equality, never a licence to rewrite -- the interpreter gate in `ml_opt.c` still
adjudicates every proposal. Straight-line evaluation ignores control flow and
uses last-def-wins, matching the approximation `build_graph` already makes for
its def-use edges.

PORTABILITY CONTRACT
Everything here must be reproducible bit-for-bit by `obs_feats` in
`src/ir/ml_gnn.c`, or the model reads different inputs at compile time than it
trained on. That is why the PRNG (splitmix64), the name hash (FNV-1a 64), the
projection matrix (generated, not shipped), and the arithmetic (uint64 wraparound,
shift masked to 6 bits, division by zero yields 0, signed comparisons) are all
pinned here and nowhere else. `obs_golden.py` writes the cross-check vectors that
prove the C port agrees.
"""
import re

# Bump on ANY change to the probe scheme, evaluation semantics, projection, or
# edge-eligibility rule. `train_oracle` folds this into its graph-cache key: the
# cache is keyed on corpus file mtimes, so without a version here an edited
# featurizer silently reuses graphs built by the old one.
OBS_VERSION = 3

MASK64 = (1 << 64) - 1
SIGN64 = 1 << 63

NPROBE = 8               # probe assignments per function; 8 * 64 = 512 fp bits
NSMALL = 5               # ...of which the first NSMALL are structured/small
SMALL_MOD = 4
NPROJ = 32               # SimHash output dims
NSEM = 4                 # derived semantic scalars (see obs_features)
NOBS = NPROJ + NSEM      # total features contributed by this module

_PROJ_SEED = 0x9E3779B97F4A7C15
_PROBE_SEED = 0xD1B54A32D192ED03
_OPAQUE_SEED = 0xA24BAED4963EE407

_BINOP = re.compile(r"^(\S+) (<<|>>|\+|-|\*|/|%|&|\||\^|==|!=|<=|>=|<|>) (\S+)$")
_LIT = re.compile(r"^-?\d+$")
# A readable operand is a literal or a bare IR name -- NOT any whitespace-free
# token. `__acrt_iob_func(2)` contains no space, so a `^\S+$` test accepts it as
# a copy source and then two distinct calls to the same function alias to one
# leaf value, making unrelated call results compare equal.
_NAME = re.compile(r"^[%@][A-Za-z0-9_.$]*$")


def splitmix64(x):
    """The reference splitmix64 step. Pinned: the C port must match exactly."""
    x = (x + 0x9E3779B97F4A7C15) & MASK64
    z = x
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return z ^ (z >> 31)


def fnv1a64(s):
    """FNV-1a over the raw bytes of a name. Pinned for the C port."""
    h = 0xCBF29CE484222325
    for b in s.encode("utf-8"):
        h = ((h ^ b) * 0x100000001B3) & MASK64
    return h


def _probe_salt(k):
    return splitmix64(_PROBE_SEED ^ ((k + 1) * 0x9E3779B97F4A7C15 & MASK64))


def _mix(h, k):
    """Probe k's value for a leaf with name-hash h.

    Uniform 64-bit leaves alone are a broken probe design for this IR: two random
    64-bit values are essentially never equal and essentially never ordered
    close, so EVERY comparison evaluates to 0 on every probe and `x == y`,
    `x < y`, and the literal `0` all collapse to the same all-zero fingerprint.

    So the probe set is deliberately mixed:
      probe 0   every leaf is 0   -- `x == y` is 1 here, the constant 0 is not,
      probe 1   every leaf is 1      so equality can never be confused with zero
      probes 2-4 small (mod 4)    -- orderings and divisions actually vary
      probes 5-7 full 64-bit      -- the discriminating power

    The wide probes are what keep false positives away: unrelated values may
    agree on the degenerate probes, but they must then also agree on three
    full-width probes, a ~2^-192 event. Both kinds are needed.

    A residual limitation, worth stating plainly: a comparison yields one bit,
    so any 8-probe fingerprint gives comparison results at most 8 bits of
    identity and unrelated comparisons will sometimes share a fingerprint. Three
    small probes hold that around a quarter of pairs rather than the two thirds a
    single small probe left. It costs a spurious candidate edge, never a wrong
    rewrite -- the gate still adjudicates."""
    if k == 0:
        return 0
    if k == 1:
        return 1
    v = splitmix64(h ^ _probe_salt(k))
    return v % SMALL_MOD if k < NSMALL else v


def leaf_values(name):
    """The NPROBE pseudo-random values standing in for an unknown leaf. Keyed by
    NAME only, so every use of `@n` in a function agrees on its value."""
    h = fnv1a64(name)
    return tuple(_mix(h, k) for k in range(NPROBE))


def opaque_values(name, idx):
    """Values for a def we cannot evaluate (call result, load, unknown form).
    Keyed by name AND instruction index: two calls to the same function must not
    be assumed to return the same value, since either may have side effects."""
    h = fnv1a64(name) ^ splitmix64(_OPAQUE_SEED ^ (idx & MASK64))
    return tuple(_mix(h, k) for k in range(NPROBE))


# ---------------------------------------------------------------- projection

_PROJ = None


def projection():
    """NPROJ rows of 512 +-1 signs, packed as 512-bit ints (bit set = +1).
    Generated from a fixed seed rather than shipped in the weight blob: the C
    port regenerates the identical matrix from the same splitmix64 stream."""
    global _PROJ
    if _PROJ is not None:
        return _PROJ
    rows = []
    state = _PROJ_SEED
    for _ in range(NPROJ):
        row = 0
        for w in range(NPROBE):          # 8 words of 64 bits = 512 signs
            state = (state + 1) & MASK64
            row |= splitmix64(state) << (64 * w)
        rows.append(row)
    _PROJ = rows
    return _PROJ


_PROJ_MEMO = {}


def project(fp):
    """512-bit fingerprint -> NPROJ floats in (-1, 1).

    With both the row and the fingerprint read as +-1 vectors, their dot product
    is 512 - 2 * popcount(row XOR fp). Scaling by 1/sqrt(512) puts a random pair
    near unit variance; tanh bounds it. Equal fingerprints therefore give equal
    features, and a few flipped bits move the features only slightly.

    Memoized: real IR repeats a handful of fingerprints (0, 1, loop bounds) many
    times per function, and the projection is a pure function of the input."""
    hit = _PROJ_MEMO.get(fp)
    if hit is not None:
        return list(hit)
    import math
    out = [math.tanh((512 - 2 * (row ^ fp).bit_count()) / 22.627416997969522)
           for row in projection()]                      # 22.627... = sqrt(512)
    if len(_PROJ_MEMO) < 200000:
        _PROJ_MEMO[fp] = tuple(out)
    return out


# ---------------------------------------------------------------- evaluation

def _tosigned(v):
    return v - (1 << 64) if v >= SIGN64 else v


def _apply(op, a, b):
    """uint64 semantics matching sopt.evalop, plus signed comparisons.
    Division and modulo by zero yield 0 (the interpreter's convention), so a
    fingerprint never depends on trap behaviour."""
    if op == "+":  return (a + b) & MASK64
    if op == "-":  return (a - b) & MASK64
    if op == "*":  return (a * b) & MASK64
    if op == "&":  return a & b
    if op == "|":  return a | b
    if op == "^":  return a ^ b
    if op == "<<": return (a << (b & 63)) & MASK64
    if op == ">>": return a >> (b & 63)
    if op == "/":  return ((a // b) & MASK64) if b else 0
    if op == "%":  return ((a % b) & MASK64) if b else 0
    if op == "==": return 1 if a == b else 0
    if op == "!=": return 1 if a != b else 0
    if op == "<":  return 1 if _tosigned(a) < _tosigned(b) else 0
    if op == "<=": return 1 if _tosigned(a) <= _tosigned(b) else 0
    if op == ">":  return 1 if _tosigned(a) > _tosigned(b) else 0
    if op == ">=": return 1 if _tosigned(a) >= _tosigned(b) else 0
    return None


def _is_control(ins):
    return (ins.startswith("label ") or ins.startswith("jump ") or
            ins.startswith("branch") or ins.startswith("return ") or
            ins.startswith("local ") or ins.startswith("*"))


def _split_def(ins):
    """-> (dest, rhs) for a value-defining instruction, else None. Mirrors
    sopt.split_def but also recognizes `+=`, which the IR emits for accumulators."""
    if _is_control(ins):
        return None
    m = re.match(r"^(\S+)\s*(=|<-|\+=)\s*(.*)$", ins)
    if not m:
        return None
    return m.group(1), m.group(2), m.group(3).strip()


_CALL = re.compile(r"[A-Za-z_]\w*\s*\(")


def _mutable_names(instrs):
    """Names whose value straight-line evaluation cannot be trusted to know.

    Two sources:

    1. Defined more than once. Straight-line evaluation cannot model a loop
       back-edge, so a fingerprint for `@i` after `@i <- 0; ...; @i = @i + 1`
       would claim `@i` is the constant 0 on every iteration.

    2. Passed to a call or touched by a store. `QueryPerformanceCounter(&counter)`
       writes through a pointer, which is invisible to a scan that only reads
       defs -- so `@counter <- 0` followed by that call leaves the evaluator
       believing `@counter` is still 0, and every expression downstream of it
       collapses to 0 and matches every other zero in the function. Being
       conservative here costs a few fingerprints and removes a whole class of
       false equivalences.

    Such names are held opaque: their defs get no fingerprint and their uses read
    one stable leaf value. What survives is the SSA-temp domain that GVN and
    COLLAPSE operate on, where straight-line evaluation is faithful."""
    seen, mut = set(), set()
    for ins in instrs:
        if ins.startswith("*") or _CALL.search(ins):
            for tok in re.findall(r"@[A-Za-z0-9_.$]*", ins):
                mut.add(tok.split(".", 1)[0])
                mut.add(tok)
        d = _split_def(ins)
        if not d:
            continue
        dest = d[0]
        if not dest.startswith(("%", "@")):
            continue
        if dest in seen:
            mut.add(dest)
        seen.add(dest)
    return mut


def fingerprints(instrs):
    """-> (fps, env, leafnames).

    fps[i] is instruction i's NPROBE-value tuple, or None when i defines nothing
    evaluable (control flow, store, a mutable name, or an opaque def). Opaque
    DEFS still get a value recorded in the environment so that downstream
    arithmetic on them stays consistent -- we just do not claim to know what they
    compute. `leafnames` are the names whose values were invented rather than
    derived: params, globals, call results, loads, and mutable locals."""
    env = {}
    leafnames = set()
    mut = _mutable_names(instrs)
    fps = [None] * len(instrs)

    def operand(tok):
        """-> value tuple, or None when the token is not a literal or a name."""
        if _LIT.match(tok):
            v = int(tok) & MASK64
            return (v,) * NPROBE
        if not _NAME.match(tok):
            return None
        if tok in env:
            return env[tok]
        env[tok] = leaf_values(tok)
        leafnames.add(tok)
        return env[tok]

    for i, ins in enumerate(instrs):
        d = _split_def(ins)
        if not d:
            continue
        dest, eq, rhs = d
        if not dest.startswith(("%", "@")):
            continue
        if dest in mut:                  # loop-carried: one stable opaque value
            if dest not in env:
                env[dest] = leaf_values(dest)
                leafnames.add(dest)
            continue
        vals = None
        m = _BINOP.match(rhs)
        if m:
            a = operand(m.group(1))
            b = operand(m.group(3))
            op = m.group(2)
            if a is not None and b is not None:
                vals = tuple(_apply(op, a[k], b[k]) for k in range(NPROBE))
                if vals[0] is None:
                    vals = None
        elif eq == "+=":
            cur, add = operand(dest), operand(rhs)
            if cur is not None and add is not None:
                vals = tuple((cur[k] + add[k]) & MASK64 for k in range(NPROBE))
        else:                                          # copy or literal
            vals = operand(rhs)
        if vals is None:                     # call, load, or a form we cannot read
            env[dest] = opaque_values(dest, i)
            leafnames.add(dest)
            continue
        env[dest] = vals
        fps[i] = vals
    return fps, env, leafnames


def edge_eligible(vals):
    """Is this value worth linking as a reuse candidate?

    No for two kinds of node, and the single predicate is shared by
    `semantic_edges`, `gnn_oracle.build_oracle_graph`, and `obs_gap` so the three
    cannot drift apart:

    CONSTANTS -- every `<- 0` computes what every other one computes, so linking
    them wires each constant into a clique and hands the graph hub nodes whose
    mean-aggregated messages drown out real dataflow. Rematerializing a literal
    is not a saving anyway.

    BOOLEANS -- a comparison yields one bit, so `@i < @words` and `@i < @n` carry
    at most 8 bits of fingerprint identity between them and collide constantly.
    That is a property of the output domain, not a fixable bug in the probe set,
    and the honest response is to not claim these as semantic matches. Genuinely
    identical comparisons are still linked by the syntactic same-expr edge, which
    is exact for them.

    Both classes keep their OBS features; only the EDGE is withheld."""
    if all(x == vals[0] for x in vals):
        return False
    return any(x > 1 for x in vals)


def _pack(vals):
    fp = 0
    for k in range(NPROBE):
        fp |= vals[k] << (64 * k)
    return fp


def obs_features(instrs):
    """-> list of NOBS floats per instruction: NPROJ SimHash dims then NSEM
    semantic scalars.

    The scalars are the value-level predicates the transforms actually turn on,
    handed to the model directly rather than left to be inferred:
      0 is_const     value identical across every probe   -> FOLD candidate
      1 eq_leaf      value matches an in-scope opaque leaf -> COLLAPSE candidate
      2 eq_zero      value is 0 on every probe
      3 dup_earlier  an earlier node has the same fingerprint -> GVN candidate
    """
    fps, env, leafnames = fingerprints(instrs)
    # Leaf fingerprints: names whose value was invented rather than derived
    # (params, globals, call results, loads, mutable locals). A computed value
    # matching one of these is the COLLAPSE relation -- "this tangle is just x".
    leaf_fps = {_pack(env[nm]) for nm in leafnames}

    out = []
    seen = set()
    zero = _pack((0,) * NPROBE)
    for i in range(len(instrs)):
        v = fps[i]
        if v is None:
            out.append([0.0] * NOBS)
            continue
        fp = _pack(v)
        row = project(fp)
        row.append(1.0 if all(x == v[0] for x in v) else 0.0)
        row.append(1.0 if fp in leaf_fps else 0.0)
        row.append(1.0 if fp == zero else 0.0)
        row.append(1.0 if fp in seen else 0.0)
        seen.add(fp)
        out.append(row)
    return out


def semantic_edges(instrs):
    """Value-equality edges: (src, dst) pairs linking each node to the NEAREST
    EARLIER node with an identical fingerprint.

    This is the semantic counterpart of `build_graph`'s syntactic `same-expr`
    edge. The syntactic edge fires when two instructions are spelled alike; this
    one fires when they compute alike, which is the relation GVN and COLLAPSE
    actually care about and the one the model previously had to guess at.

    Constant- and boolean-valued nodes are excluded; see `edge_eligible`."""
    fps, _, _ = fingerprints(instrs)
    src, dst = [], []
    last = {}
    for i, v in enumerate(fps):
        if v is None or not edge_eligible(v):
            continue
        fp = _pack(v)
        if fp in last:
            src.append(last[fp]); dst.append(i)
        last[fp] = i
    return src, dst
