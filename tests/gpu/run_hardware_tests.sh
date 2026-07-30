#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
COMPILER="$ROOT/bin/mettle"
CC_BIN="${CC:-cc}"
GPU_ARCH="auto"
REQUIRE_GB10=0
EXPERIMENTAL_TMA=0
SANITIZER=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --compiler)
      COMPILER="$2"
      shift 2
      ;;
    --cc)
      CC_BIN="$2"
      shift 2
      ;;
    --gpu-arch)
      GPU_ARCH="$2"
      shift 2
      ;;
    --require-gb10)
      REQUIRE_GB10=1
      GPU_ARCH="gb10"
      shift
      ;;
    --sanitizer)
      SANITIZER=1
      shift
      ;;
    --experimental-tma)
      EXPERIMENTAL_TMA=1
      shift
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ ! -x "$COMPILER" ]]; then
  echo "Mettle compiler not found or not executable: $COMPILER" >&2
  exit 2
fi
command -v nvidia-smi >/dev/null
command -v "$CC_BIN" >/dev/null
if [[ $EXPERIMENTAL_TMA -eq 1 &&
      "${MTLC_ALLOW_EXPERIMENTAL_TMA:-}" != "I_ACCEPT_GPU_RESET_RISK" ]]; then
  echo "Experimental TMA execution is quarantined. Run only on a disposable/recoverable host and set MTLC_ALLOW_EXPERIMENTAL_TMA=I_ACCEPT_GPU_RESET_RISK to acknowledge possible GPU reset or host loss." >&2
  exit 2
fi
if [[ $EXPERIMENTAL_TMA -eq 1 &&
      "${MTLC_TMA_RECOVERY_READY:-}" != "I_HAVE_OUT_OF_BAND_RECOVERY" ]]; then
  echo "Experimental TMA execution requires out-of-band recovery. Set MTLC_TMA_RECOVERY_READY=I_HAVE_OUT_OF_BAND_RECOVERY only when an independent watchdog/operator can reset or reboot the disposable host." >&2
  exit 2
fi

COMPUTE_CAPABILITY="$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader | head -n 1 | tr -d '[:space:]')"
if [[ ! "$COMPUTE_CAPABILITY" =~ ^([0-9]+)\.([0-9]+)$ ]]; then
  echo "cannot parse CUDA compute capability: $COMPUTE_CAPABILITY" >&2
  exit 2
fi
COMPUTE_MAJOR="${BASH_REMATCH[1]}"
COMPUTE_MINOR="${BASH_REMATCH[2]}"
if [[ "$GPU_ARCH" == "auto" ]]; then
  GPU_ARCH="sm_${COMPUTE_MAJOR}${COMPUTE_MINOR}"
fi
if [[ $REQUIRE_GB10 -eq 1 ]]; then
  if [[ "$(uname -m)" != "aarch64" || "$COMPUTE_CAPABILITY" != "12.1" ]]; then
    echo "GB10 gate refused before compilation: requires AArch64 + compute 12.1" >&2
    exit 2
  fi
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/mettle-gpu-hardware.XXXXXX")"
trap 'rm -rf -- "$TMP"' EXIT
PTX="$TMP/hardware_kernels.ptx"
MXFP4_PTX=""
MXFP6_PTX=""
TENSOR_TRANSFER_PTX=""
HARNESS="$TMP/hardware_harness"

"$COMPILER" -O --emit-ptx "--gpu-arch=$GPU_ARCH" \
  "$ROOT/tests/gpu/hardware_kernels.mettle" -o "$PTX"
