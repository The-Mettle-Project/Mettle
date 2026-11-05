# Standards and references

## Standards Press follows

| Format | Standard |
|---|---|
| DEFLATE | RFC 1951, *DEFLATE Compressed Data Format Specification version 1.3* |
| zlib | RFC 1950, *ZLIB Compressed Data Format Specification version 3.3* |
| gzip | RFC 1952, *GZIP file format specification version 4.3* |
| Zstandard | RFC 8878, *Zstandard Compression and the 'application/zstd' Media Type*, and `doc/zstd_compression_format.md` in the zstd repository |
| LZ4 frame | *LZ4 Frame Format Description* (`doc/lz4_Frame_format.md` in the lz4 repository) |
| LZ4 block | *LZ4 Block Format Description* (`doc/lz4_Block_format.md`) |
| xxHash | *xxHash fast digest algorithm* (`doc/xxhash_spec.md` in the xxHash repository) |
| CRC-32 | the CRC of RFC 1952 section 8 (ISO 3309 / ITU-T V.42 polynomial 0xEDB88320, reflected) |
| Adler-32 | RFC 1950 section 8 |

## Implementations studied

Press contains no code from these projects. It uses their published ideas,
re-written in Mettle. All of them are under permissive licenses (MIT, zlib
or BSD); none is GPL-only.

### libdeflate (Eric Biggers, MIT)

The model for the DEFLATE decoder and much of the encoder:

- the 64-bit bit buffer refilled by one unaligned 8-byte load, keeping 56 or
  more bits so a whole length/distance pair decodes without another refill
- decode table entries that carry the codeword length, extra-bit count, base
  value and flags together, with 11-bit and 8-bit main tables plus subtables
- a fast loop with no bounds checks while enough input and output remain,
  and 8-byte match copies that may run past the end into reserved room
- the block-split heuristic (compare the newest symbols' mix with the
  block's so far) used to end DEFLATE blocks
- the Moffat and Katajainen in-place algorithm for Huffman code lengths

### zlib (Jean-loup Gailly and Mark Adler, zlib license) and zlib-ng (zlib license)

- the "enough" table sizes for Huffman decode tables (2342 and 402 entries)
- the incomplete-code rule (only a single one-bit code may be incomplete)
- the level table shape (chain depth, nice length, lazy and good lengths),
  lazy matching, and length-3 matches only within 4 KiB
- Adler-32 summing with the 5552-byte limit before reduction; zlib-ng's
  approach of summing a fixed-size block with weights
- the sync flush (empty stored block) for streaming over sockets
- zlib-ng's slicing CRC-32 tables (Press uses 16 tables)

### Zstandard (Meta, BSD)

- FSE and Huffman table layouts and the backward bit reader
- 4-stream Huffman literals, repeat offsets, and sequence execution with
  wide copies
- the fast, double-fast, lazy and optimal (price-based) match finders, and
  per-level parameter tables
- the golden test files in `tests/vectors/zstd` (BSD licensed, see the
  README there)

### LZ4 (Yann Collet, BSD)

- the single-hash fast compressor with skip acceleration on data that does
  not compress
- the hash-chain (HC) compressor for higher levels
- the decoder's wide copies with safe margins at the end of the buffer

### xxHash (Yann Collet, BSD)

- XXH32 and XXH64 exactly as the specification gives them, with streaming
  states

## Tools used for checking

`gzip`, `zstd` (v1.5.7) and `lz4` (v1.10) command-line tools, and Python's
`zlib` module, are used in the tests to check Press's output and to make
input for Press. `mbuild bench` compares speed and size against the same
tools.
