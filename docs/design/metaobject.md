# Metaobject protocol

Struct layout, field access, allocation, dispatch and generated functions are
decided by Mettle functions that run while compiling. A library can write `@soa`,
hot/cold splitting, tagged pointers, its own vtables, `@memoize`, tracing and
`grad(f)`, and the code they produce goes through the same checks as code you
typed.

A metafunction returns one of two things:

- A `Layout`, which is data. The compiler generates every access from it.
- Mettle source text. The compiler parses it and checks it with the program.

Neither can produce something `mettle expand` cannot print.

## Stages

A metafunction has to run before the type checker can finish, and the
interpreter runs on lowered IR. So the compiler builds the metaprogram first, as
a smaller program, and runs it.

```mermaid
flowchart TD
  parse[parse, imports, monomorphize] --> scan{any decorator<br/>or generator call?}
  scan -- no --> check[type check]
  scan -- yes --> clone[clone the program, keep types and<br/>the functions metafunctions reach]
  clone --> stagecheck[type check and lower the clone]
  stagecheck --> run[run metafunctions in the interpreter]
  run --> validate[prove each Layout, parse each source]
  validate --> more{new decorators?}
  more -- yes --> clone
  more -- no --> check
  check --> lower[lower, rules, optimize, emit]
```

A program with no decorator pays for one walk over its declarations.

A metafunction may not reach a type whose layout depends on it. That is a cycle
and the build stops with the chain printed.

Metafunctions are dropped from the final program like rules, and normal code
cannot call them.

## Layouts

```mettle
import "std/layout";

@soa struct Particle {
  x: float32;
  y: float32;
  alive: bool;
}

fn total(ps: Particle[]) -> float32 {
  var sum: float32 = 0.0;
  for i in 0..ps.length { sum = sum + ps[i].x; }
  return sum;
}
```

`@soa` is a function in `std/layout`:

```mettle
@layout fn soa(t: TypeInfo, l: Layout) -> Layout { ... }
```

A layout function takes the type and the layout so far, plus the decorator's
arguments as `string[]` if it declares them. Stacked decorators run in order, and
the first one receives the default layout.

A `Layout` holds regions and places. A region is a run of bytes per element, with
a stride and an alignment. A place says where one field lives: its region and
offset, and for a packed field its word size, bit offset, bit width and left
shift.

```mermaid
flowchart LR
  subgraph a[Particle array of N]
    r0[region 0: x, stride 4] --> r1[region 1: y, stride 4] --> r2[region 2: alive, stride 1]
  end
  access["ps[i].y"] --> calc["base + size of region 0 times N + i times 4"]
```

The compiler proves a layout before it uses one. Every field has one place, no
two places share a bit, every place fits its region's stride, every byte place
sits at its field's alignment, and a packed value fits its width. That is a proof
over intervals, so no probing is involved. A layout that fails stops the build
with `L0002` at the struct.

What a layout means for the rest of the language:

- A lone value keeps its regions next to each other, so `sizeof`, `&p`, by-value
  calls and `p.x` work with one offset per field. `T[N]` takes the size the
  regions need for N elements.
- With more than one region, `a[i].f` needs the element count. Fixed arrays have
  it and slices carry it. `new T[n]` returns a slice.
- `T*` for such a `T` is a reference: 24 bytes holding the base, the index and the
  count. `&a[i]` makes one, `new T` makes one with a count of 1, and `0` is the
  all-zero reference. `e.f`, `*e`, `e[i]`, `e + n`, `e - f`, `==`, `free(e)` and
  methods work as they do on a pointer. `&a[i].f` is a plain pointer into a column.
  Reading or writing a whole element copies region by region.
- Every access through a reference checks its index against its count, in debug
  and under `--safe`, because a run past a column stays inside the allocation.
  `--release` drops the check. The borrow analyser reads `T*` by its spelling, so
  use after free and returning a stack address report as they do for a pointer.
- A reference does not cast to or from a thin pointer. There is no count to invent
  and no single address to hand over. `(T*)p` and `var e: T* = malloc(n)` stop with
  `L0003`, and so does assigning `s.data`, whose only reader is `free(s.data)`.
- A struct with a `T*` field for its own type needs the layout first and the
  reference size second, so the stage runs twice: once to learn which types have
  several regions, once to lay them out with their fields at 24 bytes.
- A packed field has no address, and `&p.f` stops with `L0003`. Its type stays
  what you wrote, so the borrow analyser and effects see a `Node*` where a tagged
  `Node*` is stored. Loads and stores become shift and mask.
- `extern` signatures and GPU kernels read memory by C rules, so a custom layout
  crossing either stops with `L0004`.
- The live descriptor prints the regions and places, so conversion between two
  layouts works through the same field names as every other struct.

