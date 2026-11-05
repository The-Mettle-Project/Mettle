# References

What Etch follows, where its methods come from, and how it is checked.

## Standards

| Area | Specification |
|---|---|
| PNG | ISO/IEC 15948:2004, and the W3C PNG Specification (Third Edition, 2025), which also defines APNG (acTL, fcTL, fdAT) |
| zlib and DEFLATE (inside PNG) | RFC 1950 and RFC 1951, implemented by [Press](https://github.com/Lex-Dellimere/Press) |
| JPEG | ITU-T T.81 / ISO/IEC 10918-1 (baseline, extended and progressive Huffman coding) |
| JFIF | ITU-T T.871 / ISO/IEC 10918-5 (YCbCr conversion, APP0) |
| Adobe JPEG color | Adobe Technical Note 5116 (the APP14 "Adobe" segment and its color transform flag) |
| QOI | The QOI Specification, version 1.0 (qoiformat.org) |
| BMP | Microsoft's BITMAPCOREHEADER, BITMAPINFOHEADER, BITMAPV4HEADER and BITMAPV5HEADER documentation |
| TGA | Truevision TGA File Format Specification, version 2.0 |
| GIF | CompuServe GIF89a Specification (1990), and the NETSCAPE2.0 application extension for loop counts |
| Radiance HDR | Greg Ward, "Real Pixels", Graphics Gems II (1991), and the Radiance file format notes (RGBE, run-length scanlines) |
| PNM | The Netpbm format pages for PGM, PPM and PAM |
| sRGB | IEC 61966-2-1:1999 (the transfer curve in ops/color) |
| Gray from color | ITU-R BT.601 luma weights |
| OpenGL formats | The OpenGL 4.6 Core Profile specification (internal formats and pixel transfer) |

## Where the methods come from

No code was copied; the methods below were rewritten in Mettle. None of
these projects is GPL.

- **libjpeg / libjpeg-turbo** (IJG license, BSD-3-Clause). The decoder's
  integer IDCT is the "islow" method of `jidctint.c` (Loeffler, Ligtenberg
  and Moschytz, with the same 13-bit constants and rounding). Chroma
  upsampling uses the "fancy" triangle filters of `jdsample.c` for 2:1
  factors (h2v1, h1v2, h2v2) and plain replication for the rest, and the
  YCbCr to RGB tables are those of `jdcolor.c`. Matching these exactly is why
  Etch's output equals `djpeg`'s. The encoder's quality scaling
  (`jpeg_quality_scaling`) and the standard tables of ITU-T T.81 Annex K are
  the same as libjpeg's.
- **stb_image** (public domain or MIT). The JPEG Huffman decoder's 9-bit
  fast lookup table with a slow path through `maxcode`, the 32-bit bit
  buffer that stops at markers, the progressive refinement steps, and the
  treatment of inverted Adobe CMYK. Its broad, forgiving approach to BMP and
  TGA variants guided what Etch accepts.
- **stb_image_write** (public domain or MIT). The encoder's float AAN
  forward DCT and folding the DCT scale factors into the quantizer.
- **libspng and libpng** (BSD-2-Clause, libpng license). Chunk policy: check
  every CRC, refuse a damaged needed chunk, drop a damaged extra one,
  refuse unknown needed chunks. The encoder's filter choice is libpng's
  heuristic: per row, the filter with the smallest sum of absolute
  differences.
- **The QOI reference, qoi.h** (MIT). Encoder and decoder follow it step for
  step, including the index update after every chunk.
- **Pillow's resampling and stb_image_resize** (MIT-CMU, public domain).
  Separable resizing with precomputed weights per output pixel, the filter
  support widened by the shrink factor so every source pixel counts, and
  premultiplied alpha during filtering. Kernels: Catmull-Rom; Mitchell and
  Netravali, "Reconstruction Filters in Computer Graphics" (1988), with
  B = C = 1/3; Lanczos with three lobes.
- **GIF LZW** follows the GIF89a specification's appendix, with the usual
  prefix/suffix tables and the KwKwK case; disposal and composition follow
  what browsers do (background clears to transparent).
- **Bit widening** of 5 and 6-bit channels by repeating their bits, as
  libpng and ImageMagick do.

## How it is checked

- **Conformance:** the PngSuite by Willem van Schaik (all 176 files; the 14
  broken ones must be refused). Expected pixels come from ImageMagick.
- **Reference tools:** JPEGs made by libjpeg-turbo's `cjpeg` in every
  sampling mode, progressive, with restarts, gray, RGB and CMYK must decode
  to exactly what `djpeg` gives. BMP, TGA, QOI, GIF and HDR files made by
  ImageMagick and Pillow, and APNGs made by ffmpeg and by
  `tools/apng.py`, must match ImageMagick (GIF, with `-coalesce`) or Pillow
  (APNG) frame for frame. `tools/refs.py` makes the expected hashes and
  `tools/make_testdata.sh` remakes the files.
- **Round trips** for every encoder, and a PSNR bound for JPEG (the encoder
  matches `cjpeg`'s quality at the same setting).
- **Fuzzing:** `tests/fuzz_test.mettle` mutates every test file and feeds the
  result to every decoder; `mbuild fuzz N` runs it under `--safe`, where
  every memory access is bounds checked.
- **Benchmarks** against libspng, libpng, stb_image, libjpeg-turbo and
  qoi.h: `mbuild bench` (see the README).
