# font

Fonts and text for [Mettle](https://github.com/Lex-Dellimere), written in
pure Mettle with no C dependencies.

- **Fonts:** TrueType (`glyf`) and OpenType CFF (including CID-keyed CJK
  fonts), single files and collections (`.ttc`/`.otc`); metrics, names,
  every common `cmap` format and variation sequences.
- **Glyphs:** outlines as paths, anti-aliased bitmaps with sub-pixel
  positioning, signed distance fields, and multi-channel distance fields
  (MSDF) for sharp text at any scale.
- **Glyph atlas:** skyline-packed texture cache for GPU text, coverage,
  SDF or MSDF.
- **Shaping:** GSUB (ligatures, contextual alternates and every other
  lookup type), GPOS (kerning, mark and cursive attachment), the legacy
  `kern` table, Unicode normalisation and cluster handling done the way
  HarfBuzz does them.
- **Layout:** UAX #14 line breaking, wrapping, alignment, measuring, hit
  testing and caret positions.
- **Unicode:** UTF-8, NFC/NFD (UAX #15), general category, combining
  class, line break classes.

## Example

```mettle
import "std/io";
import "font/font";

fn draw(data: uint8*, len: int64) {
  var face: FontFace;
  if (font_init(&face, data, len, 0) != FONT_OK) { return; }

  var o: FontLayoutOpts;
  o.size = 18.0;          // pixels per em
  o.max_width = 320.0;    // wrap here
  var text: string = "The office affluent fjord, waving AVATARs.";
  var l: FontLayout;
  font_layout(&face, text.chars, (int64)text.length, &o, &l);

  var bmp: FontBitmap;
  for i: int64 in 0..l.glyphs.len {
    var g: FontPlacedGlyph = l.glyphs.data[i];
    font_glyph_bitmap(&face, g.id, o.size, &bmp);
    // blend bmp.data (bmp.w x bmp.h) at (g.x + bmp.x, g.y + bmp.y)
    font_bitmap_free(&bmp);
  }
  font_layout_free(&l);
}
```

See [docs/api.md](docs/api.md) for the whole API and
[examples/](examples) for programs that render text to an image and
build an atlas.

## Build and test

```
mbuild build
mbuild test          # debug
mbuild test -r       # release
mbuild bench         # speed (see below)
```

The tests use fonts found on most Linux systems (DejaVu, Linux
Libertine, Gentium); tests whose font is missing say so and skip.

## How it is checked

| Check | Result |
|---|---|
| Metrics, cmap and every glyph outline against fontTools (`tools/compare_fonttools.py`) | 302 of 302 system fonts identical, including CFF CID collections |
| Shaping against HarfBuzz's OpenType shaper (`tools/compare_harfbuzz.py`) | 300 of 300 fonts give the same glyphs, clusters, advances and offsets on the test texts |
| Unicode LineBreakTest.txt (UAX #14) | 19346 of 19346 lines |
| Unicode NormalizationTest.txt (UAX #15) | 20171 of 20171 lines |
| Rasteriser against an exact-area reference | under 1.5/255 mean error per pixel |
| SDF and 4x-scaled MSDF against the rasteriser | inside/outside agree on over 99.8% of pixels |
| Fuzzing: damaged fonts through every entry point | no crash, also under `--safe` bounds checking |

## Speed

One thread on an AMD Ryzen 7 8845HS; DejaVu Sans (TrueType) / Linux
Libertine (CFF). `mbuild bench` measures font; `tools/bench_reference.py`
runs the same work through the system FreeType 2 and HarfBuzz (called from
Python, which adds about a microsecond per glyph to their numbers).

| Work | font | Reference |
|---|---|---|
| Rasterise ASCII at 16 px (glyphs/s) | 289 k / 133 k | FreeType 771 k / 254 k |
| Rasterise ASCII at 64 px (glyphs/s) | 106 k / 68 k | FreeType 287 k / 131 k |
| SDF at 32 px (glyphs/s) | 2 318 / 1 286 | FreeType SDF 497 / 167 |
| MSDF at 32 px (glyphs/s) | 1 021 / 764 | (msdfgen not installed) |
| Shape English text (MB/s) | 4.1 / 3.5 | HarfBuzz 21.8 / 16.5 |
| Shape and wrap into lines (MB/s) | 2.8 / 2.5 | |

Rasterising is about 2.5x slower than FreeType's C code, SDF is about 5x
faster than FreeType's (which uses a wider spread and exact curve
distances), and shaping is about 5x slower than HarfBuzz.

## Not done yet

Complex-script shaping (Arabic joining, Indic reordering), bidirectional
text, variable fonts, colour and bitmap glyphs, and hinting. See
[docs/design.md](docs/design.md).

## Standards and credits

Built from the OpenType 1.9.1 specification, Adobe's CFF and Type 2
charstring notes, and Unicode UAX #14 and #15, with techniques from
HarfBuzz, FreeType, stb_truetype, font-rs, fontdue, msdfgen and
stb_rect_pack. Details in [docs/references.md](docs/references.md).

## License

Apache-2.0. See [LICENSE](LICENSE).
