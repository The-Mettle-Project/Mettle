# API guide

Everything is reached with one import:

```mettle
import "font/font";
```

All names start with `font_`, `Font` or `FONT_`. Sizes are in pixels per
em (`px`); font-unit values are turned into pixels with
`font_scale_em(face, px)`. Fonts are read in place: the bytes you pass to
`font_init` must stay alive while the face is used. Nothing here crashes
on damaged fonts; bad data gives empty glyphs or `false`.

## Opening a font

| Function | What it does |
|---|---|
| `font_count(data, len) -> int32` | Faces in the file: 1, the collection size, or 0 when not a font. |
| `font_init(&face, data, len, index) -> int32` | Opens a face. `FONT_OK`, or `FONT_E_FORMAT`, `FONT_E_MISSING`, `FONT_E_INDEX`. |
| `font_name(&face, FONT_NAME_FULL, buf, cap) -> int64` | A name from the `name` table as UTF-8. |

`FontFace` fields you may read: `upem`, `num_glyphs`, `outline`
(`FONT_OUTLINE_GLYF` or `FONT_OUTLINE_CFF`), `ascent`, `descent`,
`line_gap`, `typo_*`, `win_*`, `cap_height`, `x_height`,
`underline_pos`, `underline_thick`, `strike_pos`, `strike_thick`,
`weight`, `italic_angle`, `mono`.

## Characters, metrics and kerning

| Function | What it does |
|---|---|
| `font_glyph_index(&face, cp)` | Glyph for a code point (0 when missing). |
| `font_glyph_index_vs(&face, cp, vs)` | Same, for a variation sequence (cmap format 14). |
| `font_advance`, `font_lsb`, `font_advance_v` | Horizontal advance, left side bearing, vertical advance (font units). |
| `font_line_metrics(&face, &asc, &desc, &gap)` | The line metrics layout should use (OS/2 typo when the font asks, else hhea). |
| `font_scale_em(&face, px)`, `font_scale_height(&face, px)` | Font units to pixels for an em size, or for ascent-to-descent height. |
| `font_kern(&face, left, right)` | Legacy `kern` table value. Shaping applies GPOS kerning itself. |

## Outlines

`font_glyph_path(&face, g, &path)` appends the glyph's outline to a
`FontPath` (font units, y up). Commands are `FONT_MOVE`, `FONT_LINE`,
`FONT_QUAD`, `FONT_CUBIC` and `FONT_CLOSE`, with points in `path.pts`
(see `font_path_cmd_points`). `font_glyph_box` gives its bounds. Build
your own paths with `font_path_move`, `_line`, `_quad`, `_cubic`,
`_close`; free them with `font_path_free`.

## Rasterising

| Function | What it does |
|---|---|
| `font_glyph_bitmap(&face, g, px, &bmp)` | Anti-aliased coverage bitmap. |
| `font_glyph_bitmap_scaled(&face, g, sx, sy, shift_x, shift_y, &bmp)` | Separate scales and a sub-pixel shift. |
| `font_path_bitmap(&path, ...)`, `font_raster_path(&path, ..., out, w, h, stride)` | Any path, into a new bitmap or your own memory. |
| `font_glyph_sdf(&face, g, px, spread, &bmp)` | Signed distance field (128 = edge, inside is brighter). |
| `font_glyph_msdf(&face, g, px, spread, &bmp)` | Multi-channel SDF (RGB); sample with `median(r, g, b)`. |

A `FontBitmap` has `w`, `h`, `ch` (bytes per pixel) and `data`; draw it
with its top-left corner at `(pen_x + bmp.x, baseline_y + bmp.y)`. Free it
with `font_bitmap_free`.

## Glyph atlas

```mettle
var atlas: FontAtlas;
font_atlas_init(&atlas, 512, 512, 4096, 1);          // 1 byte per pixel
var e: FontAtlasGlyph* = font_atlas_glyph(&atlas, &face, g, 18.0, 0, FONT_ATLAS_COVERAGE);
// draw a quad at (pen_x + e->bx, baseline + e->by), size e->w x e->h,
// texture coordinates (e->u0, e->v0)-(e->u1, e->v1)
// upload atlas.pixels where atlas.dirty_* says, then font_atlas_clean(&atlas)
```

The atlas doubles in height up to its maximum; after that it returns null
and you can `font_atlas_clear`. For SDF use `FONT_ATLAS_SDF`; for MSDF
make the atlas with 3 channels and use `FONT_ATLAS_MSDF`.

## Shaping

```mettle
var buf: FontBuffer;
font_shape(&face, text.chars, (int64)text.length, (FontShapeOpts*)0, &buf);
for i: int64 in 0..buf.g.len {
  var gl: FontGlyph = buf.g.data[i];   // id, cluster, x_adv, x_off, y_off (font units)
}
font_buffer_free(&buf);
```

`FontShapeOpts` sets the script and language tags (0 = guess, default),
the direction, and feature switches with `font_shape_feature(&opts,
"smcp", true)` or `"liga", false`.

## Layout

```mettle
var o: FontLayoutOpts;
o.size = 16.0;           // pixels per em
o.max_width = 300.0;     // 0 = no wrapping
o.align = FONT_ALIGN_LEFT;
var l: FontLayout;
font_layout(&face, text.chars, (int64)text.length, &o, &l);
// l.glyphs: id, x, y (baseline), cluster, line; l.lines; l.width, l.height
font_layout_free(&l);
```

`font_measure` gives width and height without wrapping.
`font_layout_hit(&l, x, y)` turns a point into a byte offset, and
`font_layout_caret(&l, offset, &x, &top, &height)` does the reverse.

## Unicode helpers

| Function | What it does |
|---|---|
| `font_utf8_decode`, `font_utf8_encode`, `font_utf8_valid`, `font_utf8_count` | UTF-8 (bad bytes become U+FFFD). |
| `font_line_breaks(s, n, out)` | UAX #14 break status before each byte. |
| `font_ucd_nfc`, `font_ucd_nfd` | UAX #15 normal forms. |
| `font_ucd_gc`, `font_ucd_ccc`, `font_ucd_decompose`, `font_ucd_compose` | Character properties. |
