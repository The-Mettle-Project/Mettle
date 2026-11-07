# The --explain-json schema

`--explain-json` writes `<output-stem>.explain.json` beside the object or
executable: the same report [`--explain`](compilation.md) prints, in a form an
editor or an analysis tool can read. It implies `--explain`, which needs `-O`
or `--release`.

```bash
mettle --release --explain-json --build program.mettle -o program.exe
```

The current schema is version 2. A reader should check `schema` and refuse a
number it does not know.

## Top level

| Field | Type | Contents |
|-------|------|----------|
| `schema` | number | Schema version, currently 2 |
| `source` | string | The entry file's name |
| `path` | string | The entry file's absolute path |
| `changes` | object | What moved since the last build |
| `remarks` | array | Every optimization decision |
| `startHere` | array | The decisions worth acting on first |
| `safety` | object | `--safe` checks that survived elision |
| `proven_by_type` | array | Bounds checks a declared type removed |
| `payoffs` | array | Optimizations a declared type earned |
| `ownership` | array | Every drop, move and live bit, from [ownership](ownership.md) |
| `memory` | array | Memory diagnostics for this file |
| `layout`, `proofs`, `effects`, `rules`, `beliefs` | array | The ledger: layout facts, conversions proven, effects held, rules run, and what the build took on trust |
| `backend` | object | How much of the program reached the register-allocating backend |
| `functions` | array | Per-function totals |
| `loops` | array | Per-loop cost model |
| `machine`, `machineTotal` | array, number | The emitted instructions of every function in the file |
| `callGraph` | array | Caller and callee pairs with inlining outcomes |
| `hotspots` | array | The costliest sites |
| `passes` | array | What each optimizer pass did |
| `stats` | object | Whole-run counters |

## remarks

One entry per decision. This is the same material `--explain` prints.

| Field | Type | Contents |
|-------|------|----------|
| `kind` | string | `loop` or `call` |
| `fn` | string | The function holding the site |
| `entity` | string | What the decision was about, such as ``call to `scale` `` |
| `line`, `column`, `endLine` | number | Where it is |
| `positive` | bool | True when the optimizer did the thing |
| `headline` | string | The verdict in words |
| `reason` | string or null | Why it declined |
| `fix` | string or null | What to change |
| `verified` | bool or null | Whether the fix was applied to a clone and re-checked |
| `stillBlocked` | bool or null | Whether the fix leaves the loop blocked on something else |
| `callee` | string or null | For a call remark |
| `depth` | number | Loop nesting depth |
| `code` | string | The decision code, such as `store-only-fill` |
| `declined` | bool | True when the optimizer chose not to act and loses nothing by it, such as a call outside every loop kept as a call |
| `inlinedInto` | string | The function this copy of the code was inlined into; `fn` is then the function that wrote it |
| `quantities` | object | Numbers behind the verdict, such as `calleeInstructions` |

`mettle explain <code>` prints the long form of any `code` value.

```json
{
  "kind": "loop",
  "fn": "main",
  "line": 7,
  "positive": false,
  "headline": "NOT vectorized",
  "reason": "this is a histogram: the element each iteration updates is chosen by a value the loop reads, so two lanes of one vector can land on the same element and a vector store would keep only one of their updates. Catching that collision takes AVX-512's vpconflictd, and AVX2 has nothing like it",
  "fix": "nothing to change: the scalar loop is the floor for a scatter on this target",
  "advisory": true,
  "code": "scatter-store",
  "depth": 1,
  "column": 3,
  "endLine": 7
}
```

## startHere

The subset of `remarks` that carries a fix, ranked. Each entry has `kind`,
`fn`, `line`, `code`, `fix`, `proven`, `stillBlocked`, `depth`, and `sites`.

`proven` is the important one: true means the compiler applied that fix to a
clone and confirmed it worked.

## changes

`baseline` is true when an earlier build of the same file left a baseline to
compare against. `entries` then lists what improved and what regressed since
that build, each with `kind`, `fn`, `line`, `direction` (`improved` or
`regressed`) and, for a regression, `reason`. On a first build `baseline` is
false and `entries` is empty.

## functions

| Field | Contents |
|-------|----------|
| `fn`, `line` | Which function |
| `instructionsBefore`, `instructionsAfter` | IR instruction count around optimization |
| `loops`, `loopsVectorized` | Loop counts |
| `callsInlined`, `callsRefused` | Inlining outcomes |
| `backendOk` | Whether it reached the register-allocating backend |
| `backendInstructions`, `backendReason` | Size and, when it did not, why |
| `spills`, `regsUsed` | Register allocation results |
| `throughput`, `hotCost` | Cost model output |
| `vectorOps`, `estimatedSpans` | Vector work and estimated live spans |

## loops

