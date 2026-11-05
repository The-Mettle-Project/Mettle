# Standards and references

What canvas follows, which ideas came from where, and why.

## Standards

| Area | Standard | Where it is used |
|---|---|---|
| Path data | SVG 2, section 9.3.9 "The grammar for path data" (and SVG 1.1 section 8.3.9) | `geom/svg_path`: every command, implicit repeats, number forms like `-.5.5` and `1e-3`, arc flags without separators, and "draw up to the error" |
| Elliptical arcs | SVG 1.1 appendix F.6 "Elliptical arc implementation notes" (F.6.5 conversion, F.6.6 out-of-range radii) | `canvas_svg_arc_to` |
| Strokes | SVG 2, section 13.5: `stroke-linejoin` (miter, round, bevel), `stroke-linecap`, `stroke-miterlimit`, `stroke-dasharray` and `stroke-dashoffset` | `geom/stroke`, `geom/dash` |
| Canvas paths | HTML Living Standard, the 2D context: `arc()`, `ellipse()`, `arcTo()`, the current-point rules, `createRadialGradient()` (two-point conical), `createConicGradient()`, `globalAlpha` | `geom/path`, `paint/gradient` |
| Compositing | W3C Compositing and Blending Level 1: the Porter-Duff operators, the separable and non-separable blend modes (with ClipColor, SetLum, SetSat), and "shape" multiplying source alpha | `paint/blend`, `draw/blit` |
| Gradient colors | CSS Images Level 4, section 3.5.3: colors between stops are mixed premultiplied | `canvas_gradient_lut` |
| Text | OpenType, through font (GSUB/GPOS shaping, glyf and CFF outlines) | `draw/text` |
| Image files | PNG and the rest, through Etch | `io/image` |

## Techniques and their sources

**Coverage (anti-aliasing).** Each line adds the exact area it covers to
cells, and a running sum along a row gives the coverage. This is the
method of FreeType's "gray" rasteriser, font-rs and stb_truetype 2, and
the one MythicUI's `mythic_raster_path` uses; canvas starts from
MythicUI's code. Non-zero coverage is `min(1, |sum|)`; even-odd folds the
sum back into [0, 1], as Blend2D does. Exact areas give better edges than
supersampling (cairo samples 15 rows a pixel, Skia 4 or 16) at lower cost.

**Overlapping paths.** The running sum is exact only while no part of a
pixel is covered twice. Where a path overlaps itself (strokes crossing,
overlapping contours) a pixel holding a doubly covered part and an empty
part comes out too dark. FreeType meets the same problem with overlapping
glyphs and supersamples them 4x4 (`FT_OUTLINE_OVERLAP`, FreeType 2.10.3).
canvas does it only where needed: a row whose sum passes 1 has overlap,
so it and the rows next to it are done again with 4 sub-rows, each
clamped on its own. Rows without overlap stay exact and fast. With this
the crossing of two strokes matches cairo to the level.

**Bands.** Edges are sorted by their top and filled 16 rows at a time, so
memory stays small for big pixmaps (Blend2D works in bands the same way).
Runs of cells no edge touches (the inside of a shape) are filled in one
step.

