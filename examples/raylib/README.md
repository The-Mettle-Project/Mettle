# raylib bindings

Pure-Mettle bindings for [raylib](https://www.raylib.com) 5.5, plus a rotating
sphere demo that reports its own frame statistics.

- [`raylib.mettle`](raylib.mettle) — the bindings: raylib's structs declared field for field, and ~70 `extern fn` declarations bound to the DLL's exports.
- [`sphere.mettle`](sphere.mettle) — the demo: a UV sphere generated in Mettle, uploaded once, drawn with a model matrix built in Mettle.
- [`sphere.vs`](sphere.vs) / [`sphere.fs`](sphere.fs) — GLSL, embedded at compile time with `import_str`.

No C shim, no import library, no C runtime. The internal PE linker resolves the
imports by probing `libraylib.dll`'s export table:

```bat
examples\raylib\build.bat
examples\raylib\sphere.exe
```

`build.bat` takes the directory holding the DLL as its first argument, or reads
`RAYLIB_DIR`, or finds it on `PATH`.

## What makes it work

raylib passes small structs by value everywhere, which is the part of a C ABI a
new language usually has to punt on. Mettle applies the Microsoft x64 aggregate
rule to `extern` callees, so the declarations need no help:

| raylib type | size | how it travels |
|---|---|---|
| `Color` | 4 | one integer register |
| `Vector2` | 8 | one integer register, returned in RAX |
| `Vector3` | 12 | pointer to a caller copy |
| `Camera3D` | 44 | pointer to a caller copy |
| `Mesh`, `Model` | 112, 120 | returned through a hidden first argument |

So `DrawSphere(vec3(0.0, 0.0, 0.0), 2.0, RED())` and
`LoadModelFromMesh(GenMeshSphere(1.0, 16, 16))` both land on the registers the C
build of raylib reads.

Two things do not cross the boundary: raymath is header-only, so
`sphere.mettle` builds its own rotation matrix, and mesh buffers handed to
`UploadMesh` are allocated with raylib's `MemAlloc` so `UnloadMesh` can free
them.

## Frame rates

RTX 5060 Ti, 1280x720, 4x MSAA, no vsync, one sphere of 16,641 vertices and
32,768 triangles rotating. Five-second runs, `--bench=5`:

| Configuration | Average FPS | Median frame |
|---|---:|---:|
| baseline (three runs) | 8,106 / 8,596 / 8,637 | 0.089 ms |
| default shader (`--plain`) | 8,812 | 0.089 ms |
| 1920x1080 | 9,003 | 0.081 ms |
| 2560x1440 | 6,389 | 0.129 ms |
| 512 triangles (`--rings=16 --slices=16`) | 8,567 | 0.089 ms |
| 129,032 triangles (`--rings=254 --slices=254`) | 9,013 | 0.083 ms |
| nothing drawn (`--empty`) | 9,437 | 0.079 ms |
| 64 spheres, 2.1M triangles (`--spheres=8`) | 3,718 | 0.231 ms |
| 256 spheres, 8.4M triangles (`--spheres=16`) | 1,206 | 0.748 ms |
| `--vsync` | 60.5 | 16.677 ms |

The sphere is close to free: 512 triangles and 129,032 triangles run at the same
rate, and drawing nothing at all only buys another ~10%. What the frame costs is
raylib's own per-frame work — `EndDrawing` polls input and swaps buffers — at
roughly 90 microseconds. Loading the GPU takes hundreds of spheres.

## Flags

```
--bench[=SECONDS]     run uncapped and print frame statistics
--rings=N --slices=N  tessellation (default 128, capped at 254 by 16-bit indices)
--width=N --height=N  window size (default 1280x720)
--spheres=N           draw an NxN grid instead of one
--vsync               wait for the display refresh
--plain               raylib's default shader instead of sphere.fs
--nomsaa              drop the 4x MSAA hint
--empty               draw no geometry
--hud                 keep the overlay on while benchmarking
--shot=FILE.png       screenshot after the first second
```

Keys: `V` vsync, `T` shader, `UP`/`DOWN` tessellation, `S` screenshot, `ESC` quit.

Linux is untested here: the compiler applies the System V rule to `extern`
callees, but these bindings have only been run against `libraylib.dll`.
