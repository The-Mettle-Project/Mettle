# Proofs

`verified_lowering.mettle` proves three things on every build:

- the shift-and-add multiplier equals `x * c`, by induction on `c` with a
  lemma the solver proves, and the optimizer uses that proof to replace
  calls to it with one multiply;
- the branchless absolute value equals the branchy one for every `int32`;
- a hash bucket index is below the bucket count wherever it is returned.

`ring_buffer.mettle` proves facts about memory and floats:

- `push` and `pop` keep the ring's index in bounds, and the slot written
  through the pointer holds the value pushed after the count is updated;
- the mean of two floats computed as `a * 0.5 + b * 0.5` lies between them
  when both are normal numbers. Without that guard the prover finds
  `a = b = 2.2250738585072009e-308`, the largest subnormal: half of it rounds
  up to an even significand, and the two halves sum to the smallest normal,
  above both inputs.

```bash
mettle prove examples/proofs/verified_lowering.mettle
mettle run -O examples/proofs/verified_lowering.mettle
mettle run -O examples/proofs/ring_buffer.mettle
```

See [docs/proofs.md](../../docs/proofs.md).
