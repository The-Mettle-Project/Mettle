# References

Erasure codes have no single standards body. The methods here are the
published ones, and the shards are checked byte for byte against the most
used implementation.

## Papers and specifications

- I. S. Reed and G. Solomon, "Polynomial Codes over Certain Finite Fields",
  J. SIAM 8(2), 1960. The code itself.
- J. S. Plank, "A Tutorial on Reed-Solomon Coding for Fault-Tolerance in
  RAID-like Systems", Software: Practice and Experience 27(9), 1997, and the
  correction note by Plank and Ding (2005). Systematic Vandermonde matrices
  and why the plain Vandermonde matrix must be made systematic first.
- J. Blomer, M. Kalfane, R. Karp, M. Karpinski, M. Luby, D. Zuckerman,
  "An XOR-Based Erasure-Resilient Coding Scheme", ICSI TR-95-048, 1995.
  Cauchy Reed-Solomon: every square sub-matrix is invertible.
- L. Rizzo, "Effective Erasure Codes for Reliable Computer Communication
  Protocols", ACM CCR 27(2), 1997. Rebuild by inverting only the rows for
  the lost data.
- J. S. Plank, K. M. Greenan, E. L. Miller, "Screaming Fast Galois Field
  Arithmetic Using Intel SIMD Instructions", FAST 2013. The split-table
  (4-bit nibble) multiply with PSHUFB.
- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 2:
  VPSHUFB, GF2P8AFFINEQB and the VEX encodings used by the kernels.

## Implementations studied

| Implementation | License | What erasure takes from it |
|---|---|---|
| [Intel ISA-L](https://github.com/intel/isa-l) (erasure_code) | BSD-3-Clause | The split-table multiply with byte shuffles; "dot product" kernels that compute several outputs in one pass over the sources (gf_Nvect_dot_prod); 32-byte tables per coefficient (low and high nibble); building the rows for lost parity as parity row x decode matrix, so data and parity are rebuilt in one pass (gf_gen_decode_matrix in its examples) |
| [klauspost/reedsolomon](https://github.com/klauspost/reedsolomon) | MIT | The API shape (Encode, Verify, Reconstruct, ReconstructData, Split, Join, StreamEncoder); the systematic Vandermonde matrix and the Cauchy option; caching inverted matrices by loss pattern; the GFNI affine-matrix multiply; splitting work into cache-sized blocks. Used as the reference: tests/vectors/klauspost.txt holds its v1.14.2 output |
| [Backblaze JavaReedSolomon](https://github.com/Backblaze/JavaReedSolomon) | MIT | The field (polynomial 0x11D, generator 2), the matrix construction klauspost also uses, and the field test values in tests/gf8_test.mettle |
| [Jerasure 2](https://github.com/tsuraan/Jerasure) and GF-Complete | BSD-3-Clause (new BSD) | Cauchy Reed-Solomon; the GF(2^16) polynomial 0x1100B (the w = 16 default); building multiply tables from c * 2^i with XORs |

No code was copied; all of the above have permissive licenses, and erasure
is written from the methods.

## Compatibility

- GF(2^8) Vandermonde shards equal klauspost/reedsolomon's default
  `reedsolomon.New(k, m)` and Backblaze's, for the same k and m.
- GF(2^8) Cauchy shards equal klauspost's `WithCauchyMatrix()`.
- GF(2^16) uses systematic Cauchy Reed-Solomon (parity p, data j has
  coefficient 1 / ((k + p) xor j)). It is not the same code as klauspost's
  Leopard GF(2^16) mode, which uses an FFT-based code.
