# Press API

```mettle
import "press/press";
```

Every function returns an `int32` status: `PRESS_OK` (0) or a negative
`PRESS_E_*` code. `press_error_text(rc)` gives a short message. No function
crashes on bad input; damaged or hostile data gives an error code.

## Status codes

| Code | Meaning |
|---|---|
| `PRESS_OK` | success |
| `PRESS_E_ARG` | a bad argument (null pointer, negative length, bad level) |
| `PRESS_E_DATA` | the input is not valid data for the format |
| `PRESS_E_TRUNC` | the input ended before the data did |
| `PRESS_E_CHECK` | a checksum in the data does not match |
| `PRESS_E_LIMIT` | the output would pass the buffer's limit |
| `PRESS_E_MEM` | out of memory |
| `PRESS_E_UNSUPPORTED` | valid data that needs a feature Press lacks (zlib preset or zstd/LZ4 dictionary) |
| `PRESS_E_STATE` | a stream call made in the wrong order (feed after finish) |

## PressBuf

All output goes into a `PressBuf`, a growable byte buffer. Functions append
to it, so one buffer can collect the output of many calls.

```mettle
export struct PressBuf { data: uint8*; len: int64; cap: int64; limit: int64; }
```

| Function | Does |
|---|---|
| `press_buf_limit(b, n)` | refuse to grow past n bytes (0: no limit). Set it before decoding untrusted data. |
| `press_buf_clear(b)` | empty it, keep the memory |
| `press_buf_free(b)` | free the memory |
| `press_buf_reserve(b, n)` | make room for n more bytes |
| `press_buf_append(b, p, n)`, `press_buf_push(b, v)` | add bytes |

A zeroed `PressBuf` (`var out: PressBuf;`) is empty and has no limit. When a
call fails, the bytes it appended are taken back off.

```mettle
var out: PressBuf;
press_buf_limit(&out, 64 << 20);
var rc: int32 = press_gzip_decompress(p, n, &out);
if (rc != PRESS_OK) { println("gzip: {press_error_text(rc)}"); }
// use out.data[0 .. out.len]
press_buf_free(&out);
```

## One-shot functions

Compression levels: DEFLATE family 0 (store) to 9, -1 for the default 6;
zstd 1 to 22 (0 means 3, negative means 1); LZ4 1 (fast) to 12 (smallest).

| Format | Compress | Decompress |
|---|---|---|
| raw DEFLATE (RFC 1951) | `press_deflate_compress(src, n, level, out)` | `press_deflate_decompress(src, n, out)` |
| zlib (RFC 1950) | `press_zlib_compress(src, n, level, out)` | `press_zlib_decompress(src, n, out)` |
| gzip (RFC 1952) | `press_gzip_compress(src, n, level, out)` | `press_gzip_decompress(src, n, out)` |
| Zstandard (RFC 8878) | `press_zstd_compress(src, n, level, out)` | `press_zstd_decompress(src, n, out)` |
| LZ4 frame | `press_lz4_compress(src, n, level, out)` | `press_lz4_decompress(src, n, out)` |
| LZ4 block | `press_lz4_block_compress(src, n, level, out)` | `press_lz4_block_decompress(src, n, out, max_out)` |

Notes:

- `press_gzip_decompress` reads every member of a multi-member file, and
  allows zero padding after the last one.
- `press_zstd_decompress` reads every frame, skips skippable frames, checks
  the XXH64 checksum when present, and refuses windows above 128 MiB with
  `PRESS_E_LIMIT`. `press_zstd_decompress_ex(src, n, out, window_max)` sets
  another limit.
- `press_lz4_decompress` reads every frame, linked or independent blocks,
  block and content checksums, skippable frames and the legacy format.
- An LZ4 block has no header, so the caller stores its size;
  `press_lz4_block_bound(n)` is the largest a compressed block of n bytes
  can be.
- `press_zstd_compress` and `press_lz4_compress` record the content size and
  a checksum in the frame.

## Streaming

Streams take input in pieces of any size, down to one byte, and append
output to `out` as it is ready. You may use and clear `out` between calls.
Memory use stays small whatever the total size.

Every stream type follows the same pattern:

```mettle
var e: PressZstdEnc;
press_zstd_enc_init(&e, 3);
press_zstd_enc_feed(&e, chunk, n, &out);   // as many times as needed
press_zstd_enc_finish(&e, &out);
press_zstd_enc_free(&e);

var d: PressZstdDec;
press_zstd_dec_init(&d);
press_zstd_dec_feed(&d, chunk, n, &out);   // as many times as needed
rc = press_zstd_dec_finish(&d);            // PRESS_E_TRUNC if cut short
press_zstd_dec_free(&d);
```

| Format | Encoder | Decoder |
|---|---|---|
| DEFLATE, zlib, gzip | `PressDeflateEnc`, `press_deflate_enc_init(e, format, level)` | `PressDeflateDec`, `press_deflate_dec_init(d, format)` |
| Zstandard | `PressZstdEnc`, `press_zstd_enc_init(e, level)` | `PressZstdDec`, `press_zstd_dec_init(d)` |
| LZ4 frame | `PressLz4Enc`, `press_lz4_enc_init(e, level)` | `PressLz4Dec`, `press_lz4_dec_init(d)` |

Then `_feed(x, p, n, out)`, `_finish`, `_free` as above: encoders'
`_finish(e, out)` writes the rest; decoders' `_finish(d)` returns
`PRESS_OK` only if the input ended at the end of a complete stream.

DEFLATE formats are `PRESS_FMT_RAW`, `PRESS_FMT_ZLIB` and `PRESS_FMT_GZIP`;
decoders also take `PRESS_FMT_AUTO` (zlib or gzip, told apart by the first
two bytes, as HTTP `Content-Encoding: deflate` and `gzip` need).
`press_deflate_enc_flush(e, out)` writes out everything fed so far so the
reader can decode it at once (zlib's `Z_SYNC_FLUSH`); use it between
messages on a socket.

Streams honour `out->limit` too. Input after the end of a raw DEFLATE or
zlib stream is ignored.

## Checksums

| Function | Algorithm | Start value |
|---|---|---|
| `press_crc32(crc, p, n)` | CRC-32 (gzip, zip, PNG) | 0 |
| `press_adler32(adler, p, n)` | Adler-32 (zlib) | 1 |
| `press_xxh32(p, n, seed)` | XXH32 (LZ4 frames) | |
| `press_xxh64(p, n, seed)` | XXH64 (zstd frames) | |

CRC-32 and Adler-32 continue from the value you pass, so you can feed data
in pieces. XXH32 and XXH64 have streaming states: `PressXxh32` with
`press_xxh32_init(s, seed)`, `press_xxh32_update(s, p, n)`,
`press_xxh32_digest(s)`, and the same for `PressXxh64`.

## Threads

There is no global mutable state except a table built once on first use,
so every function is safe to call from any thread. A stream object must be
used by one thread at a time.

## What is not there yet

- Dictionaries (zlib preset dictionaries, zstd and LZ4 dictionaries) give
  `PRESS_E_UNSUPPORTED`.
- Brotli.
- DEFLATE levels above 9 (an optimal parser like libdeflate's 10 to 12).