**Flattening.** Curves become lines with the count from Wang's formula,
`n = sqrt(d(d-1)/8 * M / tol)` (as in Skia's `GrWangsFormula`), with a
tolerance of 0.1 device pixels by default (cairo's default too).

**Strokes.** The stroker follows Skia's `SkStroke`: each contour gives an
outer and an inner offset side, joins go on the outer side, and a sharp
inner corner goes through the corner point so the overlap fills solid
with non-zero. Where the two inner offset lines cross inside both
segments, canvas uses the crossing instead: the outline then has no
overlap on its inner edge, which keeps the exact-area rasteriser exact (a
ring stroked at width 8 has the right area to 0.01 pixel). Points inside a
flattened curve get round joins, so wide curves stay smooth. Strokes are
made in user space and then transformed, like SVG and the HTML canvas
(and tiny-skia).

**Dashes.** Dashing works on the flattened polyline, flattened 10 times
finer so dash ends land within 0.1 pixel of the true arc length. A dash
running over the start of a closed contour joins the first one, as in
Skia's `SkDashPath`.

**Gradients.** Colors come from a 1024-entry premultiplied table built
once per gradient and opacity (Blend2D uses such tables). The radial
gradient solves the HTML canvas two-circle equation per pixel, with the
square root started from the last pixel's root (two Newton steps), which
is as fast as a hardware square root and stays pure Mettle. The sweep
angle uses the polynomial from Skia's raster pipeline
(`xy_to_unit_angle`).

**Blending.** Source-over runs in 8-bit integer math, two channels per
32-bit word. Other modes run in floats with the W3C formulas, then mix
with the coverage: `lerp(dst, blend(src, dst), coverage)`, as Skia does,
so pixels outside the shape stay untouched. For `plus` (which clamps) the
coverage scales the source first, as the W3C model says; the result
matches cairo's `ADD`.

**Patterns.** Nearest and bilinear sampling with pad, repeat, reflect and
decal, Skia's four tile modes.

## Proof

- `tests/ref_test.mettle` draws the 57 scenes of `tests/data/scenes.txt`
  and compares them with cairo 1.18.4 (`tools/refs.py` makes the
  references): fills, both rules, arcs, curves, slivers, joins, caps,
  miter limits, dashes, transforms, all gradients and spreads, clips,
  opacity and every blend mode cairo has. Most pixels match within a
  few levels; the worst single difference is 63 of 255 (an anti-aliased
  edge pixel).
- `tests/api_test.mettle` checks exact areas (rectangles, triangles,
  circles, rings, caps, joins, dashes, crossings, hairlines), SVG path
  parsing and its errors, gradient mixing, blend formulas, clips,
  patterns and bad input.
- `tests/text_test.mettle` checks glyphs against font's own rasteriser
  (within 32 levels; the two flatten curves differently), ligatures and
  kerning.
- `tests/fuzz_test.mettle` feeds random and broken SVG path data and
  random drawing (NaN, huge and tiny numbers, singular transforms, odd
  strokes and dashes, every paint and blend mode, clips) to random
  pixmaps. 2000 rounds pass in debug and release, and 1000 rounds (25
  batches of `mbuild fuzz`) in the bounds-checked `safe` profile. It found two hangs
  (arcs with huge angles, dash offsets too big to place) and two cases of
  runaway work (dashes along lines far off the pixmap, which are now
  clipped to the visible area as Skia does, and absurd strokes, which now
  stop at `CANVAS_MAX_OUTLINE` points or a million dashes with
  `CANVAS_E_LIMIT`).
- Every test passes in debug and release.

## Benchmarks

`mbuild bench` times the same work in canvas and in cairo (through
pycairo; Python adds little since each call does a lot of drawing). The
reference we would have liked, tiny-skia, needs a Rust toolchain that was
not available, so cairo, a mature CPU rasteriser of the same kind, is the
reference here. See the README for numbers.

## Known differences

- Gradients with stops of different alpha mix premultiplied (CSS); cairo
  and Skia mix straight colors, so such gradients differ slightly.
- A dash that starts exactly on a corner gets no join (as in Skia); cairo
  draws one.
- Clips multiply coverage (as Skia and tiny-skia do), so an anti-aliased
  clip edge meeting an anti-aliased shape edge in one pixel can differ a
  little from an exact intersection.

## Licenses

canvas is Apache-2.0. MythicUI's rasteriser is the user's own code.
font-rs is Apache-2.0/MIT, stb_truetype public domain, Skia BSD-3,
Blend2D Zlib: ideas were taken, no code copied. FreeType (FTL or GPLv2)
and cairo (LGPL or MPL) were used only as descriptions and as a reference
renderer; none of their code is in canvas. The reference images in
`tests/data/ref` are cairo's output.
