# erasure

Erasure coding for [Mettle](https://github.com/Lex-Dellimere), written in
pure Mettle. Store data as k data shards plus m parity shards; any k of
them give it all back.

- Reed-Solomon over GF(2^8): any k + m up to 256, Vandermonde or Cauchy
  matrix. Shards are byte-for-byte the same as
  [klauspost/reedsolomon](https://github.com/klauspost/reedsolomon) and
  Backblaze JavaReedSolomon.
- Reed-Solomon over GF(2^16) for wide stripes, up to 65536 shards.
- Rebuild any lost shards, or only the lost data shards.
- Streaming split, encode, verify, rebuild and join for data larger than
  memory.
- Fast multiply on x86-64: AVX-512 + GFNI, AVX2 + GFNI, AVX2 split tables
  (the PSHUFB method), and a portable path. The best one is picked at run
  time.
- Simple XOR parity (RAID 5 style).
- Never crashes on bad input: every call returns a status code.

## Example

```mettle
import "erasure/erasure";

var c: ErasureRS;
erasure_rs_new(&c, 4, 2);                  // 4 data + 2 parity shards
// shards: 6 pointers to `size` bytes each; fill the first 4 with data.
erasure_rs_encode(&c, shards, size);
present[1] = false;                        // lose any two shards...
present[4] = false;
erasure_rs_reconstruct(&c, shards, present, size);   // ...and get them back
erasure_rs_free(&c);
```

`examples/shards.mettle` stores a file as 6 + 3 shard files, deletes
three, and rebuilds the file:

```
mbuild run shards FILE
```

## Build and test

```
mbuild build
mbuild test          # debug
mbuild test -r       # release
mbuild bench
```

## Speed

`mbuild bench`, one thread, 1 MiB shards, MB/s of data shards encoded, on
an AMD Ryzen 7 8845HS. klauspost/reedsolomon v1.14.2 was run on the same
machine with `GOMAXPROCS=1`.

| Shape | erasure (AVX-512 + GFNI) | erasure (AVX2) | erasure (portable) | klauspost |
|---|---|---|---|---|
| 4+2 | 60900 | 25700 | 890 | 60700 |
| 10+4 | 24300 | 9600 | 470 | 40700 |
| 12+4 | 21800 | 9100 | 470 | 3300 |
| 50+20 | 3600 | 1900 | 90 | 540 |

Rebuild speed is about the same as encode speed. The GF(2^16) codec runs
the portable path: about 130 MB/s at 100+20.

## Docs

- [docs/api.md](docs/api.md): every function
- [docs/design.md](docs/design.md): how it works, limits
- [docs/references.md](docs/references.md): papers and the implementations
  it learns from

## License

Apache-2.0. See [LICENSE](LICENSE).
