# Mettle examples

Runnable programs demonstrating the language and serving as the runtime benchmark suite.

## Benchmark examples

Each directory below contains `*.mettle`, `*.c`, `*.rs`, and `build.bat`. They are wired into [`docs/benchmarks/harness.json`](../docs/benchmarks/harness.json) and run via [`tools/benchmark/run-benchmarks.ps1`](../tools/benchmark/run-benchmarks.ps1). Every benchmark entry carries a `suite` number; benchmarks without one default to Suite 1.

### Suite 1 (original)

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

The full Suite 1 roster (including microbenchmarks like `saxpy`, `memcpy_bench`, `dot_product`, etc.) is listed in [`docs/benchmarks/harness.json`](../docs/benchmarks/harness.json).

### Suite 2 (new)

| Directory | Description |
|-----------|-------------|
| [`quicksort/`](quicksort/) | Recursive quicksort (Lomuto partition) over 2048 int32 values |
| [`crc32/`](crc32/) | CRC-32 (bit-by-bit) checksum over a 256 KB buffer |
| [`base64_encode/`](base64_encode/) | Base64 encoding of a 256 KB buffer |
| [`linked_list_sum/`](linked_list_sum/) | Pointer-chasing sum over a shuffled 65536-node singly-linked list |
| [`matvec/`](matvec/) | float64 512×512 matrix-vector multiply |
| [`heapsort/`](heapsort/) | In-place binary-heap sort over 2048 int32 values |
| [`merge_sort/`](merge_sort/) | Recursive top-down merge sort over 2048 int32 values |
| [`radix_sort/`](radix_sort/) | LSD radix sort (4× 8-bit digit passes) over 4096 uint32 values |
| [`rle_encode/`](rle_encode/) | Run-length encoding of a 256 KB buffer |
| [`bst_insert/`](bst_insert/) | Binary-search-tree build + recursive in-order traversal, 4096 nodes |

Shared timing helpers live in [`bench_time.h`](bench_time.h) (C) and [`bench_time.rs`](bench_time.rs) (Rust). Mettle programs import `std/bench`.

Build one example manually:

```bat
examples\fib\build.bat
examples\fib\fib.exe
```

Run the full Mettle-vs-C suite (both Suite 1 and Suite 2):

```powershell
.\tools\benchmark\run-benchmarks.ps1
```

Run a single suite:

```powershell
.\tools\benchmark\run-benchmarks.ps1 -Suite 1
.\tools\benchmark\run-benchmarks.ps1 -Suite 2
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