if [[ "$COMPUTE_MAJOR" == "12" && "$COMPUTE_MINOR" -le 1 ]]; then
  MXFP4_PTX="$TMP/tensor_native_fp4.ptx"
  MXFP4_ARCH="sm_${COMPUTE_MAJOR}${COMPUTE_MINOR}a"
  if [[ "$GPU_ARCH" == "gb10" ]]; then
    MXFP4_ARCH="gb10"
  fi
  "$COMPILER" -O --emit-ptx "--gpu-arch=$MXFP4_ARCH" \
    "$ROOT/tests/gpu/tensor_native_fp4.mettle" -o "$MXFP4_PTX"

  MXFP4_ENTRY="$(sed -n '/^\.visible \.entry tensor_mxfp4_m16n16k64(/,/^\.visible \.entry /p' "$MXFP4_PTX")"
  if [[ -z "$MXFP4_ENTRY" ||
        "$MXFP4_ENTRY" != *"mtlc.tensor_mma native-mma mxfp4 whole-tile lowering"* ||
        "$(grep -c 'mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4\.block_scale\.scale_vec::2X\.f32\.e2m1\.e2m1\.f32\.ue8m0' <<<"$MXFP4_ENTRY")" -ne 2 ||
        "$(grep -c 'ld\.global\.b32' <<<"$MXFP4_ENTRY")" -ne 12 ||
        "$(grep -c 'ld\.global\.f32' <<<"$MXFP4_ENTRY")" -ne 8 ||
        "$(grep -c 'st\.global\.f32' <<<"$MXFP4_ENTRY")" -ne 8 ||
        "$MXFP4_ENTRY" == *"wmma."* ]]; then
    echo "hardware MXFP4 module did not retain packed block-scale MMA" >&2
    exit 1
  fi
  MXFP4_CHAIN_ENTRY="$(sed -n '/^\.visible \.entry tensor_mxfp4_chain3_m16n16k64(/,/^\.visible \.entry /p' "$MXFP4_PTX")"
  if [[ -z "$MXFP4_CHAIN_ENTRY" ||
        "$MXFP4_CHAIN_ENTRY" != *"mtlc.tensor_chain resident native-mma mxfp4 tiles=3 subtiles=2"* ||
        "$(grep -c 'mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4\.block_scale' <<<"$MXFP4_CHAIN_ENTRY")" -ne 6 ||
        "$(grep -c 'ld\.global\.f32' <<<"$MXFP4_CHAIN_ENTRY")" -ne 8 ||
        "$(grep -c 'st\.global\.f32' <<<"$MXFP4_CHAIN_ENTRY")" -ne 8 ||
        "$MXFP4_CHAIN_ENTRY" == *"replay"* || "$MXFP4_CHAIN_ENTRY" == *"wmma."* ]]; then
    echo "hardware MXFP4 module did not retain chain residency" >&2
    exit 1
  fi
  MXFP4_LOOP_ENTRY="$(sed -n '/^\.visible \.entry tensor_mxfp4_runtime_k_m16n16k64(/,/^\.visible \.entry /p' "$MXFP4_PTX")"
  if [[ -z "$MXFP4_LOOP_ENTRY" ||
        "$MXFP4_LOOP_ENTRY" != *"mtlc.tensor_loop resident native-mma mxfp4 group=1 subtiles=2"* ||
        "$(grep -c 'mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4\.block_scale' <<<"$MXFP4_LOOP_ENTRY")" -ne 4 ||
        "$(grep -c 'ld\.global\.f32' <<<"$MXFP4_LOOP_ENTRY")" -ne 8 ||
        "$(grep -c 'st\.global\.f32' <<<"$MXFP4_LOOP_ENTRY")" -ne 8 ||
        "$MXFP4_LOOP_ENTRY" == *"replay"* || "$MXFP4_LOOP_ENTRY" == *"wmma."* ]]; then
    echo "hardware MXFP4 module did not retain runtime-K residency" >&2
    exit 1
  fi
  NVFP4_ENTRY="$(sed -n '/^\.visible \.entry tensor_nvfp4_m16n16k64(/,/^\.visible \.entry /p' "$MXFP4_PTX")"
  if [[ -z "$NVFP4_ENTRY" ||
        "$NVFP4_ENTRY" != *"mtlc.tensor_mma native-mma nvfp4 whole-tile lowering"* ||
        "$(grep -c 'mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4nvf4\.block_scale\.scale_vec::4X\.f32\.e2m1\.e2m1\.f32\.ue4m3' <<<"$NVFP4_ENTRY")" -ne 2 ||
        "$(grep -c 'ld\.global\.b32' <<<"$NVFP4_ENTRY")" -ne 12 ||
        "$(grep -c 'ld\.global\.f32' <<<"$NVFP4_ENTRY")" -ne 8 ||
        "$(grep -c 'st\.global\.f32' <<<"$NVFP4_ENTRY")" -ne 8 ||
        "$NVFP4_ENTRY" == *"wmma."* ]]; then
    echo "hardware NVFP4 module did not retain packed block-scale MMA" >&2
    exit 1
  fi
  NVFP4_CHAIN_ENTRY="$(sed -n '/^\.visible \.entry tensor_nvfp4_chain3_m16n16k64(/,/^\.visible \.entry /p' "$MXFP4_PTX")"
  if [[ -z "$NVFP4_CHAIN_ENTRY" ||
        "$NVFP4_CHAIN_ENTRY" != *"mtlc.tensor_chain resident native-mma nvfp4 tiles=3 subtiles=2"* ||
        "$(grep -c 'mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4nvf4\.block_scale' <<<"$NVFP4_CHAIN_ENTRY")" -ne 6 ||
        "$(grep -c 'ld\.global\.f32' <<<"$NVFP4_CHAIN_ENTRY")" -ne 8 ||
        "$(grep -c 'st\.global\.f32' <<<"$NVFP4_CHAIN_ENTRY")" -ne 8 ||
        "$NVFP4_CHAIN_ENTRY" == *"replay"* || "$NVFP4_CHAIN_ENTRY" == *"wmma."* ]]; then
    echo "hardware NVFP4 module did not retain chain residency" >&2
    exit 1
  fi
  NVFP4_LOOP_ENTRY="$(sed -n '/^\.visible \.entry tensor_nvfp4_runtime_k_m16n16k64(/,/^\.visible \.entry /p' "$MXFP4_PTX")"
  if [[ -z "$NVFP4_LOOP_ENTRY" ||
        "$NVFP4_LOOP_ENTRY" != *"mtlc.tensor_loop resident native-mma nvfp4 group=1 subtiles=2"* ||
        "$(grep -c 'mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4nvf4\.block_scale' <<<"$NVFP4_LOOP_ENTRY")" -ne 4 ||
        "$(grep -c 'ld\.global\.f32' <<<"$NVFP4_LOOP_ENTRY")" -ne 8 ||
        "$(grep -c 'st\.global\.f32' <<<"$NVFP4_LOOP_ENTRY")" -ne 8 ||
        "$NVFP4_LOOP_ENTRY" == *"replay"* || "$NVFP4_LOOP_ENTRY" == *"wmma."* ]]; then
    echo "hardware NVFP4 module did not retain runtime-K residency" >&2
    exit 1
  fi
  MXFP6_PTX="$TMP/tensor_native_fp6.ptx"
  "$COMPILER" -O --emit-ptx "--gpu-arch=$MXFP4_ARCH" \
    "$ROOT/tests/gpu/tensor_native_fp6.mettle" -o "$MXFP6_PTX"
  MXFP6_INSTRUCTION='mma\.sync\.aligned\.m16n8k32\.row\.col\.kind::mxf8f6f4\.block_scale\.scale_vec::1X\.f32\.e3m2\.e2m3\.f32\.ue8m0'
  MXFP6_ENTRY="$(sed -n '/^\.visible \.entry tensor_mxfp6_m16n16k32(/,/^\.visible \.entry /p' "$MXFP6_PTX")"
  if [[ -z "$MXFP6_ENTRY" ||
        "$MXFP6_ENTRY" != *"mtlc.tensor_mma native-mma mxf8f6f4 whole-tile lowering"* ||
        "$(grep -c "$MXFP6_INSTRUCTION" <<<"$MXFP6_ENTRY")" -ne 2 ||
        "$(grep -c 'setp\.gt\.u32' <<<"$MXFP6_ENTRY")" -ne 0 ||
        "$(grep -c 'ld\.global\.f32' <<<"$MXFP6_ENTRY")" -ne 8 ||
        "$(grep -c 'st\.global\.f32' <<<"$MXFP6_ENTRY")" -ne 8 ||
        "$MXFP6_ENTRY" == *"wmma."* ]]; then
    echo "hardware FP6 module did not retain dense block-scale MMA fast path" >&2
    exit 1
  fi
  MXFP6_CHAIN_ENTRY="$(sed -n '/^\.visible \.entry tensor_mxfp6_chain3_m16n16k32(/,/^\.visible \.entry /p' "$MXFP6_PTX")"
  if [[ -z "$MXFP6_CHAIN_ENTRY" ||
        "$MXFP6_CHAIN_ENTRY" != *"mtlc.tensor_chain resident native-mma mxf8f6f4 tiles=3 subtiles=2"* ||
        "$(grep -c "$MXFP6_INSTRUCTION" <<<"$MXFP6_CHAIN_ENTRY")" -ne 6 ||
        "$(grep -c 'setp\.gt\.u32' <<<"$MXFP6_CHAIN_ENTRY")" -ne 0 ||
        "$(grep -c 'ld\.global\.f32' <<<"$MXFP6_CHAIN_ENTRY")" -ne 8 ||
        "$(grep -c 'st\.global\.f32' <<<"$MXFP6_CHAIN_ENTRY")" -ne 8 ||
        "$MXFP6_CHAIN_ENTRY" == *"replay"* || "$MXFP6_CHAIN_ENTRY" == *"wmma."* ]]; then
    echo "hardware FP6 module did not retain chain residency and packed fast path" >&2
    exit 1
  fi
  MXFP6_LOOP_ENTRY="$(sed -n '/^\.visible \.entry tensor_mxfp6_runtime_k_m16n16k32(/,/^\.visible \.entry /p' "$MXFP6_PTX")"
  if [[ -z "$MXFP6_LOOP_ENTRY" ||
        "$MXFP6_LOOP_ENTRY" != *"mtlc.tensor_loop resident native-mma mxf8f6f4 group=1 subtiles=2"* ||
        "$(grep -c "$MXFP6_INSTRUCTION" <<<"$MXFP6_LOOP_ENTRY")" -ne 4 ||
        "$(grep -c 'setp\.gt\.u32' <<<"$MXFP6_LOOP_ENTRY")" -eq 0 ||
        "$(grep -c 'ld\.global\.f32' <<<"$MXFP6_LOOP_ENTRY")" -ne 8 ||
        "$(grep -c 'st\.global\.f32' <<<"$MXFP6_LOOP_ENTRY")" -ne 8 ||
        "$MXFP6_LOOP_ENTRY" == *"replay"* || "$MXFP6_LOOP_ENTRY" == *"wmma."* ]]; then
    echo "hardware FP6 module did not retain runtime-K general packed path" >&2
    exit 1
  fi
