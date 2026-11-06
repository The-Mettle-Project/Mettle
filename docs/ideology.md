# What is wrong with systems programming

This explains what Mettle is and what it refuses to do. Read it before you
propose a change to the language.

Around 1980 you could stop a running program on a Lisp machine, redefine a
function, look at any object in memory, and continue. Today you edit a file,
rebuild, restart, and walk the program back by hand to the state that showed the
bug. That is a regression, and it has lasted forty years.

The excuse is speed, and the excuse is wrong. A Mettle `--live` build runs within
0.7% of a normal build, geometric mean over eight Suite 3 programs.

The rest of systems programming isn't much better.

## What's wrong

Start with restarting. C, C++ and Rust stop at link time. After that the only way
to change the program is to kill it, and the state dies with it. If a bug takes
eleven thousand frames to reach, every idea you test costs you those frames, so
you test fewer ideas.

The toolchain is a pile of unrelated programs. A C++ program with GPU kernels
means C++, the preprocessor, CMake, a linker script, CUDA C or GLSL, assembler
syntax and CI YAML. That is seven languages for one program. None of those tools
shares a model of what you're building. The compiler doesn't know what the linker
decided, and when the tools disagree you debug it by hand.

The compiler is a black box. `inline` is a hint. A loop you needed vectorized can
stay scalar with no message, and you find out from a profiler a month later. If
you need a new data layout or a new machine concept, you can write a proposal to a
committee that ships every three years, or you can fork millions of lines of LLVM.
Neither is a serious answer.

C++ got destructors right. Tie a resource to a scope and it gets released on every
path out, early returns and exceptions included. Rust kept them and added moves. The
trouble is that you can't see any of it. The compiler decides where each destructor
runs, which values move, which get copied and how long each temporary lives, and it
writes none of that down. To find a destructor call you work the language rules in
your head or read the disassembly. Every implicit feature in both languages has the
same problem.

Metaprogramming is a mess. C macros can't see a type. C++ kept piling on
templates, SFINAE, `constexpr`, `consteval` and concepts, and removed none of
them. Rust has `macro_rules!`, procedural macros in their own crate working on
token streams, and `const fn`. The errors point at code nobody wrote. This is what
you get when compile-time code is something other than the language.

C and C++ optimizers assume your code has no undefined behavior, and they act on
it. They delete a null check or an overflow test because the standard says the bad
case can't happen. `restrict` is a promise the compiler uses and never checks.
Optimizer passes are trusted because of who wrote them, and when one is wrong you
find out in production.

Rust has the work backwards. Where its inference stops, you write lifetimes by
hand. Where its borrow checker can't follow a correct program, you rewrite the
program. Working that out is the machine's job.

"No runtime" is nonsense. C has `crt0`, static initializers, `atexit` chains and an
allocator with its own locks. C++ adds exception tables and static initialization
order. Rust adds a panic runtime and unwind tables. Go collects garbage and
preempts your goroutines. Swift counts references at run time and frees an object
wherever the last one goes away.

And all of it is designed for big teams. Modern languages try to stop a thousand
engineers from hurting each other. That's fine if you run a thousand engineers.
Mettle is for the one person who wants to understand and control the whole machine.

## What Mettle does

Mettle is a live world that runs at machine speed. You work by talking to a running
program: change it, ask it what it did, ask the compiler what it proved, change it
again.

The sections below are the rules. If a proposal breaks one, it doesn't go in.

### 1. Programs stay alive

Every program gets built so it can be changed while it runs. That comes from the
shape of the code: patchable function entries and a description of every layout. A
change can replace function bodies, signatures and layouts, move every frame on the
stack, convert heap objects in place, and undo itself. A program that crashed can
wait for the fix and continue from its last `quiesce`.

The cost of all this has to reach zero. Until it does, the number gets published.

### 2. One toolchain

Frontend, IR, optimizer, interpreter, code generators, assembler, linkers, debugger,
profiler and build live in one codebase and share one model of your program. Code
generation never goes through LLVM, Cranelift or an external assembler, even as a
reference. Instructions are built from the ISA.

Everything else here depends on this. A checked live patch needs a linker,
translation validation and an interpreter in one place. Showing that a runtime
component is absent needs a linker that knows what it left out. Recording every drop
the compiler inserts, down to the instructions it became, needs the frontend and the
backend in one place.

The toolchain stops at the operating system.

### 3. Leave the outside world alone

