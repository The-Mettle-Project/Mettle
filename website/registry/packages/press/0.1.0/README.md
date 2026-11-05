# Press

Fast compression for [Mettle](https://github.com/The-Mettle-Project/Mettle),
written in pure Mettle: no C and no shared libraries.

- **DEFLATE** (RFC 1951) with **zlib** (RFC 1950) and **gzip** (RFC 1952)
  framing, levels 0 to 9
- **Zstandard** (RFC 8878), levels 1 to 22
- **LZ4** frames and raw blocks, levels 1 to 12
- **Checksums**: CRC-32, Adler-32, XXH32, XXH64
- One-shot functions and streaming (feed chunks in, get chunks out)
- Plain error codes and an output limit; damaged or hostile input never
  crashes it

```mettle
import "std/io";
import "press/press";

fn main() -> int32 {
  var text: string = "hello hello hello hello";
  var packed: PressBuf;
  var rc: int32 = press_gzip_compress(text.chars, (int64)text.length, 6, &packed);
  if (rc != PRESS_OK) { return 1; }

  var back: PressBuf;
  press_buf_limit(&back, 1 << 20);          // never grow past 1 MiB
  rc = press_gzip_decompress(packed.data, packed.len, &back);
  if (rc != PRESS_OK) { println("gzip: {press_error_text(rc)}"); return 1; }

  press_buf_free(&packed);
  press_buf_free(&back);
  return 0;
}
```

## Install

Add Press to your project's `mbuild.mettle`:

```mettle
fn deps() {
  mb_dep_path("press", "../press");     // or mb_dep("press", "v0.1.0") from a registry
}
```

Then `import "press/press";`. See [examples/gz](examples/gz) for a small gzip
tool built on the streaming API.

## Documentation

- [docs/api.md](docs/api.md): every function, buffers, streaming, errors
- [docs/design.md](docs/design.md): how the decoders and encoders work
- [docs/references.md](docs/references.md): the standards and the
  implementations whose ideas Press uses

## Speed

`mbuild bench FILE...` compares Press with the gzip, zstd and lz4 tools
(one thread each). Numbers below are from an x86-64 Linux machine, release
build, on 6.3 MB of C source code. Sizes are in bytes, speeds in MB/s. The
tools' times include starting the process and reading the file, so their
decompression speeds look lower than their libraries would be.

| | Press size | compress | decompress | tool size | compress | decompress |
|---|---|---|---|---|---|---|
| gzip -1 | 1,364,710 | 204 | 688 | 1,484,535 | 146 | 268 |
| gzip -6 | 1,123,085 | 61 | 837 | 1,121,596 | 50 | 300 |
| gzip -9 | 1,112,789 | 22 | 848 | 1,111,952 | 18 | 307 |
| zstd -1 | 1,355,866 | 279 | 682 | 1,322,628 | 340 | 986 |
| zstd -3 | 1,237,902 | 264 | 725 | 1,221,518 | 301 | 946 |
| zstd -9 | 1,009,778 | 46 | 826 | 999,282 | 70 | 936 |
| zstd -19 | 913,679 | 3.2 | 798 | 859,019 | 3.1 | 1,049 |
| lz4 -1 | 1,951,942 | 584 | 1,946 | 1,951,934 | 459 | 824 |
| lz4 -9 | 1,373,923 | 23 | 1,935 | 1,354,079 | 38 | 965 |

In short: the DEFLATE family is faster than gzip at the same size, and LZ4
level 1 matches the reference. Zstandard is within a few percent of the
reference's size up to level 9 but slower; its highest levels and its
decoder are the main places left to tune. Much of the remaining gap is
code generation in the young Mettle compiler (see design.md).

## Testing

```bash
mbuild test       # debug build
mbuild test -r    # release build
```

The tests cover round trips at every level, streaming split at random
points and one byte at a time, output limits, the Zstandard reference's own
golden files, interop in both directions with the gzip, zstd and lz4 tools
and with Python's zlib, and fuzzing (bit flips, random stretches,
truncation and noise) in both debug and release builds. Tests that need a
tool skip that part with a note when it is not installed.

## Not supported yet

Dictionaries (zlib preset dictionaries, zstd and LZ4 dictionaries), Brotli,
and DEFLATE levels above 9.

## License

Apache License 2.0. See [LICENSE](LICENSE).