elif [[ $REQUIRE_GB10 -eq 1 ]]; then
  echo "GB10 hardware gate did not select compute-12.1 native narrow-float modules" >&2
  exit 1
fi
if [[ $EXPERIMENTAL_TMA -eq 1 && "$COMPUTE_MAJOR" -ge 9 ]]; then
  TENSOR_TRANSFER_PTX="$TMP/tensor_transfer.ptx"
  "$COMPILER" -O --emit-ptx "--gpu-arch=$GPU_ARCH" \
    "$ROOT/tests/gpu/tensor_transfer.mettle" -o "$TENSOR_TRANSFER_PTX"
  TRANSFER_LOAD_ENTRY="$(sed -n '/^\.visible \.entry tensor_transfer_load_2d(/,/^\.visible \.entry /p' "$TENSOR_TRANSFER_PTX")"
  TRANSFER_STORE_ENTRY="$(sed -n '/^\.visible \.entry tensor_transfer_store_5d(/,/^\.visible \.entry /p' "$TENSOR_TRANSFER_PTX")"
  if [[ -z "$TRANSFER_LOAD_ENTRY" || -z "$TRANSFER_STORE_ENTRY" ]]; then
    echo "tensor-transfer module omitted its load or store kernel" >&2
    exit 1
  fi
  NATIVE_TRANSFER=0
  if [[ "$GPU_ARCH" == "gb10" ||
        ( "$GPU_ARCH" =~ ^sm_([0-9]+)a?$ && "${BASH_REMATCH[1]}" -ge 90 ) ]]; then
    NATIVE_TRANSFER=1
  fi
  if [[ $NATIVE_TRANSFER -eq 1 ]]; then
    if [[ "$(grep -c 'cp\.async\.bulk\.tensor\.2d\.shared::cta\.global\.tile\.mbarrier::complete_tx::bytes' <<<"$TRANSFER_LOAD_ENTRY")" -ne 1 ||
          "$(grep -c 'cp\.async\.bulk\.tensor\.5d\.global\.shared::cta\.tile\.bulk_group' <<<"$TRANSFER_STORE_ENTRY")" -ne 1 ||
          "$(grep -c 'fence\.proxy\.tensormap::generic\.acquire\.sys' "$TENSOR_TRANSFER_PTX")" -ne 2 ||
          "$(grep -c 'mtlc\.tensor_transfer cooperative-fallback' "$TENSOR_TRANSFER_PTX")" -ne 5 ]]; then
      echo "hardware tensor-transfer module did not retain native TMA plus exact fallback" >&2
      exit 1
    fi
  elif grep -Eq 'cp\.async\.bulk\.tensor|fence\.proxy|mbarrier' "$TENSOR_TRANSFER_PTX"; then
    echo "portable hardware tensor-transfer module leaked native TMA state" >&2
    exit 1
  fi
