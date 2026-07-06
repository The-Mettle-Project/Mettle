#!/usr/bin/env python3
"""Canonicalize + tokenize Mettle IR dumps for the ML IR-opt model.

The raw --dump-ir text is deterministic but carries model-hostile noise: dead
`nop`s, globally-counted non-SSA temp names (`%.t140`), inlined-temp names
(`%__inl_*`), and `ir_*` label names whose numeric suffixes drift with program
size. This module strips that noise so two functions that differ only by where
they sit in a program tokenize identically.

Pipeline:
  parse_program(text)      -> {func_name: [instruction lines]}   (block headers
                              dropped; the label/jump/branch stream already
                              encodes control flow)
  canonicalize_fn(lines)   -> [lines] with nops removed and every temp / local /
                              label renamed to dense first-occurrence ids
                              (%t0.., @v0.., L0..), per fn
  tokenize(lines)          -> [tokens] over a small regular vocabulary

CLI: read a pairs.jsonl (from build_pairs.py), align input/target functions by
name, emit per-function canonical token pairs to fnpairs.jsonl, and report
vocab size + token-length distribution.
"""
import argparse
import json
import os
import re
import statistics

HERE = os.path.dirname(os.path.abspath(__file__))

# An instruction line in the dump looks like "     12: <text>"; a block header
# like "  block 0 ir_entry_12 [0..6] preds: - succs: 1"; function delimiters are
# "function NAME {" and "}".
_INSTR_RE = re.compile(r"^\s*\d+:\s+(.*)$")
_FUNC_RE = re.compile(r"^function (\S+)\s*\{")

# Token classes, longest-match first. Identifiers keep a trailing .field chain.
_TOKEN_RE = re.compile(r"""
    (?P<temp>   \%[A-Za-z0-9_.$]+ )         |  # %.t12, %__inl_..$0
    (?P<local>  @[A-Za-z0-9_.$]+ )          |  # @v, @v.field, @sv47$0
    (?P<num>    -?\d+\.\d+ | -?\d+ )         |  # float literal before int
    (?P<word>   [A-Za-z_][A-Za-z0-9_]*       # keywords, opcodes, types, labels
                (?:\[\d+\])? )               # array type suffix e.g. uint8[43]
              | (?P<op>  <-|->|==|<=|>=|!=|<<|>>|&&|\|\| ) |
    (?P<punct>  [=+\-*/&|^<>():,~%\[\]] )       # incl. % modulo, [N] deref size
""", re.VERBOSE)


def parse_program(text):
    """Split a dump into {func_name: [raw instruction-text lines]}."""
    funcs = {}
    cur = None
    for line in text.splitlines():
        m = _FUNC_RE.match(line)
        if m:
            cur = m.group(1)
            funcs[cur] = []
            continue
        if cur is None:
            continue
        if line.strip() == "}":
            cur = None
            continue
        im = _INSTR_RE.match(line)
        if im:
            funcs[cur].append(im.group(1).strip())
    return funcs


def _base(ident):
    """Identifier base without a .field chain: '@s.f0' -> '@s'."""
    return ident.split(".", 1)[0] if not ident.startswith("%.") else ident


def canonicalize_fn(lines, rename_locals=False):
    """Drop nops; rename temps and labels to dense first-occurrence ids.

    `@`-locals/params are left ALONE by default: they are source-derived and
    stable, and renaming them breaks irexec's used-before-declared parameter
    inference (a renamed local can steal a param's first-occurrence slot, so the
    positional call ABI mismaps). The position-dependent noise lives in the
    `%.tNNN` temp counter and `ir_*NN` label suffixes, which this DOES normalize.
    `rename_locals=True` is available for analysis on corpora where local names
    themselves carry counters, but must not be used for irexec round-tripping."""
    # First pass: drop nops.
    body = [ln for ln in lines if ln != "nop"]

    temp_map, local_map, label_map = {}, {}, {}

    def rename_token(tok):
        if tok.startswith("%") and len(tok) > 1:  # real temp, not the % modulo op
            if tok not in temp_map:
                temp_map[tok] = f"%t{len(temp_map)}"
            return temp_map[tok]
        if tok.startswith("@") and rename_locals:
            base, _, field = tok.partition(".")
            if base not in local_map:
                local_map[base] = f"@v{len(local_map)}"
            return local_map[base] + (("." + field) if field else "")
        if tok.startswith("ir_"):
            if tok not in label_map:
                label_map[tok] = f"L{len(label_map)}"
            return label_map[tok]
        return tok

    out = []
    for ln in body:
        toks = tokenize_line(ln)
        out.append(detokenize([rename_token(t) for t in toks]))
    return out


def tokenize_line(line):
    return [m.group(0) for m in _TOKEN_RE.finditer(line)]


def detokenize(toks):
    """Reconstruct an IR line from tokens with the dump's spacing rules. Only
    calls need tight spacing in this corpus: `name(a, b)` — no space before `(`
    after a name, none after `(` or before `)`/`,`. Everything else (incl. infix
    `*` multiply) stays space-separated. Deref/cast/array forms are absent here;
    add rules before extending the corpus to them."""
    s = ""
    for i, t in enumerate(toks):
        if i == 0:
            s = t
            continue
        prev = toks[i - 1]
        attach = (t in (")", ",")) or prev == "(" or \
                 (t == "(" and (prev[-1].isalnum() or prev[-1] == "_"))
        s += t if attach else " " + t
    return s


