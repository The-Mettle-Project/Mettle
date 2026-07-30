#!/usr/bin/env python3
"""Assemble every measurement into one report.

The bake-off writes a variant table. That is one of several things worth reading
together, and several of the others are controls without which the table is easy
to over-read. This collects them:

  - the ablation table (A / B / C / D on held-out real IR)
  - the RANDOM-INIT controls, so "the model scores X" can be compared against
    "an untrained network of the same shape scores Y"
  - the TRIVIAL baselines for the risk head, which is the number that decides
    whether that head is interesting or is reading instruction type
  - the split audit, so held-out means something
  - the model-free semantic probe, which is the strongest single result
  - the Python/C forward-pass agreement, since a model that behaves differently
    in the compiler than in training is not a result at all

    python summarize.py --work _bakeoff > _bakeoff/SUMMARY.md
"""
import argparse
import glob
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def load(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None


def f1of(r, name):
    pc = (r or {}).get("per_class", {})
    return f"{pc[name]['f1']:.3f}" if name in pc else "-"


def variant_row(r, label=None):
    if not r or r.get("error"):
        return f"| {label or '?'} |" + " - |" * 9   # must match the 10-col header
    ptr = ("-" if r.get("ptr_acc") is None else
           f"{r['ptr_acc']:.3f} / {(r.get('ptr_acc_pointing') or 0):.3f}")
    risk = "-" if r.get("risk_auc") is None else f"{r['risk_auc']:.3f}"
    steps = "-" if r.get("mean_steps") is None else f"{r['mean_steps']:.1f}"
    return (f"| {label or r.get('variant')} | {r.get('params',0)/1e6:.2f}M "
            f"| {r.get('train_epoch') or '-'} | {r.get('acc',0):.4f} "
            f"| {f1of(r,'DELETE')} | {f1of(r,'GVN')} | {f1of(r,'COLLAPSE')} "
            f"| {ptr} | {risk} | {steps} |")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=os.path.join(HERE, "_bakeoff"))
    args = ap.parse_args()
    work = os.path.abspath(args.work)

    out = ["# gnn_oracle: results", "",
           f"Generated {time.strftime('%Y-%m-%d %H:%M:%S')}.", ""]

    # ---- the headline, because it needs no model -------------------------
    out += [
        "## 1. Semantic reuse candidates, adjudicated by the compiler's own validator",
        "",
        "No model involved. Value-equal dominating pairs with no syntactic GVN",
        "edge are emitted directly as `COPY` dispositions and sent through the",
        "interpreter differential (`sem_probe.py`).",
        "",
        "585 sources, 429 of which compiled. Every proposal is at an",
        "instruction the shipped `--ml-opt` pass does not touch: the overlap",
        "with its own dispositions is **0**, checked rather than assumed.",
        "",
        "| | count |",
        "|---|---|",
        "| proposed | 405 |",
        "| overlapping the shipped pass's own proposals | **0** |",
        "| the gate ruled on | 112 |",
        "| **validated equivalent** | **111** |",
        "| rejected | 1 |",
        "| applied unadjudicated (`proven-only`) | **0** |",
        "| skipped (applier declined / function unverifiable) | 293 |",
        "",
        "Runtime check (`sem_runtime.py`): of the 47 example programs containing",
        "at least one validated semantic rewrite, **47/47 produce byte-identical",
        "output** to their baseline build.",
        "",
        "Caveats that belong next to the number, not in a footnote:",
        "",
        "- Nothing was applied without adjudication. An earlier run applied 239",
        "  rows unvalidated because `ml_opt.c` treated every non-NOP disposition",
        "  as proven by construction; these proposals now mark themselves",
        "  `COPY?` and are gated like any unproven rewrite.",
        "- ~111 rewrites across ~500 programs is about one per five programs.",
        "  The class is new; the volume is modest.",
        "",
    ]

    # ---- the model-level headline, and its retraction -------------------
    out += [
        "## 2. Speculative DELETE: a result that did not survive its own test",
        "",
        "On the shape-key held-out split, training the action head on the gate's",
        "verdicts looked like a large win: the shipped model's rejection rate of",
        "59.1% fell to 3.0%. That result is corpus-specific and should not be",
        "quoted. MettleWarband -- 17 441 lines, 863 functions, a different",
        "repository never harvested -- is the first evaluation from outside the",
        "training distribution, and it does not reproduce.",
        "",
        "| | shape-key held-out (40 toolchain programs) | MettleWarband (unseen) |",
        "|---|---|---|",
        "| `gnn_genius` reject rate | 59.1% | 11.4% |",
        "| `oracle_C` reject rate | 3.0% | 11.4% |",
        "| `gnn_genius` validated | 72 | 895 |",
        "| `oracle_C` validated | 65 | 326 |",
        "",
        "Full Warband numbers, whole-program build:",
        "",
        "| model | proposals | validated | rejected | reject rate |",
        "|---|---|---|---|---|",
        "| `gnn_genius` (shipped) | 27 776 | **895** | 115 | 11.4% |",
        "| `oracle_A` new data, shipped architecture | 592 | 197 | 28 | 12.4% |",
        "| `oracle_C` + semantic + pointer | 1 129 | 326 | 42 | 11.4% |",
        "",
        "- **The precision advantage does not exist out of distribution.** All",
        "  three models sit at 11-12%.",
        "- **Recall is worse and the shipped model wins**, 895 against 326 and 197.",
        "  A model trained on these labels is a regression on unfamiliar code.",
        "- **Semantic features are what partially generalizes.** `oracle_C` finds",
        "  65% more than `oracle_A` at identical precision: `oracle_A` memorized",
        "  which toolchain sites get rejected and went quiet on unfamiliar code,",
        "  while `oracle_C`'s node identity is structural. That ordering is the",
        "  clearest evidence the architecture earns anything at all.",
        "",
        "How far outside the distribution Warband actually is (`dist_overlap.py`):",
        "",
        "| | held-out set | MettleWarband |",
        "|---|---|---|",
        "| exact shape-key match in training | 0% by construction | 9.6% (stdlib) |",
        "| median max shape similarity | 0.529 | **0.333** |",
        "| functions >= 0.7 similar | 29.3% | **11.2%** |",
        "",
        "**The lesson generalizes past this project: a held-out split within one",
        "codebase measures memorization of that codebase's idioms.** Report the",
        "distribution distance next to the held-out number, or the held-out number",
        "gets believed for more than it is worth.",
        "",
        "Section 1's capability was re-run on the same unseen codebase and holds:",
        "820 semantic-only candidates, 0 overlapping the shipped pass, **103",
        "validated against 17 rejected**. More sound rewrites in one application",
        "than the 111 found across ~500 toolchain programs. Precision falls out of",
        "distribution (14.2% rejected against 0.9%) and the gate catches all of it.",
        "",
    ]

    # ---- the DAgger result ----------------------------------------------
    out += [
        "## 3. True DAgger: one round, and it converged",
        "",
        "`dagger_round.py` exports the trained model, compiles the whole corpus",
        "with `METTLE_ML_MODEL` set to it, and trains on the gate's verdicts",
        "about the proposals *that* model made. The held-out set is untouched",
        "(566 of the newly harvested rows were dropped for landing on held-out",
        "shape keys).",
        "",
        "| | acc | DELETE F1 | GVN F1 | ptr | risk AUC |",
        "|---|---|---|---|---|---|",
        "| round 1 (`oracle_C`) | 0.9918 | 0.624 | 0.861 | 0.993 | 0.9957 |",
        "| round 2 (true DAgger) | 0.9916 | 0.619 | 0.853 | 0.991 | 0.9952 |",
        "",
        "**No improvement.** Flat to marginally worse on every metric. The reason",
        "is visible in the labels rather than the model:",
        "",
        "| harvest round | model in the loop | adjudicated | hard negatives |",
        "|---|---|---|---|",
        "| 1 | `gnn_genius` (shipped) | 5033 | 808 (**16.1%**) |",
        "| 2 | `oracle_C` | 4136 | 3 (**0.1%**) |",
        "",
        "The loop ran out of signal. Round 1 worked because the shipped model",
        "made mistakes worth labelling; by round 2 the model is right about 99.9%",
        "of what it proposes, so the gate has almost nothing to correct.",
        "",
        "A control separates 'the DAgger mechanism failed' from 'there is nothing",
        "left to learn'. Warm-restarting variant A on the SAME rows, no new",
        "harvest at all, is equally flat:",
        "",
        "| | acc | DELETE F1 | GVN F1 | risk AUC |",
        "|---|---|---|---|---|",
        "| A round 1 | 0.9920 | 0.643 | 0.845 | 0.9876 |",
        "| A warm refit, same rows | 0.9906 | 0.645 | 0.850 | 0.9876 |",
        "| C round 1 | 0.9918 | 0.624 | 0.861 | 0.9957 |",
        "| C true DAgger, new rows | 0.9916 | 0.619 | 0.853 | 0.9952 |",
        "",
        "Neither more training nor fresh self-collected data moves anything. The",
        "models have extracted what this label set contains. The binding",
        "constraint is the labels' COVERAGE, not the training procedure -- and",
        "Section 1 locates the uncovered headroom exactly: 111 sound rewrites the",
        "model never proposes, so the gate is never asked about them.",
        "",
        "This is a real limitation of the design and worth stating as one: the",
        "harvest only labels sites the model actually proposed, so the loop can",
        "teach **precision** but never **recall**. Nothing in it can discover a",
        "sound rewrite the model declined to propose. Improving further needs",
        "exploration during harvesting -- deliberately proposing past the",
        "model's confidence to surface new failure modes -- not more rounds of",
        "the same. Section 1 is the evidence that the recall headroom is real:",
        "111 sound rewrites the model never proposes.",
        "",
    ]

    # ---- ablation --------------------------------------------------------
    out += ["## 4. Ablation on held-out real IR", "",
            "Identical data, seed, split, and epoch count. Labels on the real",
            "rows are the validator's own verdicts.", "",
            "**Read the rare-class columns with their support in mind.** The",
            "held-out set has 36112 nodes but only 38 GVN and 1 COLLAPSE label,",
            "because validator-adjudicated reuse on real code is rare. A GVN F1",
            "moving from 0.845 to 0.889 is two instructions changing hands, not a",
            "finding. The DELETE column (support 343) and the risk sweep",
            "(1111 adjudicated proposals) are the only well-powered numbers here;",
            "for the rest, see the synthetic validation table below.", "",
            "| variant | params | ep | acc | DELETE F1 | GVN F1 | COLLAPSE F1 |"
            " ptr all/pointing | risk AUC | ACT steps |",
            "|---|---|---|---|---|---|---|---|---|---|"]
    names = {"A": "A baseline", "B": "B +OBS", "C": "C +pointer",
             "D": "D +adaptive depth"}
    got_any = False
    for v in ("A", "B", "C", "D"):
        r = load(os.path.join(work, f"eval_oracle_{v}.json"))
        if r:
            got_any = True
        out.append(variant_row(r, names[v]))
    for extra in sorted(glob.glob(os.path.join(work, "eval_oracle_*_r2.json"))):
        out.append(variant_row(load(extra), "round 2 (warm refit, same rows)"))
    # dagger_round.py records before/after together, so both sides of the
    # comparison come from one file and are guaranteed to be the same eval.
    for dg in sorted(glob.glob(os.path.join(work, "dagger*_result.json"))):
        d = load(dg)
        if not d:
            continue
        out.append(variant_row(d.get("before"), "round 1 (same model, re-eval)"))
        out.append(variant_row(d.get("after"), "**round 2 (TRUE DAgger)**"))
    if not got_any:
        out.append("| *(no evaluations yet)* | | | | | | | | | |")
    out.append("")

    # ---- the well-powered real-IR comparison ----------------------------
    out += ["### Risk filter per variant (1111 adjudicated proposals)", "",
            "The best-powered real-IR number. Only propose where predicted",
            "rejection risk is below 0.3: how much validator work is avoided,",
            "and how many sound rewrites survive the filter?", "",
            "| variant | proposals kept | rejection rate | sound rewrites retained |",
            "|---|---|---|---|"]
    for v in ("A", "B", "C", "D"):
        r = load(os.path.join(work, f"eval_oracle_{v}.json"))
        sw = ((r or {}).get("risk_threshold_sweep") or {}).get("0.3")
        if not sw:
            continue
        out.append(f"| {names[v]} | {sw['kept_frac']:.2f} | "
                   f"{sw['reject_rate']:.3f} | "
                   f"{sw['good_kept']}/{sw['good_total']} "
                   f"({100*sw['good_kept']/max(1,sw['good_total']):.0f}%) |")
    out += ["| *kind-only baseline* | 0.31 | 0.081 | 317/382 (83%) |",
            "", "Baseline row repeated from above for comparison.", ""]

    # ---- the well-powered synthetic comparison --------------------------
    out += ["### Synthetic validation set (15.5k functions, well powered)", "",
            "The same models on the held-out slice of the synthetic corpus, from",
            "the final training epoch. Far more support per class, at the cost of",
            "not being real IR. This is where an OBS effect should be visible if",
            "there is one.", "",
            "| variant | acc | DELETE | AFFINE | GVN | COLLAPSE |",
            "|---|---|---|---|---|---|"]
    import re as _re
    for v in ("A", "B", "C", "D"):
        lg = os.path.join(work, f"train_{v}.log")
        if not os.path.exists(lg):
            continue
        last = None
        for line in open(lg, encoding="utf-8", errors="replace"):
            if line.startswith("ep "):
                last = line
        if not last:
            continue
        m = _re.search(r"acc ([\d.]+) DEL ([\d.]+) AFF ([\d.]+) GVN ([\d.]+) "
                       r"COL ([\d.]+)", last)
        if m:
            out.append(f"| {names[v]} | " + " | ".join(m.groups()) + " |")
    out += ["",
            "Here the picture is consistent rather than noisy: OBS gives a small",
            "gain on every class, largest on COLLAPSE -- the action that asks",
            "'does this tangle equal an in-scope value', which is precisely what",
            "an observational fingerprint encodes.", ""]

    out += ["### Random-init controls", "",
            "The same architectures, untrained. A single random draw is not a",
            "meaningful control on its own -- the spread below is the point.", "",
            "| variant | acc | risk AUC | ptr (pointing) |", "|---|---|---|---|"]
    for v in ("A", "B", "C", "D"):
        r = load(os.path.join(work, f"control_random_{v}.json"))
        if not r:
            continue
        out.append(f"| {v} | {r.get('acc',0):.3f} | "
                   f"{r.get('risk_auc', float('nan')):.3f} | "
                   f"{(r.get('ptr_acc_pointing') or 0):.3f} |")
    out.append("")

    out += ["### What the risk head must beat", "",
            "Predicting validator rejection from trivial features",
            "(`risk_baseline.py`, 1111 adjudicated proposals, 65.6% rejected):",
            "",
            "| baseline | parameters | AUC |", "|---|---|---|",
            "| rejection rate per instruction kind | 10 | **0.963** |",
            "| rejection rate per (kind, operator) | ~60 | 0.956 |",
            "| logistic regression on 9 features + one-hot kind | ~19 | 0.862 |",
            "",
            "Read the risk column against 0.963, not against 0.5.",
            "",
            "AUC is also the wrong question. Operationally: if the pass only",
            "proposes where predicted risk is below a threshold, how much",
            "validator work does it avoid and how many sound rewrites does it",
            "lose? At threshold 0.3 on the same held-out split:",
            "",
            "| model | proposals kept | rejection rate | sound rewrites retained |",
            "|---|---|---|---|",
            "| kind-only baseline (10 numbers) | 0.31 | 0.081 | 317/382 (83%) |",
            "| kind+op baseline | 0.32 | 0.085 | 324/382 (85%) |",
            "| features + logistic regression | 0.25 | 0.044 | 263/382 (69%) |",
            "| **GNN, variant A** | **0.34** | **0.048** | **360/382 (94%)** |",
            "",
            "The GNN keeps more proposals than the kind baseline, at half its",
            "rejection rate, and retains 43 more sound rewrites. Read that way,",
            "the pass could skip roughly two thirds of its speculative",
            "validation work and keep 94% of what that work would have found.",
            ""]

    # ---- provenance ------------------------------------------------------
    out += ["### The pointer head, in the compiler", "",
            "`METTLE_ML_PTR=1` lets the trained head name its own reuse target",
            "instead of taking the analysis's. Anything it picks that the sound",
            "analysis did not is emitted `COPY?` and must clear the gate.",
            "Run over 70 example programs with variant C (`ptr_probe.py`):", "",
            "| disposition | validated | rejected | skipped |",
            "|---|---|---|---|",
            "| `COPY` (analysis's own choice) | 89 | 0 | 30 |",
            "| `COPY?` (model-chosen) | 0 | 0 | 1 |",
            "",
            "**A negative result, and a clean one.** The head is accurate --",
            "99.3% correct across 1805 candidate sites, 90.2% at naming the",
            "target on the 51 sites that have one -- but on real code it does",
            "not find reuse the available-expressions analysis misses. It",
            "reproduces the analysis rather than exceeding it.",
            "",
            "That is the predicted outcome, not a surprise. Pointer labels come",
            "from `gvn_targets`, which mirrors the *syntactic* analysis, so every",
            "semantically-equal-but-syntactically-different pair is labelled",
            "'decline' during training. The head cannot exceed a teacher it was",
            "trained to imitate. Section 1 shows those pairs are real and sound;",
            "reaching them needs labels the gate produces, which is what the",
            "DAgger round is for.", ""]

    out += ["### Compile-time cost", "",
            "`sort_insertion`, mean of 3 runs, same machine:", "",
            "| build | wall clock |", "|---|---|",
            "| `--release` (no `--ml-opt`) | 80 ms |",
            "| `--ml-opt`, variant A (10.8M params) | 846 ms |",
            "| `--ml-opt`, variant B (15.6M, +OBS) | 870 ms |",
            "",
            "OBS featurization costs about 3%. The forward pass dominates.",
            "",
            "**Adaptive depth did not work.** Variant D is worse on every metric",
            "(GVN F1 0.636 against 0.845-0.889, risk AUC 0.970 against",
            "0.988-0.996 -- barely above the 0.963 kind-only baseline) at a third",
            "the parameters. It ran 13.3 ACT steps during training against the",
            "baseline's 8 fixed layers, so it bought fewer parameters with MORE",
            "compute and ~2x slower epochs. The 0.01 ponder penalty was far too",
            "weak to buy halting.",
            "",
            "One confound, stated plainly: D hit the 100-minute wall-clock budget",
            "at ~22 epochs where the others ran 30, precisely because its steps",
            "made it slower. Some of the gap is undertraining. The gap is large",
            "enough (GVN 0.636 vs 0.845) that undertraining is unlikely to be all",
            "of it, but a clean rerun at matched epochs would be needed to say so",
            "for certain. Parameter count was the wrong thing to optimize; steps",
            "are the bill.",
            "OBS featurization itself is ~1.2 microseconds per node and flat in",
            "function size after the hash indexes went in (it was 4.6 and",
            "climbing before).", ""]

    out += ["## 5. Is the held-out set really held out?", "",
            "Maximum similarity of each held-out body to any training body over",
            "abstracted instruction shapes (`split_audit.py`):", "",
            "| split rule | held-out bodies with an identical-shape twin |",
            "|---|---|",
            "| by source file | 72% (exact-body twins) |",
            "| by body hash | 32.5% |",
            "| **by shape key (used)** | **0%**, max similarity 0.966 |",
            "",
            "Residual: 3.2% of held-out bodies sit at 0.9 similarity or above.",
            "", ]

    out += ["## 6. Does the compiler run the model that was trained?", "",
            "`check_forward.py` dumps the compiler's raw per-instruction argmax",
            "and compares it against PyTorch on the same graphs.", "",
            "| setup | agreement |", "|---|---|",
            "| random weights, canonicalized text (the original harvester) | 97.8% |",
            "| random weights, raw dump text | 99.3% |",
            "| **trained baseline (A), raw dump text** | **100.00%** (20347/20347 nodes) |",
            "| **trained OBS model (B), raw dump text** | **99.99%** (17042/17043 nodes) |",
            "",
            "The trained number is the meaningful one: near-uniform logits from",
            "random weights make the argmax trivial to flip, so those rows are an",
            "upper bound on disagreement rather than a measurement of it. The",
            "residual there concentrates on bare call statements and casts, and",
            "is present in the baseline variant too -- it predates this work.",
            "", "---", "",
            "Nothing here is shipped. `gnn_genius.bin` is untouched and",
            "`--ml-opt` behaves exactly as before; the variants are reachable",
            "only through `METTLE_ML_MODEL`.", ""]

    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
