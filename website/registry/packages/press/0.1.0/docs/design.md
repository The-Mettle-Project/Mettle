# How Press works

This page explains the inside of Press for people who want to change it or
tune it. To use it, read [api.md](api.md).

## Layout

```
src/press.mettle        facade: imports every module, has no exports of its own
src/core/status.mettle  PRESS_OK and the PRESS_E_* codes
src/core/buf.mettle     PressBuf, little-endian loads and stores, ctz
src/check/              CRC-32, Adler-32, XXH32, XXH64
src/deflate/            DEFLATE decoder and encoder, zlib and gzip framing, streams
src/lz4/                LZ4 blocks and frames
src/zstd/               Zstandard frames
tests/                  test programs (`mbuild test`)
bench/bench.mettle      the benchmark (`mbuild bench`)
examples/gz/            a small gzip tool built on the streaming API
```

Exported names start with `press_`, `Press` or `PRESS_`. Helpers shared
between files of one format start with `pdf_` (DEFLATE), `plz_` (LZ4) or
`pzs_` (zstd); they are exported only because Mettle has no package-private
visibility, and they are not part of the API.

## Output buffers and limits

Every function appends to a `PressBuf` (`data`, `len`, `cap`, `limit`). The
buffer grows by doubling. When `limit` is set, growing past it fails with
`PRESS_E_LIMIT`, so a small hostile input cannot make a huge output.
Decoders reserve room in large steps and then write without checks, which is
what makes them fast.

## DEFLATE decoder

The decoder follows the ideas of libdeflate:

- A 64-bit bit buffer, refilled with one unaligned 8-byte load. The refill
  adds whole bytes and leaves the buffer holding 56 to 63 bits, enough for a
  length code, its extra bits, a distance code and its extra bits together.
- One table lookup per symbol. A table entry holds the codeword length, the
  extra-bit count, the base value and flags (literal, end of block, subtable
  link, invalid), so the decode loop never looks anything else up. Main
  tables are 11 bits (literal/length) and 8 bits (distance), with subtables
  for longer codes.
- A fast loop that runs while at least 8 input bytes and 272 output bytes are
  left, with no other checks. It decodes up to two literals per refill and
  copies matches 8 bytes at a time; for distances under 8 it steps by the
  distance so each 8-byte store still writes correct bytes.
- A careful loop for the last few bytes of input. It reads past the end as
  zero bytes and counts them; if a symbol used any of those bits, the input
  ran out in the middle of it, and the decoder puts its state back to before
  the symbol and returns "need more input". The same trick restarts a block
  header that is cut short.

That last point is what makes streaming simple: the decoder state is a
plain struct (`PdfInflate`), it can stop at any byte, and the streaming
decoder only has to keep the unused input and the last 32 KiB of output.

Invalid codes, distances that reach before the start of the output, and
over-subscribed or incomplete Huffman codes (except the single one-bit code
zlib allows) are all `PRESS_E_DATA`.

## DEFLATE encoder

- Matches come from hash chains over 4-byte prefixes (`head` and `prev`
  tables, 32 KiB window). The lazy levels also keep a 3-byte hash table for
  length-3 matches within 4 KiB, as zlib does.
- Levels 1 to 3 are greedy; 4 to 9 are lazy (a match is held back one byte
  to see if the next position has a longer one). Each level sets the chain
  depth, the "nice" length that stops a search, and how much to search after
  a good match. See `pdf_level` in `src/deflate/deflate.mettle`.
- Symbols are counted as they are made. A block ends when it is full or when
  the newest 512 symbols look different from the rest of the block (the
  block-split test libdeflate uses). Then Huffman codes are built (Moffat and
  Katajainen, then limited to 15 bits), and the block is written as dynamic,
  fixed or stored, whichever is smallest.
- One-shot calls use the caller's buffer in place. The streaming encoder
  copies input into a 256 KiB window that slides down by multiples of 32 KiB,
  so the chain tables keep their slots and only need their positions moved.

## Checksums

CRC-32 reads 16 bytes per step with 16 tables of 256 entries ("slicing by
16"). The tables are computed by the compiler at build time. Adler-32 sums 32
bytes per step and reduces modulo 65521 every 5552 bytes. XXH32 and XXH64 are
the reference algorithms, with streaming states for the frame checksums.

## Testing

- Round trips at every level and around block and window edges.
- The Zstandard reference's golden files (`tests/vectors/zstd`).
- Interop both ways with the gzip, zstd and lz4 tools, and with Python's
  zlib for every zlib strategy (default, filtered, Huffman only, RLE,
  fixed) and for flush points.
- Streaming: random split points and one byte at a time must give the same
  output as one shot.
- Fuzzing: bit flips, random stretches, truncation and pure noise, run in
  debug and release builds. The DEFLATE fuzz test also passes under Mettle's
  `--safe` checks (bounds, use after free).
