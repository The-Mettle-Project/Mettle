# Mettle benchmark harness

Canonical matrix: [`harness.json`](harness.json)

Every benchmark entry carries a `suite` number; an entry without one counts as
Suite 1. Suites 1 and 2 measure kernels and data structures. Suite 3 measures
whole programs.

## Suite 1: kernels (Mettle vs C)

Each example under `examples/` ships a matched triple: `.mettle`, `.c`, and `.rs`.

| Name | Workload |
|------|----------|
| `fib` | fib(35) × 10M (Mettle uses an unrolled hot loop) |
| `word_count` | Count words in a 256 KB buffer × 500 |
| `grep` | Count lines containing `ERROR` in 1 MiB × 200 |
| `sum_squares` | Sum 1²…100000² × 200 |
| `collatz` | Collatz steps for n=1..100000 × 10 (heavy pass, not scaled to 200) |
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

The rest of the Suite 1 roster (`float_sum`, `mandelbrot`, `rec_fib`,
`int_divmod`, `leaf_call`, `func_ptr`, `saxpy`, `fp_div`, `struct_byval`,
`switch_vm`, `aos_sum`, `transpose`, `const_mod`, `global_acc`, `i2f_conv`,
`byte_arith`) is listed in [`harness.json`](harness.json).

## Suite 2: data structures and codecs

Matched pairs: `.mettle` and `.c`.

| Name | Workload |
|------|----------|
| `quicksort` | Recursive quicksort (Lomuto) over 2048 int32 values × 200 |
| `crc32` | CRC-32 bit by bit over 256 KB × 200 |
| `base64_encode` | Base64 encode 256 KB × 200 |
| `linked_list_sum` | Pointer chase over a shuffled 65536-node list × 200 |
| `gather_sum` | Random gather `a[b[i]]` over a 64 MB table × 20 |
| `field_sum` | One field out of 1048576 32-byte records × 100 |
| `matvec` | float64 512×512 matrix-vector multiply × 200 |
| `heapsort` | In-place binary-heap sort over 2048 int32 values × 200 |
| `merge_sort` | Recursive top-down merge sort over 2048 int32 values × 200 |
| `radix_sort` | LSD radix sort, 4 digit passes, 4096 uint32 values × 200 |
| `rle_encode` | Run-length encode 256 KB × 200 |
| `bst_insert` | Build a 4096-node BST, recursive in-order traversal × 200 |
| `bignum_mul` | factorial(1000), wide schoolbook multiplies over base 2^32 limbs, decimal rendering × 24 |
| `life_bits` | 512×1024 bit grid, 200 Life generations through word-parallel uint64 full adders × 3 |

## Suite 3: applications

Matched pairs: `.mettle` and `.c`. Each entry is a small complete program that
parses, builds, transforms, and verifies, rather than a loop over an array.

Suite 3 has two sets. Set 1 is the original eight. Set 2 adds eight more chosen
for shapes Set 1 never reaches: a cache-resident tree, an interpreter dispatch
loop, unpredictable backtracking, float64 without `sqrt`, a reachability walk
over an object graph, a dense dynamic-programming table, symbol resolution over
a record stream, and a parse-group-sort query pipeline. Run either with
`-Set 1` or `-Set 2`.

The admission test for Set 2 is the one the rules already imply: **could a
recognizer win this without making a real program faster?** If yes, it belongs
in Suite 1 or 2, where a fixed shape is the point. A named textbook kernel is
disqualifying however much time it takes, and so is any program where a single
loop owns the measurement. Two candidates were written and rejected on exactly
that ground: a bignum schoolbook multiply and a word-parallel Game of Life. Both
are honest kernels, both are now in Suite 2, and neither is a Suite 3 program.

### Set 1

| Name | Workload |
|------|----------|
| `json_parse` | Generate a 330 KB JSON catalogue, then tokenize and parse it into a node arena and walk the tree × 30 |
| `interp_ast` | Generate a 165 KB program, lex it, parse it with precedence climbing, run the AST × 20 |
| `word_freq` | Tokenize 1 MB, count words in an open-addressing map that grows and rehashes, select the top 16 × 15 |
| `huffman` | Histogram 128 KB, build the tree through a min-heap, assign codes, bit-pack, decode, verify × 6 |
| `lz77` | Hash-chain match finder over 256 KB with a 32 KB window, decompress, verify × 4 |
| `astar_grid` | Grow a 192×192 cave, run 16 A* searches with a binary-heap open set × 3 |
| `regex_match` | Compile 5 backtracking patterns, match each at every offset of 4000 log lines × 3 |
| `physics_grid` | Bucket 8192 particles into a 64×64 grid, resolve neighbour collisions, integrate, 12 steps × 2 |

### Set 2

