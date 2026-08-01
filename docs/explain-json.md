# The `--explain-json` schema

`mettle --release --explain-json <file>` writes `<output-stem>.explain.json` beside the output: a
machine-readable account of what the compiler did to that file. It is what editors and analysis
tools read; the prose report on stderr says the same things to a person.

```bash
mettle -i app.mettle -o build/app.obj --release --explain-json
```

The report covers the **focus file** only. Imported modules compile into the same program but their
loops and calls are not the file you asked about, so they are filtered out.

## Design

Two rules the schema keeps:

- **The prose is for people; the ids are for tools.** Every decision carries a stable `code`
  alongside its `reason`. Wordings get improved as we learn how to say things better; ids do not
  move. Anything matching on the English text will break, and deserves to.
- **Additive versions.** `schema` is `2`. Every schema 1 key still means exactly what it did, so a
  consumer written against 1 keeps working. New information arrives as new keys.

## Top level

| Key | |
|--|--|
| `schema` | Format version. Currently `2`. |
| `source` | Basename of the focus file. |
| `changes` | What flipped since the previous explain build of this file. |
| `startHere` | The top findings that have a fix, ranked the way the prose report ranks them. Each entry carries a `kind`: `backend` for a whole function that missed the register allocator (fields `fn`, `instructions`, `why`, `fix`), `remark` for a loop or call decision (fields `fn`, `line`, `code`, `fix`, `proven`, `stillBlocked`, `depth`, `sites`). `backend` entries come first: they are a measured cost over a whole function, where a remark is a prediction about one loop. |
| `remarks` | Every optimizer decision: loops, calls, branches, allocations, contracts. |
| `functions` | One row per function: weight in and out, decisions, backend outcome, cost. |
| `loops` | What the backend measured about each loop: cycles, bottleneck port, depth. |
| `callGraph` | Caller/callee edges with sites inlined, sites refused, callee weight. |
| `hotspots` | Every decision with a modelled cost, heaviest first. |
| `passes` | What each optimization pass did to this file. |
| `memory` | Compile-time memory-safety findings. |
| `backend` | Register-allocation coverage, grouped by why a function missed it. |
| `stats` | The one-line totals. |

## `remarks`

One entry per decision the optimizer made.

| Field | |
|--|--|
| `kind` | `loop`, `call`, `function` or `other`. |
| `fn` | The function the decision was made in. |
| `entity` | `loop`, ``call to `f` ``, `branch`, `allocation`, ... |
| `line`, `column` | 1-based position of the construct. |
| `endLine` | Last line the construct covers, so an editor can highlight a whole loop. Absent when unknown. |
| `positive` | `true` when the optimizer did the good thing. |
| `headline` | The one-line verdict. |
| `reason` | Why, when it declined. May be null. |
| `fix` | What to change. May be null. |
| `verified` | Set only when the compiler **applied that fix to a clone, re-ran its own optimizer, and confirmed the result**. Never a guess. The kernel it names is the one the edit produces: each simulation is pinned in the test suite against the same loop written by hand. |
| `stillBlocked` | Set when the compiler applied that fix to a clone and the loop **still** did not vectorize: the text is the obstacle that surfaced next. `fix` remains worth making, and it is not the whole job. Mutually exclusive with `verified`, and shown as `step 1` in the prose plan. |
| `code` | The stable decision id. See below. |
| `callee` | For calls, the callee's name. |
| `depth` | Loop nest depth, 1 for a top-level loop. |
| `trivial` | `true` for housekeeping a reader can collapse: inlining a one-line stdlib wrapper. |
| `advisory` | `true` when `fix` says there is nothing to change: the loop is at its floor, or the gap is the compiler's. Not work to do, and never ranked in `startHere`. |
| `quantities` | Whatever the pass measured: `calleeInstructions`, `iterations`. |
| `count`, `lineEnd`, `calls` | Present when a run of identical refusals was folded into one entry. |

### Decision ids