elif [[ $EXPERIMENTAL_TMA -eq 1 ]]; then
  echo "experimental TMA qualification requires compute capability 9.0 or newer" >&2
  exit 1
fi
CHAIN_ENTRY="$(sed -n '/^\.visible \.entry tensor_chain4(/,/^\.visible \.entry /p' "$PTX")"
if [[ -z "$CHAIN_ENTRY" ||
      "$CHAIN_ENTRY" != *"mtlc.tensor_chain resident tiles=4"* ||
      "$(grep -c 'wmma\.mma\.sync' <<<"$CHAIN_ENTRY")" -ne 4 ||
      "$(grep -c 'wmma\.load\.c\.sync' <<<"$CHAIN_ENTRY")" -ne 1 ||
      "$(grep -c 'wmma\.store\.d\.sync' <<<"$CHAIN_ENTRY")" -ne 1 ]]; then
  echo "hardware module did not retain the four-tile accumulator chain" >&2
  exit 1
fi
LOOP_ENTRY="$(sed -n '/^\.visible \.entry gemm_full_tiles_f16_f32(/,/^\.visible \.entry /p' "$PTX")"
if [[ -z "$LOOP_ENTRY" ||
      "$LOOP_ENTRY" != *"mtlc.tensor_loop resident"* ||
      "$(grep -c 'wmma\.mma\.sync' <<<"$LOOP_ENTRY")" -ne 2 ||
      "$(grep -c 'wmma\.load\.c\.sync' <<<"$LOOP_ENTRY")" -ne 1 ||
      "$(grep -c 'wmma\.store\.d\.sync' <<<"$LOOP_ENTRY")" -ne 1 ]]; then
  echo "hardware module did not retain the runtime-K accumulator loop" >&2
  exit 1