A loop through references costs what a loop over columns costs. The reference is
a local struct, so SROA splits it into words, the words forward to the loads they
came from, and the column base hoists out of the loop with the index split off.
The recognizers then see the same `base + i * stride` a slice gives them. Summing
one field of 1048576 records, 100 passes, pinned, minimum of 15 interleaved runs:

```mermaid
xychart-beta
  title "Microseconds for 100 passes"
  x-axis ["hand columns", "@soa first field", "@soa second field", "T* indexed", "T* passed per element", "T* local per element", "T* stepped", "int32* stepped"]
  bar [8257, 8269, 8244, 8265, 8214, 8234, 22432, 22493]
```

Stepping a reference with `r = r + 1` stays scalar, and so does stepping a plain
`int32*`, at the same speed.

A field of type `T*` costs 24 bytes in place of 8. Chasing 65536 shuffled nodes:
plain struct with thin pointers 168087 us, `@hot_cold(key, next)` with references
170945 us. The port of `physics_grid` to `@hot_cold(x, y)` prints the same hash as
the struct version.

## Allocation

A `Layout` may name an allocator, `fn(bytes: int64, align: int64) -> rawptr`, which
returns zeroed memory, and a releaser, `fn(p: rawptr)`. `new T` and `new T[n]` call
the allocator and `free` on a `T*` calls the releaser. The compiler checks the
allocator's signature at each `new`.

## Generators

```mettle
@generate fn memoize(f: FunctionInfo) -> Generated { ... }

@memoize fn fib(n: int64) -> int64 { ... }
```

A generator returns Mettle source. With a decorator on a function, the compiler
renames the original to `f.inner_name`, so the source can define the original name
and call it. `grad(f)` is an ordinary call to a generator: the call site becomes
the name the `Generated` value carries.

`FunctionInfo` holds the signature, the locals and temporaries with their types,
every function it reaches, the structs those hold, and the body as a structured
tree: code, `if`, `loop`, `block`, `break`, `continue` and `return`. The tree
comes through the structurizer the Metal backend uses, so an irreducible function
is refused by name. `std/meta` prints the tree back as Mettle.

`@generate` reads the IR as lowered. `@generate(optimized)` reads it after the
whole optimizer ran on the target and everything it reaches, in a stage build of
its own. What the optimizer leaves is spelled in scalar operations before the
generator sees it: `select` and `rotate_add` become branches and assigns, each
SIMD kernel becomes the loop it stands for, and the names SSA leaves behind stay
as locals. So `render_function` and `grad` have one vocabulary. `f.kernels` lists
what was spelled. `simd_matmul_n32`, `inline_asm` and anything else with no scalar
form is refused by name, in the operation's own words.

`grad` reads optimized code. It builds the same tape either way, and on the same
inputs the two return the same bits. Pinned minimums on three loop-heavy
functions, optimized against `grad_unoptimized`:

```mermaid
xychart-beta
  title "grad_unoptimized = 100, lower is faster"
  x-axis ["pairs of structs --release", "pairs of structs no flags", "scalar loop and stencil --release"]
  bar [97.5, 98.2, 97.7]
```

The cost is 13% more compile time on the first of those and a 4.6% larger binary.

Parsed source runs through the type checker, borrow analysis, effects and
contracts like anything else. Each generated function records which decorator
wrote it, and `mettle expand` prints that above it. Diagnostics in generated code
point into the generated source.

Each round's output goes through the import resolver and the monomorphizer before
it joins the program. The resolver keeps the set of files it already merged, so a
generated import of a module the program holds adds nothing. The monomorphizer
keeps the generic definitions, traits and impls it removed from the program, so
generated code can instantiate a generic the program wrote and the program can
instantiate a generic a generator wrote. An instantiation already in the program
is dropped.

```mermaid
flowchart TD
  layouts[run layouts] --> expand[expand each comptime for<br/>that holds a decorator or generator call]
  expand --> gens[run generators]
  gens --> resolve[resolve imports, monomorphize,<br/>convert closures, append]
  resolve --> more{anything new?}
  more -- yes --> layouts
  more -- no --> check[type check the program]
```

A module-scope `comptime for` that names a type or table no round has written yet
is left out of that round's stage build and tried again in the next. The type
checker expands what is left, and reports anything still unknown by name. A
`comptime for` expanded early records its sequence and iteration on the
declarations it makes, so every later stage build and the final type check bind
the same names to their own types. If a stage build fails for the whole list of
generators, each generator runs alone, so one that needs another's output waits
for the next round.

## Gradients through memory

A float temporary carries its tape node in a shadow local. Memory carries nodes in
a table keyed by address, which a store and a load update. A struct passed by
value lands at a new address, so the differentiated function takes the caller's
address as an extra argument and copies the table entries for the struct's bytes
on entry. A returned struct names its address in `ad_return_address`, and the
caller copies from there straight after the call. A zeroing or integer store
clears the entries it overwrites, so a stale node from an earlier frame cannot
reach a fresh local.

