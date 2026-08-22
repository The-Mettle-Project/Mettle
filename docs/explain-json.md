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
| `source` | string | The entry file |
| `changes` | object | What moved since the last build |
| `remarks` | array | Every optimization decision |
| `startHere` | array | The decisions worth acting on first |
| `safety` | object | `--safe` checks that survived elision |
| `memory` | array | Memory diagnostics for this file |
| `backend` | object | How much of the program reached the register-allocating backend |
| `functions` | array | Per-function totals |
| `loops` | array | Per-loop cost model |
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
| `quantities` | object | Numbers behind the verdict, such as `calleeInstructions` |

`mettle explain <code>` prints the long form of any `code` value.

```json
{
  "kind": "loop",
  "fn": "main",
  "line": 7,
  "positive": false,
  "headline": "NOT vectorized",
  "reason": "the loop fills the stack array `xs`, whose address is retaken on every iteration; the fill kernel indexes off one invariant base pointer, and a fresh base each iteration is not one",
  "fix": "bind the array to a pointer once before the loop (`var p: int32* = &xs[0];`) and write `p[i]` in the body",
  "code": "store-only-fill",
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

`baseline` is true on a first build, when there is nothing to compare against.
Otherwise `entries` lists what improved and what regressed since the last
build of the same file.

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
claimed it), and `estimated`.

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
static elision could not remove, which is where the run-time cost lives.

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

- [Compilation](compilation.md)
- [Diagnostics](diagnostics.md)
