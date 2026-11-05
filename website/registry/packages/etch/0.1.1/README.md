# Etch

Images for [Mettle](https://github.com/The-Mettle-Project/Mettle), written
in pure Mettle: no C and no shared libraries.

| Format | Decode | Encode |
|---|---|---|
| PNG | every color type and bit depth, palettes, transparency, interlacing | gray, gray+alpha, RGB, RGBA at 8 or 16 bits |
| APNG | every frame, composed (offsets, blending, disposal) | |
| JPEG | baseline and progressive, gray, YCbCr, RGB, CMYK, any subsampling | baseline, 4:2:0 or 4:4:4, gray |
| QOI | RGB, RGBA | RGB, RGBA |
| BMP | 1 to 32 bits, palettes, bit fields, RLE4, RLE8 | 24-bit, 32-bit with alpha |
| TGA | color-mapped, true-color, gray, raw or RLE | gray, RGB, RGBA, raw or RLE |
| GIF | every frame, transparency, interlacing, disposal | |
| Radiance HDR | flat and run-length | run-length |
| PGM, PPM, PAM | 8 and 16-bit | 8 and 16-bit |

- **Pixel tools:** one image type (gray, gray+alpha, RGB, RGBA; 8-bit,
  16-bit or float), conversion between any formats, flip, rotate, crop
  (with or without a copy), blit, fill, resize (nearest, box, bilinear,
  Catmull-Rom, Mitchell, Lanczos), premultiplied alpha, sRGB and linear
  light.
- **Ready for the GPU:** the OpenGL formats and unpack settings for any
  image, for [Glint](https://github.com/Lex-Dellimere/Glint).
- **Safe:** size limits are checked before anything is allocated, every
  decoder is fuzzed under Mettle's `--safe` bounds checks, and broken files
  give an error code, never a crash.
- **Correct:** all of the PngSuite, and JPEGs that decode bit for bit the
  same as libjpeg-turbo's `djpeg`.

```mettle
import "std/io";
import "etch/etch";

fn main() -> int32 {
  var img: EtchImage;
  var rc: int32 = etch_load("photo.jpg", &img, (EtchOpts*)0);
  if (rc != ETCH_OK) { println("load: {etch_error_text(rc)}"); return 1; }

  var small: EtchImage;
  etch_resize(&img, img.width / 4, img.height / 4, ETCH_FILTER_LANCZOS, &small);
  rc = etch_save("thumb.png", &small);      // format from the extension

  etch_image_free(&small);
  etch_image_free(&img);
  return 0;
}
```

## Install

Etch uses [Press](https://github.com/Lex-Dellimere/Press) for PNG's
DEFLATE. Until both are on the registry, clone them side by side:

```bash
git clone https://github.com/Lex-Dellimere/Press press
```

```bash
git clone https://github.com/Lex-Dellimere/Etch etch
```

and add Etch to your project's `mbuild.mettle`:

```mettle
fn deps() {
  mb_dep_path("etch", "../etch");
}
```

Then `import "etch/etch";`. `mbuild new app --from etch` starts a project
that loads, resizes and saves an image.

## Examples

- [examples/convert](examples/convert): `mbuild run convert -- in.jpg out.png`
  converts between any formats.
- [examples/info](examples/info): `mbuild run info -- a.png b.gif` prints
  what Etch sees in files, frame by frame.

## Documentation

- [docs/api.md](docs/api.md): every function, the image type, options and
  limits, animations, buffers
- [docs/formats.md](docs/formats.md): what each decoder reads and each
  encoder writes
- [docs/textures.md](docs/textures.md): images as OpenGL textures
- [docs/references.md](docs/references.md): the standards, the libraries
  whose methods Etch uses, and how it is tested

## Tests

```bash
mbuild test
```

```bash
mbuild test -r
```

```bash
mbuild fuzz 300
```

`mbuild test` runs the PngSuite, about 60 files made by libjpeg-turbo,
ImageMagick, Pillow and ffmpeg (checked pixel for pixel, frame by frame),
round trips through every encoder, and a short fuzz run. `mbuild fuzz N`
runs N rounds of mutations per test file under `--safe`, where every memory
access is bounds checked. `tools/make_testdata.sh` remakes the test files.

## Speed

`mbuild bench` makes 1920x1080 test files, times Etch (release build),
then builds `tools/refbench.c` against the reference C libraries and times
them on the same files. One thread each, best of 5, in megapixels per
second (higher is better). Numbers are from an AMD Ryzen 7 8845HS on Linux,
GCC -O2 for the C libraries; the system's libjpeg-turbo uses SIMD.

| Work | Etch | Fastest reference | Others |
|---|---|---|---|
| PNG decode, RGB8 | 59 | 95 libpng | 94 stb_image, 93 libspng |
| PNG decode, RGBA8 | 51 | 81 libspng | |
| PNG encode, level 6 | 3.2 (1523 KiB) | 2.0 libpng (1529 KiB) | |
| JPEG decode, 4:2:0 | 81 | 608 libjpeg-turbo | 268 stb_image |
| JPEG decode, progressive | 53 | 211 libjpeg-turbo | 140 stb_image |
| JPEG encode, quality 90 | 35 (195 KiB) | 636 libjpeg-turbo (199 KiB) | |
| QOI decode | 176 | 235 qoi.h | |
| QOI encode | 103 | 210 qoi.h | |
| BMP decode | 879 | 431 stb_image | |
| TGA decode (RLE) | 193 | 228 stb_image | |
| GIF decode | 136 | 152 stb_image | |

Etch also resizes this image to a quarter (Lanczos) at 74 megapixels per
second and converts RGB8 to RGBA8 at 880.

In short: PNG encoding is faster than libpng at the same size (Press's
DEFLATE is fast), BMP, TGA, GIF and QOI are close to the C references, and
PNG decoding is at about 60% of libpng. Inflating takes as long as zlib
does; the rest is unfiltering. JPEG is where the gap is widest: decoding is
3 times slower than stb_image and much slower than libjpeg-turbo's
hand-written SIMD. Most of that gap is code generation in the young Mettle
compiler (registers spilled in hot loops, no automatic vectorizing of the
IDCT and color conversion), so it should shrink as the compiler improves.

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
