This directory holds the private implementation modules for `ir_optimize.c`.

The public optimizer entrypoint remains `../ir_optimize.c`. The files here are
compiled separately and share declarations through `ir_optimize_internal.h`.
That header is intentionally private to the optimizer; it exists to keep pass
families separate without turning their helpers into public IR API.

Module map:

Foundation:

- `ir_optimize_internal.h`: optimizer-private limits, shared structs, pass
  metadata, and cross-module prototypes.
- `ir_optimize_core.c`: common data structures, instruction helpers, CSE, and
  dead-temp support.
- `ir_optimize_inline.c`: function indexing and small-function inlining.
- `ir_optimize_scalar_simplify.c`: propagation, folding, branch cleanup,
  rotate fusion, and scalar copy cleanup.

Loop recognizers:

- `ir_optimize_word_count_and_div_shift.c`: word-count and positive div-by-two
  loop recognizers.
- `ir_optimize_loop_unroll.c`: counted-loop parsing, small-loop unrolling, and
  reduction unrolling.
- `ir_optimize_null_check_licm.c`: loop-invariant null-check hoisting.

Intrinsic and SIMD lowering:

- `ir_optimize_feature_scan.c`: cheap function feature collection for the pass
  scheduler.
- `ir_optimize_memory_intrinsics.c`: memcpy constant-size lowering.
- `ir_optimize_simd_sum_i32.c`: int32 horizontal-sum vectorization.
- `ir_optimize_simd_float.c`: float sum/dot/affine-map vectorization.
- `ir_optimize_shift_sort.c`: shift-loop detection and insertion-sort SIMD.
- `ir_optimize_load_copy_cleanup.c`: load-to-symbol-copy cleanup.

Specialized loop idioms:

- `ir_optimize_pointer_induction.c`: pointer induction rewrite support.
- `ir_optimize_popcount_collatz.c`: popcount and Collatz loop recognizers.
- `ir_optimize_simd_memory_maps.c`: scale/reverse/clamp memory-map
  vectorization.
- `ir_optimize_scan_search.c`: min/max, prefix-sum, lower-bound, dot-product,
  and memcmp loop recognizers.

Pipeline:

- `ir_optimize_pipeline.c`: pre-inline, fixpoint, and post-fixpoint optimizer
  pipelines.
- `ir_optimize_pass_driver.c`: pass names, `METTLE_SKIP_PASS` diagnostics,
  named-pass sequences, and fixpoint pass scheduling.
- `ir_optimize_hotness.c`: zero-run PGO/block-like source-site hotness policy
  for code-size/speed thresholds.
- `ir_optimize_sroa.c`: scalar replacement of aggregate locals.
- `ir_optimize_induction_cleanup.c`: congruent induction-variable elimination.

Diagnostics:

- `METTLE_SKIP_PASS=sroa,simd_dot_i32` skips named fixpoint passes. Numeric
  pass IDs still work for older bisection notes.
- `METTLE_TRACE_IR_PASSES=1` prints pass decisions (`changed`, `clean`,
  `disabled`, `already_clean`) with function, version, and fixpoint iteration.
