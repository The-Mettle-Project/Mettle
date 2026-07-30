param(
  [string]$CompilerPath = "bin/mettle.exe",
  [string]$GpuArch = "auto",
  [string]$CCompiler = "gcc",
  [switch]$RequireGb10,
  [switch]$ExperimentalTma,
  [switch]$Sanitizer
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$compiler = if ([System.IO.Path]::IsPathRooted($CompilerPath)) {
  $CompilerPath
} else {
  Join-Path $root $CompilerPath
}
if (-not (Test-Path -LiteralPath $compiler)) {
  throw "Mettle compiler not found: $compiler"
}
if (-not (Get-Command nvidia-smi -ErrorAction SilentlyContinue)) {
  throw "nvidia-smi is required for the real-device GPU gate"
}
if (-not (Get-Command $CCompiler -ErrorAction SilentlyContinue)) {
  throw "C compiler '$CCompiler' is required for the hardware harness"
}
if ($ExperimentalTma -and
    $env:MTLC_ALLOW_EXPERIMENTAL_TMA -ne "I_ACCEPT_GPU_RESET_RISK") {
  throw "Experimental TMA execution is quarantined. Run only on a disposable/recoverable host and set MTLC_ALLOW_EXPERIMENTAL_TMA=I_ACCEPT_GPU_RESET_RISK to acknowledge possible GPU reset or host loss."
}
if ($ExperimentalTma -and
    $env:MTLC_TMA_RECOVERY_READY -ne "I_HAVE_OUT_OF_BAND_RECOVERY") {
  throw "Experimental TMA execution requires out-of-band recovery. Set MTLC_TMA_RECOVERY_READY=I_HAVE_OUT_OF_BAND_RECOVERY only when an independent watchdog/operator can reset or reboot the disposable host."
}

$computeCapability = (& nvidia-smi --query-gpu=compute_cap --format=csv,noheader |
  Select-Object -First 1).Trim()
if ($computeCapability -notmatch '^([0-9]+)\.([0-9]+)$') {
  throw "cannot parse CUDA compute capability '$computeCapability'"
}
$computeMajor = [int]$Matches[1]
$computeMinor = [int]$Matches[2]
if ($RequireGb10) {
  if ($env:PROCESSOR_ARCHITECTURE -ne "ARM64" -or $computeCapability -ne "12.1") {
    throw "GB10 gate refused before compilation: requires ARM64 + compute 12.1"
  }
  $GpuArch = "gb10"
} elseif ($GpuArch -eq "auto") {
  $GpuArch = "sm_$($Matches[1])$($Matches[2])"
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) `
  ("mettle-gpu-hardware-" + [Guid]::NewGuid().ToString("N"))
$null = New-Item -ItemType Directory -Path $temp
try {
  $ptx = Join-Path $temp "hardware_kernels.ptx"
  $harness = Join-Path $temp "hardware_harness.exe"
  $kernelSource = Join-Path $root "tests/gpu/hardware_kernels.mettle"
  $harnessSource = Join-Path $root "tests/gpu/hardware_harness.c"

  & $compiler -O --emit-ptx "--gpu-arch=$GpuArch" $kernelSource -o $ptx
  if ($LASTEXITCODE -ne 0) { throw "optimized PTX compilation failed" }
  $ptxText = Get-Content -Raw $ptx
  $mxfp4Ptx = $null
  $mxfp6Ptx = $null
  $tensorTransferPtx = $null
  if ($computeMajor -eq 12 -and $computeMinor -le 1) {
    $mxfp4Ptx = Join-Path $temp "tensor_native_fp4.ptx"
    $mxfp4Arch = if ($GpuArch -eq "gb10") {
      "gb10"
    } else {
      "sm_$computeMajor$($computeMinor)a"
    }
    $mxfp4Source = Join-Path $root "tests/gpu/tensor_native_fp4.mettle"
    & $compiler -O --emit-ptx "--gpu-arch=$mxfp4Arch" `
      $mxfp4Source -o $mxfp4Ptx
    if ($LASTEXITCODE -ne 0) { throw "native MXFP4 PTX compilation failed" }
    $mxfp4Text = Get-Content -Raw $mxfp4Ptx
    $mxfp4Entry = [regex]::Match(
      $mxfp4Text,
      '(?s)\.visible \.entry tensor_mxfp4_m16n16k64\(.*?(?=\.visible \.entry|\z)'
    ).Value
    if (-not $mxfp4Entry -or
        $mxfp4Entry -notmatch
          'mtlc\.tensor_mma native-mma mxfp4 whole-tile lowering' -or
        [regex]::Matches(
          $mxfp4Entry,
          'mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4\.block_scale\.scale_vec::2X\.f32\.e2m1\.e2m1\.f32\.ue8m0'
        ).Count -ne 2 -or
        [regex]::Matches($mxfp4Entry, 'ld\.global\.b32').Count -ne 12 -or
        [regex]::Matches($mxfp4Entry, 'ld\.global\.f32').Count -ne 8 -or
        [regex]::Matches($mxfp4Entry, 'st\.global\.f32').Count -ne 8 -or
        $mxfp4Entry -match 'wmma\.') {
      throw "hardware MXFP4 module did not retain packed block-scale MMA"
    }
    $mxfp4ChainEntry = [regex]::Match(
      $mxfp4Text,
      '(?s)\.visible \.entry tensor_mxfp4_chain3_m16n16k64\(.*?(?=\.visible \.entry|\z)'
    ).Value
    if (-not $mxfp4ChainEntry -or
        $mxfp4ChainEntry -notmatch
          'mtlc\.tensor_chain resident native-mma mxfp4 tiles=3 subtiles=2' -or
        [regex]::Matches(
          $mxfp4ChainEntry,
          'mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4\.block_scale'
        ).Count -ne 6 -or
        [regex]::Matches($mxfp4ChainEntry, 'ld\.global\.f32').Count -ne 8 -or
        [regex]::Matches($mxfp4ChainEntry, 'st\.global\.f32').Count -ne 8 -or
        $mxfp4ChainEntry -match 'replay|wmma\.') {
      throw "hardware MXFP4 module did not retain chain residency"
    }
    $mxfp4LoopEntry = [regex]::Match(
      $mxfp4Text,
      '(?s)\.visible \.entry tensor_mxfp4_runtime_k_m16n16k64\(.*?(?=\.visible \.entry|\z)'
    ).Value
    if (-not $mxfp4LoopEntry -or
        $mxfp4LoopEntry -notmatch
          'mtlc\.tensor_loop resident native-mma mxfp4 group=1 subtiles=2' -or
        [regex]::Matches(
          $mxfp4LoopEntry,
          'mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4\.block_scale'
        ).Count -ne 4 -or
        [regex]::Matches($mxfp4LoopEntry, 'ld\.global\.f32').Count -ne 8 -or
        [regex]::Matches($mxfp4LoopEntry, 'st\.global\.f32').Count -ne 8 -or
        $mxfp4LoopEntry -match 'replay|wmma\.') {
      throw "hardware MXFP4 module did not retain runtime-K residency"
    }
    $nvfp4Entry = [regex]::Match(
      $mxfp4Text,
      '(?s)\.visible \.entry tensor_nvfp4_m16n16k64\(.*?(?=\.visible \.entry|\z)'
    ).Value
    if (-not $nvfp4Entry -or
        $nvfp4Entry -notmatch
          'mtlc\.tensor_mma native-mma nvfp4 whole-tile lowering' -or
        [regex]::Matches(
          $nvfp4Entry,
          'mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4nvf4\.block_scale\.scale_vec::4X\.f32\.e2m1\.e2m1\.f32\.ue4m3'
        ).Count -ne 2 -or
        [regex]::Matches($nvfp4Entry, 'ld\.global\.b32').Count -ne 12 -or
        [regex]::Matches($nvfp4Entry, 'ld\.global\.f32').Count -ne 8 -or
        [regex]::Matches($nvfp4Entry, 'st\.global\.f32').Count -ne 8 -or
        $nvfp4Entry -match 'wmma\.') {
      throw "hardware NVFP4 module did not retain packed block-scale MMA"
    }
    $nvfp4ChainEntry = [regex]::Match(
      $mxfp4Text,
      '(?s)\.visible \.entry tensor_nvfp4_chain3_m16n16k64\(.*?(?=\.visible \.entry|\z)'
    ).Value
    if (-not $nvfp4ChainEntry -or
        $nvfp4ChainEntry -notmatch
          'mtlc\.tensor_chain resident native-mma nvfp4 tiles=3 subtiles=2' -or
        [regex]::Matches(
          $nvfp4ChainEntry,
          'mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4nvf4\.block_scale'
        ).Count -ne 6 -or
        [regex]::Matches($nvfp4ChainEntry, 'ld\.global\.f32').Count -ne 8 -or
        [regex]::Matches($nvfp4ChainEntry, 'st\.global\.f32').Count -ne 8 -or
        $nvfp4ChainEntry -match 'replay|wmma\.') {
      throw "hardware NVFP4 module did not retain chain residency"
    }
    $nvfp4LoopEntry = [regex]::Match(
      $mxfp4Text,
      '(?s)\.visible \.entry tensor_nvfp4_runtime_k_m16n16k64\(.*?(?=\.visible \.entry|\z)'
    ).Value
    if (-not $nvfp4LoopEntry -or
        $nvfp4LoopEntry -notmatch
          'mtlc\.tensor_loop resident native-mma nvfp4 group=1 subtiles=2' -or
        [regex]::Matches(
          $nvfp4LoopEntry,
          'mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4nvf4\.block_scale'
        ).Count -ne 4 -or
        [regex]::Matches($nvfp4LoopEntry, 'ld\.global\.f32').Count -ne 8 -or
        [regex]::Matches($nvfp4LoopEntry, 'st\.global\.f32').Count -ne 8 -or
        $nvfp4LoopEntry -match 'replay|wmma\.') {
      throw "hardware NVFP4 module did not retain runtime-K residency"
    }
    $mxfp6Ptx = Join-Path $temp "tensor_native_fp6.ptx"
    $mxfp6Source = Join-Path $root "tests/gpu/tensor_native_fp6.mettle"
    & $compiler -O --emit-ptx "--gpu-arch=$mxfp4Arch" `
      $mxfp6Source -o $mxfp6Ptx
    if ($LASTEXITCODE -ne 0) { throw "native FP6 PTX compilation failed" }
    $mxfp6Text = Get-Content -Raw $mxfp6Ptx
    $mxfp6Instruction =
      'mma\.sync\.aligned\.m16n8k32\.row\.col\.kind::mxf8f6f4\.block_scale\.scale_vec::1X\.f32\.e3m2\.e2m3\.f32\.ue8m0'
    $mxfp6Entry = [regex]::Match(
      $mxfp6Text,
      '(?s)\.visible \.entry tensor_mxfp6_m16n16k32\(.*?(?=\.visible \.entry|\z)'
    ).Value
    if (-not $mxfp6Entry -or
        $mxfp6Entry -notmatch
          'mtlc\.tensor_mma native-mma mxf8f6f4 whole-tile lowering' -or
        [regex]::Matches($mxfp6Entry, $mxfp6Instruction).Count -ne 2 -or
        [regex]::Matches($mxfp6Entry, 'setp\.gt\.u32').Count -ne 0 -or
        [regex]::Matches($mxfp6Entry, 'ld\.global\.f32').Count -ne 8 -or
        [regex]::Matches($mxfp6Entry, 'st\.global\.f32').Count -ne 8 -or
        $mxfp6Entry -match 'wmma\.') {
      throw "hardware FP6 module did not retain dense block-scale MMA fast path"
    }
    $mxfp6ChainEntry = [regex]::Match(
      $mxfp6Text,
      '(?s)\.visible \.entry tensor_mxfp6_chain3_m16n16k32\(.*?(?=\.visible \.entry|\z)'
    ).Value
    if (-not $mxfp6ChainEntry -or
        $mxfp6ChainEntry -notmatch
          'mtlc\.tensor_chain resident native-mma mxf8f6f4 tiles=3 subtiles=2' -or
        [regex]::Matches($mxfp6ChainEntry, $mxfp6Instruction).Count -ne 6 -or
        [regex]::Matches($mxfp6ChainEntry, 'setp\.gt\.u32').Count -ne 0 -or
        [regex]::Matches($mxfp6ChainEntry, 'ld\.global\.f32').Count -ne 8 -or
        [regex]::Matches($mxfp6ChainEntry, 'st\.global\.f32').Count -ne 8 -or
        $mxfp6ChainEntry -match 'replay|wmma\.') {
      throw "hardware FP6 module did not retain chain residency and packed fast path"
    }
    $mxfp6LoopEntry = [regex]::Match(
      $mxfp6Text,
      '(?s)\.visible \.entry tensor_mxfp6_runtime_k_m16n16k32\(.*?(?=\.visible \.entry|\z)'
    ).Value
    if (-not $mxfp6LoopEntry -or
        $mxfp6LoopEntry -notmatch
          'mtlc\.tensor_loop resident native-mma mxf8f6f4 group=1 subtiles=2' -or
        [regex]::Matches($mxfp6LoopEntry, $mxfp6Instruction).Count -ne 4 -or
        [regex]::Matches($mxfp6LoopEntry, 'setp\.gt\.u32').Count -eq 0 -or
        [regex]::Matches($mxfp6LoopEntry, 'ld\.global\.f32').Count -ne 8 -or
        [regex]::Matches($mxfp6LoopEntry, 'st\.global\.f32').Count -ne 8 -or
        $mxfp6LoopEntry -match 'replay|wmma\.') {
      throw "hardware FP6 module did not retain runtime-K general packed path"
    }
  } elseif ($RequireGb10) {
    throw "GB10 hardware gate did not select compute-12.1 native narrow-float modules"
  }
  if ($ExperimentalTma -and $computeMajor -ge 9) {
    $tensorTransferPtx = Join-Path $temp "tensor_transfer.ptx"
    $tensorTransferSource = Join-Path $root "tests/gpu/tensor_transfer.mettle"
    & $compiler -O --emit-ptx "--gpu-arch=$GpuArch" `
      $tensorTransferSource -o $tensorTransferPtx
    if ($LASTEXITCODE -ne 0) {
      throw "tensor-transfer PTX compilation failed"
    }
    $tensorTransferText = Get-Content -Raw $tensorTransferPtx
    $transferLoadEntry = [regex]::Match(
      $tensorTransferText,
      '(?s)\.visible \.entry tensor_transfer_load_2d\(.*?(?=\.visible \.entry|\z)'
    ).Value
    $transferStoreEntry = [regex]::Match(
      $tensorTransferText,
      '(?s)\.visible \.entry tensor_transfer_store_5d\(.*?(?=\.visible \.entry|\z)'
    ).Value
    if (-not $transferLoadEntry -or -not $transferStoreEntry) {
      throw "tensor-transfer module omitted its load or store kernel"
    }
    $nativeTransfer = $GpuArch -eq "gb10" -or
      ($GpuArch -match '^sm_([0-9]+)a?$' -and [int]$Matches[1] -ge 90)
    if ($nativeTransfer) {
      if ([regex]::Matches(
            $transferLoadEntry,
            'cp\.async\.bulk\.tensor\.2d\.shared::cta\.global\.tile\.mbarrier::complete_tx::bytes'
          ).Count -ne 1 -or
          [regex]::Matches(
            $transferStoreEntry,
            'cp\.async\.bulk\.tensor\.5d\.global\.shared::cta\.tile\.bulk_group'
          ).Count -ne 1 -or
          [regex]::Matches($tensorTransferText,
                           'fence\.proxy\.tensormap::generic\.acquire\.sys').Count -ne 2 -or
          [regex]::Matches($tensorTransferText,
                           'mtlc\.tensor_transfer cooperative-fallback').Count -ne 5) {
        throw "hardware tensor-transfer module did not retain native TMA plus exact fallback"
      }
    } elseif ($tensorTransferText -match
              'cp\.async\.bulk\.tensor|fence\.proxy|mbarrier') {
      throw "portable hardware tensor-transfer module leaked native TMA state"
    }
  } elseif ($ExperimentalTma) {
    throw "experimental TMA qualification requires compute capability 9.0 or newer"
  }
  $chainEntry = [regex]::Match(
    $ptxText,
    '(?s)\.visible \.entry tensor_chain4\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $chainEntry -or
      $chainEntry -notmatch 'mtlc\.tensor_chain resident tiles=4' -or
      [regex]::Matches($chainEntry, 'wmma\.mma\.sync').Count -ne 4 -or
      [regex]::Matches($chainEntry, 'wmma\.load\.c\.sync').Count -ne 1 -or
      [regex]::Matches($chainEntry, 'wmma\.store\.d\.sync').Count -ne 1) {
    throw "hardware module did not retain the four-tile accumulator chain"
  }
  $loopEntry = [regex]::Match(
    $ptxText,
    '(?s)\.visible \.entry gemm_full_tiles_f16_f32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $loopEntry -or
      $loopEntry -notmatch 'mtlc\.tensor_loop resident' -or
      [regex]::Matches($loopEntry, 'wmma\.mma\.sync').Count -ne 2 -or
      [regex]::Matches($loopEntry, 'wmma\.load\.c\.sync').Count -ne 1 -or
      [regex]::Matches($loopEntry, 'wmma\.store\.d\.sync').Count -ne 1) {
    throw "hardware module did not retain the runtime-K accumulator loop"
  }
  $tailGemmEntry = [regex]::Match(
    $ptxText,
    '(?s)\.visible \.entry gemm_tail_complete_f16_f32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $tailGemmEntry -or
      $tailGemmEntry -notmatch 'mtlc\.tensor_loop resident' -or
      [regex]::Matches($tailGemmEntry, 'wmma\.mma\.sync').Count -ne 2 -or
      [regex]::Matches($tailGemmEntry, 'wmma\.load\.c\.sync').Count -ne 1 -or
      [regex]::Matches($tailGemmEntry, 'wmma\.store\.d\.sync').Count -ne 1 -or
      [regex]::Matches($tailGemmEntry, 'cvt\.f32\.f16').Count -ne 2 -or
      [regex]::Matches($tailGemmEntry, 'bar\.sync 0').Count -ne 1) {
    throw "hardware module did not retain tail-complete tensor/scalar GEMM"
  }
  $fp8Entry = [regex]::Match(
    $ptxText,
    '(?s)\.visible \.entry tensor_fp8_m16n16k32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $fp8Entry -or
      $fp8Entry -notmatch 'mtlc\.tensor_mma native-mma fp8 whole-tile lowering' -or
      [regex]::Matches(
        $fp8Entry,
        'mma\.sync\.aligned\.m16n8k32\.row\.col\.f32\.e4m3\.e5m2\.f32'
      ).Count -ne 2 -or
      $fp8Entry -match 'wmma\.') {
    throw "hardware module did not retain native mixed-FP8 m16n16k32 lowering"
  }
  $fp8TransposedEntry = [regex]::Match(
    $ptxText,
    '(?s)\.visible \.entry tensor_fp8_m32n24k16_transposed\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $fp8TransposedEntry -or
      $fp8TransposedEntry -notmatch 'mtlc\.tensor_mma native-mma fp8 whole-tile lowering' -or
      [regex]::Matches(
        $fp8TransposedEntry,
        'mma\.sync\.aligned\.m16n8k16\.row\.col\.f32\.e5m2\.e4m3\.f32'
      ).Count -ne 6 -or
      $fp8TransposedEntry -match 'wmma\.') {
    throw "hardware module did not retain tiled mixed-FP8 transpose/layout lowering"
  }
  $fp8ChainEntry = [regex]::Match(
    $ptxText,
    '(?s)\.visible \.entry tensor_fp8_chain4_m16n16k32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $fp8ChainEntry -or
      $fp8ChainEntry -notmatch
        'mtlc\.tensor_chain resident native-mma fp8 tiles=4 subtiles=2' -or
      [regex]::Matches(
        $fp8ChainEntry,
        'mma\.sync\.aligned\.m16n8k32\.row\.col\.f32\.e4m3\.e5m2\.f32'
      ).Count -ne 8 -or
      [regex]::Matches($fp8ChainEntry, 'ld\.global\.f32').Count -ne 8 -or
      [regex]::Matches($fp8ChainEntry, 'st\.global\.f32').Count -ne 8 -or
      $fp8ChainEntry -match 'replay|wmma\.') {
    throw "hardware module did not retain native FP8 accumulator residency"
  }
  $fp8LoopEntry = [regex]::Match(
    $ptxText,
    '(?s)\.visible \.entry tensor_fp8_runtime_k_m16n16k32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $fp8LoopEntry -or
      $fp8LoopEntry -notmatch
        'mtlc\.tensor_loop resident native-mma fp8 group=1 subtiles=2' -or
      [regex]::Matches(
        $fp8LoopEntry,
        'mma\.sync\.aligned\.m16n8k32\.row\.col\.f32\.e4m3\.e5m2\.f32'
      ).Count -ne 4 -or
      [regex]::Matches($fp8LoopEntry, 'ld\.global\.f32').Count -ne 8 -or
      [regex]::Matches($fp8LoopEntry, 'st\.global\.f32').Count -ne 8 -or
      $fp8LoopEntry -match 'replay|wmma\.') {
    throw "hardware module did not retain runtime-K native FP8 residency"
  }
  $asyncEntry = [regex]::Match(
    $ptxText,
    '(?s)\.visible \.entry async_stage_u32x4\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $asyncEntry) {
    throw "hardware module omitted the asynchronous-staging kernel"
  }
  if ($GpuArch -eq "gb10" -or
      ($GpuArch -match '^sm_([0-9]+)$' -and [int]$Matches[1] -ge 80)) {
    if ([regex]::Matches($asyncEntry,
                        'cp\.async\.cg\.shared\.global').Count -ne 1 -or
        [regex]::Matches($asyncEntry,
                        'cp\.async\.commit_group').Count -ne 1 -or
        [regex]::Matches($asyncEntry,
                        'cp\.async\.wait_group 0').Count -ne 1 -or
        $asyncEntry -match 'synchronous-fallback') {
      throw "hardware module did not select native asynchronous staging"
    }
  }
  $autoEntry = [regex]::Match(
    $ptxText,
    '(?s)\.visible \.entry auto_stage_u32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  $autoCommit = $autoEntry.IndexOf('cp.async.commit_group')
  $autoOverlap = $autoEntry.IndexOf('mul.lo.u32', $autoCommit + 1)
  $autoWait = $autoEntry.IndexOf('cp.async.wait_group 0', $autoCommit + 1)
  $autoBarrier = $autoEntry.IndexOf('bar.sync 0', $autoWait + 1)
  if (-not $autoEntry -or
      $autoEntry -notmatch 'mtlc\.async_copy auto-promoted native' -or
      $autoCommit -lt 0 -or $autoOverlap -le $autoCommit -or
      $autoWait -le $autoOverlap -or $autoBarrier -le $autoWait) {
    throw "hardware module did not retain optimizer-generated staging overlap"
  }
  $exchangeEntry = [regex]::Match(
    $ptxText,
    '(?s)\.visible \.entry subgroup_exchange_vote\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $exchangeEntry -or
      [regex]::Matches($exchangeEntry, 'shfl\.sync\.idx\.b32').Count -ne 2 -or
      [regex]::Matches($exchangeEntry, 'vote\.sync\.ballot\.b32').Count -ne 2 -or
      [regex]::Matches($exchangeEntry, 'vote\.sync\.any\.pred').Count -ne 2 -or
      [regex]::Matches($exchangeEntry, 'vote\.sync\.all\.pred').Count -ne 2) {
    throw "hardware module did not retain subgroup exchange/vote operations"
  }
  $pipelineEntry = [regex]::Match(
    $ptxText,
    '(?s)\.visible \.entry tensor_pipeline_f16_f32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $pipelineEntry -or
      $pipelineEntry -notmatch 'mtlc\.tensor_pipeline resident' -or
      [regex]::Matches($pipelineEntry, 'wmma\.mma\.sync').Count -ne 2 -or
      [regex]::Matches($pipelineEntry, 'wmma\.load\.c\.sync').Count -ne 1 -or
      [regex]::Matches($pipelineEntry, 'wmma\.store\.d\.sync').Count -ne 1 -or
      [regex]::Matches($pipelineEntry, 'bar\.sync 0').Count -ne 2) {
    throw "hardware module did not retain staged-tensor accumulator residency"
  }
  $pipeline4Entry = [regex]::Match(
    $ptxText,
    '(?s)\.visible \.entry tensor_pipeline4_f16_f32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $pipeline4Entry -or
      $pipeline4Entry -notmatch 'mtlc\.tensor_pipeline resident' -or
      [regex]::Matches($pipeline4Entry, 'wmma\.mma\.sync').Count -ne 4 -or
      [regex]::Matches($pipeline4Entry, 'wmma\.load\.c\.sync').Count -ne 1 -or
      [regex]::Matches($pipeline4Entry, 'wmma\.store\.d\.sync').Count -ne 1 -or
      [regex]::Matches($pipeline4Entry, 'bar\.sync 0').Count -ne 4) {
    throw "hardware module did not retain four-stage tensor residency"
  }
  if ($GpuArch -eq "gb10" -or
      ($GpuArch -match '^sm_([0-9]+)$' -and [int]$Matches[1] -ge 80)) {
    $pipelineSecondCommit = $pipelineEntry.LastIndexOf('cp.async.commit_group')
    $pipelineWaitOne = $pipelineEntry.IndexOf(
      'cp.async.wait_group 1', $pipelineSecondCommit + 1)
    $pipelineFirstBarrier = $pipelineEntry.IndexOf(
      'bar.sync 0', $pipelineWaitOne + 1)
    $pipelineFirstMma = $pipelineEntry.IndexOf(
      'wmma.mma.sync', $pipelineFirstBarrier + 1)
    $pipelineWaitZero = $pipelineEntry.IndexOf(
      'cp.async.wait_group 0', $pipelineFirstMma + 1)
    $pipelineSecondBarrier = $pipelineEntry.IndexOf(
      'bar.sync 0', $pipelineWaitZero + 1)
    $pipelineSecondMma = $pipelineEntry.IndexOf(
      'wmma.mma.sync', $pipelineFirstMma + 1)
    if ([regex]::Matches($pipelineEntry,
                        'cp\.async\.cg\.shared\.global').Count -ne 4 -or
        [regex]::Matches($pipelineEntry,
                        'cp\.async\.commit_group').Count -ne 2 -or
        $pipelineSecondCommit -lt 0 -or
        $pipelineWaitOne -le $pipelineSecondCommit -or
        $pipelineFirstBarrier -le $pipelineWaitOne -or
        $pipelineFirstMma -le $pipelineFirstBarrier -or
        $pipelineWaitZero -le $pipelineFirstMma -or
        $pipelineSecondBarrier -le $pipelineWaitZero -or
        $pipelineSecondMma -le $pipelineSecondBarrier -or
        $pipelineEntry -match 'synchronous-fallback') {
      throw "hardware module did not retain native staged-tensor overlap"
    }
    if ([regex]::Matches($pipeline4Entry,
                        'cp\.async\.cg\.shared\.global').Count -ne 8 -or
        [regex]::Matches($pipeline4Entry,
                        'cp\.async\.commit_group').Count -ne 4 -or
        $pipeline4Entry -match 'synchronous-fallback') {
      throw "hardware module did not retain native four-stage tensor overlap"
    }
    $pipeline4Cursor = -1
    foreach ($needle in @('cp.async.wait_group 3', 'bar.sync 0',
                           'wmma.mma.sync', 'cp.async.wait_group 2',
                           'bar.sync 0', 'wmma.mma.sync',
                           'cp.async.wait_group 1', 'bar.sync 0',
                           'wmma.mma.sync', 'cp.async.wait_group 0',
                           'bar.sync 0', 'wmma.mma.sync',
                           'wmma.store.d.sync')) {
      $next = $pipeline4Entry.IndexOf($needle, $pipeline4Cursor + 1)
      if ($next -le $pipeline4Cursor) {
        throw "hardware four-stage tensor ordering failed at '$needle'"
      }
      $pipeline4Cursor = $next
    }
  }

  & $CCompiler -std=c11 -O2 -Wall -Wextra -Werror $harnessSource -o $harness -lm
  if ($LASTEXITCODE -ne 0) { throw "hardware harness compilation failed" }

  $harnessArgs = @($ptx)
  if ($RequireGb10) { $harnessArgs += "--require-gb10" }
  if ($mxfp4Ptx) { $harnessArgs += @("--mxfp4", $mxfp4Ptx) }
  if ($mxfp6Ptx) { $harnessArgs += @("--mxfp6", $mxfp6Ptx) }
  & $harness @harnessArgs
  if ($LASTEXITCODE -ne 0) { throw "hardware differential suite failed" }

  if ($Sanitizer) {
    $computeSanitizer = Get-Command compute-sanitizer -ErrorAction SilentlyContinue
    if (-not $computeSanitizer) {
      throw "compute-sanitizer is required when -Sanitizer is set"
    }
    & $computeSanitizer.Source --tool memcheck --error-exitcode=86 `
      $harness @harnessArgs
    if ($LASTEXITCODE -ne 0) { throw "compute-sanitizer memcheck failed" }
    & $computeSanitizer.Source --tool racecheck --error-exitcode=87 `
      $harness @harnessArgs
    if ($LASTEXITCODE -ne 0) { throw "compute-sanitizer racecheck failed" }
  }

  # Experimental TMA is deliberately the final phase and a separate process.
  # The harness rejects attempts to mix it with the ordinary suite.
  if ($tensorTransferPtx) {
    $tmaHarnessArgs = @(
      $tensorTransferPtx,
      "--tensor-transfer-only",
      "--tensor-transfer", $tensorTransferPtx
    )
    if ($RequireGb10) { $tmaHarnessArgs += "--require-gb10" }
    & $harness @tmaHarnessArgs
    if ($LASTEXITCODE -ne 0) { throw "experimental TMA suite failed" }

    if ($Sanitizer) {
      & $computeSanitizer.Source --tool memcheck --error-exitcode=86 `
        $harness @tmaHarnessArgs
      if ($LASTEXITCODE -ne 0) { throw "experimental TMA memcheck failed" }
      & $computeSanitizer.Source --tool racecheck --error-exitcode=87 `
        $harness @tmaHarnessArgs
      if ($LASTEXITCODE -ne 0) { throw "experimental TMA racecheck failed" }
    }
  }
}
finally {
  if (Test-Path -LiteralPath $temp) {
    Remove-Item -LiteralPath $temp -Recurse -Force
  }
}
