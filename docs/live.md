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
tick 13 counter 14
tick 14 counter 15
Patch 1: bonus (new), step, main (1408 bytes, 1 new global)
  main is running at the quiesce point and keeps its old code until it is next called
  bonus #6 on 16
tick 15 counter 32
```

The program needs a `quiesce;` where it may change, usually the top of its
main loop. A `--live` build without one is refused, since it could never take
a patch.

Windows x64 only for now.

## What a patch can change

- Any function body, including functions that were inlined into others. A
  caller whose machine code changed because of the edit is patched with it.
- New functions.
- New module-scope variables, with their initial values.
- String literals and constant data.

The running program keeps the value of every variable that already existed.
Changing a variable's initializer does not reset it.

## What it refuses

The build of the patch fails, and the program keeps running the code it has,
when:

- a variable changed size
- the new code needs an imported function or a runtime component the running
  program never linked
- the program has used up its 4096 spare slots for new functions or its 1 MB
  for new variables

## Frames on the stack

A function that is running when the patch lands finishes in its old code, and
takes the new code the next time it is called. The function holding the
quiesce point is always running, so the patch summary names it. For `main`,
"next time" is never; move the loop body into a function that `main` calls to
make it patchable.

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
every function, and the program's output must equal a normal build's.

The plan beyond this, from layout migration to rewinding a crash, is in
[the design](design/live.md).
