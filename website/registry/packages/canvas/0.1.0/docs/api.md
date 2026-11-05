# canvas API

Three entry points:

| Import | Gives | Needs |
|---|---|---|
| `import "canvas/canvas";` | pixmaps, paths, drawing, paint, clipping, SVG path data | nothing |
| `import "canvas/text";` | all of the above, plus text | font |
| `import "canvas/image";` | all of the above, plus image files | Etch |

Every name starts with `canvas_`, `Canvas` or `CANVAS_`. Functions that
can fail return an `int32` status: `CANVAS_OK` (0) or a negative code
(`CANVAS_E_ARG`, `_MEM`, `_PARSE`, `_IO`, `_LIMIT`, `_FONT`).
`canvas_error_text(rc)` gives a short message. Bad numbers (NaN, infinity,
huge values) never crash; such a path draws nothing.

## A first drawing

```mettle
import "canvas/image";

fn main() -> int32 {
  var pm: CanvasPixmap;
  canvas_pixmap_new(&pm, 200, 200);
  var ctx: CanvasContext;
  canvas_init(&ctx, &pm);
  var path: CanvasPath;                       // zeroed: empty
  canvas_svg_path("M20 20 L180 60 Q 120 180 20 140 Z", &path);
  var red: CanvasPaint = canvas_paint_hex(0xd03030ff);
  canvas_fill_path(&ctx, &path, &red, CANVAS_NONZERO);
  var st: CanvasStroke = canvas_stroke(4.0);
  st.join = CANVAS_JOIN_ROUND;
  var black: CanvasPaint = canvas_paint_hex(0x000000ff);
  canvas_stroke_path(&ctx, &path, &st, &black);
  canvas_pixmap_save(&pm, "out.png");
  canvas_path_free(&path);
  canvas_free(&ctx);
  canvas_pixmap_free(&pm);
  return 0;
}
```

## Pixmaps

`CanvasPixmap { width, height, stride, data, order, owned }` holds
premultiplied 8-bit pixels, rows top to bottom. `order` is `CANVAS_RGBA`
(bytes R, G, B, A) or `CANVAS_BGRA` (B, G, R, A, as X11 and Win32 use).

| Function | Does |
|---|---|
| `canvas_pixmap_new(pm, w, h)` | new pixmap of transparent pixels (up to 32767 a side) |
| `canvas_pixmap_wrap(pm, data, w, h, stride, order)` | draw into memory you own (stride 0 = `w * 4`) |
| `canvas_pixmap_free(pm)` | frees owned pixels |
| `canvas_pixmap_get(pm, x, y)` / `_set` | one premultiplied pixel as `uint32`, red in the low byte, whatever the order |
| `canvas_pixmap_color(pm, x, y)` | one pixel as a straight `CanvasColor` |
| `canvas_pixmap_fill(pm, c)` | every pixel set to `c` |
| `canvas_pixmap_copy(src, dst)` | an owned copy |
| `canvas_pixmap_row(pm, y)` | the address of a row |

With `canvas/image`: `canvas_pixmap_save(pm, path)` (format from the
extension: `.png`, `.qoi`, `.bmp`, `.tga`, `.jpg` ...),
`canvas_pixmap_load(pm, path)`, `canvas_pixmap_encode_png(pm, &buf)` (PNG
bytes in an `EtchBuf`), and `canvas_pixmap_to_image` /
`canvas_pixmap_from_image` to move between pixmaps and `EtchImage`s.

## Colors

`CanvasColor { r, g, b, a }` is straight (not premultiplied), each 0 to 1.
Make one with `canvas_rgba(r, g, b, a)`, `canvas_rgba8(r, g, b, a)` (0 to
255) or `canvas_hex(0xRRGGBBAA)`.

## Paths

`CanvasPath` is a list of contours. A zeroed one is empty; free it with
`canvas_path_free`, or empty it with `canvas_path_reset` to keep its
memory. Like the HTML canvas, a segment with no current point starts a
contour at its first point, and a segment after a close starts a new
contour where the closed one began.

