# `tools/mlopt` - the `--ml-opt` model and its pipeline

Offline tooling for the learned IR optimizer. The compiler runs inference
natively in C (`src/ir/ml_gnn.c`); nothing here is invoked at compile time. See
[`docs/ml-opt.md`](../../docs/ml-opt.md) for the user-facing description.

## Shipped artifacts (loaded by the compiler)

| file | what | built by |
|------|------|----------|
| `gnn_genius.bin` | GNN weights (fp32), 6-class | `export_gnn.py` from `gnn_genius.pt` |
| `bw_lib.txt` | optimal `&\|^~` form per truth table (k<=4) | `bw_superopt.py` |
| `gf2_lib1.txt` | optimal `^~<<>>` form per GF(2) matrix (k=1) | `gf2_superopt.py` |
| `gnn_genius.pt` | PyTorch master checkpoint (source for export) | `_train_unified.py` |

`build.bat` copies the three runtime files into `bin/mlopt/`.

## The model

Relational GNN over the IR dataflow graph (`gnn_model.py`): nodes are
instructions; typed edges are def-use, control, same-expr, and dominating-same-expr
(8 types). d=384, 8 layers, 6 classes (`KEEP/DELETE/FOLD/AFFINE/GVN/COLLAPSE`),
~10.8M params. Each node carries 9 scalar features (`NFEAT`): is-temp, is-symbol,
const-count, use-count, and five binary-operand features (operands-equal, operand
is -1 / 0 / 1 / power-of-two). The operand features must be computed IDENTICALLY in
`gnn_model.py` `_operand_feats` (training) and `src/ir/ml_gnn.c` `operand_feats`
(inference) -- if `NFEAT` or any feature changes, change BOTH and rebuild, or the
model reads different inputs at compile time than it trained on. It learns dataflow-structural features, so it generalizes off the
synthetic training distribution to real IR. Inference strips dead `nop` nodes so
the graph matches what the model trained on.

## Rebuild the model

```sh
# Training corpus, all 6-class action-labelled (KEEP/DELETE/FOLD/AFFINE/GVN/COLLAPSE):
#   div_*/arr_*/xgen_*_act.jsonl  - action data (gen_fn/gen_int64/gen_arr -> make_action_labels)
#   super_train*.jsonl            - context-rich superopt/collapse tangles (gen_super.py)
#   ident_train.jsonl             - canonical bitwise/shift COLLAPSE identities (gen_identities.py)
#   collapse_train*.jsonl         - isolated obfuscated-identity functions (gen_collapse.py)
python _train_unified.py \
    --action "div_*_act.jsonl" "arr_*_act.jsonl" "xgen_train_act.jsonl" \
             "super_train*.jsonl" "ident_train.jsonl" \
    --collapse "collapse_train.jsonl" "collapse_train2.jsonl" \
    --out gnn_genius.pt --epochs 55
python export_gnn.py gnn_genius.pt gnn_genius.bin     # flat blob the compiler loads
```

To KEEP the current model and refine it on added data (warm-start instead of
relearning from scratch), pass `--init` with a gentler LR and fewer epochs:

```sh
python _train_unified.py --action ... --collapse ... \
    --init gnn_genius.pt --lr 3e-4 --epochs 20 --out gnn_genius_next.pt
```

`gen_super.py` and `gen_identities.py` are the COLLAPSE-corpus generators that made
the model GENERALIZE (the original model collapsed to 0.61 accuracy on
realistically-shaped functions vs 0.95 on its training shapes). gen_super embeds a
superoptimizable bitwise-shift tangle among distractor arithmetic and downstream
uses, labelled by a cost<=4 optimal library (`_lib4.pkl`, cached) so a tangle is
COLLAPSE iff strictly cheaper and a hard NEGATIVE if already optimal. gen_identities
densely covers the canonical idioms ((a^B)^B->a, ~~a->a, a&a, a|a, a^a, a&~a, a|~a)
across many shift amounts and contexts.

After retraining, validate on the held-out sets (compare to the prior model with
`_cmp_eval.py`): COLLAPSE F1 on the context-rich `super_val.jsonl` and the isolated
`collapse_val.jsonl` must both hold/improve, GVN recall must stay ~1.0, and every
`--ml-opt` program's runtime output must match its classical build (the realizer is
verifier-gated, so a smarter model can only find MORE sound rewrites, never an
unsound one).

## Rebuild the superoptimizer libraries

```sh
python bw_superopt.py 6      # -> bw_lib.txt   (bitwise, complete for k<=3)
python gf2_superopt.py 3     # -> gf2_lib1.txt (xor-shift, k=1)
```

These enumerate every expression in their theory up to a cost bound and keep the
cheapest per *exact* fingerprint (truth table / GF(2) matrix). The compiler looks
an expression's fingerprint up and rewrites only when strictly cheaper.

## Key sound transforms (also used to label training data)

`liveness.py` (DCE), `gvn.py` (global value numbering), `affine.py` (linear-form
cancellation), `collapse.py` (value-equivalence), `sopt.py` (shared helpers).
`ml_pass.py` is the legacy Python reference for the in-compiler pass; the C port in
`src/ir/ml_gnn.c` is authoritative.

## Soundness discipline

- Applied transforms are sound by proof/construction, never by input sampling.
- Differential testing over sampled inputs is unsound for branch-dependent code;
  it is used only for the COLLAPSE `==leaf/==0` claims, where agreement over
  hundreds of random 64-bit vectors is decisive.
- The bitwise and GF(2) superoptimizers are *exact* (truth table / GF(2) matrix).