| Name | Workload | Ground it covers |
|------|----------|------------------|
| `btree_index` | 50000 inserts with node splits, a full lookup pass, a half-miss pass, an in-order walk × 3 | 256-byte nodes, in-node shifting, recursive descent |
| `bytecode_vm` | Assemble a register program, then run collatz over [2,9000) through a switch dispatch loop × 3 | Interpreter dispatch and a memory register file |
| `sudoku_solve` | 120 generated puzzles, 61 holes each, bitmask propagation with MRV backtracking × 3 | Deep recursion, early exit, branches that never predict |
| `voxel_trace` | 64^3 grid, 200×150 rays, float64 DDA traversal with one reflection bounce × 3 | float64 without `sqrt`, axis-aligned normals |
| `mark_sweep` | Build a 160000-object graph, mark from roots through a worklist, sweep, compact and remap, mutate, 3 cycles × 2 | Pointer chasing in a data-dependent order, forwarding remap |
| `diff_lcs` | 90 document pairs of 240 lines, line hashing, LCS dynamic programming, backtrack to an edit script, replay and verify × 3 | A DP table with a serial row recurrence, then a replay pass |
| `link_resolve` | Parse 1200 object records, intern 48000 symbols, resolve cross-unit references, apply relocations to a 1 MB image × 3 | Byte-stream parsing, string interning, probe chains |
| `csv_query` | Generate 60000 rows, parse, hash group-by into 4096 groups, quicksort by value × 5 | Integer parsing, open addressing, a comparison sort on records |

`voxel_trace` is written without `sqrt` on purpose. Mettle's `std/math` `sqrt`
is a Newton-Raphson iteration and C's is the hardware instruction, so the two
do not agree bit for bit and a checksum over their results would fork. Axis-
aligned normals and a slab traversal need no roots, which keeps rule 6 intact
while still putting float64 arithmetic and division on the clock.

### The rules Suite 3 is written to

A microbenchmark rewards a compiler for recognizing one loop. These programs are
built so that recognizing one loop buys almost nothing, while everything an
application actually spends time on is on the clock.

1. Three phases at least. Each program generates its input, transforms it,
   and checks the result, so no single loop owns the measurement.
2. The two sources are mirrors. Same functions in the same order, same
   control flow, same types. Neither side is unrolled, annotated, or hand tuned;
   no `@simd`, no `@inline`, no pointer tricks on one side only.
3. Control flow follows the data. Recursive descent, switch dispatch, probe
   loops, greedy backtracking, hash chains, heap sift paths, frontier
   expansion. The branch predictor and the prefetcher both have to work.
4. The input is made in the process from a fixed seed. No files, no clock,
   no environment, and every machine gets identical work.
5. A checksum covers the whole result. A pair is accepted only when Mettle
   and C print the same one, which pins both the timing loop and the answer.
   `huffman` and `lz77` also print a roundtrip mismatch count.
6. Float constants are exact in binary. A decimal like `0.35` has to be
   rounded when it is parsed, and two front ends need not round it the same way,
   which would fork the checksum before the program ran. `physics_grid` sticks
   to integers and dyadic fractions, and its collision response is written in
   the sqrt-free form so both sides run the identical IEEE sequence. Writing
   this suite caught Mettle's own conversion rounding per scaling step; the
   owned strtod now converts exactly on every path and
   `tests/test_float_literal_parse.mettle` pins 49 literals to the bit patterns
   gcc produces. The rule stays as a guard against toolchains that round
   differently.

All runtime programs print `Time: <N> us` using QueryPerformanceCounter
(`examples/bench_time.h`, `stdlib/std/bench.mettle`).

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

# One suite
.\tools\benchmark\run-benchmarks.ps1 -Suite 3

# One set within a suite
.\tools\benchmark\run-benchmarks.ps1 -Suite 3 -Set 2

# Subset
.\tools\benchmark\run-benchmarks.ps1 -Benchmark fib,grep

# Compile/build only (skip runtime execution)
.\tools\benchmark\run-benchmarks.ps1 -CompileOnly

# Skip large compile fixtures
.\tools\benchmark\run-benchmarks.ps1 -SkipCompileBenchmarks

# More stable timings
.\tools\benchmark\run-benchmarks.ps1 -Runs 7 -Warmup 2
```

On Linux the same matrix runs through `tools/benchmark/run-benchmarks.sh`, which
takes `--suite 3`, `--benchmark fib,grep`, `--runs`, and `--warmup`.

## Other harnesses

| Script | Compares |
|--------|----------|
| `compare-rust.ps1` | Rust vs C on the runtime matrix |
| `compare-mettle-versions.ps1` | Two Mettle compiler builds |

## Output

- `docs/benchmarks/latest.json`: canonical results (includes per-run timings, host info, summary stats)
- `web/benchmarks.json`: mirror for the site (created on first run)

C benchmarks compile with `-O3` by default (see `defaults.c_flags` in `harness.json`).