def tokenize(lines):
    """Flatten canonical lines to a token stream with explicit line breaks."""
    toks = []
    for ln in lines:
        toks.extend(tokenize_line(ln))
        toks.append("\\n")
    return toks


def canonical_tokens(func_lines):
    return tokenize(canonicalize_fn(func_lines))


def canonical_program(ir_text):
    """{func_name: canonicalized instruction lines} for a whole program."""
    return {name: canonicalize_fn(body)
            for name, body in parse_program(ir_text).items()}


def to_dump(canon_funcs):
    """Render canonical functions back to an irexec-parseable dump: function
    wrappers + densely-renumbered `N:` instruction lines (block headers and nops
    already gone). Labels stay name-resolved, so renumbering is safe."""
    out = []
    for name, lines in canon_funcs.items():
        out.append(f"function {name} {{")
        for i, ln in enumerate(lines):
            out.append(f"  {i}: {ln}")
        out.append("}")
    return "\n".join(out) + "\n"


def digitize(toks, max_digits=5):
    """Replace short integer literals with `<num> [-] d d ... </num>` so the
    model can generate constants digit-by-digit with a bounded vocabulary.
    Literals with more than `max_digits` digits (e.g. the recurring 40-bit mask
    1099511627775) are kept ATOMIC: digitizing them would balloon sequence
    length on every masked op, and they are too few to pressure the vocab.
    Inverse: undigitize (atomic numbers pass through unchanged)."""
    out = []
    for t in toks:
        if re.fullmatch(r"-?\d+", t) and len(t.lstrip("-")) <= max_digits:
            out.append("<num>")
            if t[0] == "-":
                out.append("-")
                t = t[1:]
            out.extend(list(t))
            out.append("</num>")
        else:
            out.append(t)
    return out


def undigitize(toks):
    out, i = [], 0
    while i < len(toks):
        if toks[i] == "<num>":
            i += 1
            s = ""
            while i < len(toks) and toks[i] != "</num>":
                s += toks[i]
                i += 1
            i += 1  # skip </num>
            out.append(s if s else "0")
        else:
            out.append(toks[i])
            i += 1
    return out


def program_tokens(canon_funcs):
    """Token stream for a whole canonical program (function-delimited)."""
    toks = []
    for name, lines in canon_funcs.items():
        toks += ["function", name, "{", "\\n"]
        toks += tokenize(lines)
        toks += ["}", "\\n"]
    return toks


def tokens_to_dump(toks):
    """Inverse of program_tokens: reconstruct an irexec-parseable dump from a
    flat token stream. Lines are split on the '\\n' marker; function wrappers are
    re-emitted and instruction lines densely renumbered per function."""
    out, cur, line = [], None, []
    idx = 0
    i = 0
    while i < len(toks):
        t = toks[i]
        if t == "function" and i + 2 < len(toks):
            name = toks[i + 1]
            out.append(f"function {name} {{")
            cur, idx = name, 0
            i += 3  # skip 'function', name, '{'
            if i < len(toks) and toks[i] == "\\n":
                i += 1
            continue
        if t == "}":
            out.append("}")
            cur = None
            i += 1
            if i < len(toks) and toks[i] == "\\n":
                i += 1
            continue
        if t == "\\n":
            if cur is not None and line:
                out.append(f"  {idx}: {detokenize(line)}")
                idx += 1
            line = []
            i += 1
            continue
        line.append(t)
        i += 1
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pairs", default=os.path.join(HERE, "pairs.jsonl"))
    ap.add_argument("--out", default=os.path.join(HERE, "fnpairs.jsonl"))
    args = ap.parse_args()

    rows = [json.loads(l) for l in open(args.pairs, encoding="utf-8")]
    vocab = {}
    in_lens, out_lens = [], []
    n_fnpairs = 0
    out_f = open(args.out, "w", encoding="utf-8")

    for r in rows:
        fin = parse_program(r["input_ir"])
        fout = parse_program(r["target_ir"])
        for name in fin.keys() & fout.keys():
            in_toks = canonical_tokens(fin[name])
            out_toks = canonical_tokens(fout[name])
            for t in in_toks + out_toks:
                vocab[t] = vocab.get(t, 0) + 1
            in_lens.append(len(in_toks))
            out_lens.append(len(out_toks))
            out_f.write(json.dumps(dict(seed=r["seed"], func=name,
                                        input=in_toks, target=out_toks)) + "\n")
            n_fnpairs += 1
    out_f.close()

    def pct(xs, p):
        return int(statistics.quantiles(xs, n=100)[p - 1]) if len(xs) > 1 else xs[0]

    print(f"function pairs:  {n_fnpairs}")
    print(f"vocab size:      {len(vocab)}")
    print(f"input tokens:    med={int(statistics.median(in_lens))} "
          f"p90={pct(in_lens,90)} max={max(in_lens)}")
    print(f"target tokens:   med={int(statistics.median(out_lens))} "
          f"p90={pct(out_lens,90)} max={max(out_lens)}")
    top = sorted(vocab.items(), key=lambda kv: -kv[1])[:25]
    print("top tokens:      " + ", ".join(f"{t}:{c}" for t, c in top))
    # constant cardinality: how much of the vocab is just integer literals?
    nums = sum(1 for t in vocab if re.fullmatch(r"-?\d+", t))
    print(f"numeric-literal vocab entries: {nums}/{len(vocab)}")
    print(f"fnpairs -> {args.out}")


if __name__ == "__main__":
    main()
