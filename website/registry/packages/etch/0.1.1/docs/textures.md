# Images as OpenGL textures

Etch does not call OpenGL. It gives you the numbers `glTexImage2D` needs,
and Glint makes the calls.

```mettle
import "etch/etch";
import "glint/glint";

var img: EtchImage;
var o: EtchOpts;
etch_opts_init(&o);
o.want = ETCH_RGBA8;                 // one format for every file
if (etch_load("wood.png", &img, &o) != ETCH_OK) { return 1; }
etch_flip_v(&img);                   // GL's first row is the bottom one

var t: EtchGlFormat = etch_gl_format(img.format, true);   // sRGB color
var tex: uint32 = 0;
glGenTextures(1, &tex);
glBindTexture(GL_TEXTURE_2D, tex);
glPixelStorei(GL_UNPACK_ALIGNMENT, etch_gl_alignment(&img));
glPixelStorei(GL_UNPACK_ROW_LENGTH, etch_gl_row_length(&img));
glTexImage2D(GL_TEXTURE_2D, 0, (int32)t.internal, img.width, img.height,
             0, t.format, t.type, (rawptr)img.data);
glGenerateMipmap(GL_TEXTURE_2D);
etch_image_free(&img);
```

- Pass `srgb = true` for color images and `false` for normal maps, masks
  and other data, so the GPU only linearizes what is really sRGB.
- Gray images upload as `GL_RED` (and gray+alpha as `GL_RG`); set a texture
  swizzle if a shader expects gray in all channels.
- 16-bit and float images upload as `GL_R16`..`GL_RGBA16` and
  `GL_R32F`..`GL_RGBA32F`. HDR files give `ETCH_RGBF`.
- Premultiplied alpha blends better when filtering: call `etch_premultiply`
  before upload and use `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`.
- A view (`etch_image_view`) uploads without a copy: `etch_gl_row_length`
  tells GL how wide the rows really are.
