# Formats

What each decoder reads and each encoder writes. Every decoder checks the
image size against the limits before it allocates, never reads past the end
of its input, and returns an error code for broken data (see the fuzz test).

## PNG and APNG

- Reads every color type (gray, RGB, palette, gray+alpha, RGBA) at every
  bit depth (1, 2, 4, 8, 16), Adam7 interlacing, PLTE and tRNS
  (transparent palette entries and color keys). All 176 files of the
  PngSuite give the same pixels as ImageMagick, and the 14 broken ones are
  refused.
- Output: gray and RGB stay as they are; tRNS adds alpha; palette images
  become RGB8 or RGBA8; depths below 8 are scaled up to 8 bits; 16-bit
  files give 16-bit formats.
- Chunk CRCs are checked. A bad CRC on a needed chunk is `ETCH_E_CHECK`; a
  bad one on an extra chunk drops that chunk. An unknown needed chunk is
  `ETCH_E_UNSUPPORTED`.
- gAMA and sRGB are reported by `etch_png_info` but not applied. iCCP,
  text and time chunks are skipped.
- APNG: acTL, fcTL and fdAT, frame offsets, both blend modes and all three
  dispose modes. A broken animation falls back to the still image.
- Writes 8 and 16-bit gray, gray+alpha, RGB and RGBA, choosing each row's
  filter by the smallest sum of absolute differences, with DEFLATE from
  Press at the level you pick.

## JPEG

- Reads baseline, extended and progressive Huffman JPEGs with 8-bit
  samples: gray, YCbCr, RGB (Adobe transform 0 or component ids R, G, B),
  CMYK and YCCK (Adobe), any sampling factors, restart markers.
- The IDCT, chroma upsampling and color conversion follow libjpeg's
  defaults, so output matches `djpeg` from libjpeg-turbo bit for bit on the
  test set (4:2:0, 4:2:2, 4:4:0, 4:1:1, 4:4:4, progressive, restarts,
  quality 5 to 100).
- Output is GRAY8 or RGB8. EXIF orientation is not applied.
- Not supported: arithmetic coding, 12-bit, lossless and hierarchical
  JPEG (`ETCH_E_UNSUPPORTED`).
- Writes baseline JPEG with the standard tables scaled by quality (the same
  scale as libjpeg's `-quality`), 4:2:0 or 4:4:4, or gray.

## QOI

Reads and writes RGB8 and RGBA8, as the spec at qoiformat.org.

## BMP

- Reads 1, 2, 4 and 8-bit palette, 16-bit (5-5-5 and bit fields), 24-bit
  and 32-bit (with or without alpha) pixels, RLE4 and RLE8, OS/2 headers and
  the V1 to V5 headers, bottom-up and top-down rows. Channels narrower than
  8 bits are widened by repeating their bits.
- 32-bit files whose alpha is all zero are read as opaque.
- Writes 24-bit RGB, or 32-bit with an alpha mask (V4 header) when the image
  has alpha.

## TGA

- Reads color-mapped, true-color and gray images, raw or RLE, 8, 15, 16, 24
  and 32 bits, any origin corner. TGA has no magic number, so detection
  checks that the header makes sense and is tried last.
- Writes gray, 24-bit and 32-bit, raw or RLE, top-left origin, with a
  TGA 2.0 footer.

## GIF

- Reads GIF87a and GIF89a: global and local palettes, transparency,
  interlacing, all frames, disposal methods (none, background, previous) and
  the NETSCAPE loop count. Frames are composed onto the canvas as RGBA8;
  "background" clears to transparent, as browsers do.
- `delay_ms` is the file's delay as written (many viewers show 0 as 100 ms).
- No encoder.

## Radiance HDR

- Reads `#?RADIANCE` and `#?RGBE` files with flat or run-length scanlines
  (old and new style) in the usual `-Y H +X W` orientation, to RGBF in
  linear light.
- Writes RGBF with run-length scanlines.

## PNM

Reads and writes binary PGM (P5), PPM (P6) and PAM (P7) at 8 or 16 bits.
Handy for tests and for piping to other tools.
