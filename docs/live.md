# Live programs

`mettle live` runs a program and changes it while it runs. Every save of a
`.mettle` file becomes a patch, and the program takes the patch at its next
`quiesce;`. Nothing in the source is annotated, and the program keeps all of
its state.

```bash
mettle live game.mettle
```

```text
Built executable 'game.exe'
[live] running game.exe; saving a .mettle file in . patches it
tick 6 score 7 old
Patch 1: bonus (new), main (5384 bytes)
  main moves to the new code at its next quiesce point
[live] patch 1: 2 functions
[live] main moves to its new code
[live] main: 'speed' is new here and starts from its initial value
tick 7 score 77 new speed 7
```

The program needs a `quiesce;` where it may change, usually the top of its
main loop. A `--live` build without one is refused, since it could never take
a patch.

Windows x64 only for now.

## What a patch can change

- Any function body, including functions that were inlined into others. A
  caller whose machine code changed because of the edit is patched with it.
- The function that holds the `quiesce;`, `main` and its loop included. Its
  running frame moves to the new code at the quiesce point.
- Function signatures, when every frame that could call the old way moves to
  the new code first.
- New functions.
- New module-scope variables, with their initial values.
- String literals and constant data.

The running program keeps the value of every variable that already existed.
Changing a variable's initializer does not reset it.

## Moving a running frame

When a patch changes the function that holds the `quiesce;`, the running frame
stops at the quiesce point and the new code continues from the same point: the
same iteration of the same loop, with every local carried over by name.

- A local keeps its value when its name and type are unchanged.
- A local that is new, or whose type changed, starts from its initializer when
  that is a constant, and from zero otherwise. The program prints a line naming
  it.
- An array or struct the code reaches through its address stays where it is,
  so pointers the old code saved into it stay valid.
- A function with several `quiesce;` points matches them by order. If the
  count changed, the frame keeps its old code until the function is next
  called.

Each move leaves the old frame waiting under the new one, so a long session of
edits to `main` grows the stack by one frame per patch. Arrays that move in
place take no extra space.

A function that is on the stack without holding a quiesce point, such as the
caller of a function that does, finishes in its old code and takes the new
code the next time it is called. Functions with inline assembly or
`@parallel` loops keep their old code at their quiesce points as well.

## What it refuses

The build of the patch fails, and the program keeps running the code it has,
when:

- a variable changed size
- a function's parameters or result changed, and a frame that keeps its old
  code may still call it the old way
- the new code needs an imported function or a runtime component the running
  program never linked
- the program has used up its 4096 spare slots for new functions or its 1 MB
  for new variables

## The pieces

| Command | What it does |
|---|---|
| `mettle live file.mettle [flags] [-o app.exe] [-- args]` | Build with `--live`, run, watch the directory, patch on save |
| `mettle --build --live file.mettle -o app.exe` | Build a program that can take patches, and write `app.exe.live` |
| `mettle --live-patch app.exe file.mettle` | Compile the source and write the next patch into `app.exe.live.d/` |
| `mettle --live-patch app.exe --live-patch-all file.mettle` | Replace every function, changed or not |

In a `--live` build every function `f` is called through an eight-byte thunk
that jumps through `f$slot`. A patch is new machine code plus the slots to
point at it. The running program finds the next patch file at `quiesce;`,
places the code within reach of the image, fixes its addresses, and stores the
slots.

`tests/live/live_oracle.ps1` checks the mechanism on every test program: it
puts a `quiesce;` at the top of `main`, builds a patch from the unchanged
source (which must say "No function changed"), then builds one that replaces
every function, and the program's output must equal a normal build's. With
`-Loops` it puts the `quiesce;` at the top of each loop in `main` instead, so
the frame moves in the middle of a loop; `-Optimize` builds everything with
`-O`.

The plan beyond this, from layout migration to rewinding a crash, is in
[the design](design/live.md).
