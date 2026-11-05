# Glint

Glint is OpenGL 4.6 for [Mettle](https://github.com/The-Mettle-Project/Mettle). It gives you:

- **every GL 4.6 core function and enum** (657 functions, 1367 enums), generated from the Khronos registry file `gl.xml`;
- **the ARB, KHR and EXT extensions** (308 extensions, 889 more functions), with `glint_has_ext` to check for one;
- **a context** on Glint's own window, on a window your program already has (an X11 window id or an HWND), or **headless** with no window at all, with pixel readback;
- **Linux** through EGL, with GLX as the fallback, and **Windows** through WGL;
- shaders from strings or files with `#include`, errors that name the file and line, programs that rebuild when you save their files, cached uniforms and setters, SPIR-V, and program pipelines;
- a live preview window (`glint_preview`): docked beside your editor, it reloads shaders and files when you save, shows errors in the window, keeps the last good frame, has an orbit camera, and works with `mbuild live` for code changes;
- small helpers: debug output, pixel readback, PPM saving, and a little vector and matrix math (`glint_math`).

It is a plain library, not an engine: use it for a game engine, a GUI renderer, a tool, or tests.
The system GL libraries are loaded when your program runs (`dlopen` / `LoadLibraryA`), so nothing links against libGL and a machine without GL gets a clear error instead of a failed start.

## Start

```bash
mbuild get glint
```

```mettle
import "std/io";
import "glint/glint";

fn main() -> int32 {
  var ctx: GlintContext* = glint_open_window("Hello", 800, 600, (GlintConfig*)0);
  if (ctx == 0) { print_cstr(glint_error()); println(""); return 1; }
  var ev: GlintEvent;
  while (!glint_should_close(ctx)) {
    while (glint_poll(ctx, &ev)) {
      if (ev.kind == GLINT_EV_KEY_DOWN && ev.key == GLINT_KEY_ESCAPE) { glint_set_should_close(ctx, true); }
      if (ev.kind == GLINT_EV_RESIZE) { glViewport(0, 0, ev.w, ev.h); }
    }
    glClearColor(0.2, 0.3, 0.3, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    glint_swap(ctx);
  }
  glint_close(ctx);
  return 0;
}
```

`mbuild new myapp --from glint` starts a project like this one.

GL calls look the same as in C: `glClear(GL_COLOR_BUFFER_BIT)`, `glGenBuffers(1, &vbo)`. GL types map to Mettle types like this:

| GL | Mettle |
|----|--------|
| `GLenum`, `GLbitfield`, `GLuint` | `uint32` |
| `GLint`, `GLsizei` | `int32` |
| `GLboolean` | `uint8` (pass `(uint8)0` for `GL_FALSE`) |
| `GLfloat` / `GLdouble` | `float32` / `float64` |
| `GLintptr`, `GLsizeiptr`, `GLint64` | `int64` |
| `const GLchar*` / `const GLubyte*` | `cstring` |
| `void*`, `GLsync` | `rawptr` |
| `GLDEBUGPROC` | `fn(uint32, uint32, uint32, uint32, int32, cstring, rawptr) -> void` |

Enums are `uint32` constants. Parameters that are `GLint` but take an enum (like `glTexParameteri`) need a cast: `(int32)GL_LINEAR`.

## Windows and input

Glint can open a simple window of its own (keys, mouse, resize, close), which is enough for tools, tests and demos. For a full app or game (typed text and input methods, cursors, pointer lock, fullscreen, HiDPI, clipboard, gamepads, Wayland), use the [`window`](https://github.com/Lex-Dellimere/window) library for the window and Glint for the GL context on it (`glint_open_existing`).

## Three kinds of context

```mettle
glint_open_window(title, w, h, &cfg)    // Glint makes the window and reads its events
glint_open_existing(handle, &cfg)       // your window: an X11 window id (Linux) or an HWND (Windows)
glint_open_headless(w, h, &cfg)         // no window; draws into an off-screen framebuffer
```

Each returns a `GlintContext*` that is current, or 0 with the reason in `glint_error()`.
Pass `(GlintConfig*)0` for the defaults, or start from `glint_config()` and change fields:
`want_major`/`want_minor` (tried first, default 4.6), `major`/`minor` (the lowest you accept, default 3.3), `debug`, `vsync`, `depth_bits`, `stencil_bits`, `samples`, `srgb`, `resizable`, `visible`, `backend`.
Glint asks for 4.6 and steps down (4.5, 4.4 ... 3.3) until the driver says yes; `glint_gl_version(ctx)` tells you what you got.

`glint_open_shared(ctx, w, h)` adds an off-screen context that shares objects with `ctx` and draws into a texture, for things like 3D viewports inside a GUI.

For a headless context, `glint_framebuffer(ctx)` is the framebuffer to bind where you would bind 0.
`glint_read_rgba(ctx, out)` reads the whole picture top row first, and `glint_write_ppm` saves it.

See [docs/api.md](docs/api.md) for every Glint function and [docs/platforms.md](docs/platforms.md) for how each platform works.

## Extensions

```mettle
if (glint_has_ext("GL_ARB_bindless_texture")) {
  var h: uint64 = glGetTextureHandleARB(tex);
  glMakeTextureHandleResidentARB(h);
}
```

The functions of every ARB, KHR and EXT extension are loaded when a context is made. Extensions that became core (like direct state access) use their core names. For anything else (NV, AMD, ...) use `glint_getproc("glSomethingNV")` and cast the address to the right `fn` type.
Always check `glint_has_ext` first: on Linux a driver may hand out a stub for a name it does not support.

## Examples

Ports of [LearnOpenGL](https://learnopengl.com) chapters, in `examples/`:

| Bin | Chapter |
|-----|---------|
| `triangle` | Hello Triangle |
| `textures` | Textures (two textures made in code instead of image files) |
| `transforms` | Transformations |
| `preview` | A live preview: a lit cube docked on the right; edit its shaders or `color.txt` while it runs. See [docs/preview.md](docs/preview.md). |
| `lighting` | Basic Lighting (Phong, a lamp, an orbiting camera). Its shaders are files you can edit while it runs. |

```bash
mbuild run lighting
mbuild run lighting -- --headless out.ppm    # one frame, no window
```

## Building and testing

```bash
mbuild build
mbuild test          # Linux; window tests start their own Xvfb, never your screen
mbuild test -r       # release build
mbuild wintest       # the same tests as Windows programs under wine (needs MBUILD_METTLE_WINDOWS)
mbuild gen           # remake the bindings from registry/gl.xml
```

The tests need Xvfb for the window parts. Headless tests run on any GPU or on Mesa's software renderer.

## Regenerating the bindings

`src/glint_gl.mettle` and `src/glint_ext.mettle` are generated; don't edit them by hand.
To update them to a newer registry:

```bash
curl -fsSL -o registry/gl.xml https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/xml/gl.xml
mbuild gen           # runs tools/glgen.py (Python 3, standard library only)
mbuild test
```

To change what is generated (types, which extensions), edit `tools/glgen.py`.

## License

Apache-2.0 (see LICENSE). `registry/gl.xml` is Copyright The Khronos Group Inc., Apache-2.0; see NOTICE.
