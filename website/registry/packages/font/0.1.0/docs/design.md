# Design

## Modules

```
src/font.mettle              one import for everything
src/core/vec.mettle          FontVec<T>, a growable array
src/core/utf8.mettle         UTF-8 decode, encode, validate
src/sfnt/face.mettle         FontFace and bounds-checked big-endian readers
src/sfnt/init.mettle         table directory, collections, head/hhea/maxp/OS/2/post
src/sfnt/cmap.mettle         cmap formats 0, 4, 6, 10, 12, 13, 14
src/sfnt/metrics.mettle      hmtx/vmtx, scales, line metrics, name table
src/outline/path.mettle      FontPath (move, line, quad, cubic, close)
src/outline/glyf.mettle      TrueType outlines, simple and composite
src/outline/cff.mettle       CFF tables and Type 2 charstrings (CID fonts too)
src/outline/outline.mettle   one call for either outline format
src/raster/raster.mettle     anti-aliased rasteriser
src/raster/sdf.mettle        SDF and MSDF
src/atlas/atlas.mettle       glyph atlas with skyline packing
src/layout/buffer.mettle     FontBuffer, the glyph run
src/layout/otl.mettle        OpenType layout common tables and context matching
src/layout/gsub.mettle       glyph substitution
src/layout/gpos.mettle       glyph positioning
src/layout/kern.mettle       legacy kern table
src/text/unicode_data.mettle generated Unicode properties (tools/gen_unicode.py)
src/text/normalize.mettle    NFC and NFD
src/text/linebreak_data.mettle generated line break classes (tools/gen_linebreak.py)
src/text/breaks.mettle       UAX #14 line breaking
src/text/shape.mettle        the shaper
src/text/layout.mettle       lines, wrapping, alignment, hit testing
```

Each module only imports modules listed above it.

## Reading fonts safely

A `FontFace` keeps offsets into the caller's bytes, not copies. Every read
goes through `font_u8/u16/i16/u24/u32`, which return 0 past the end, so a
damaged table yields wrong numbers but never a bad memory access. Loops
whose counts come from the font are capped by the table or file size, and
recursive structures (composite glyphs, CFF subroutines, nested lookups)
have depth limits. GSUB and GPOS have a work budget so a hostile font
cannot make shaping run forever, and the buffer may grow at most eightfold.

## Shaping pipeline

1. Decode UTF-8; each character keeps its byte offset as its cluster.
2. Set Unicode properties and merge each grapheme into one cluster.
3. Normalise like HarfBuzz: decompose what the font cannot show, sort marks
   by combining class, recompose pairs the font has a glyph for.
4. Map to glyphs (variation selectors pick variants and stay as hidden
   glyphs); set GDEF classes, or guess them from Unicode when there is no
   GDEF.
5. GSUB features `ccmp locl rlig liga clig calt rclt` plus user features.
6. Advances from `hmtx`; GPOS features `kern mark mkmk curs dist abvm blwm`,
   or the legacy `kern` table, or fallback mark placement when the font has
   no GPOS.
7. Zero mark advances, hide default ignorables, and turn attachments into
   final offsets.

Not done yet: complex-script shaping (Arabic joining forms, Indic and other
syllable reordering), bidirectional reordering, variable fonts (fvar,
gvar, CFF2), colour and bitmap glyphs, hinting.

## Rasterising

The rasteriser accumulates exact signed area per pixel (font-rs). Curves
are flattened with Wang's formula so no piece is more than 0.02 px off the
curve. SDF uses true distance to finely flattened edges and the non-zero
fill for the sign; MSDF follows msdfgen (see `references.md`).

## Testing

`mbuild test` and `mbuild test -r` run ten test programs: UTF-8, font
parsing, rasteriser accuracy, SDF/MSDF, atlas, line breaking (official
LineBreakTest.txt), normalisation (official NormalizationTest.txt),
shaping (HarfBuzz output), layout, and a fuzzer that damages real fonts
and runs every entry point on them.

`mbuild -p safe test` builds the same programs with every memory access
bounds checked. Two Mettle compiler problems affect that profile today:
a false use-after-free when an index is made from a hashed pointer
(report 124, avoided in the atlas), and a wrong result in `--safe
--release` line breaking (report 125, 7 of 19346 official lines; debug,
release and plain `--safe` builds are correct).

Comparison tools, which need Python and the system HarfBuzz/FreeType:

- `tools/compare_fonttools.py FONT...`: metrics, cmap and outlines against fontTools.
- `tools/compare_harfbuzz.py --file FONT tests/data/shape_texts.txt`: shaping against HarfBuzz.
- `tools/bench_reference.py`: FreeType and HarfBuzz speed for the `mbuild bench` work.