Every id has a page of its own. `mettle explain <id>` prints what the compiler saw, whether it
is a gap in the compiler or a fact about the code, and what to change. `mettle explain list`
indexes them. The prose report prints the id in brackets after each verdict, so the line and its
long form are one command apart.

Vectorization refusals use the analyzer's own diagnosis ids:

`call-in-body`, `extern-call-in-body`, `indirect-call`, `alloc-in-body`, `inline-asm`,
`control-flow`, `early-exit`, `int16-elements`, `int64-elements`, `serial-recurrence`,
`mixed-float-widths`, `byte-sum-narrow-acc`, `int32-sum-narrow-acc`, `inlined-param-local`,
`body-local`, `dot-shape-address`, `store-only-fill`, `unrecognized-shape`

Inlining refusals: `callee-no-body`, `callee-noinline`, `callee-denylisted`,
`too-many-parameters`, `callee-parameter-names`, `callee-over-budget`, `callee-call-count`,
`callee-inline-asm`, `callee-has-loop`, `callee-no-return`, `callee-has-kernel`, `recursive`,
`caller-over-budget`, `argument-count`, `rounds-exhausted`

Positive outcomes: `vectorized`, `vectorized-inner`, `outer-of-nest`, `eliminated`, `inlined`,
`unrolled`, `hoisted`, `if-converted`, `prefetched`, `layout-optimized`, `noalloc-verified`

A new diagnosis adds an id. It never hides under an existing one.

## `startHere`

Line order answers "what happened". This answers "what do I change", and an editor showing a
fix-it panel should not have to re-derive the order or guess at the tie-breaks.

| Field | |
|--|--|
| `fn`, `line` | Which finding. Join onto `remarks` by (`fn`, `line`). |
| `code` | Its stable decision id. |
| `fix` | The full suggestion, untruncated (the prose report cuts it to fit a line). |
| `proven` | `true` when the compiler applied the fix to a clone, re-ran its own optimizer, and confirmed it. |
| `depth` | Loop nest depth, a sort key after `proven` and specificity. |
| `sites` | How many findings this entry stands for. One line per distinct piece of advice: four loops needing the same change are one decision and four edits. Advice, not code, because one code can cover several causes with different fixes. |

At most five entries, one per distinct fix, and empty when nothing in the file has a fix. Advice
that says there is nothing to change (`advisory`) is never here. Unlike the prose report, this array
ignores `--explain=SELECTOR`.

## `functions`

The report's spine: what the whole pipeline achieved on each function.

| Field | |
|--|--|
| `fn`, `line` | Name and where it starts. |
| `instructionsBefore`, `instructionsAfter` | Non-nop IR weight entering and leaving the optimizer. The difference is what the pipeline achieved. |
| `loops`, `loopsVectorized` | Loop decisions recorded against it. |
| `callsInlined`, `callsRefused` | Call decisions. |
| `backendOk`, `backendReason`, `backendInstructions` | Whether it reached the register-allocating backend, and the gate's reason when it did not. |
| `spills`, `regsUsed` | From the register allocator. |
| `throughput`, `hotCost` | Summed reciprocal throughput and its loop-depth-weighted form, both in centicycles. |
| `vectorOps`, `estimatedSpans` | Vector ops emitted; spans whose cost fell back to an opcode estimate. |

## `loops`

Every loop the backend measured, whether or not the optimizer had anything to say about it. Join
onto `remarks` by (`fn`, `line`).

| Field | |
|--|--|
| `fn`, `line`, `endLine`, `depth` | Which loop. |
| `cyclesPerIter` | **Centicycles**: `720` is 7.2 cycles per iteration. Modelled from the port pressure of the emitted instructions, not measured. |
| `bottleneck` | The execution port the model says saturates first. |
| `hasKernel` | A SIMD kernel is present in the body. |
| `estimated` | Some span's cost came from an opcode estimate rather than a measured table entry. |

## `hotspots`

