# Etch API

Everything is in `import "etch/etch";`. Every name starts with `etch_`,
`Etch` or `ETCH_`. Functions that can fail return an `int32` status:
`ETCH_OK` (0) or a negative `ETCH_E_*` code. `etch_error_text(rc)` gives a
short message.

## Status codes

| Code | Meaning |
|---|---|
| `ETCH_OK` | success |
| `ETCH_E_ARG` | a bad argument (null pointer, bad size or format) |
| `ETCH_E_DATA` | the input is not a valid file of this format |
| `ETCH_E_TRUNC` | the input ended early |
| `ETCH_E_CHECK` | a checksum in the file does not match |
| `ETCH_E_LIMIT` | the image is bigger than the limits allow |
| `ETCH_E_MEM` | out of memory |
| `ETCH_E_UNSUPPORTED` | valid data that uses a feature Etch lacks |
| `ETCH_E_IO` | a file could not be opened, read or written |
| `ETCH_E_FORMAT` | the data is not in any format Etch knows |

## Images

```mettle
export struct EtchImage {
  width: int32;
  height: int32;
  format: int32;
  stride: int64;    // bytes from one row to the next
  data: uint8*;
  owned: bool;      // true when etch_image_free should free data
}
```

Rows run top to bottom. A pixel is `data + y * stride + x * bpp`. 16-bit
channels are `uint16` in the machine's byte order; float channels are
`float32`, normally 0 to 1 (HDR images go higher).

| Format | Channels | Bytes a pixel |
|---|---|---|
| `ETCH_GRAY8`, `ETCH_GA8`, `ETCH_RGB8`, `ETCH_RGBA8` | 1, 2, 3, 4 | 1, 2, 3, 4 |
| `ETCH_GRAY16`, `ETCH_GA16`, `ETCH_RGB16`, `ETCH_RGBA16` | 1, 2, 3, 4 | 2, 4, 6, 8 |
| `ETCH_GRAYF`, `ETCH_GAF`, `ETCH_RGBF`, `ETCH_RGBAF` | 1, 2, 3, 4 | 4, 8, 12, 16 |

| Function | Does |
|---|---|
| `etch_image_new(img, w, h, fmt)` | new image, pixels zeroed, rows tightly packed |
| `etch_image_alloc(img, w, h, fmt)` | same, pixels left unset |
| `etch_image_wrap(img, data, w, h, fmt, stride)` | wrap memory you own (stride 0 = packed); never freed by Etch |
| `etch_image_free(img)` | free if owned, and empty |
| `etch_image_view(src, x, y, w, h, out)` | a view of a rectangle, sharing memory (clipped) |
| `etch_image_copy(src, out)` | an owned, packed copy (of an image or a view) |
| `etch_pixel(img, x, y) -> uint8*` | address of a pixel, no bounds check |
| `etch_image_row_bytes(img)` | bytes in one packed row |
| `etch_image_equal(a, b)` | same size, format and pixel bytes |
| `etch_format_ok(fmt)`, `etch_format_channels`, `etch_format_kind`, `etch_format_depth`, `etch_format_bpp`, `etch_format_has_alpha`, `etch_format_name` | facts about a format |
| `etch_format_make(channels, kind)` | a format from a channel count and `ETCH_U8`, `ETCH_U16` or `ETCH_F32` |

## Loading and saving

| Function | Does |
|---|---|
| `etch_load(path, out, opts)` | read a file and decode it (first frame of an animation) |
| `etch_load_anim(path, anim, opts)` | read a file and decode every frame |
| `etch_save(path, img)` | encode by the file name's extension and write |
| `etch_decode(p, n, out, opts)` | decode bytes in any supported format |
| `etch_decode_anim(p, n, anim, opts)` | decode every frame (APNG, GIF; one frame for the rest) |
| `etch_encode(img, ETCH_FMT_*, buf)` | encode with default settings |
| `etch_detect(p, n) -> ETCH_FMT_*` | which format, from the first bytes |
| `etch_format_of_name(path) -> ETCH_FMT_*` | which format, from the extension |
| `etch_file_read(path, buf)`, `etch_file_write(path, p, n)` | whole files |

`ETCH_FMT_PNG`, `_JPEG`, `_QOI`, `_BMP`, `_TGA`, `_GIF`, `_HDR`, `_PNM` and
`_UNKNOWN`. `etch_encode` uses PNG level 6, JPEG quality 90 with 4:2:0
chroma, TGA with RLE. GIF has no encoder.

### Options and limits

Every decoder takes an `EtchOpts*`. Pass `(EtchOpts*)0` for the defaults.

```mettle
export struct EtchLimits {
  max_width: int32;    // 0 means no limit, in every field
  max_height: int32;
  max_pixels: int64;   // width * height
  max_bytes: int64;    // memory for one decoded image or frame
}
export struct EtchOpts {
  limits: EtchLimits;
  want: int32;         // convert to this format; 0 keeps the file's own
}
```

```mettle
var o: EtchOpts;
etch_opts_init(&o);              // the default limits, no conversion
o.want = ETCH_RGBA8;             // always get RGBA8
o.limits.max_pixels = 16777216;  // refuse anything over 16 megapixels
etch_load("in.png", &img, &o);
```

The defaults are 65535 pixels a side, 2^28 pixels and 1 GiB per image.
`etch_default_limits()` and `etch_set_default_limits(l)` read and change
them. A decoder checks the size in the header before it allocates, so a tiny
file that claims to be huge costs nothing. `etch_load` also refuses files
bigger than `max_bytes`.

