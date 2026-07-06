# GPU Offload

Mettle can compile functions to **NVIDIA PTX** and run them on the GPU through
the CUDA Driver API, with no `nvcc`, no `cudart`, and no LLVM. Kernels are
written in Mettle, compiled to a `.ptx` module with `--emit-ptx`, and launched
from a normal Mettle host program via the [`std/gpu`](standard-library.md#stdgpu)
bindings and the `dispatch` statement.

The model is **two-stage and explicit**: kernels live in their own file, the
host manages device memory itself, and `dispatch` only performs the launch. This
mirrors how real GPU code manages persistent VRAM.

## Writing a kernel

A kernel file is compiled with `mettle --emit-ptx`. Use the `kernel` keyword for
GPU entry points (it parses like `fn` and is emitted as a PTX `.entry`):

```mettle
// kernels.mettle  ->  mettle --emit-ptx kernels.mettle -o kernels.ptx
kernel vadd(a: float32*, b: float32*, c: float32*, n: int32) {
  var i: int32 = block.x * block_dim.x + thread.x;
  if (i < n) {
    c[i] = a[i] + b[i];
  }
}
```

### Index built-ins

Inside `--emit-ptx` compiles, the GPU thread/block indices are built-in member
expressions that mirror CUDA:

| Mettle        | CUDA          | PTX special register |
|---------------|---------------|----------------------|
| `thread.x`    | `threadIdx.x` | `%tid.x`             |
| `block.x`     | `blockIdx.x`  | `%ctaid.x`           |
| `block_dim.x` | `blockDim.x`  | `%ntid.x`            |
| `grid_dim.x`  | `gridDim.x`   | `%nctaid.x`          |

`.x`, `.y`, and `.z` are all available. The canonical global-thread index is:

```mettle
var i: int32 = block.x * block_dim.x + thread.x;
```

These built-ins are only active under `--emit-ptx`, so member access on an
ordinary struct named `block` in a CPU program is unaffected.

### Supported kernel constructs

Kernels use the same syntax as CPU code: arithmetic, comparisons, `if`/`while`,
pointer indexing, casts, and a set of GPU math intrinsics declared as `extern`:
`sqrtf`, `rsqrtf`, `fabsf`, `sinf`, `cosf`, `logf`, `expf` (lowered to PTX
`sqrt.rn` / `rsqrt.approx` / `ex2.approx` etc.), plus `h2f` / `f2h` for fp16
conversion. The PTX backend is validated by round-tripping emitted PTX through
`ptxas` and by differential execution against a CPU reference on real hardware.

## Launching from the host

The host is a normal Mettle program. Import `std/gpu`, set up device buffers
explicitly, then launch with `dispatch`:

```mettle
import "std/io";
import "std/mem";
import "std/gpu";

fn main() -> int32 {
  if (gpu_init() == 0) { println(cstr("GPU init failed")); return 1; }

  // load the emitted PTX and resolve the kernel
  var fp: cstring = fopen(cstr("kernels.ptx"), cstr("rb"));
  var ptx: uint8* = (uint8*)malloc(65536);
  var len: int64 = fread((cstring)ptx, 1, 65535, fp); fclose(fp); ptx[len] = 0;
  var mod: int64 = gpu_module(ptx);
  var vadd: int64 = gpu_func(mod, cstr("vadd"));

  var n: int32 = 1 << 20;
  var bytes: int64 = (int64)n * 4;
  var ha: float32* = (float32*)malloc(bytes);
  var hb: float32* = (float32*)malloc(bytes);
  var hc: float32* = (float32*)malloc(bytes);
  var i: int32 = 0;
  while (i < n) { ha[i] = (float32)i; hb[i] = (float32)(2 * i); i = i + 1; }

  // device buffers (you own VRAM)
  var da: int64 = gpu_malloc(bytes);
  var db: int64 = gpu_malloc(bytes);
  var dc: int64 = gpu_malloc(bytes);
  gpu_to_device(da, (uint8*)ha, bytes);
  gpu_to_device(db, (uint8*)hb, bytes);

  // launch: one line replaces param-packing + cuLaunchKernel + sync
  dispatch vadd[(n + 255) / 256, 256](da, db, dc, n);

  gpu_to_host((uint8*)hc, dc, bytes);
  gpu_free(da); gpu_free(db); gpu_free(dc);
  return 0;
}
```

### The `dispatch` statement

```
dispatch KERNEL[grid, block](arg0, arg1, ...);
```

- `KERNEL` is a handle (the `int64` returned by `gpu_func`).
- `grid` and `block` are integer expressions: the number of blocks and the
  number of threads per block (1-D).
- The arguments are passed by value. Device pointers are `int64` handles; scalars
  (`int32`, `float32`, …) are forwarded with their natural width.

`dispatch` desugars to argument marshalling plus a call to `gpu_launch`, which
issues `cuLaunchKernel` and then `cuCtxSynchronize`. It is **launch-only**:
device allocation and host/device copies remain explicit (the `gpu_malloc` /
`gpu_to_device` / `gpu_to_host` calls above).

## Building

```bash
# 1. compile the kernels to a PTX module
mettle --emit-ptx kernels.mettle -o kernels.ptx

# 2. build the host, linking the CUDA driver import stub (build-time only)
mettle --build host.mettle -o host \
  --link-arg "<CUDA>/lib/x64/cuda.lib"        # Windows: cuda.lib; Linux: -lcuda
```

The host links `nvcuda` (the OS driver), exactly as a Mettle program links
`kernel32` or libc; there is no bundled CUDA DLL. At run time the driver JITs
the PTX to SASS for the installed GPU.

## Notes and limits

- The emitter targets `.target sm_90`, which is forward-compatible: the driver
  JITs it to newer architectures (e.g. sm_120 / Blackwell).
- `dispatch` grids are 1-D for now (`grid`, `block`). Multi-dimensional launches
  go through `cuLaunchKernel` in `std/gpu` directly.
- Kernels and host code live in **separate files** (the kernel file is compiled
  with `--emit-ptx`; the host with `--build`).

See `examples/gpu_vadd/` for the complete, runnable version of the program above,
and `examples/llm/qwen3/gpu/` for a full set of LLM inference kernels.
```
