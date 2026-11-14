# Proofs

`verified_lowering.mettle` proves three things on every build:

- the shift-and-add multiplier equals `x * c`, by induction on `c` with a
  lemma the solver proves, and the optimizer uses that proof to replace
  calls to it with one multiply;
- the branchless absolute value equals the branchy one for every `int32`;
- a hash bucket index is below the bucket count wherever it is returned.

```bash
mettle prove examples/proofs/verified_lowering.mettle
mettle run -O examples/proofs/verified_lowering.mettle
```

See [docs/proofs.md](../../docs/proofs.md).