fi
TAIL_GEMM_ENTRY="$(sed -n '/^\.visible \.entry gemm_tail_complete_f16_f32(/,/^\.visible \.entry /p' "$PTX")"
if [[ -z "$TAIL_GEMM_ENTRY" ||
      "$TAIL_GEMM_ENTRY" != *"mtlc.tensor_loop resident"* ||
      "$(grep -c 'wmma\.mma\.sync' <<<"$TAIL_GEMM_ENTRY")" -ne 2 ||
      "$(grep -c 'wmma\.load\.c\.sync' <<<"$TAIL_GEMM_ENTRY")" -ne 1 ||
      "$(grep -c 'wmma\.store\.d\.sync' <<<"$TAIL_GEMM_ENTRY")" -ne 1 ||
      "$(grep -c 'cvt\.f32\.f16' <<<"$TAIL_GEMM_ENTRY")" -ne 2 ||
      "$(grep -c 'bar\.sync 0' <<<"$TAIL_GEMM_ENTRY")" -ne 1 ]]; then
  echo "hardware module did not retain tail-complete tensor/scalar GEMM" >&2
  exit 1
fi
FP8_ENTRY="$(sed -n '/^\.visible \.entry tensor_fp8_m16n16k32(/,/^\.visible \.entry /p' "$PTX")"
if [[ -z "$FP8_ENTRY" ||
      "$FP8_ENTRY" != *"mtlc.tensor_mma native-mma fp8 whole-tile lowering"* ||
      "$(grep -c 'mma\.sync\.aligned\.m16n8k32\.row\.col\.f32\.e4m3\.e5m2\.f32' <<<"$FP8_ENTRY")" -ne 2 ||
      "$FP8_ENTRY" == *"wmma."* ]]; then
  echo "hardware module did not retain native mixed-FP8 m16n16k32 lowering" >&2
  exit 1
