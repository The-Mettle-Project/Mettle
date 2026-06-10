# Mettle examples

Runnable programs demonstrating the language and serving as the runtime benchmark suite.

## Benchmark examples

Each directory below contains `*.mettle`, `*.c`, `*.rs`, and `build.bat`. They are wired into [`docs/benchmarks/harness.json`](../docs/benchmarks/harness.json) and run via [`tools/benchmark/run-benchmarks.ps1`](../tools/benchmark/run-benchmarks.ps1).

| Directory | Description |
|-----------|-------------|
| [`fib/`](fib/) | Iterative Fibonacci; 10M× fib(35) |
| [`word_count/`](word_count/) | Whitespace word counting on a synthetic buffer |
| [`grep/`](grep/) | Line grep with uint64 pattern matching |
| [`sum_squares/`](sum_squares/) | Sum of squares 1..n |
| [`collatz/`](collatz/) | Collatz step counting |
| [`byte_hash/`](byte_hash/) | djb2 byte hash |
| [`prime_count/`](prime_count/) | Trial-division prime counting |
| [`matrix_mul/`](matrix_mul/) | 32×32 matrix multiply |
| [`sort_insertion/`](sort_insertion/) | Insertion sort |

Shared timing helpers live in [`bench_time.h`](bench_time.h) (C) and [`bench_time.rs`](bench_time.rs) (Rust). Mettle programs import `std/bench`.

Build one example manually:

```bat
examples\fib\build.bat
examples\fib\fib.exe
```

Run the full Mettle-vs-C suite:

```powershell
.\tools\benchmark\run-benchmarks.ps1
```

## Mettle vs Rust demo

[`mettle_vs_rust/`](mettle_vs_rust/) — single workload in Mettle and Rust with a script that compares **compile time**, **binary size**, and **runtime** side by side. Run `examples\mettle_vs_rust\build.bat`.

## Other examples

| Directory | Description |
|-----------|-------------|
| [`grep/`](grep/) | Also the reference string-search benchmark |
| [`hexdump/`](hexdump/) | Hex dump utility |
| [`ui_demo/`](ui_demo/) | Win32 UI demo (`std/ui`); see [ui_demo/README.md](ui_demo/README.md) |
| [`tracy_demo/`](tracy_demo/) | Tracy profiler demo (`std/tracy`); see [tracy_demo/README.md](tracy_demo/README.md) |
| [`gpu_vadd/`](gpu_vadd/) | GPU offload demo: a `kernel` compiled to PTX and launched with `dispatch` (`std/gpu`); see [docs/gpu.md](../docs/gpu.md) |
| [`guessing-game/`](guessing-game/) | Simple interactive game |
| [`direct_object_smoke/`](direct_object_smoke/) | Direct object backend smoke test |

## Regenerating compile stress fixtures

```powershell
python tests/gen_parse_stress_test.py
python tests/gen_profiler_test.py
```

See [`docs/benchmarks/README.md`](../docs/benchmarks/README.md) for compile-only benchmark details.