`fn`, `line`, `endLine`, `depth`, `cyclesPerIter`, `bottleneck` (the port or
unit that limits it, such as `store`), `hasKernel` (whether a vectorizer
claimed it), `estimated`, and `trips` when the loop runs a constant number of
times. `cyclesPerIter` is in hundredths of a cycle, so `50` is half a cycle.
The Overview ranks loops by cycles times ten per nesting level, and a loop
with known `trips` counts that many iterations where an unknown one counts
ten. A loop the backend inlined from another file is left out, since its
lines belong to that file.

## machine

The machine code of every function in the file, as the backend emitted it.
Each element is `{"fn": name, "insns": [...]}`, and each instruction is an
array:

```text
[line, text, ports, cost, latency, kernel, depth, label, origin, offset]
```

- `line`: the source line it came from, or 0 when it came from another file.
- `text`: Intel syntax, decoded from the emitted bytes. Branches and calls keep
  their symbolic targets; a branch inside a vector kernel names the target's
  offset.
- `ports` and `cost`: the execution ports the cost model charges and its
  reciprocal throughput in hundredths of a cycle. A vector kernel's
  instructions carry neither.
- `latency`: cycles until the result is ready.
- `kernel`: 1 for an instruction inside a vector kernel.
- `depth`: loop nesting depth.
- `label`: 1 for a label row, whose `text` is the label's name.
- `origin`: `file:line` for an instruction inlined from another file.
- `offset`: the byte offset in the function.

`machineTotal` counts every instruction seen; `machine` stops at 60000.

## backend

`ok` and `total` count functions, `instructions` and `okInstructions` count IR
instructions, and `groups` explains each reason a function missed the
register-allocating backend:

```json
{
  "reason": "contains a call form the register allocator doesn't support yet",
  "functions": 1,
  "instructions": 109,
  "consequence": "every value in the function is kept on the stack instead of in registers",
  "fix": null,
  "members": [{"fn": "main", "instructions": 109}]
}
```

## passes

One entry per optimizer pass that ran: `pass`, `runs`, `changedRuns`,
`instructionsRemoved`, an `effects` breakdown by category, and `sites` naming
the functions and lines it touched with a `delta` each.

## safety

`enabled` says whether `--safe` was on. `survivors` lists the checks that
static elision could not remove, which is where the run-time cost lives: each
has `line`, `function`, `kind` (`extent`, `span` or `runtime`) and `why`.
`survivorsTotal` counts them all, since the list stops at its cap. With `--safe`
on, `accesses`, `proved`, `hoisted`, `spanned`, `exempt`, `extentTests` and
`regionCalls` give the totals.

## proven_by_type and payoffs

`proven_by_type` lists bounds checks a declared type removed: `line`, `fn`,
`type`, `min`, `max` and the array `length`. `payoffs` lists other
optimizations a declared type earned: `line`, `fn`, `type`, `what` and
`detail`. `provenByTypeTotal` and `payoffsTotal` count past the cap.

## ownership

One entry per record: `kind` (`scope-drop`, `parameter-drop`, `arm-drop`,
`assign-drop`, `discard-drop`, `temporary`, `glue`, `move`, `live-bit` or
`drop-runs`), `line`, `column`, `function`, `subject`, `type`, `text` (the Mettle
the compiler wrote), `reason`, `lowered`, `optimized` and `inserted`. The TUI
groups records into one value per subject, reading a temporary's name from its
`text`.

## expanded

The main file as the compiler rewrote it, the same source `mettle expand`
prints, with every inserted drop, defer and temporary. Each line starts with
byte 1, the source line it came from, and byte 2.

## The ledger

| Field | Entry fields |
|-------|--------------|
| `layout` | `line`, `fn`, `detail`: placement and layout facts the types state |
| `proofs` | `line`, `type`, `expression`, `route`, `consumer`: conversions into a declared type |
| `effects` | `fn`, `performs`, `needs` |
| `rules` | `rule`, `verdict`, `steps` |
| `beliefs` | `what`, `why`: claims the build took on trust |

Each has a `<name>Total` beside it, such as `beliefsTotal`, that counts past
the cap of 64 entries.

## memory

The [memory diagnostics](memory-safety.md) for this file, each with its code,
location, and message.

## stats

`loopsVectorized`, `loopsScalar`, `fixesVerified`, `callsInlined`,
`callsRefused`, `changesImproved`, `changesRegressed`, and `hadBaseline`.

## A related sidecar

`--annotate-asm` writes `<stem>.annot.json` alongside its printed report, with
the per-instruction codegen provenance. That is a different schema, and
[Compilation](compilation.md) covers the flag.

## See also

- [mettle explain](explain.md), which reads this file
- [Compilation](compilation.md)
- [Diagnostics](diagnostics.md)
