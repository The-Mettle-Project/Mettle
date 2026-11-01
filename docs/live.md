# Live programs

`mettle live` runs a program and changes it while it runs. Every save of a
`.mettle` file the program was built from becomes a patch, and the program
takes the patch at its next `quiesce;`. Nothing in the source is annotated.
The program keeps all of its state, and every frame on the stack moves to the
new code.

```bash
mettle live game.mettle
```

```text
Built executable 'game.exe'
[live] running G:\game\game.exe; saving any of its 3 source files patches it
tick 6 score 7 old
Patch 1: bonus (new), main (5384 bytes)
  running frames of main move to the new code at the next statement that can reach a quiesce point
[live] patch 1: 2 functions
[live] main moves to the current code
[live] main: 'speed' is new here and starts from its initial value
tick 7 score 77 new speed 7
```

The program needs a `quiesce;` where it may change, usually the top of its
main loop. A `--live` build without one is refused, since it could never take
a patch.

Windows x64 and Linux x64.

## What a patch can change

- Any function body, including functions that were inlined into others. A
  caller whose machine code changed because of the edit is patched with it.
- Every function on the stack, `main` and its loop included. See
  [Moving running frames](#moving-running-frames).
- Function signatures. Callers are patched with them, and frames on the stack
  move before they could call the old way.
- The layout of a module-scope variable: struct fields added, removed,
  reordered or retyped, arrays grown or shrunk. See
  [Layouts](#layouts).
- New functions, new module-scope variables with their initial values, string
  literals and constant data.
- On Windows, functions imported from DLLs the program never called before:
  the patch binds them when it loads.

The running program keeps the value of every `var` that already existed.
Changing its initializer does not reset it. A `const` follows the source:
changing one patches every function that uses it.

## Moving running frames

A frame moves to the new code at the next statement that can reach a
`quiesce;`: the quiesce point itself, or a call to a function that may reach
one. The new code continues from the same statement, in the same iteration of
the same loop, with every local carried over by name.

```mermaid
sequenceDiagram
    participant Old as main (old code)
    participant Rt as live runtime
    participant New as main$resume (new code)
    Old->>Rt: quiesce; applies patch 3
    Rt-->>Old: main moved
    Old->>Rt: put locals by name
    Old->>New: call with the statement key
    Rt-->>New: take locals by name
    New->>New: runs the rest of main
    Note over Old,New: when a later patch moves main$resume, it hands control back to main, which calls the newest main$resume
```

- A local keeps its value when its name is the same. When its type changed,
  the value is converted: integers widen or narrow, floats and integers
  convert, struct fields match by name, arrays keep the elements that still
  fit. The program prints one line for each converted local.
- A local that is new starts from its initializer when that is a constant, and
  from zero otherwise. The program prints a line naming it.
- An array or struct the code reaches through its address stays where it is,
  so pointers the old code saved into it stay valid.
- Statements are matched by their source text, the statement before them, and
  the loops around them, so an edit elsewhere in the function, or a struct
  whose fields moved, does not stop a frame from moving. A frame whose
  statement is gone keeps its old code until the function is next called, and
  the patch summary says so.

Callers move too. A function on the stack below the quiesce point moves when
control returns to it, at the statement that made the call. The stack does not
grow: each function has one continuation, and `main` returns to a loop that
calls the newest one, so a session of a thousand edits to `main` runs two
frames deep.

Functions with inline assembly or `@parallel` loops keep their old code until
they are next called.

## Layouts

When the type of a module-scope variable changed, the patch carries the old
and new layout of the variable and new storage for it. The running program
converts the value into the new storage, with every thread stopped, before any
new code runs.

```text
Patch 1: step, main (14224 bytes, 2 globals converted to a new layout)
```

Fields match by name at any depth. A field that is new starts at zero. An
integer field that became `int64`, a `float32` that became `float64`, a
`string`, an array of structs whose struct gained fields: all keep their
values. A field whose new type cannot hold the old value, a struct that became
an integer for example, starts at zero.

Pointers that other variables or the heap hold into the old storage keep
pointing at it, and the old storage keeps the value it had at the patch.
Objects on the heap keep their old layout.

## Undo

```bash
mettle --live-undo game.exe
```

Undo writes a patch that restores the slots, frame tables and variables the
latest patch changed. Variables the patch converted are converted back into
their old storage, with the values they have now. Frames move back to the old
code the same way they moved forward. Undo can be repeated back to the
original build.

## Threads

Patches apply on the thread that reaches `quiesce;`. On Windows every other
thread is suspended while slots are stored and variables are converted. On
Linux the other threads keep running, and each function entry is rewritten by
one aligned eight-byte store, so a thread calls either the old or the new
function. A frame
moves only on the thread that runs it, at its own next site.

## Crashes in patched code

A patch carries line tables for its functions. In a program built with `-s`,
a crash inside patched code prints the function and line from the source the
patch was built from:

```text
Fatal error: null pointer dereference
  --> s.mettle:8:9 in step
Stack trace:
  #0 step at s.mettle:8:9 (0x00000001401C0038)
  #1 main at s.mettle:14:21 (0x000000014000B887)
```

## What it refuses

The build of the patch fails, and the program keeps running the code it has,
when:

- a function's parameters or result changed, and a frame that keeps its old
  code may still call it the old way
- the new code needs a runtime component the running program never linked
- the program has used up its 4096 spare slots for new functions or its 1 MB
  for new and converted variables

The running program refuses a patch, prints the reason and keeps running, when
an import cannot be bound or the code cannot be placed within 2 GB of the
image.

## The pieces

- `mettle live file.mettle [flags] [-o app.exe] [-- args]` builds with
  `--live`, runs the program, and patches it each time a source file it was
  built from changes, imports and stdlib included.
- `mettle --build --live file.mettle -o app.exe` builds a program that can
  take patches, and writes `app.exe.live` and `app.exe.live.files`.
- `mettle --live-patch app.exe file.mettle` compiles the source and writes the
  next patch into `app.exe.live.d/`.
- `mettle --live-patch app.exe --live-patch-all file.mettle` replaces every
  function, changed or not.
- `mettle --live-undo app.exe` undoes the latest patch.

In a `--live` build every function starts with an eight-byte no-op, and calls
go straight to it. A patch is new machine code plus the functions it replaces.
The running program finds the next patch file at `quiesce;`, places the code
within 2 GB of the image, fixes its addresses, and writes a jump over the
no-op of each replaced function. A live build runs within 0.4% of a normal
build (geometric mean over eight Suite 3 programs at `-O`).

`tests/live/live_oracle.ps1` checks the mechanism on every test program: it
puts a `quiesce;` at the top of `main`, builds a patch from the unchanged
source (which must say "No function changed"), then builds one that replaces
every function, and the program's output must equal a normal build's. With
`-Loops` it puts the `quiesce;` at the top of each loop in `main`, so frames
move in the middle of loops; `-Optimize` builds everything with `-O`. It runs
on Windows and on Linux.

The design, and the plan from representations to rewinding a crash, is in
[the design](design/live.md).
