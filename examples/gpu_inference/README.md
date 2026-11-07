# A decode step on the GPU

One transformer feed-forward block, end to end, in Mettle: RMS normalization,
the gate and up projections, SwiGLU, the down projection, and a softmax. Six
launches, the shape a decode step actually has.

There is no `nvcc`, no `cudart`, and no LLVM anywhere in this. The kernels are
Mettle source compiled straight to PTX by Mettle's own backend, or to Metal
Shading Language on a Mac; the host is an ordinary Mettle program that links
`nvcuda`, the OS driver, or runs on Metal.

## Running it

```bash
mettle --gpu-info

mettle -O --emit-ptx decode_kernels.mettle -o decode_kernels.ptx \
  --emit-kernel-decls=decode_decls.mettle --report-occupancy

mettle --build --release decode_host.mettle -o decode_host.exe

./decode_host.exe
```

Neither compile names a GPU architecture. `--emit-ptx` asks the driver what is
in the machine and targets that; `mettle --gpu-info` prints the same answer
before you build anything.

On a Mac the kernels go to Metal and the host is unchanged:

```bash
mettle -O --emit-metal decode_kernels.mettle -o decode_kernels.metal \
  --emit-kernel-decls=decode_decls.mettle

mettle --build --release decode_host.mettle -o decode_host

./decode_host
```

`gpu_open_kernels("decode_kernels")` loads `decode_kernels.ptx` under CUDA
and `decode_kernels.metal` under Metal.

On an RTX 5060 Ti:

```
device   NVIDIA GeForce RTX 5060 Ti
         36 SMs, compute 120, CUDA 12.9
layer    dim 1024 -> hidden 2816, 200 steps

correct  every stage within its float32 error bound, worst at 82.660693% of it

per step launched   0.06207 ms
per step replayed   0.050183 ms  (one captured graph)
```

## What each part is showing

**The output is checked, not asserted.** The host reads every stage back and
recomputes it in float64 from the GPU's own inputs. Each element must land
within the worst-case float32 error bound for its stage: for a dot product of
K terms that is about K·2⁻²⁴ times the sum of the terms' magnitudes, which
holds for any summation order. A subgroup reduction adds in a different order
than a CPU loop, and the bound does not care; a kernel that drops a term or
misrounds a constant lands far outside it. The line reports the worst
element's share of its bound.

**`--emit-kernel-decls` writes the host's declarations.** `decode_decls.mettle`
is generated from the kernels, and the host imports it rather than restating
it. Every `dispatch` is checked against the kernel as compiled, block shape
included; change a signature without re-emitting and the host stops building.

**`work:` sizes the grid from the kernel.** `matvec` is declared
`kernel(block = 256, per = warp)`: one output row per subgroup, so
`dispatch matvec[work: rows]` divides by subgroups per block rather than by
threads per block. The host never writes `(n + 255) / 256`.

**Launch graphs pay for themselves.** Six launches whose shapes never change
from one token to the next is exactly what stream capture is for. The capture
happens once; each later step is a single submission, and about a quarter of
the per-step time goes away.

**`--report-occupancy` says what the kernels cost.** With `ptxas` installed it
reports registers per thread, resident warps, and how many blocks it takes to
fill this specific card:

```
matvec: 26 registers, block 256 (8 warps/block, 6 blocks) -> 48/48 resident warps (100%); full card = 216 blocks (36 SMs x 6)
```

## Files

| File | What it is |
| --- | --- |
| `decode_kernels.mettle` | The four kernels, compiled to PTX |
| `decode_decls.mettle` | Generated host declarations; do not edit |
| `decode_host.mettle` | Buffers, the launch chain, the CPU reference, the timings |
