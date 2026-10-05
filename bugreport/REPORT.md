# Bug: --release drops a loop's guard on a store through a loaded index

Found 2026-10-03 in ip Webcraft/inference (expert store placement,
src/experts.mettle). Repro: `guarded_indirect_store.mettle` (30 lines).

```
mettle --build --release guarded_indirect_store.mettle -o rel.exe   -> taken = 128
mettle --build           guarded_indirect_store.mettle -o dbg.exe   -> taken = 54 (correct)
```

Shape: `while (r < n) { var cell = (int64)order[r]; if (r < q) { mark[cell] = 1; taken = taken + 1; } r = r + 1; }`
where `order` was filled by a call (`fill`) beforehand. In release the
`if (r < q)` body runs on every iteration (taken == n). Replacing
`order[r]` with `r` (no load feeding the store index) makes it correct.

`mettle --build --release --verify` on the full program reported
"translation validation: OK - 8099 pass applications validated ... 2317
skipped", so the offending pass is among the skipped ones or the
validator's input sets never take the false arm.

Compiler: G:/Projects/Mettle/bin/mettle.exe (v0.17.0, HEAD 9e0147c3).
Workaround in the inference repo: hoist the index load into the guarded
branch.

## Still failing after c00c6e7c: nested guard inside the guarded block

`guarded_indirect_store_nested.mettle`: the same loop, but the guarded
block holds a nested guard:

```
while (r < E) {
  var cell: int64 = (int64)order[r];
  if (r < q) {
    inv[cell] = 1;
    if (r < core) { inv[cell] = 5; }
    nv = nv + 1;
  }
  if (r >= core) { inv[cell] = inv[cell] | 2; homes = homes + 1; }
  r = r + 1;
}
```

release: `nv = 128` (want 32); debug: `nv = 32`. homes and the core marks
come out right, so it is the outer `r < q` guard that is lost.

## Also after c00c6e7c: data-dependent guard holding a nested search + break

`guarded_store_nested_search.mettle`: release prints `moves = 7`, debug
`moves = 32` (correct). Shape (the expert store's rebalancer fill loop):

```
while (r < n && moves < 1000) {
  var cell: int64 = (int64)ord[r];
  if (cur[cell] == home[cell]) {
    while (free_slot < nslots && occ[free_slot] >= 0) { free_slot = free_slot + 1; }
    if (free_slot >= nslots) { break; }
    cur[cell] = (int64)(5000 + free_slot);
    occ[free_slot] = (int32)cell;
    moves = moves + 1;
  }
  r = r + 1;
}
```

The same loop without the inner while/break (`datagate` shape) is
correct in release. In the real program the effect was two experts
written into one slot: corrupt weights, silently different output.