## Checks on generated code

A generator states what it promises by writing a `@rule` with its code. The rule
calls the original and the generated function in the interpreter on probe inputs,
so the check runs on every build under the rule budget and never reaches the
binary. `grad` compares against finite differences, `@memoize` walks the call
graph for global writes and undeclared externs and compares results, and `@trace`
compares results.

A generator that writes no rule gets `L0005` as a warning naming it.

## Dispatch

A trait's vtable scheme is a generator on the trait. `@dyn trait Shape { ... }`
writes a fat pointer type, one table per implementation and the forwarding
functions as source. A table is a `const`, and `devirtualize_known_tables` turns a
call through a table it can trace within a block into a direct call.

## Passes

`@pass(point) fn p(f: PassFunction) -> PassFunction` runs a Mettle function on a
function's IR at a named point in the optimizer. A point is a stage (`pre_inline`,
`fixpoint`, `loop_canonical`, `recognizers`, `cleanup`, `ssa`, `end`) or
`after_<pass>` for any built-in pass, whether that pass fired or not. Passes run
only when the optimizer runs, so at `-O` and `--release`.

`std/pass` holds the data. A `PassOp` carries its opcode, operands and flags as
written, plus `origin`, the index of the instruction it came from. What the data
does not hold, such as a tensor descriptor or a volatile mark's kin, stays on the
instruction through `origin`. A new op has `origin` -1 and `place`, the instruction
whose source location it takes. Each temporary operand says how many times the
function reads it.

```mermaid
flowchart LR
  ir[function's IR] --> image[PassFunction in interpreter memory]
  image --> run[pass runs in the interpreter]
  run --> same{same code?}
  same -- yes --> done[nothing changes]
  same -- no --> shape[structure check]
  shape --> check[run old and new on generated inputs]
  check -- agree --> keep[keep it]
  check -- differ --> stop[L0007 with the counterexample]
```

The structure check and the run on generated inputs happen on every build, the
way a `@rewrite` rule is checked. A pass goes through the same driver as a
built-in pass, so `--verify`, `METTLE_SKIP_PASS`, the volatile audit and the value
checks all see it. A function the checker cannot run is counted, and
`--report-meta` prints how many. `mettle expand` runs the optimizer when the
program has a pass and prints each result as Mettle under the pass's name.

A pass cannot reach a metafunction or the program it rewrites. It reads
`PassFunction` and returns one.

### A built-in pass written this way

`or_chain_bitset` is `or_chain_bitset` in `std/pass`, and the C is gone. It turns
`x == 'a' || x == 'b' || ...` into a mask test. The optimizer scans for three
`branch_eq` in a row on one label, and only then loads `std/pass` as a small
program of its own, so a program with no such chain pays for the scan. Across
Suite 3 it fires in `json_parse` and `interp_ast`, and all 16 binaries are
byte-identical to those of the C pass. `json_parse` compiles in 164 ms against
139 ms, and programs where it does not fire compile as before.

## Register allocation

`@regalloc fn a(g: AllocGraph) -> Assignment` takes over colouring for the whole
program. For each function the compiler builds the exact interference graph and
coalesces moves as it always does, then hands the metafunction every vreg with
its class, width, the mask of registers it may use, the mask that cost a save,
its spill cost, a hint and its neighbours. The answer is one register per vreg,
or -1 to spill it.

```mermaid
flowchart LR
  live[liveness, interference, coalescing] --> graph[AllocGraph in interpreter memory]
  graph --> color[metafunction runs]
  color --> mask{register in the vreg's mask?}
  mask -- no --> stop[stop the build, naming the allocator]
  mask -- yes --> verify[MIR allocation verifier, always on]
  verify -- two live values share a register --> stop
  verify -- passes --> encode[encode]
```

The allocation verifier dataflows live values through the finished code, so it
trusts nothing the allocator said. `std/regalloc` holds `chaitin_briggs`, a plain
optimistic colourer. All 16 Suite 3 programs build with it under
`METTLE_MIR_VERIFY` and print what the built-in allocator's binaries print.
Functions too large for the graph allocator keep the built-in linear scan.

## Costs

`--report-meta` prints the layouts, generators, source bytes, stage builds and
interpreter steps, and `--meta-budget=N` stops the build past N steps. It prints
how many pass runs there were and how many results were checked.

## Error codes

- `L0001`: a metafunction could not run, or it returned something unreadable.
- `L0002`: a layout failed its proof.
- `L0003`: an operation needs an address or a count the layout does not have: a
  packed field's address, or a cast between a reference and a thin pointer.
- `L0004`: a custom layout crossed `extern` or a kernel.
- `L0005`: a generator stated no relation.
- `L0006`: a metafunction reaches a type whose layout depends on it.
- `L0007`: a pass changed what a function does, or wrote code the compiler cannot
  use.