| Function | Does |
|---|---|
| `canvas_move_to`, `canvas_line_to`, `canvas_quad_to`, `canvas_cubic_to`, `canvas_close` | the basic segments |
| `canvas_svg_arc_to(p, rx, ry, rotation_deg, large, sweep, x, y)` | SVG's elliptical arc (the `A` command) |
| `canvas_arc(p, cx, cy, r, a0, a1, ccw)` | HTML canvas `arc()`: angles in radians, a line from the current point to the start |
| `canvas_ellipse_arc(p, cx, cy, rx, ry, rotation, a0, a1, ccw)` | HTML canvas `ellipse()` |
| `canvas_arc_to(p, x1, y1, x2, y2, r)` | HTML canvas `arcTo()`: a corner rounded with radius r |
| `canvas_rect`, `canvas_round_rect(p, x, y, w, h, rx, ry)`, `canvas_oval`, `canvas_circle` | closed shapes |
| `canvas_path_bounds(p, &rect)` | bounds of all points (false when empty) |
| `canvas_path_transform(p, m)`, `canvas_path_append(dst, src)` | edit a path |
| `canvas_svg_path(text, p)`, `canvas_svg_path_bytes(ptr, n, p)` | SVG path data; on an error the path keeps what came before and the result is `CANVAS_E_PARSE` |

Arcs become cubic Béziers of at most 90 degrees each.

## Drawing

`canvas_init(&ctx, &pm)` starts drawing on a pixmap; `canvas_free(&ctx)`
frees the context (not the pixmap). The context holds a transform and a
clip, kept on a stack by `canvas_save` / `canvas_restore`.

| Function | Does |
|---|---|
| `canvas_fill_path(ctx, path, &paint, rule)` | fills with `CANVAS_NONZERO` or `CANVAS_EVENODD` |
| `canvas_stroke_path(ctx, path, &stroke, &paint)` | strokes |
| `canvas_fill_rect`, `canvas_stroke_rect` | rectangles |
| `canvas_draw_pixmap(ctx, src, x, y, &paint)` | an image at (x, y) in user space (nearest sampling on whole pixels, else bilinear); paint gives opacity and blend mode |
| `canvas_clear(ctx, color)` | every pixel, ignoring transform and clip |
| `canvas_set_tolerance(ctx, px)` | how far a flattened curve may stray, in device pixels (default 0.1) |

### Transforms

`CanvasTransform { a, b, c, d, e, f }` is SVG's `matrix(a b c d e f)`:
x' = a x + c y + e, y' = b x + d y + f. Make one with `canvas_matrix`,
`canvas_identity`, `canvas_translation`, `canvas_scaling`,
`canvas_rotation` (radians, clockwise on screen) or `canvas_skewing`;
combine with `canvas_t_mul(m, n)` (n applies first); invert with
`canvas_t_invert(m, &out)`; map a point with `canvas_t_point`.

On the context: `canvas_set_transform`, `canvas_get_transform`,
`canvas_transform(ctx, m)` (multiplies, m first), `canvas_translate`,
`canvas_scale`, `canvas_rotate`.

### Clipping

`canvas_clip_path(ctx, path, rule)` and `canvas_clip_rect(ctx, x, y, w, h)`
narrow the clip; clips are anti-aliased. A rectangle that lands on whole
device pixels costs nothing; anything else uses a coverage mask the size of
the pixmap. `canvas_restore` brings back the clip of the matching save;
`canvas_reset_clip` removes the clip of the current level.

## Strokes

