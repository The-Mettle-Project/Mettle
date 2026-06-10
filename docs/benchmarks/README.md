# Mettle benchmark harness

Canonical matrix: [`harness.json`](harness.json)

## Runtime benchmarks (Mettle vs C)

Each example under `examples/` ships a matched triple: `.mettle`, `.c`, and `.rs`.

| Name | Workload |
|------|----------|
| `fib` | fib(35) × 10M (Mettle uses an unrolled hot loop) |
| `word_count` | Count words in a 256 KB buffer × 500 |
| `grep` | Count lines containing `ERROR` in 1 MiB × 200 |
| `sum_squares` | Sum 1²…100000² × 200 |
| `collatz` | Collatz steps for n=1..100000 × 10 (heavy pass — not scaled to 200) |
| `byte_hash` | djb2 hash over 256 KB × 200 |
| `prime_count` | Trial-division prime count to 50000 × 200 |
| `matrix_mul` | Naive 32×32 int32 matrix multiply × 200 |
| `sort_insertion` | Insertion sort 512 int32 values × 200 |
| `memcpy_bench` | `memcpy` over 256 KB × 200 |
| `memset_bench` | `memset` over 256 KB × 200 |
| `memcmp_bench` | Byte-compare 256 KB (4 KB chunks) × 200 |
| `binary_search` | Lower-bound in sorted 65536 int32 array, 50000 queries × 200 |
| `dot_product` | int32 dot product of 65536-element vectors × 200 |
| `lcg_rng` | LCG PRNG 1M iterations × 200 |
| `prefix_sum` | Inclusive prefix sum, 65536 int32 values × 200 |
| `popcount` | Population count over 256 KB byte buffer × 200 |
| `reverse_i32` | Reverse-copy 65536 int32 values × 200 |
| `minmax_scan` | Min/max scan over 65536 int32 values × 200 |
| `scale_i32` | `dst[i] = src[i]*3+7` over 65536 values × 200 |
| `clamp_i32` | clamp to [-100,100] over 65536 int32 values × 200 |

All runtime programs print `Time: <N> us` using QueryPerformanceCounter (`examples/bench_time.h`, `stdlib/std/bench.mettle`).

## Compile-only benchmarks

Large fixtures under `tests/` for compiler phase stress (no linked executable):

| Name | Generator | ~LOC |
|------|-----------|------|
| `parse_stress` | `tests/gen_parse_stress_test.py` | 200k flat globals |
| `profiler` | `tests/gen_profiler_test.py` | 226k functions + call graph |

These are timed with `mettle --profile` (total compile ms).

## Running

```powershell
# Full suite (runtime + compile-only)
.\tools\benchmark\run-benchmarks.ps1

# Rebuild compiler first
.\tools\benchmark\run-benchmarks.ps1 -BuildCompiler

# Subset
.\tools\benchmark\run-benchmarks.ps1 -Benchmark fib,grep

# Compile/build only (skip runtime execution)
.\tools\benchmark\run-benchmarks.ps1 -CompileOnly

# Skip large compile fixtures
.\tools\benchmark\run-benchmarks.ps1 -SkipCompileBenchmarks

# More stable timings
.\tools\benchmark\run-benchmarks.ps1 -Runs 7 -Warmup 2
```

## Other harnesses

| Script | Compares |
|--------|----------|
| `compare-rust.ps1` | Rust vs C on the runtime matrix |
| `compare-mettle-versions.ps1` | Two Mettle compiler builds |

## Output

- `docs/benchmarks/latest.json` — canonical results (includes per-run timings, host info, summary stats)
- `docs/benchmarks/latest.html` — self-contained HTML report with charts and comparison tables
- `web/benchmarks.json` — mirror for the site (created on first run)

C benchmarks compile with `-O3` by default (see `defaults.c_flags` in `harness.json`).