## Animations

```mettle
export struct EtchFrame { img: EtchImage; delay_ms: int32; }
export struct EtchAnim {
  frames: EtchFrame*;
  count: int32;
  width: int32;
  height: int32;
  loops: int32;       // times to play; 0 means forever
}
```

Every frame is the whole canvas, already composed (blending and disposal
done), so you can show `frames[i].img` as it is. `etch_anim_free(a)` frees
all frames.

## Buffers

Encoders append to an `EtchBuf`. A zeroed one is empty.

```mettle
var out: EtchBuf;                // { data, len, cap, limit }
etch_png_encode(&img, &out, 6);
etch_file_write("out.png", out.data, out.len);
etch_buf_free(&out);
```

`limit` (0 = none) caps how big the buffer may grow. `etch_buf_reserve`,
`etch_buf_append`, `etch_buf_push` and the `etch_buf_u16le/u32le/u16be/u32be`
writers are there for your own formats, as is the bounded reader `EtchRd`
(`etch_rd_init`, `etch_rd_u8`, `_u16le`, `_u16be`, `_u32le`, `_u32be`,
`_skip`, `_take`, `_left`), which returns 0 and sets `bad` instead of reading
past its end.

## Formats

| Function | Notes |
|---|---|
| `etch_png_decode(p, n, out, opts)` | any PNG; APNG gives its still image |
| `etch_png_decode_anim(p, n, anim, opts)` | APNG frames as RGBA8 (RGBA16 for 16-bit files) |
| `etch_png_info(p, n, info)` | size, depth, color type, gamma, sRGB, frame count, without decoding |
| `etch_png_encode(img, buf, level)` | level 0 (store) to 9, -1 = 6; float becomes 16-bit |
| `etch_jpeg_decode(p, n, out, opts)` | GRAY8 or RGB8 |
| `etch_jpeg_info(p, n, info)` | size, components, progressive |
| `etch_jpeg_encode(img, buf, quality, subsample)` | quality 1 to 100; `subsample` = 4:2:0, else 4:4:4; gray stays gray |
| `etch_qoi_decode`, `etch_qoi_encode(img, buf, linear)` | RGB8 or RGBA8 |
| `etch_bmp_decode`, `etch_bmp_encode(img, buf)` | writes 24-bit, or 32-bit with alpha |
| `etch_tga_decode`, `etch_tga_encode(img, buf, rle)` | writes gray, 24 or 32-bit |
| `etch_gif_decode`, `etch_gif_decode_anim` | RGBA8 |
| `etch_hdr_decode`, `etch_hdr_encode(img, buf)` | RGBF |
| `etch_pnm_decode`, `etch_pnm_encode(img, buf)` | PGM, PPM, PAM; 8 or 16-bit |
| `etch_*_is(p, n)` | does it start like that format |

Encoders take any format and convert when the file format needs something
else. See [formats.md](formats.md) for what each decoder accepts.

## Pixel tools

| Function | Does |
|---|---|
| `etch_convert(src, fmt, out)` | new image in another format |
| `etch_convert_into(src, dst)` | convert into an image you already have (same size) |
| `etch_convert_self(img, fmt)` | convert in place |
| `etch_flip_v(img)`, `etch_flip_h(img)` | flip in place |
| `etch_rotate(src, degrees, out)` | 90, 180 or 270 clockwise |
| `etch_crop(src, x, y, w, h, out)` | an owned copy of a rectangle (`etch_image_view` for no copy) |
| `etch_fill(img, px)` | every pixel set to one pixel's bytes |
| `etch_blit(dst, src, x, y)` | copy one image onto another (same format, clipped, no blending) |
| `etch_resize(src, w, h, filter, out)` | `ETCH_FILTER_NEAREST`, `_BOX`, `_BILINEAR`, `_CUBIC` (Catmull-Rom), `_MITCHELL`, `_LANCZOS` (3 lobes) |
| `etch_premultiply(img)`, `etch_unpremultiply(img)` | color times alpha, and back |
| `etch_srgb_to_linear(img)`, `etch_linear_to_srgb(img)` | the sRGB curve on color channels, in place |
| `etch_srgb_to_linear_f(v)`, `etch_linear_to_srgb_f(v)` | the curve for one value |
| `etch_row_to_f32(row, fmt, w, rgba)`, `etch_row_from_f32(rgba, fmt, w, row)` | one row to and from float RGBA, for your own filters |

Conversion rules: gray from color uses the Rec. 601 weights; a missing alpha
becomes opaque and a dropped alpha is just dropped (no blending); 8 to 16
bits multiplies by 257 and 16 to 8 rounds; float is clamped to 0..1 on the
way to integers. Conversion never changes the color space: use the sRGB
functions for that.

`etch_resize` shrinks without aliasing (the filter widens with the scale),
premultiplies alpha while it filters so clear pixels do not bleed color, and
works in the stored values. For the most correct result on sRGB images,
convert to `ETCH_RGBAF`, call `etch_srgb_to_linear`, resize, then
`etch_linear_to_srgb`.

## OpenGL

`etch_gl_format(fmt, srgb)` gives the `internal`, `format` and `type`
enums for `glTexImage2D`; `etch_gl_alignment(img)` and
`etch_gl_row_length(img)` give `GL_UNPACK_ALIGNMENT` and
`GL_UNPACK_ROW_LENGTH`. See [textures.md](textures.md).