C libraries, vendor drivers and operating systems are good at their jobs, and Mettle
has no reason to own them. Calling them is first class and it's staying that way.

`--explain` lists every `extern` whose effects were taken on its word.
`--check-effects` and the interpreter catch one that lied.

### 4. One language

Metaprograms, rules, schedules, kernels, target descriptions and machine descriptions
are all Mettle, checked by one type checker. There is no preprocessor, macro grammar,
build DSL, annotation language or shading language. A metaprogram is a normal function
that runs earlier. If it needs a capability, add that capability to Mettle.

### 5. The compiler is open

Any decision the compiler makes for you should be one you can make from Mettle. That
means data layout, field access, dispatch, allocation, IR, passes and register
allocation, with no plugin ABI and no fork. If you need an abstraction, a rule, a
type, an execution model or a machine concept, you build it, and the compiler handles
it the way it handles its own.

This means compiler internals can break your code. When one changes, the release
notes say so.

Generated code has to stay readable. `mettle expand` prints it as normal Mettle you
can diff. Every generated node points back to what you wrote, in diagnostics and in
`trace`. Expansion is hygienic, and a metaprogram declares the files it reads.

### 6. The compiler does what you say

It does what you tell it and reports what happened. When it can't vouch for something:

- If it can prove you're wrong, it stops and shows the evidence. You can overrule it
  in source, and the override stays on the record.
- If it doesn't know, it goes ahead and says what it couldn't check.
- If a contract you wrote isn't met, it stops at the site that broke it.
- If a change to a running program fails its checks, the change doesn't land and the
  program keeps running.

`@simd!`, `@inline!`, `@noalloc`, `@explicit`, `where cycles < N` and a declared
type's predicate are contracts. You asked for them. If you don't want one, delete it.

### 7. Everything gets checked

The compiler says nothing it hasn't checked. Every other rule here depends on this
one.

A learned optimizer, a metaprogram, a pass you wrote and a patch sent to a running
process are all untrusted, and the pipeline checks what comes out of each.
`--verify` executes every pass on every function, before and after, and reports a
divergence with a runnable counterexample. `--explain` simulates a suggested fix
before printing it. Anything that goes unchecked gets reported by name.

Contracts run on the final program. A metaprogram can't forge one, and a rule can't
exempt its own output.

An optimization has to name its proof and the pass that used it. Signed arithmetic
wraps, so there is no overflow to bet on. An assumption the compiler acts on needs a
flag you typed.

### 8. Types and proofs

You write function signatures, struct fields, effects and declared types. The compiler
works out lifetimes, moves, aliasing, allocation, purity, vectorization and effects
through the call graph. `@owned` and `@borrow` are never going in. Two pointers can't
be declared disjoint, because a declaration the compiler acts on and can't check is a
hand-written proof.

A binding can leave off its type where the line already says it. Nobody wants to
write the primitive of every variable at every point in time.

### 9. What the compiler inserts

The compiler writes some code for you. A value that owns a resource gets dropped when
its scope ends, on every path out. Passing it by value, returning it or storing it
moves it, and a moved value isn't dropped again. Assigning over one drops the old
value first. This is RAII as C++ has it, with Rust's moves.

Each of these is decided by the program text, and each is recorded with where it came
from: the declaration, the scope that ended, the move that cancelled the drop. You can
see all of them:

- `mettle expand` prints your source with every drop and move written out as normal
  Mettle.
- `--explain` lists each one with its line, its reason and what it compiled to.
- Your editor shows them on the lines where they happen.

Mark a function `@explicit` and the compiler inserts nothing in it. Every place it
would have dropped a value becomes an error that says what to write and where, and
the compiler can write it for you. The program means the same thing either way, so
`@explicit` code and implicit code mix freely, and moving code from one to the other
is mechanical.

Nothing else gets inserted. There are no collection points, implicit reference counts,
injected yields, scheduler ticks or work stealing. Each of those runs where run-time
state puts it, and no printout of the source can say where. There are no exceptions to
this, and extensions follow it too.

If other work has to happen, the source names the point. `quiesce` is the model: work
gets staged, the program reaches the point it named, and the work lands there. A
schedule is data the compiler reads, and what it generates prints as normal Mettle. A
live patch stops threads because you sent it.

`async` with a compiler-placed yield is out, for the same reason a garbage collector
is out. Mettle used to ship a collector, an async executor, a coroutine scheduler and
channels. They were deleted. The execution model belongs to your program, written in
Mettle, with its properties held by contracts, effects, declared types and rules.