Every decision with a number on it, sorted by `cost` descending. Line order buries the loop that
costs the program its afternoon under a cold one-liner; this is the order to show a user.

| Field | |
|--|--|
| `fn`, `line`, `kind` | Which decision. |
| `code` | Its stable id, when it has one. |
| `cost` | Loops: modelled cycles times nest weight. Calls: callee weight times the nest weight of the loop containing the site. Ten per nest level, capped at three levels. |

A static proxy, not a measurement — nothing here has run the program. With `--pgo` the frequencies
are measured and the ranking follows them.

## `passes`

What each optimization pass did to this file, heaviest first.

| Field | |
|--|--|
| `pass` | Pass name, as `METTLE_SKIP_PASS` and `METTLE_TIME_IR_PASSES` spell it. |
| `runs`, `changedRuns` | Times it executed, and times it reported a change. The fixpoint driver re-runs passes until nothing moves. |
| `instructionsRemoved` | Net non-nop instructions removed across those runs. Negative means the pass added instructions, which vectorization and unrolling legitimately do. |
| `effects` | Per-opcode net change, keyed by the mnemonic `ir_opcode_name` gives. Positive removed that opcode, negative introduced it. |
| `sites` | Up to twelve `{fn, line, delta}` entries, heaviest first: where the pass moved instructions. |

`effects` and `sites` come from diffing the function's shape either side of every
pass run, so they describe what a pass did without the pass having to report it:

```json
{ "pass": "simd_affine_map_float", "runs": 19, "changedRuns": 4,
  "instructionsRemoved": 51,
  "effects": { "binary": 29, "load": 7, "store": 5, "label": 5, "jump": 5,
               "branch_zero": 5, "simd_affine_map_f32": -5 },
  "sites": [ { "fn": "saxpy", "line": 13, "delta": 8 },
             { "fn": "with_call", "line": 20, "delta": 4 } ] }
```

That reads as: the vectorizer replaced 29 scalar arithmetic instructions, 7 loads, 5 stores and a
loop's worth of control flow with 5 SIMD kernel instructions, at `saxpy:13` and `with_call:20`.

## `callGraph`

One edge per caller/callee pair: `caller`, `callee`, `inlined`, `refused`, `calleeInstructions`.

## `changes`

`baseline` is false on the first explain build of a file, true afterwards. `entries` carries every
decision that flipped: `kind`, `fn`, `line`, `direction` (`improved` or `regressed`), and the
`reason` for regressions.

The baseline lives beside the output as `<stem>.explain.base`, so keeping a stable output path is
what makes this section work.

## `memory` and `backend`

`memory` carries the compile-time memory-safety findings for the file (`severity`, `line`,
`headline`, `fix`). `backend` reports register-allocation coverage: `ok`, `total`, `instructions`,
`okInstructions`, and `groups` of functions that missed it, each with the `reason`, the
`consequence`, a `fix`, and its `members`. A group's `fix` carries `advisory: true` when it says
nothing needs doing, matching the `note:` the prose report prints for it.

Whether a group is advisory turns on how big its functions are. A SIMD kernel the allocator cannot
pass through costs only the scalar code around it, which on a small function is not worth an edit;
past 64 optimized IR instructions the same fallback is the largest single cost in the file, so the
advice becomes an instruction and the group is ranked into `startHere`. Each member carries a
`kernelLine` when the report can say which loop the kernel came from, including the case where the
function never wrote one: inlining brings a callee's vectorized loop in with it, and the member
line then names the call instead.

## Consumers

- The CLion plugin in the [MettleMisc](https://github.com/The-Mettle-Project/MettleMisc)
  project reads all of it.
- `tests/run_tests.ps1` pins the shape in `explain_changes_and_json`.

## See also

- [Compilation](compilation.md) for `--explain`, `--annotate-asm` and the other report flags.
- [Control Flow](control-flow.md#vectorization-contracts) for `@simd` and what makes a loop
  vectorizable.