fi
FP8_TRANSPOSED_ENTRY="$(sed -n '/^\.visible \.entry tensor_fp8_m32n24k16_transposed(/,/^\.visible \.entry /p' "$PTX")"
if [[ -z "$FP8_TRANSPOSED_ENTRY" ||
      "$FP8_TRANSPOSED_ENTRY" != *"mtlc.tensor_mma native-mma fp8 whole-tile lowering"* ||
      "$(grep -c 'mma\.sync\.aligned\.m16n8k16\.row\.col\.f32\.e5m2\.e4m3\.f32' <<<"$FP8_TRANSPOSED_ENTRY")" -ne 6 ||
      "$FP8_TRANSPOSED_ENTRY" == *"wmma."* ]]; then
  echo "hardware module did not retain tiled mixed-FP8 transpose/layout lowering" >&2
  exit 1
fi
FP8_CHAIN_ENTRY="$(sed -n '/^\.visible \.entry tensor_fp8_chain4_m16n16k32(/,/^\.visible \.entry /p' "$PTX")"
if [[ -z "$FP8_CHAIN_ENTRY" ||
      "$FP8_CHAIN_ENTRY" != *"mtlc.tensor_chain resident native-mma fp8 tiles=4 subtiles=2"* ||
      "$(grep -c 'mma\.sync\.aligned\.m16n8k32\.row\.col\.f32\.e4m3\.e5m2\.f32' <<<"$FP8_CHAIN_ENTRY")" -ne 8 ||
      "$(grep -c 'ld\.global\.f32' <<<"$FP8_CHAIN_ENTRY")" -ne 8 ||
      "$(grep -c 'st\.global\.f32' <<<"$FP8_CHAIN_ENTRY")" -ne 8 ||
      "$FP8_CHAIN_ENTRY" == *"replay"* || "$FP8_CHAIN_ENTRY" == *"wmma."* ]]; then
  echo "hardware module did not retain native FP8 accumulator residency" >&2
  exit 1
fi
FP8_LOOP_ENTRY="$(sed -n '/^\.visible \.entry tensor_fp8_runtime_k_m16n16k32(/,/^\.visible \.entry /p' "$PTX")"
if [[ -z "$FP8_LOOP_ENTRY" ||
      "$FP8_LOOP_ENTRY" != *"mtlc.tensor_loop resident native-mma fp8 group=1 subtiles=2"* ||
      "$(grep -c 'mma\.sync\.aligned\.m16n8k32\.row\.col\.f32\.e4m3\.e5m2\.f32' <<<"$FP8_LOOP_ENTRY")" -ne 4 ||
      "$(grep -c 'ld\.global\.f32' <<<"$FP8_LOOP_ENTRY")" -ne 8 ||
      "$(grep -c 'st\.global\.f32' <<<"$FP8_LOOP_ENTRY")" -ne 8 ||
      "$FP8_LOOP_ENTRY" == *"replay"* || "$FP8_LOOP_ENTRY" == *"wmma."* ]]; then
  echo "hardware module did not retain runtime-K native FP8 residency" >&2
  exit 1
fi
ASYNC_ENTRY="$(sed -n '/^\.visible \.entry async_stage_u32x4(/,/^\.visible \.entry /p' "$PTX")"
if [[ -z "$ASYNC_ENTRY" ]]; then
  echo "hardware module omitted the asynchronous-staging kernel" >&2
  exit 1
