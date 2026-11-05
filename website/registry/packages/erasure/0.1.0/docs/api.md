# erasure API

```mettle
import "erasure/erasure";
```

Every function that can fail returns an `int32` status: `ERASURE_OK` (0)
or a negative `ERASURE_E_*` code. `erasure_error_text(rc)` gives a short
message.

| Code | Meaning |
|---|---|
| `ERASURE_E_ARG` | bad argument: null pointer, bad count or size |
| `ERASURE_E_SHARDS` | fewer than k shards present, so nothing can be rebuilt |
| `ERASURE_E_VERIFY` | parity does not match the data |
| `ERASURE_E_MEM` | out of memory |
| `ERASURE_E_IO` | a stream read or write callback failed |
| `ERASURE_E_SIZE` | stream shards of different lengths, or a short input |

A shard set is an array of `k + m` pointers (`uint8**`), data shards first,
then parity. Every shard has the same size and every pointer must be valid,
lost ones included: a rebuild writes into the buffers of the lost shards.
A `bool*` array of `k + m` entries says which shards hold good data.

## Reed-Solomon over GF(2^8)

For up to 256 shards in total.

```mettle
var c: ErasureRS;
erasure_rs_new(&c, 10, 4);                 // 10 data + 4 parity
erasure_rs_encode(&c, shards, size);       // fills shards[10..14]
erasure_rs_verify(&c, shards, size);       // ERASURE_OK or ERASURE_E_VERIFY
present[3] = false;                        // shard 3 is lost
erasure_rs_reconstruct(&c, shards, present, size);
erasure_rs_free(&c);
```

| Function | What it does |
|---|---|
| `erasure_rs_new(c, k, m)` | Codec with the systematic Vandermonde matrix. Shards equal klauspost/reedsolomon's default and Backblaze's |
| `erasure_rs_new_with(c, k, m, kind)` | `kind` is `ERASURE_MATRIX_VANDERMONDE` or `ERASURE_MATRIX_CAUCHY` (equal to klauspost's `WithCauchyMatrix`) |
| `erasure_rs_free(c)` | Frees the codec |
| `erasure_rs_encode(c, shards, size)` | Computes the m parity shards |
| `erasure_rs_encode_range(c, shards, off, n)` | Same, for bytes `off..off+n` only |
| `erasure_rs_verify(c, shards, size)` | Checks the parity |
| `erasure_rs_reconstruct(c, shards, present, size)` | Rebuilds every lost shard |
| `erasure_rs_reconstruct_data(c, shards, present, size)` | Rebuilds only lost data shards (faster when parity is not needed) |
| `erasure_rs_reconstruct_range(c, shards, present, off, n, data_only)` | Rebuild for bytes `off..off+n` |
| `erasure_rs_coef(c, row, col)` | An entry of the coding matrix |
| `erasure_shard_size(len, k)` | Shard size that holds `len` bytes over k shards |
| `erasure_split(data, len, shards, k, size)` | Copies data into the k data shards, zero padded |
| `erasure_join(shards, k, size, out, len)` | Copies the first `len` data bytes back out |

Rebuild plans (inverted matrices) are cached per loss pattern, 32 at a
time. A codec is not safe to use from two threads while one rebuilds;
encode and verify only read it.

## Streaming

For shards larger than memory. The caller passes two closures:

```mettle
read:  Fn(int32, uint8*, int64) -> int64   // (shard, buf, max) -> bytes, 0 at end, < 0 error
write: Fn(int32, uint8*, int64) -> int32   // (shard, buf, n) -> 0 ok, nonzero error
```

Reads may return short counts. `chunk` is the bytes per shard per step;
0 means `ERASURE_STREAM_CHUNK` (1 MiB).

| Function | Reads | Writes |
|---|---|---|
| `erasure_rs_stream_split(c, total, read, write, chunk)` | one input, shard `-1` | data shards `0..k` |
| `erasure_rs_stream_encode(c, read, write, chunk)` | data shards | parity shards `k..k+m` |
| `erasure_rs_stream_verify(c, read, chunk)` | all shards | nothing |
| `erasure_rs_stream_reconstruct(c, present, read, write, chunk, data_only)` | present shards | lost shards |
| `erasure_rs_stream_join(c, total, read, write, chunk)` | data shards | one output, shard `-1` |

See `examples/shards.mettle` for files on disk.

## Reed-Solomon over GF(2^16)

For wide stripes: up to 65536 shards in total. Systematic Cauchy
Reed-Solomon over GF(2^16) with polynomial 0x1100B. Shard sizes must be
even (16-bit symbols, little-endian).

| Function | What it does |
|---|---|
| `erasure_rs16_new(c, k, m)` / `erasure_rs16_free(c)` | Set up and free an `ErasureRS16` |
| `erasure_rs16_encode(c, shards, size)` | Parity |
| `erasure_rs16_verify(c, shards, size)` | Check |
| `erasure_rs16_reconstruct(c, shards, present, size)` | Rebuild all lost shards |
| `erasure_rs16_reconstruct_data(c, shards, present, size)` | Rebuild lost data only |
| `erasure_rs16_coef(c, p, j)` | Coefficient of data j in parity p |

A rebuild inverts only the e x e block for the e lost data shards, so it
costs about e x k multiply passes, not k^3.

## XOR parity

One parity shard, the XOR of the k data shards (as RAID 5). `shards` has
k + 1 pointers.

| Function | What it does |
|---|---|
| `erasure_xor_encode(shards, k, size)` | Writes `shards[k]` |
| `erasure_xor_verify(shards, k, size)` | Checks it |
| `erasure_xor_rebuild(shards, k, size, lost)` | Rebuilds shard `lost` (0..k) |

## Lower level

| Function | What it does |
|---|---|
| `erasure_gf8_mul/div/inv/pow` | GF(2^8) arithmetic (call `erasure_gf8_init()` first, or make a codec) |
| `erasure_gf8_mat_mul`, `erasure_gf8_mat_invert` | Matrices over GF(2^8) |
| `erasure_gf8_vandermonde`, `erasure_gf8_cauchy` | Coding matrices |
| `erasure_gf8_plan_init/apply/free` | Multiply a set of buffers by any coefficient matrix with the fastest kernel |
| `erasure_gf16_mul/inv/exp`, `erasure_gf16_mul_add` | GF(2^16) arithmetic (call `erasure_gf16_init()` first) |
| `erasure_simd_best()` | Best kernel level here: `ERASURE_SIMD_NONE`, `_AVX2`, `_GFNI` (AVX2 + GFNI) or `_AVX512` (AVX-512 + GFNI) |
| `erasure_simd_set(level)` | Use at most this level (for tests and benchmarks) |
| `erasure_simd_name(level)` | Its name |

## Kernels

On x86-64 the GF(2^8) multiply uses, best first:

- AVX-512 + GFNI: one `VGF2P8AFFINEQB` per coefficient and 64 bytes,
  128 bytes per loop step.
- AVX2 + GFNI: one `VGF2P8AFFINEQB` per coefficient and 32 bytes. Used
  for up to 16 sources; with more, the AVX2 kernel measured faster.
- AVX2: the split-table method, two `VPSHUFB` lookups (low and high nibble).
- Portable: one 256-byte table row per coefficient.

Each vector kernel computes up to four outputs per pass over the sources,
in 16 KiB blocks so the sources stay in cache. Bytes left over after the
widest kernel go through the narrower ones, then the portable path. All levels give the same
bytes; the tests compare them. The GF(2^16) codec uses the portable path
(two 256-entry tables per coefficient).
