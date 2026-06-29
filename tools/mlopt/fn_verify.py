#!/usr/bin/env python3
"""Multi-input equivalence verification for parameterized int64 functions.

Verifies a transformed function against the original by running irexec on the
SAME K argument vectors (random + boundary values) and requiring all results to
match. Unlike single-execution checking, this rejects "optimizations" that only
hold for one input, so removable code is genuinely dead for ALL inputs — a
GCC/Clang-grade soundness target (high-confidence; a true proof would need SMT).

Param symbols are PINNED to the original function's, so a deletion that drops a
param's first use cannot silently shift the positional arg mapping.
"""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "fuzz"))
import irexec as IR  # noqa: E402

# Values chosen for branch coverage: generated branches compare params against
# constants in [0,255], so each param must take values densely spanning that
# range (both sides of every threshold) plus boundaries and wide randoms.
BOUNDARY = [0, 1, 2, 3, 255, 256, 1023, 1 << 20, (1 << 40) - 1]
_COVER_POOL = (list(range(0, 256, 7)) + BOUNDARY +
               [60, 97, 100, 118, 128, 150, 200])


def func_params(funcs, fname):
    fn = IR.parse_lines(_dump_lines(funcs))[fname]
    return IR.Machine(IR.parse_lines(_dump_lines(funcs))).param_symbols(fn)


def _dump_lines(funcs):
    out = []
    for name, body in funcs.items():
        out.append(f"function {name} {{")
        for i, ins in enumerate(body):
            out.append(f"  {i}: {ins}")
        out.append("}")
    return out


def run_fn(funcs, fname, params, args, max_steps=2_000_000):
    machine = IR.Machine(IR.parse_lines(_dump_lines(funcs)), max_steps=max_steps)
    fn = machine.funcs.get(fname)
    if fn is None:
        return None
    fn._params = params  # pin
    try:
        return machine.run(fname, list(args))
    except Exception:
        return None


def arg_vectors(nparams, k=128, seed=0):
    rng = random.Random(seed)
    vecs = []
    for b in BOUNDARY:                       # all-same boundary rows
        vecs.append([b] * nparams)
    # each param independently drawn from the coverage pool (exercises both
    # sides of param-vs-constant branches), then wide-random fill
    while len(vecs) < k:
        if len(vecs) % 3:
            vecs.append([rng.choice(_COVER_POOL) for _ in range(nparams)])
        else:
            vecs.append([rng.randint(0, (1 << 40) - 1) for _ in range(nparams)])
    return vecs[:k]


def equivalent(orig, mod, fname, params, vecs):
    """True iff mod(f) matches orig(f) on every arg vector."""
    for args in vecs:
        if run_fn(orig, fname, params, args) != run_fn(mod, fname, params, args):
            return False
    return True


def oracle_delete(funcs, fname, vecs, params=None):
    """Greedy reverse pass: delete instructions from fname whose removal keeps
    the function equivalent over all arg vectors. Returns (kept_funcs,
    deleted_mask) where deleted_mask[i] is 1 if instruction i was removed."""
    if params is None:
        params = func_params(funcs, fname)
    orig = {n: list(b) for n, b in funcs.items()}
    body = list(funcs[fname])
    deleted = [False] * len(body)

    def cur():
        m = {n: list(b) for n, b in funcs.items()}
        m[fname] = [body[i] for i in range(len(body)) if not deleted[i]]
        return m

    for i in range(len(body) - 1, -1, -1):
        deleted[i] = True
        if not equivalent(orig, cur(), fname, params, vecs):
            deleted[i] = False
    return cur(), [int(x) for x in deleted]