fi
if [[ "$GPU_ARCH" == "gb10" ||
      ( "$GPU_ARCH" =~ ^sm_([0-9]+)$ && "${BASH_REMATCH[1]}" -ge 80 ) ]]; then
  if [[ "$(grep -c 'cp\.async\.cg\.shared\.global' <<<"$ASYNC_ENTRY")" -ne 1 ||
        "$(grep -c 'cp\.async\.commit_group' <<<"$ASYNC_ENTRY")" -ne 1 ||
        "$(grep -c 'cp\.async\.wait_group 0' <<<"$ASYNC_ENTRY")" -ne 1 ||
        "$ASYNC_ENTRY" == *"synchronous-fallback"* ]]; then
    echo "hardware module did not select native asynchronous staging" >&2
    exit 1
  fi
fi
AUTO_ENTRY="$(sed -n '/^\.visible \.entry auto_stage_u32(/,/^\.visible \.entry /p' "$PTX")"
if [[ -z "$AUTO_ENTRY" ||
      "$AUTO_ENTRY" != *"mtlc.async_copy auto-promoted native"* ||
      "$AUTO_ENTRY" != *"cp.async.commit_group"*"mul.lo.u32"*"cp.async.wait_group 0"*"bar.sync 0"* ]]; then
  echo "hardware module did not retain optimizer-generated staging overlap" >&2
  exit 1
fi
EXCHANGE_ENTRY="$(sed -n '/^\.visible \.entry subgroup_exchange_vote(/,/^\.visible \.entry /p' "$PTX")"
if [[ -z "$EXCHANGE_ENTRY" ||
      "$(grep -c 'shfl\.sync\.idx\.b32' <<<"$EXCHANGE_ENTRY")" -ne 2 ||
      "$(grep -c 'vote\.sync\.ballot\.b32' <<<"$EXCHANGE_ENTRY")" -ne 2 ||
      "$(grep -c 'vote\.sync\.any\.pred' <<<"$EXCHANGE_ENTRY")" -ne 2 ||
      "$(grep -c 'vote\.sync\.all\.pred' <<<"$EXCHANGE_ENTRY")" -ne 2 ]]; then
  echo "hardware module did not retain subgroup exchange/vote operations" >&2
  exit 1
fi
PIPELINE_ENTRY="$(sed -n '/^\.visible \.entry tensor_pipeline_f16_f32(/,/^\.visible \.entry /p' "$PTX")"
if [[ -z "$PIPELINE_ENTRY" ||
      "$PIPELINE_ENTRY" != *"mtlc.tensor_pipeline resident"* ||
      "$(grep -c 'wmma\.mma\.sync' <<<"$PIPELINE_ENTRY")" -ne 2 ||
      "$(grep -c 'wmma\.load\.c\.sync' <<<"$PIPELINE_ENTRY")" -ne 1 ||
      "$(grep -c 'wmma\.store\.d\.sync' <<<"$PIPELINE_ENTRY")" -ne 1 ||
      "$(grep -c 'bar\.sync 0' <<<"$PIPELINE_ENTRY")" -ne 2 ]]; then
  echo "hardware module did not retain staged-tensor accumulator residency" >&2
  exit 1
fi
PIPELINE4_ENTRY="$(sed -n '/^\.visible \.entry tensor_pipeline4_f16_f32(/,/^\.visible \.entry /p' "$PTX")"
if [[ -z "$PIPELINE4_ENTRY" ||
      "$PIPELINE4_ENTRY" != *"mtlc.tensor_pipeline resident"* ||
      "$(grep -c 'wmma\.mma\.sync' <<<"$PIPELINE4_ENTRY")" -ne 4 ||
      "$(grep -c 'wmma\.load\.c\.sync' <<<"$PIPELINE4_ENTRY")" -ne 1 ||
      "$(grep -c 'wmma\.store\.d\.sync' <<<"$PIPELINE4_ENTRY")" -ne 1 ||
      "$(grep -c 'bar\.sync 0' <<<"$PIPELINE4_ENTRY")" -ne 4 ]]; then
  echo "hardware module did not retain four-stage tensor residency" >&2
  exit 1
