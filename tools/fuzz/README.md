# Differential miscompile fuzzer

Makes Mettle hard to miscompile silently. Generates random, UB-free programs,
builds each at **debug** and **release**, runs both, and flags any exit-code
divergence. Debug is the trusted oracle (the optimizer only runs at
`-O`/`--release`), so a divergence is a genuine miscompile, never generator UB.

## Quick start

```sh
# Build the compiler first (./build.bat), then:
python tools/fuzz/fuzz.py --count 500            # seeds 1..500
python tools/fuzz/fuzz.py --count 500 --start 1000
```

Failing seeds are written to `tools/fuzz/repros/<seed>.mettle` (transient; the
seed is in the file header, so regenerate any case with
`python tools/fuzz/genprog.py <seed>`). NOTE: seeds are only meaningful for
the generator version that produced them — the saved repro file is the
authoritative artifact.

## Files

- **genprog.py** — program generator (v2). `main` computes an int64
  accumulator, masked to 40 bits after every step so signed overflow is
  impossible. v1 covered only int64 shapes; v2 adds the historical blind
  spots, each keyed to a real past bug class or SIMD recognizer: float
  scalar/array/call shapes (exact-representable values only, so vectorized
  reduction reordering cannot change results), pointer-param SIMD kernel
  helpers (sum/dot/map for f32/f64/int32/uint8, int32 matmul), structs by
  value, narrow ints (uint32/int32 div/mod/shift), byte maps/dots, globals
  with `@pure` readers, decorators, range-for, multi-param helpers, and
  occasional jumbo mains (MIR size-bail coverage). See the module docstring
  for the determinism rules.
- **fuzz.py** — the differential driver. On a divergence it automatically
  **attributes** the failure by rebuilding release under `METTLE_SKIP_PASS`:
  first group-skipping all SIMD passes, then all known passes, then
  bisecting to a single pass name. `[pass=simd_byte_map]` in the report
  means skipping exactly that pass makes release agree with debug;
  `[backend/codegen]` means the divergence survives with every IR pass
  disabled.
- **irexec.py** — reference interpreter for the `--dump-ir` sidecar (the v1
  int64 subset only). Run the **optimized** IR through it to split bug
  classes when pass attribution is inconclusive.
- **reduce.py** — ddmin source reducer (pins the debug exit code as
  invariant so reductions stay semantics-preserving). Slow but effective;
  PID-unique temp files, so several can run in parallel.

## Bisecting a miscompile to one pass

The compiler honors `METTLE_SKIP_PASS="sroa,simd_dot_i8,14"` (names or
numeric fixpoint indices) and it now covers **both** fixpoint passes and the
named-sequence stages (float vectorizers, auto_vectorize, outer_vectorize,
SLP/dot passes, hoist_pure_calls). fuzz.py does this bisection automatically
on every divergence.

## History

Built 2026-05-26. First sweep found ~13% of seeds miscompiling; fixed three
silent bugs (loop-unroller exit jump, backend symbol-alias coalescing,
empty-nested-if diamond removal); clean over 1000 seeds.

2026-06-09: generator v2 (float/struct/narrow/byte/SIMD-kernel shapes) +
automatic pass attribution. The first 40-seed sweep found ~11/40 divergences
across at least three distinct bug classes (simd_byte_map, simd_sum_u8,
and a release-codegen class independent of IR passes).
