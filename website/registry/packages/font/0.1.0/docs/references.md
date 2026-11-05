# Standards and references

## Standards followed

| Area | Standard |
|---|---|
| Font file, tables, cmap, metrics, glyf, loca, kern, GSUB, GPOS, GDEF | OpenType 1.9.1 (Microsoft), the same text as ISO/IEC 14496-22 (Open Font Format) |
| CFF outlines | Adobe Technical Note #5176, *The Compact Font Format Specification*; #5177, *The Type 2 Charstring Format* |
| Apple `kern` version 1 | Apple *TrueType Reference Manual*, "kern" |
| UTF-8 | RFC 3629; Unicode Standard chapter 3.9 (U+FFFD for each maximal ill-formed subsequence) |
| Line breaking | Unicode Standard Annex #14, *Unicode Line Breaking Algorithm* (Unicode 18.0 data) |
| Normalisation | Unicode Standard Annex #15, *Unicode Normalization Forms* |
| Character properties | Unicode Character Database: UnicodeData, DerivedCoreProperties, GraphemeBreakProperty, EastAsianWidth, emoji-data |

## Reference implementations studied

Each idea below was re-implemented from the published description or
source; no code was copied. Licences of the projects named are noted.

| Project (licence) | What we took |
|---|---|
| **HarfBuzz** (MIT) | The shaping pipeline and its exact behaviour: normalisation in "composed diacritics" mode (`hb-ot-shape-normalize`), grapheme cluster forming, which characters are default ignorable and how they are hidden, ZWJ/ZWNJ handling while matching, lookup application order, ligature cluster merging, mark attachment and `propagate_attachment_offsets`, the legacy kern split (half on each glyph), and fallback mark positioning for fonts without GPOS (`hb-ot-shape-fallback`). Output is checked glyph for glyph against HarfBuzz. |
| **FreeType** (FTL, also GPL-2) | Behaviour only, from its documentation and output: the TrueType origin at phantom point 1 (`xMin - lsb`), composite glyph offsets (unscaled unless `SCALED_COMPONENT_OFFSET`), CFF/CID private dictionaries per font DICT. Used as the speed reference. |
| **font-rs** by Raph Levien (Apache-2.0/MIT) and **fontdue** (MIT/Apache-2.0) | The signed-area accumulation rasteriser: each line adds exact coverage into a float buffer and a running sum gives the pixels. |
| **stb_truetype** by Sean Barrett (public domain/MIT) | API shape (pixel boxes with sub-pixel shift, scale-for-pixel-height), a small bounds-checked reader over borrowed font bytes, and the idea of SDF output from outlines. |
| **msdfgen** by Viktor Chlumsky (MIT) | Multi-channel distance fields: `edgeColoringSimple` with a 3 radian corner threshold and its colour switching, signed pseudo-distance with end tangents, nearest-edge choice by distance then orthogonality, and `msdfErrorCorrection` clash detection. |
| **stb_rect_pack** (public domain/MIT) and **fontstash** (zlib) | Skyline bottom-left packing for the glyph atlas, and caching glyphs by font, size and sub-pixel step. |
| **fontTools** (MIT) | Reference for parsing: metrics, cmap and every glyph outline are compared with it. |
| Wang's formula (curve flattening) | Number of line pieces needed to keep a flattened Bezier curve within a set distance of the true curve. |

## Test data from standards bodies

- `tests/data/LineBreakTest.txt` and `tests/data/NormalizationTest.txt`
  are the official Unicode conformance files (Unicode licence, kept with
  their headers).
- `tests/data/shape_expected.txt` is HarfBuzz's output for
  `tests/data/shape_texts.txt`, made by `tools/compare_harfbuzz.py`.