fi
if [[ "$GPU_ARCH" == "gb10" ||
      ( "$GPU_ARCH" =~ ^sm_([0-9]+)$ && "${BASH_REMATCH[1]}" -ge 80 ) ]]; then
  if [[ "$(grep -c 'cp\.async\.cg\.shared\.global' <<<"$PIPELINE_ENTRY")" -ne 4 ||
        "$(grep -c 'cp\.async\.commit_group' <<<"$PIPELINE_ENTRY")" -ne 2 ||
        "$PIPELINE_ENTRY" == *"synchronous-fallback"* ||
        "$PIPELINE_ENTRY" != *"cp.async.commit_group"*"cp.async.wait_group 1"*"bar.sync 0"*"wmma.mma.sync"*"cp.async.wait_group 0"*"bar.sync 0"*"wmma.mma.sync"* ]]; then
    echo "hardware module did not retain native staged-tensor overlap" >&2
    exit 1
  fi
  if [[ "$(grep -c 'cp\.async\.cg\.shared\.global' <<<"$PIPELINE4_ENTRY")" -ne 8 ||
        "$(grep -c 'cp\.async\.commit_group' <<<"$PIPELINE4_ENTRY")" -ne 4 ||
        "$PIPELINE4_ENTRY" == *"synchronous-fallback"* ||
        "$PIPELINE4_ENTRY" != *"cp.async.wait_group 3"*"bar.sync 0"*"wmma.mma.sync"*"cp.async.wait_group 2"*"bar.sync 0"*"wmma.mma.sync"*"cp.async.wait_group 1"*"bar.sync 0"*"wmma.mma.sync"*"cp.async.wait_group 0"*"bar.sync 0"*"wmma.mma.sync"*"wmma.store.d.sync"* ]]; then
    echo "hardware module did not retain native four-stage tensor overlap" >&2
    exit 1
  fi
fi
"$CC_BIN" -std=c11 -O2 -Wall -Wextra -Werror \
  "$ROOT/tests/gpu/hardware_harness.c" -o "$HARNESS" -ldl -lm

HARNESS_ARGS=("$PTX")
if [[ $REQUIRE_GB10 -eq 1 ]]; then
  HARNESS_ARGS+=(--require-gb10)
fi
if [[ -n "$MXFP4_PTX" ]]; then
  HARNESS_ARGS+=(--mxfp4 "$MXFP4_PTX")
fi
if [[ -n "$MXFP6_PTX" ]]; then
  HARNESS_ARGS+=(--mxfp6 "$MXFP6_PTX")
fi
"$HARNESS" "${HARNESS_ARGS[@]}"

if [[ $SANITIZER -eq 1 ]]; then
  command -v compute-sanitizer >/dev/null
  compute-sanitizer --tool memcheck --error-exitcode=86 \
    "$HARNESS" "${HARNESS_ARGS[@]}"
  compute-sanitizer --tool racecheck --error-exitcode=87 \
    "$HARNESS" "${HARNESS_ARGS[@]}"
fi

# Experimental TMA is deliberately the final phase and a separate process.
# The harness rejects attempts to mix it with the ordinary suite.
if [[ -n "$TENSOR_TRANSFER_PTX" ]]; then
  TMA_HARNESS_ARGS=("$TENSOR_TRANSFER_PTX" --tensor-transfer-only \
                    --tensor-transfer "$TENSOR_TRANSFER_PTX")
  if [[ $REQUIRE_GB10 -eq 1 ]]; then
    TMA_HARNESS_ARGS+=(--require-gb10)
  fi
  "$HARNESS" "${TMA_HARNESS_ARGS[@]}"

  if [[ $SANITIZER -eq 1 ]]; then
    compute-sanitizer --tool memcheck --error-exitcode=86 \
      "$HARNESS" "${TMA_HARNESS_ARGS[@]}"
    compute-sanitizer --tool racecheck --error-exitcode=87 \
      "$HARNESS" "${TMA_HARNESS_ARGS[@]}"
  fi
fi
