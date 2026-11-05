# canvas

2D vector drawing for Mettle, on the CPU, in pure Mettle.

canvas draws paths, strokes, gradients, images and text into RGBA (or
BGRA) pixmaps with exact-area anti-aliasing, and saves them as PNG.

- Paths: lines, quadratic and cubic Béziers, SVG and HTML canvas arcs,
  rectangles, rounded rectangles, ellipses; SVG path data.
- Fills: non-zero and even-odd, anti-aliased or hard-edged.
- Strokes: miter (with miter limit), round and bevel joins; butt, round
  and square caps; dashes; hairlines.
- Transforms, save/restore, anti-aliased clipping by any path.
- Paint: solid colors, linear, radial (two-point conical) and sweep
  gradients with pad, repeat and reflect; image patterns with nearest or
  bilinear sampling; opacity; all 28 W3C blend modes.
- Text through [font](https://github.com/Lex-Dellimere/Font) (shaped:
  ligatures, kerning, complex scripts).
- Image files through [Etch](https://github.com/Lex-Dellimere/Etch).

```mettle
import "canvas/image";

fn main() -> int32 {
  var pm: CanvasPixmap;
  canvas_pixmap_new(&pm, 200, 200);
  var ctx: CanvasContext;
  canvas_init(&ctx, &pm);
  var path: CanvasPath;
  canvas_svg_path("M20 20 L180 60 Q 120 180 20 140 Z", &path);
  var paint: CanvasPaint = canvas_paint_hex(0xd03030ff);
  canvas_fill_path(&ctx, &path, &paint, CANVAS_NONZERO);
  canvas_pixmap_save(&pm, "out.png");
  canvas_path_free(&path);
  canvas_free(&ctx);
  canvas_pixmap_free(&pm);
  return 0;
}
```

## Using it

Add it with mbuild (`mb_dep_path("canvas", "../canvas")` or from a
registry) and import one of:

| Import | For |
|---|---|
| `canvas/canvas` | drawing only, no dependencies |
| `canvas/text` | drawing and text (needs font) |
| `canvas/image` | drawing and image files (needs Etch) |

The full API is in [docs/api.md](docs/api.md); the standards and the
techniques behind it are in [docs/references.md](docs/references.md).

## Examples

```
mbuild run chart                                   # chart.png: a line chart
mbuild run svg -- "M10 10 L90 30 Q60 90 10 60Z" out.png 256
mbuild run text -- /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf "Hello"
```

## Tests

```
mbuild test            # debug
mbuild test -r         # release
mbuild fuzz 10         # 10 x 40 fuzz rounds in the bounds-checked safe profile
mbuild refs            # remake the cairo reference images (pycairo)
```

The tests compare 57 scenes with cairo, check exact coverage areas,
compare glyphs with font's rasteriser and fuzz the parser and the
drawing code.

## Speed

`mbuild bench`, 1024 x 768, best of 5, the same work in both, on a busy
machine (load about 11) so only the ratios mean much:

| Work | canvas | cairo 1.18.4 |
|---|---|---|
| 2000 circles, filled, 60% alpha | 80 ms | 100 ms |
| 20000-point polyline, stroked 1.5 px | 83 ms | 59 ms |
| 200 cubics, stroked 4 px, round | 62 ms | 38 ms |
| 20 full-size gradient fills (linear and radial) | 276 ms | 192 ms |

canvas computes exact pixel areas (cairo samples 15 rows a pixel), and
redoes rows where a path overlaps itself so crossings stay correct.

## License

Apache-2.0. See [LICENSE](LICENSE).