`canvas_stroke(width)` gives a `CanvasStroke` with miter joins, butt caps
and miter limit 4 (SVG's defaults). Fields:

| Field | Meaning |
|---|---|
| `width` | line width in user units; 0 draws a hairline one device pixel wide |
| `join` | `CANVAS_JOIN_MITER`, `_ROUND`, `_BEVEL` |
| `cap` | `CANVAS_CAP_BUTT`, `_ROUND`, `_SQUARE` |
| `miter_limit` | past this ratio a miter becomes a bevel (SVG `stroke-miterlimit`) |
| `dashes`, `ndashes`, `dash_offset` | SVG `stroke-dasharray` / `stroke-dashoffset`; an odd list repeats, a negative value or zero total draws solid |

A stroke that would need more than a million dashes, or an outline of
more than `CANVAS_MAX_OUTLINE` (4 million) points, is refused with
`CANVAS_E_LIMIT`. Dashes are only made near the visible area.

Strokes are made in user space, so a non-uniform scale stretches the line
like SVG and the HTML canvas do. `canvas_stroke_to_path(path, &st, tol,
&out)` gives the outline as a path to fill with non-zero.

## Paint

`CanvasPaint { color, gradient, pattern, blend, opacity, anti_alias }`.
The source is the gradient when set, else the pattern, else the color.
`canvas_paint(color)`, `canvas_paint_hex(0xRRGGBBAA)`,
`canvas_paint_gradient(&g)` and `canvas_paint_pattern(&p)` fill in the
rest: source-over, opacity 1, anti-aliased.

### Gradients

```mettle
var g: CanvasGradient;
canvas_linear_gradient(&g, x0, y0, x1, y1);
// or canvas_radial_gradient(&g, x0, y0, r0, x1, y1, r1)
// or canvas_sweep_gradient(&g, cx, cy, start_angle)
canvas_gradient_add_stop(&g, 0.0, canvas_hex(0xff0000ff));
canvas_gradient_add_stop(&g, 1.0, canvas_hex(0x0000ffff));
canvas_gradient_spread(&g, CANVAS_REFLECT);      // CANVAS_PAD (default), _REPEAT
canvas_gradient_transform(&g, m);                 // optional, gradient to user space
...
canvas_gradient_free(&g);
```

Radial gradients are two-point conical, as the HTML canvas defines them.
Colors are mixed in premultiplied form (CSS Images 4), through a
1024-entry table. A gradient with no stops paints nothing.

### Patterns

```mettle
var pt: CanvasPattern;
canvas_pattern(&pt, &image);         // repeat both ways, bilinear
pt.spread_x = CANVAS_DECAL;          // CANVAS_PAD, _REPEAT, _REFLECT, _DECAL (clear outside)
pt.filter = CANVAS_NEAREST;          // or CANVAS_BILINEAR
pt.m = canvas_scaling(2.0, 2.0);     // pattern to user space
```

The pixmap is borrowed and must outlive drawing with it.

### Blend modes

`paint.blend` takes the W3C Compositing and Blending modes:
`CANVAS_SRC_OVER` (default), `CANVAS_CLEAR`, `CANVAS_SRC`, `CANVAS_DST`,
`CANVAS_DST_OVER`, `CANVAS_SRC_IN`, `CANVAS_DST_IN`, `CANVAS_SRC_OUT`,
`CANVAS_DST_OUT`, `CANVAS_SRC_ATOP`, `CANVAS_DST_ATOP`, `CANVAS_XOR`,
`CANVAS_PLUS`, `CANVAS_MULTIPLY`, `CANVAS_SCREEN`, `CANVAS_OVERLAY`,
`CANVAS_DARKEN`, `CANVAS_LIGHTEN`, `CANVAS_COLOR_DODGE`,
`CANVAS_COLOR_BURN`, `CANVAS_HARD_LIGHT`, `CANVAS_SOFT_LIGHT`,
`CANVAS_DIFFERENCE`, `CANVAS_EXCLUSION`, `CANVAS_HUE`,
`CANVAS_SATURATION`, `CANVAS_COLOR`, `CANVAS_LUMINOSITY`.
`canvas_blend_by_name("multiply")` and `canvas_blend_name(mode)` use the
CSS names. Pixels outside the shape are never touched (as in Skia), so
`CANVAS_SRC_IN` and the like act only under the shape.

## Text (`canvas/text`)

| Function | Does |
|---|---|
| `canvas_fill_text(ctx, &face, text, x, y, size, &paint)` | fills UTF-8 text; (x, y) is the start of the baseline, size is pixels per em |
| `canvas_stroke_text(ctx, &face, text, x, y, size, &stroke, &paint)` | outlines it |
| `canvas_measure_text(&face, text, size)` | advance width |
| `canvas_text_path(&face, text, x, y, size, &path)` | appends the glyph outlines to a path; returns the advance |

`face` is a font `FontFace` (`font_init(&face, data, len, 0)`). Text is
shaped by font (ligatures, kerning, marks, complex scripts) and drawn
through the transform like any path.

## Lower layers

These are public for libraries that want them: `canvas_flatten` (path to
polylines, `CanvasPoly`), `canvas_stroke_poly` and `canvas_dash_poly`
(polylines to stroke outlines and dashes), and the rasteriser
(`CanvasRaster`, `canvas_raster_edges`, `canvas_raster_fill`), which calls
a function with coverage for each row: MythicUI or a GPU uploader can use
it to make masks of their own.
