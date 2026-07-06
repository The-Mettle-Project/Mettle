#!/usr/bin/env python3
"""Shared utilities for the Stage-1 keep/delete (verified-action) model.

The program is tokenized with an `<I>` marker prepended to every instruction;
the model classifies keep/delete at each `<I>` position. Because the output is a
subsequence of the input's own instruction STRINGS, constants and ops are copied
verbatim — the model cannot hallucinate values.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import canonicalize as C  # noqa: E402

SPECIAL = ["<pad>", "<unk>", "<I>"]


def example_tokens(funcs_record):
    """funcs_record: [{name, instrs:[str], delete:[0/1]}] -> (tokens, positions,
    labels). positions[i] is the index of the i-th instruction's <I> marker."""
    toks, positions, labels = [], [], []
    for fn in funcs_record:
        toks += ["function", fn["name"], "{"]
        for ins, d in zip(fn["instrs"], fn["delete"]):
            positions.append(len(toks))
            toks.append("<I>")
            labels.append(int(d))
            toks += C.digitize(C.tokenize_line(ins))
        toks.append("}")
    return toks, positions, labels


def infer_tokens(funcs):
    """funcs: {name: [instr str]} (canonical program) -> (tokens, positions,
    flat_instr_refs). flat_instr_refs[i] = (name, idx_in_func) for reconstruction."""
    toks, positions, refs = [], [], []
    for name, body in funcs.items():
        toks += ["function", name, "{"]
        for j, ins in enumerate(body):
            positions.append(len(toks))
            toks.append("<I>")
            refs.append((name, j))
            toks += C.digitize(C.tokenize_line(ins))
        toks.append("}")
    return toks, positions, refs
