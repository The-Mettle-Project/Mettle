# Design

## Layout

```
src/erasure.mettle        re-exports every module
src/core/status.mettle    status codes
src/gf8/field.mettle      GF(2^8) tables and arithmetic (polynomial 0x11D)
src/gf8/matrix.mettle     matrix multiply and inverse; Vandermonde and Cauchy
src/gf8/cpu.mettle        picks the kernel: AVX-512 + GFNI, AVX2 + GFNI, AVX2, or portable
src/gf8/kernels.mettle    the x86-64 vector kernels (inline asm)
src/gf8/plan.mettle       "multiply these buffers by this matrix", any kernel
src/rs/codec.mettle       GF(2^8) Reed-Solomon codec and rebuild cache
src/rs/stream.mettle      chunked streaming over read and write closures
src/gf16/field.mettle     GF(2^16) arithmetic (polynomial 0x1100B)
src/gf16/codec.mettle     wide-stripe Cauchy Reed-Solomon over GF(2^16)
src/xor/parity.mettle     single XOR parity
tests/                    test programs, run with `mbuild test` and `mbuild test -r`
bench/bench.mettle        `mbuild bench`
examples/shards.mettle    store a file as shards, lose some, get it back
```

## How a multiply runs

Everything in the GF(2^8) codec is one operation: given a coefficient
matrix with `rows` outputs and `cols` sources, compute

    out[o][i] = sum over j of coef[o][j] * src[j][i]

Encoding uses the parity rows of the coding matrix. A rebuild uses the
inverse of the rows of the first k surviving shards; the rows for lost
parity are that parity row times the inverse, so lost data and lost
parity come out of the same pass (as ISA-L does).

`ErasureGf8Plan` builds the tables for a matrix once:

- for AVX2, 64 bytes per coefficient: c * x for the 16 low nibbles and
  c * (x << 4) for the 16 high nibbles, each twice to fill a 256-bit lane;
- for AVX2 + GFNI, 32 bytes per coefficient: the 8 x 8 bit matrix of
  "multiply by c", four times;
- for AVX-512 + GFNI, that matrix once (8 bytes), broadcast into a
  register in the loop.

The kernels handle up to four outputs at once, holding them in registers
while they walk the sources, so each source byte is read once per four
outputs. Work is cut into 16 KiB blocks so the sources stay in cache when
there are more than four outputs. The AVX-512 kernels do 128 bytes per
step; what is left goes through the 32-byte kernels, then the portable
path.

The vector instructions are written as `db` bytes in the asm blocks, with
the mnemonic beside each, because the Mettle assembler has no SSSE3, AVX
or GFNI mnemonics. The bytes come from the GNU assembler.

## Matrices

The coding matrix is systematic (identity on top), so data shards are
stored as they are. The default is the Vandermonde matrix made systematic
by multiplying by the inverse of its top square (Plank's correction), the
same as klauspost/reedsolomon and Backblaze. Plain Vandermonde rows are not
safe to use directly: some k-row subsets are singular.

Cauchy is offered too. Both give any-k-of-n recovery for k + m <= 256.

## GF(2^16)

GF(2^8) limits a stripe to 256 shards. The GF(2^16) codec allows 65536.
It uses systematic Cauchy Reed-Solomon. Inverting a k x k matrix would cost
k^3, too much for k in the thousands, so a rebuild works on the e lost
data shards only: subtract the known data from e surviving parity shards
and invert the e x e Cauchy block (always invertible).

## Limits and next steps

- The vector kernels are x86-64 only; ARM64 uses the portable path.
- klauspost/reedsolomon is still faster at some shapes (10+4) because it
  has a kernel generated for each shape with the matrix held in
  registers.
- GF(2^16) multiply is portable (two table lookups per symbol). A
  split-table vector kernel and the Leopard FFT codec would make wide
  stripes faster.
- A codec's rebuild cache is not locked; use one codec per thread.
