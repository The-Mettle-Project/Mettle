# Live preview

`glint_preview` is a ready-made output window for seeing your work as you make it.
Save a shader, a texture or a data file and the window updates. A shader that
doesn't compile keeps the last good one running, and the error shows at the top
of the window. Under `mbuild live`, code changes are patched in as well.

```mettle
import "glint/glint_preview";

fn draw_frame(pv: GlintPreview*, user: rawptr) -> bool {
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  var view: GlintMat4 = glint_preview_view(pv);          // the orbit camera
  glint_set_mat4(prog, "view", &view.m[0]);
  ...
  return true;                                            // false: this frame went wrong
}

fn main() -> int32 {
  var pv: GlintPreview* = glint_preview_open("my scene", 0, 0, GLINT_DOCK_RIGHT);
  prog = glint_program_new();
  glint_program_add_file(prog, GL_VERTEX_SHADER, "shaders/scene.vert");
  glint_program_add_file(prog, GL_FRAGMENT_SHADER, "shaders/scene.frag");
  glint_program_build(prog);                    // a failure shows in the window
  glint_preview_program(pv, prog);              // rebuild when the files change
  glint_preview_watch(pv, "mesh.obj", &load_mesh, (rawptr)0);
  glint_preview_orbit(pv, glint_vec3(0.0, 0.0, 0.0), 3.0);
  glint_preview_run(pv, &draw_frame, (rawptr)0);
  glint_preview_close(pv);
  return 0;
}
```

`examples/preview` is a full example: `mbuild run preview`, then edit
`examples/preview/shaders/light.glsl` or `examples/preview/color.txt`.

## Opening

| Function | What it does |
|----------|--------------|
| `glint_preview_open(title, w, h, dock) -> GlintPreview*` | A window with a GL context. `dock` is `GLINT_DOCK_RIGHT`, `_LEFT` (full height), `_TOP`, `_BOTTOM` (full width) or `_NONE`. A 0 size picks two fifths of the screen. The environment variable `MBUILD_PREVIEW=right|left|top|bottom|off`, which `mbuild live` sets, wins over `dock`. |
| `glint_preview_headless(w, h) -> GlintPreview*` | No window, for tests and CI. Read the result with `glint_read_rgba(pv->ctx, out)`. |
| `glint_preview_wrap(ctx) -> GlintPreview*` | Use a context you already have (for example on your own window). |
| `glint_preview_close(pv)` | Closes it, and the window. Programs you registered are still yours to free. |

Docking places the window at the screen edge and asks the window manager to keep it there. Glint can't dock *into* an editor window; it puts the preview beside it.

## The frame

| Function | What it does |
|----------|--------------|
| `glint_preview_run(pv, frame_cb, user) -> int32` | Runs until the window closes or Escape is pressed. `frame_cb(pv, user) -> bool` draws one frame; return false when it went wrong. |
| `glint_preview_begin(pv) -> bool` | For your own loop: handles window events, reloads changed files, reads mbuild events, and binds the framebuffer to draw into. False once the window is closed. |
| `glint_preview_end(pv, ok: bool)` | Shows the frame (or the last good one if `ok` is false), draws messages on top, swaps, and applies live code patches. |
| `glint_preview_target(pv) -> uint32` | The framebuffer frames draw into. It is bound by `begin`; bind it again if you draw to other framebuffers first. |

`pv->time` is the seconds since the preview opened, `pv->frame` counts frames, and `pv->events[0 .. pv->nevents]` holds this frame's window events (keys, mouse, resize).

**Last good frame.** Every frame that ends with `ok = true` is kept. A frame that ends with `ok = false` shows the kept one instead, with a notice.

## Watching files

| Function | What it does |
|----------|--------------|
| `glint_preview_program(pv, p: GlintProgram*)` | Rebuild `p` when any of its files changes, `#include`d files too. On failure the old program keeps drawing and the error (with file and line) shows at the top of the window until a save fixes it. If `p` never built, its error shows straight away. |
| `glint_preview_watch(pv, path, cb, user)` | Call `cb(user, path)` at the start of the frame after `path` changes. Use it for textures, meshes and settings. |
| `glint_preview_error(pv, msg)` | Show your own error (for example "model.obj: bad face on line 12"). `""` clears it. |
| `glint_preview_status(pv, msg)` | A short note at the bottom for two seconds. Glint uses it for "reloaded ..." and "patched ...". |
| `glint_preview_has_error(pv) -> bool`, `glint_preview_error_text(pv) -> cstring` | What is showing. |

Paths are relative to the folder the program runs in: run from the project folder.

## Orbit camera

`glint_preview_orbit(pv, target, distance)` turns it on. Drag with the left mouse button to turn, scroll to zoom, R to reset.
`glint_preview_view(pv)`, `glint_preview_projection(pv)` (45 degree field of view, fits the window) and `glint_preview_eye(pv)` give the matrices and the camera position.

## With mbuild live

`mbuild live BIN` rebuilds and patches your code while it runs. It tells the preview what happened through a file named by `MBUILD_LIVE_EVENTS`. The preview reads new lines from it each frame:

| Line | The preview |
|------|-------------|
| `patched NAME` | shows "patched NAME" and clears a code error |
| `patch-error MSG` / `build-error MSG` | shows MSG at the top until the next `patched` or `built` |
| `built` | clears a code error |
| `paused` / `resumed` | shows or hides "paused (waiting for a fix)" |
| `changed PATH` / `removed PATH` | shows a note (file watching reloads on its own) |

Lines of other kinds are ignored. Patches are applied at the `quiesce;` in `glint_preview_end`, between frames, so your frame function is patched while the program keeps its state. Keep drawing code in functions that `frame_cb` calls, not in `main`. `glint_preview_live_events(pv, path)` reads events from a file you choose.