### 10. No mandatory runtime

A runtime component is fine if all of this holds:

- It runs only because you asked.
- A build exists without it, and you can check that on the binary.
  `runtime_components_excisable` does this on every suite run.
- You can read it, step it and profile it, because it's written in Mettle and ships as
  source.
- You can replace it without touching the compiler. That covers the allocator, panic
  policy, trap handler, swap policy and clock.

Section 1 follows this too. A live program carries a shape in its code and no runtime,
and the loader arrives with the first change you send.

### 11. One interpreter

`mettle test`, `mettle trace`, `--verify`, `--pgo`, the `--ml-opt` gate, `@rule`,
computed globals, the leak checker and `mettle emulate` all run on the same IR
interpreter. Your tests and the optimizer's checks use the same semantics, so they
can't drift apart.

If a new feature can reuse the interpreter, it should. One that needs a new machine
has to be much better to justify it.

### 12. Speed

Mettle programs have to run as fast as C, live builds included. Features get measured
on whole applications against clang and gcc, end to end, from a fixed starting point.
The numbers are in [benchmarks](benchmarks/README.md).

CPU vectors, threads, GPU kernels, tiles, tensors and bare metal are one language under
one checker. A machine nobody has built yet can be described as a Mettle `const` and
programmed.

### 13. Mettle is big and hard

It is hard to learn and it's going to get bigger. Nothing gets cut for being difficult,
and no power gets hidden to protect a beginner. If what one expert can reach and what a
team finds comfortable conflict, the expert wins.

Diagnostics still have to be good. Every one carries a code, a caret and the evidence,
and `help:` lines get simulated before they print.

### 14. Build time

Metaprogramming can slow a build with nothing to show where the time went. Anything
that spends build time reports what it spent, and you can make the number a contract:
`--report-expansion` with `--expansion-budget=N`, `--report-rules` with
`--rule-budget=N`, `--report-proofs` with `--proof-budget=N`, `--report-effects` with
`--effect-budget=N`.

## Proposing a change

Go through these in order. The first failure ends it.

1. Does it run anything at a point the program text doesn't decide, or insert anything
   that `mettle expand` can't print and `@explicit` can't refuse? (section 9)
2. Does it add a language that isn't Mettle? (section 4)
3. Can a program that skips it show on the binary that it paid nothing? (section 10)
4. Does the pipeline check its output, or report the gap? (section 7)
5. Does it make someone write a proof the compiler could derive? (section 8)
6. Does it work in `--release`, at full speed? (section 12)
7. Can you read what it did as normal Mettle? (section 5)
8. Does it reuse something that already exists? (section 11)
9. Does it give you a decision the compiler used to make alone? (section 5)
10. What does it make unnecessary? Name something that gets deleted: a class of bug, a
    manual check, a tool in another language.

## What works today

- `mettle live` on Windows and Linux x64: bodies, signatures, layouts, frames, heap
  objects, undo, crash recovery.
- Compile-time reflection, `comptime for`, computed globals, `@rule` over the program,
  the machine and a trace, and `rewrite` rules checked in the interpreter.
- Declared types with proofs, inferred local types, lifetimes and effects, deadlines from
  a cost model, optimization contracts.
- Layouts, generators, optimizer passes and register allocators written in Mettle.
- PE and ELF linkers, x86-64 and AArch64 backends, GPU kernels as PTX, SPIR-V and Metal,
  freestanding targets, described machines.
- `--verify`, `--explain` with simulated fixes and a list of beliefs, `mettle expand`.
- Drops and moves for every type with a `drop` method, its fields, arrays and enum
  payloads, with live bits where a value moves on some paths only. `mettle expand`
  prints them, `--explain` lists them, the editor shows them, and `@explicit` refuses
  them with fixes.

## What doesn't work yet

- Every program alive by default. Today that takes a `--live` build and a `quiesce`
  point, and only on Windows and Linux x64. Single programs move up to 8% either way.
- Overriding a proven-wrong stop in source.
- Emitters and the order of the built-in passes. Nothing reaches those from Mettle.
- The loader arriving with the first change. A live build links it now.
- Snapshots and crash rewind.
- Heap strings made by interpolation and `+`. Older code in IR lowering frees them at
  scope exit and after calls, and records none of it.
- Moving the older runtime components to Mettle. The crash handler, profiler, safety and
  debug code are still C.

[Known limitations](known-limitations.md) has the full list, and
[bare metal](bare-metal.md) says what ran each machine claim.
