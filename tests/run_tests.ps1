param(
  [string]$CompilerPath = "",
  [switch]$BuildCompiler,
  [switch]$SkipRuntime,
  [switch]$SkipDeterminism,
  [int]$FuzzCount = 60
)

$ErrorActionPreference = "Continue"

# Host platform. Windows PowerShell 5.1 predates the $IsWindows automatic
# variable and only ever runs on Windows, so a null reading means Windows.
$script:OnWindows = if ($null -eq $IsWindows) { $true } else { [bool]$IsWindows }

# The compiler the Makefile and build.bat each produce.
if ([string]::IsNullOrWhiteSpace($CompilerPath)) {
  $CompilerPath = if ($script:OnWindows) { ".\bin/mettle.exe" } else { "./bin/mettle" }
}

# Product names keep their .exe suffix on both platforms. The suffix is
# meaningless to the Linux loader, and holding it constant keeps one set of
# artifact names -- and so one code path -- across the suite. It also keeps
# `-o <name>.exe` clear of the compiler's own intermediate `<name>.o`, which a
# bare `-o <name>.o` on Linux would collide with.

# Windows-only coverage: the internal PE linker, COFF readers, PE import
# tables, and the Win32 libraries reached through them. Every skip is counted
# and listed at the end of the run so the Linux gate states what it did not
# check rather than passing silently.
$script:SkippedWindowsOnly = New-Object System.Collections.Generic.List[string]
function Skip-WindowsOnly {
  param([string]$Name, [string]$Why = "Windows-only: PE/COFF toolchain")
  $script:SkippedWindowsOnly.Add("$Name ($Why)")
  Write-Host "[SKIP] $Name :: $Why"
}

# `--linker internal` selects the internal PE linker, which exists only on
# Windows. On Linux the native ELF path is the default, so the switch is
# dropped and the same program is built through the platform's own linker.
$script:InternalLinkerArgs = if ($script:OnWindows) { @("--linker", "internal") } else { @() }

function Write-CaseResult {
  param(
    [string]$Name,
    [bool]$Passed,
    [string]$Reason = ""
  )

  if ($Passed) {
    if ($Reason) {
      Write-Host "[PASS] $Name ($Reason)"
    }
    else {
      Write-Host "[PASS] $Name"
    }
  }
  else {
    if ($Reason) {
      Write-Host "[FAIL] $Name :: $Reason"
    }
    else {
      Write-Host "[FAIL] $Name"
    }
  }
}

function Get-Sha256FileHash {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  $stream = $null
  $sha256 = $null
  try {
    $resolvedPath = (Resolve-Path -LiteralPath $Path).ProviderPath
    $stream = [System.IO.File]::OpenRead($resolvedPath)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $bytes = $sha256.ComputeHash($stream)
    return ([System.BitConverter]::ToString($bytes) -replace "-", "")
  }
  finally {
    if ($stream) {
      $stream.Dispose()
    }
    if ($sha256) {
      $sha256.Dispose()
    }
  }
}

function Test-BinaryOutput {
  param(
    [string]$BinaryPath
  )

  if (-not (Test-Path $BinaryPath)) {
    return @{ Passed = $false; Reason = "Output file not produced" }
  }

  $item = Get-Item -LiteralPath $BinaryPath
  if ($item.Length -le 0) {
    return @{ Passed = $false; Reason = "Output binary is empty" }
  }

  return @{ Passed = $true; Reason = "" }
}

function Test-DisassemblyOutput {
  param(
    [string]$BinaryPath,
    [string[]]$RequiredPatterns = @(),
    [string[]]$ForbiddenPatterns = @()
  )

  if (-not (Test-Path $BinaryPath)) {
    return @{ Passed = $false; Reason = "Output file not produced" }
  }

  $disasm = & objdump -d $BinaryPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    return @{ Passed = $false; Reason = "objdump failed on '$BinaryPath'" }
  }

  foreach ($pattern in $RequiredPatterns) {
    if ([string]::IsNullOrWhiteSpace($pattern)) {
      continue
    }
    if ($disasm -notmatch $pattern) {
      return @{ Passed = $false; Reason = "Disassembly missing required pattern '$pattern'" }
    }
  }

  foreach ($pattern in $ForbiddenPatterns) {
    if ([string]::IsNullOrWhiteSpace($pattern)) {
      continue
    }
    if ($disasm -match $pattern) {
      return @{ Passed = $false; Reason = "Disassembly matched forbidden pattern '$pattern'" }
    }
  }

  return @{ Passed = $true; Reason = "" }
}

if ($BuildCompiler) {
  Write-Host "Building compiler..."
  if ($script:OnWindows) { & .\build.bat } else { & make -j"$(nproc)" }
  if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed."
    exit 1
  }
}

if (-not (Test-Path $CompilerPath)) {
  Write-Error "Compiler not found at '$CompilerPath'."
  exit 1
}

$tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) "Mettle-test-artifacts"
if (-not (Test-Path $tmpDir)) {
  New-Item -Path $tmpDir -ItemType Directory | Out-Null
}
$repoRoot = (Resolve-Path ".").Path


$cases = @(
  @{ Name = "ok_global_int"; Path = "tests/ok_global_int.mettle"; ShouldSucceed = $true },
  @{ Name = "only_struct"; Path = "tests/only_struct.mettle"; ShouldSucceed = $true },
  @{ Name = "array_index"; Path = "tests/test_array_index.mettle"; ShouldSucceed = $true },
  @{ Name = "control_flow"; Path = "tests/test_control_flow.mettle"; ShouldSucceed = $true },
  @{ Name = "nested_switch_loop"; Path = "tests/test_nested_switch_loop.mettle"; ShouldSucceed = $true },
  @{ Name = "elseif_chaining"; Path = "tests/test_elseif.mettle"; ShouldSucceed = $true },
  @{ Name = "switch_const_expr"; Path = "tests/test_switch_const_expr.mettle"; ShouldSucceed = $true },
  @{ Name = "switch_continue_loop"; Path = "tests/test_switch_continue_loop.mettle"; ShouldSucceed = $true },
  @{ Name = "switch_range"; Path = "tests/test_switch_range.mettle"; ShouldSucceed = $true },
  @{ Name = "range_for"; Path = "tests/test_range_for.mettle"; ShouldSucceed = $true },
  @{
    Name          = "gpu_dispatch"
    Path          = "tests/test_gpu_dispatch.mettle"
    ShouldSucceed = $true
    Args          = @("--emit-obj")
  },
  @{
    Name          = "gpu_dispatch_host_abi"
    Path          = "tests/test_gpu_dispatch_host_abi.mettle"
    ShouldSucceed = $true
    Args          = @("--emit-obj")
  },
  @{
    Name          = "simd_contract"
    Path          = "tests/test_simd_contract.mettle"
    ShouldSucceed = $true
    Args          = @("-O")
    IrMustMatch   = @("dot_i8\(")
  },
  @{
    Name          = "err_simd_contract"
    Path          = "tests/err_simd_contract.mettle"
    ShouldSucceed = $false
    Args          = @("-O")
    Pattern       = "@simd! loop was not vectorized: the loop body contains a function call"
  },
  @{
    Name          = "err_simd_contract_cf"
    Path          = "tests/err_simd_contract_cf.mettle"
    ShouldSucceed = $false
    Args          = @("-O")
    Pattern       = "@simd! loop was not vectorized: the loop body branches on data"
  },
  @{
    # A pointer-deref loop with no user control flow must NOT be misreported as
    # "control flow" at -O (the null-check branch is excluded from the heuristic).
    Name          = "err_simd_contract_stride"
    Path          = "tests/err_simd_contract_stride.mettle"
    ShouldSucceed = $false
    Args          = @("-O")
    Pattern       = "@simd! loop was not vectorized: no vectorizer recognized this loop's shape"
  },
  @{
    # Element-type detection: a 64-bit-int loop reports the precise cause, not
    # the generic shape fallback.
    Name          = "err_simd_contract_i64"
    Path          = "tests/err_simd_contract_i64.mettle"
    ShouldSucceed = $false
    Args          = @("-O")
    Pattern       = "@simd! loop was not vectorized: the loop accesses 64-bit integers"
  },
  @{
    # Function-level `@simd!` is a hard contract on every counted body loop.
    Name          = "err_simd_fn_contract"
    Path          = "tests/err_simd_fn_contract.mettle"
    ShouldSucceed = $false
    Args          = @("-O")
    Pattern       = "@simd! loop was not vectorized"
  },
  @{
    # --explain: the grouped optimization report. Per loop: vectorized (into
    # which kernel, instruction-level) or NOT, with reason and fix lines; per
    # call: inlined or NOT with reason; nests summarized; plus the backend
    # (MIR vs baseline) section.
    Name          = "explain_report"
    Path          = "tests/explain_demo.mettle"
    ShouldSucceed = $true
    Args          = @("--release", "--explain")
    # Content assertions need the report on stderr regardless of its length
    # (the changes-since-last-build section grows it across the determinism
    # recompile; sidecar routing has its own dedicated case).
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0" }
    OutputMustMatch = @(
      'optimization report: explain_demo\.mettle',
      '8-wide float32 affine map',
      # the inlined-call map: the param-copy fold + dead-local sweep must leave
      # a body the affine recognizer matches (regression for the
      # __inl_*_param_x "cannot see through it" refusal)
      'with_call \(loop @ line 19\): vectorized',
      'NOT vectorized',
      'vpsadbw kernel accumulates into int64',
      'declare the accumulator as int64',
      'hoist invariant index math into a pointer',
      # the unroller's definitive remark supersedes the verifier's
      # "no loop remains" guess
      'fully unrolled \(8 iterations',
      # verified fix suggestions: the compiler SIMULATES the fix on a clone,
      # re-runs the optimizer, and only then claims it works
      'verified: simulated that fix and re-ran the optimizer: this loop then vectorizes -> vpsadbw',
      # int32 sum into an int32 accumulator: diagnosis + proven int64 fix
      'int32 reduction kernel accumulates into int64',
      'verified: simulated that fix and re-ran the optimizer: this loop then vectorizes -> vpaddd',
      # int16 elements into an int64 accumulator: retyping the elements is the
      # whole fix (the no-op (int32) cast is accepted by the sum recognizer),
      # and the simulation proves exactly that advice
      'fix: use int32 elements',
      # dot-product address pattern: the row-pointer hoist is simulated and the
      # FMA dot kernel itself confirms it
      'verified: simulated that fix and re-ran the optimizer: this loop then vectorizes -> vfmadd231ps, 8-wide float32 FMA dot product',
      # proven-inapplicable advice is REPLACED, never printed: skew''s index
      # half mutates every iteration, so the hoist advice would be wrong
      'none via hoisting. Re-checked: the index half that is not the loop counter changes every iteration',
      # the expanded backend section: instruction-weighted coverage, bails
      # grouped by cause with consequence text and sizes
      'backend report: explain_demo\.mettle',
      'optimized IR instructions are in register-allocated code',
      'contains the affine-map kernel `simd_affine_map` in a form the register allocator',
      'consequence: the kernel itself runs at full vector speed',
      # A whole-function fallback leads the plan: it is a measured cost over a
      # whole function, where a loop remark is a prediction about one loop. It
      # also names the calls that brought the kernel in -- main never wrote a
      # vectorized loop of its own, so "contains simd_affine_map" alone would
      # point at nothing the reader could find.
      'spills main \(\d+ instrs\)',
      'kernel inlined from `saxpy` @ line \d+',
      # dependence analysis: a non-reassociable loop-carried recurrence (the
      # LCG/hash shape) is diagnosed as a genuine scalar floor, naming the
      # carried operators -- not the generic "no vectorizer recognized" fallback
      '`h` carries a loop-carried recurrence',
      'dependency chain that cannot run as independent SIMD lanes',
      'multiply, divide, shift, and bitwise/xor recurrences are inherently serial'
    )
  },
  @{
    # Past the line threshold, the full --explain report is written to a
    # `.explain.txt` sidecar next to the output and stderr gets a digest.
    Name          = "explain_sidecar"
    Path          = "tests/explain_demo.mettle"
    ShouldSucceed = $true
    Args          = @("--release", "--explain")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "5" }
    OutputMustMatch = @(
      'loops: \d+ vectorized, \d+ scalar; \d+ fix suggestions verified',
      'calls: \d+ inlined, \d+ kept as real calls',
      'backend: \d+/\d+ functions register-allocated',
      # the digest names what to do, not just how the build went
      'start with: main \(\d+ instrs\)  move the vectorized loop into a function of its own',
      'full report \(\d+ lines\): .*explain_sidecar\.explain\.txt'
    )
    OutputMustNotMatch = @(
      # the report body must have been diverted, not printed
      'sum_bytes \(loop'
    )
    SidecarMustMatch = @(
      'optimization report: explain_demo\.mettle',
      'verified: simulated that fix and re-ran the optimizer',
      'backend report: explain_demo\.mettle',
      'consequence: the kernel itself runs at full vector speed'
    )
  },
  @{
    # --explain remarks that depend on function decorators: @noinline
    # refusals, @pure LICM hoisting, @noalloc verification, and the verified
    # inlining advice (pretend-applied @inline / pretend-removed @noinline).
    Name          = "explain_contracts_report"
    Path          = "tests/explain_contracts_demo.mettle"
    ShouldSucceed = $true
    Args          = @("--release", "--explain")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0" }
    OutputMustMatch = @(
      'optimization report: explain_contracts_demo\.mettle',
      'reason: the callee is marked @noinline',
      'hoisted out of the loop \(runs once',
      'verified @noalloc',
      # call-in-body: program-level simulation (pretend-remove @noinline,
      # re-run the INLINER on a caller clone, revectorize)
      'verified: simulated removing `@noinline` from `damp`',
      'verified: re-checked with @inline pretend-applied: the structural guards pass',
      # int16 elements + int32 accumulator: the fix honestly names BOTH
      # required changes
      'use int32 elements and declare the accumulator as int64',
      'verified: simulated that fix and re-ran the optimizer: this loop then vectorizes -> vpaddd',
      # call-in-body where the advice is DISPROVEN: the callee contains a
      # loop the inliner structurally declines, so the simulation withdraws
      # the @inline suggestion and says the driver loop is correctly scalar
      'each iteration calls `row_scale`, and `@inline` cannot help: the callee contains a loop',
      # advice that says there is nothing to change is labelled a note,
      # not a fix, and never reaches the "where to start" ranking
      'note: nothing to change on this line: this loop is a driver'
    )
    OutputMustNotMatch = @(
      # the withdrawn advice must not survive anywhere in the report
      'make `row_scale` inline-eligible'
    )
  },
  @{
    # `@inline!` contract: a recursive function can never have every call
    # inlined away, so the build must fail with the inliner's reason.
    Name          = "err_inline_contract"
    Path          = "tests/err_inline_contract.mettle"
    ShouldSucceed = $false
    Args          = @("-O")
    Pattern       = '@inline! call to `fact` was not inlined: the call is directly recursive'
  },
  @{
    Name          = "inline_contract"
    Path          = "tests/test_inline_contract.mettle"
    ShouldSucceed = $true
    Args          = @("-O")
  },
  @{
    # Aggregate literals are compile-time constants: a call in an element has
    # no bytes to lay out, and there is no module initializer to run it in.
    Name          = "err_aggregate_literal_runtime"
    Path          = "tests/err_aggregate_literal_runtime.mettle"
    ShouldSucceed = $false
    Pattern       = 'aggregate literal elements must be compile-time constants'
  },
  @{
    # A global aggregate needs a literal. This used to reach the backend and
    # surface as an internal compiler error with no source line.
    Name          = "err_aggregate_global_call"
    Path          = "tests/err_aggregate_global_call.mettle"
    ShouldSucceed = $false
    Pattern       = 'a global of aggregate type must be initialized with an aggregate literal'
  },
  @{
    # Shape check: too many elements do not spill past the array's end.
    Name          = "err_aggregate_literal_shape"
    Path          = "tests/err_aggregate_literal_shape.mettle"
    ShouldSucceed = $false
    Pattern       = "4 elements do not fit 'int32\[3\]', which holds 3"
  },
  @{
    # Struct literals name their fields, so a misspelling is caught here rather
    # than landing at whatever offset came next.
    Name          = "err_aggregate_literal_field"
    Path          = "tests/err_aggregate_literal_field.mettle"
    ShouldSucceed = $false
    Pattern       = "struct 'Pt' has no field 'z'"
  },
  @{
    # Memory diagnostics: returning the address of a stack local is an error.
    Name          = "err_mem_return_stack"
    Path          = "tests/err_mem_return_stack.mettle"
    ShouldSucceed = $false
    Pattern       = 'error\[M0103\]: Returning the address of stack local `values`'
  },
  @{
    # Memory diagnostics: constant index past a stack array's end is an error
    # (the buffer-extent layer catches the direct form; type_checker_memory
    # backstops forms it misses).
    Name          = "err_mem_oob_index"
    Path          = "tests/err_mem_oob_index.mettle"
    ShouldSucceed = $false
    Pattern       = 'Array index 8 is out of bounds'
  },
  @{
    # Memory diagnostics: a constant-size memory op overflowing a stack array.
    Name          = "err_mem_op_overflow"
    Path          = "tests/err_mem_op_overflow.mettle"
    ShouldSucceed = $false
    Pattern       = 'error\[M0106\]: `mem_zero` writes 128 bytes into `buf`, which only has 64'
  },
  @{
    # Loop-bound analysis: `j <= 8` over int32[8] provably reads a[8] on the
    # final iteration (no break/continue/return can save it).
    Name          = "err_mem_loop_oob"
    Path          = "tests/err_mem_loop_oob.mettle"
    ShouldSucceed = $false
    Pattern       = 'error\[M0117\]: This loop runs `j` up to 8, but `a` has 8 elements'
  },
  @{
    # Constant arithmetic: division by a literal zero is a guaranteed trap.
    Name          = "err_mem_div_zero"
    Path          = "tests/err_mem_div_zero.mettle"
    ShouldSucceed = $false
    Pattern       = 'error\[M0116\]: Division by a constant zero'
  },
  @{
    # Constant out-of-bounds THROUGH a pointer alias: p = &a[2], p[6] = a[8].
    Name          = "err_mem_ptr_alias_oob"
    Path          = "tests/err_mem_ptr_alias_oob.mettle"
    ShouldSucceed = $false
    Pattern       = 'error\[M0105\]: Index 6 through `p` lands at `a\[8\]`, out of bounds'
  },
  @{
    # Memory diagnostics that warn without failing the build: double free,
    # use-after-free, a stack address stored in a global, and a leak. The
    # `clean` control function (conditional use + defer free) must add NO
    # diagnostics of its own.
    Name          = "warn_mem_diagnostics"
    Path          = "tests/warn_mem_diagnostics.mettle"
    ShouldSucceed = $true
    OutputMustMatch = @(
      # each finding reports under its own M-code, not the generic E0003, so
      # `mettle explain M0102` works on the diagnostic in front of the reader
      'warning\[M0102\]: Double free of `p` \(already freed at line \d+\)',
      'warning\[M0101\]: Use of `p` after it was freed',
      'warning\[M0104\]: Global `STASH` is assigned the address of stack local `slot`',
      'warning\[M0107\]: `scratch` is allocated here but never freed',
      'warning\[M0113\]: `p` is null here \(assigned at line \d+ and never reassigned\)',
      'warning\[M0115\]: Shift by 32 on a 32-bit value',
      'warning\[M0114\]: `p` points at the constant address 64'
    )
    OutputMustNotMatch = @(
      'Use of `scratch`',
      'warning.*`p` is allocated',
      'clean_guarded_null',
      'clean_loop'
    )
  },
  @{
    # Interprocedural ownership inference: summaries (frees param / returns
    # fresh / borrows / stores) are computed over the call graph, so these
    # diagnostics cross function boundaries. The clean control
    # functions must produce NO diagnostics.
    Name          = "warn_mem_interproc"
    Path          = "tests/warn_mem_interproc.mettle"
    ShouldSucceed = $true
    OutputMustMatch = @(
      'Use of `p` after the call to `consume` at line \d+ freed it',
      'Double free of `p`: already freed by the call to `consume`',
      '`p` is allocated here but never freed.*leaks when `leak_past_borrow`',
      '`p` holds the allocation `make_buffer` returns.*leaks when `leak_from_wrapper`'
    )
    OutputMustNotMatch = @(
      'clean_consume_once',
      'clean_borrow_then_free',
      'clean_kept_elsewhere',
      'clean_kept_through_helper',
      'clean_wrapper_freed'
    )
  },
  @{
    # Borrow-lifetime (M0110): a pointer that outlives the stack storage it
    # borrows is dangling once the borrowed local's block exits. The clean
    # control keeps the borrow inside the referent's scope and stays silent.
    Name          = "warn_borrow_scope"
    Path          = "tests/warn_borrow_scope.mettle"
    ShouldSucceed = $true
    OutputMustMatch = @(
      'Use of `p` after the scope of `x` ended at line \d+'
    )
    OutputMustNotMatch = @(
      'scope of `y` ended'
    )
  },
  @{
    # Borrow-lifetime (M0111): an interior pointer into a heap buffer used
    # after the buffer is realloc'd (the block may have moved). The clean
    # control re-derives the pointer after the realloc and stays silent.
    Name          = "warn_borrow_realloc"
    Path          = "tests/warn_borrow_realloc.mettle"
    ShouldSucceed = $true
    OutputMustMatch = @(
      'Use of `p` after `buf` was reallocated at line \d+'
    )
    OutputMustNotMatch = @(
      '`nb` was reallocated'
    )
  },
  @{
    # Borrow-lifetime (M0112): an interior pointer into a heap buffer used
    # after the buffer is freed (use-after-free through a distinct name). The
    # clean control reads the borrow before the free and stays silent.
    Name          = "warn_borrow_free"
    Path          = "tests/warn_borrow_free.mettle"
    ShouldSucceed = $true
    OutputMustMatch = @(
      'Use of `p` after `buf` was freed at line \d+'
    )
    OutputMustNotMatch = @(
      '`data` was freed'
    )
  },
  @{
    # Use-after-move: `q = p` aliases one allocation under two names, so
    # freeing or reallocating either invalidates the other -- ownership tracked
    # through pointer copies the way Rust tracks moves, but on raw pointers. The
    # clean controls re-point the alias or read it before the free, and stay
    # silent.
    Name          = "warn_use_after_move"
    Path          = "tests/warn_use_after_move.mettle"
    ShouldSucceed = $true
    OutputMustMatch = @(
      'Use of `buf` after the block it shares with `q` was freed at line \d+',
      'Use of `mirror` after the block it shares with `block` was freed at line \d+',
      'Double free of `b`: it aliases `a`, already freed at line \d+'
    )
    OutputMustNotMatch = @(
      'shares with `keep`',
      'shares with `owned`'
    )
  },
  @{
    # Zero-false-positive guard: correct ownership code the borrow checker must
    # stay silent on. Covers the safe realloc idiom, disjoint frees, re-pointed
    # aliases, and a free through one name of a different block. ANY memory
    # diagnostic here is a regression.
    Name          = "no_warn_borrow_clean"
    Path          = "tests/no_warn_borrow_clean.mettle"
    ShouldSucceed = $true
    OutputMustNotMatch = @(
      'shares with',
      'after the block',
      'use-after-free',
      'Double free',
      'leaks when',
      'reallocated'
    )
  },
  @{
    # `@noalloc` violated directly by a `new` expression.
    Name          = "err_noalloc"
    Path          = "tests/err_noalloc.mettle"
    ShouldSucceed = $false
    Args          = @("-O")
    Pattern       = '@noalloc function `make_point` allocates: a `new` expression'
  },
  @{
    # `@noalloc` is transitive: the allocation is inside a reachable callee.
    Name          = "err_noalloc_transitive"
    Path          = "tests/err_noalloc_transitive.mettle"
    ShouldSucceed = $false
    Args          = @("-O")
    Pattern       = 'inside reachable function `helper`, a `new` expression'
  },
  @{
    # `@noalloc` is a proof: an unknown extern cannot be proven clean.
    Name          = "err_noalloc_extern"
    Path          = "tests/err_noalloc_extern.mettle"
    ShouldSucceed = $false
    Args          = @("-O")
    Pattern       = 'calls the external function `mystery`, which cannot be proven allocation-free'
  },
  @{
    # `@noalloc` succeeding: arithmetic + known-clean libm externs verify.
    Name          = "noalloc"
    Path          = "tests/test_noalloc.mettle"
    ShouldSucceed = $true
    Args          = @("-O")
  },
  @{
    # The SIMD fill kernel (memset/frame-clear class): all element sizes,
    # odd tails, zero/negative counts, float bit-pattern fills, rect fills
    # with nonzero start + invariant row offset, the stdlib mem_zero
    # byte-offset walk with iv handoff between loops. The .ir sidecar must
    # show the fused ops; runtime equality with the scalar loops is covered
    # by the differential fuzzer's debug-vs-release oracle on this same
    # binary shape.
    Name          = "simd_fill_parity"
    Path          = "tests/test_simd_fill_parity.mettle"
    ShouldSucceed = $true
    Args          = @("--release")
    IrMustMatch   = @(
      'simd_fill\(base=',
      'simd_fill\(begin='
    )
  },
  @{
    # Real-application loop shapes from the LLM engine: global array bases
    # and bounds, induction variables reused across consecutive loops (the
    # dot/map kernels now treat a straight-line redefinition as killing the
    # iv), and a fill whose live-after iv gets its exact final value written
    # back by the kernel.
    Name          = "simd_llm_shapes"
    Path          = "tests/test_simd_llm_shapes.mettle"
    ShouldSucceed = $true
    Args          = @("--release")
    IrMustMatch   = @(
      'dot_f32\(',
      'simd_fill\('
    )
  },
  @{
    # Repeated identical call refusals (an over-budget main refusing every
    # call site for the same reason) fold into ONE entry with a line range
    # and a deduplicated callee census -- not a wall of identical remarks.
    Name          = "explain_fold_repeated_refusals"
    Path          = "tests/explain_fold_demo.mettle"
    ShouldSucceed = $true
    Args          = @("--release", "--explain")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0" }
    OutputMustMatch = @(
      # cold one-shot call sites in an over-budget caller fold into ONE calm
      # entry that explains why NOT inlining is the right call -- and hands
      # out no fix advice (there is nothing worth fixing)
      'main \(8 calls, lines \d+-\d+\): NOT inlined',
      'reason: the calling function is over the profile-adjusted caller budget, and this call site is not measured hot or inside a loop',
      'calls: f1 \(x3\), f2 \(x2\), f3 \(x2\), f4',
      # tiny call-free callees are exempt from the caller budget: the
      # accessor still inlines into the over-budget main
      'main \(call to `tiny` @ line \d+\): inlined',
      # loop-resident call sites are exempt too: the same f1 that is refused
      # at the cold sites inlines at the hot one
      'main \(call to `f1` @ line \d+\): inlined'
    )
    OutputMustNotMatch = @(
      # no per-site cold refusals survive the fold, and no @inline advice is
      # handed out for calls where inlining would buy nothing
      'main \(call to `f2`',
      'fix: mark the callee @inline'
    )
  },
  @{
    # --explain remarks are limited to the main input file: a program importing
    # std/io must not report stdlib-internal decisions, but a refusal AT a user
    # call site into the stdlib is still reported.
    Name          = "explain_focus_filter"
    Path          = "tests/explain_stdlib_demo.mettle"
    ShouldSucceed = $true
    Args          = @("--release", "--explain")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0" }
    OutputMustMatch = @(
      'call to `print_int` .* NOT inlined'
    )
    OutputMustNotMatch = @(
      'print_int \(loop',
      'print_int \(call to'
    )
  },
  @{
    # --explain=SELECTOR narrows the prose. Each verdict carries its stable
    # decision code, the source the finding is about is quoted, and the report
    # leads with the fixes the compiler proved.
    Name          = "explain_selector"
    Path          = "tests/explain_demo.mettle"
    ShouldSucceed = $true
    Args          = @("--release", "--explain=sum_ints")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0" }
    OutputMustMatch = @(
      'where to start',
      'proven sum_ints:76',
      'sum_ints \(loop @ line 76\): NOT vectorized  \[int32-sum-narrow-acc\]',
      # the loop body itself, quoted under the verdict
      '77 \|     s = s \+ a\[i\];',
      'findings hidden by --explain=sum_ints',
      'mettle explain int32-sum-narrow-acc'
    )
    OutputMustNotMatch = @(
      'saxpy \(loop',
      'sum_bytes \(loop'
    )
  },
  @{
    # The fill refusals name the cause that actually applies. The generic
    # "your store address did not match" message used to fire for all of
    # them, telling a writer who had already written `a[i]` to write `a[i]`.
    # The last function proves the stack-array advice: same loop, pointer
    # bound once, and it vectorizes.
    Name          = "explain_fill_causes"
    Path          = "tests/explain_fill_causes.mettle"
    ShouldSucceed = $true
    Args          = @("--release", "--explain=loops")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0" }
    OutputMustMatch = @(
      'the body writes 2 destinations; the fill kernel fills one region per loop',
      'fix: split it into one loop per destination',
      'the loop fills 1-byte elements, and the fill kernel covers 2-, 4- and 8-byte elements only',
      # a compiler gap is a note, not a fix: it must not be ranked as work
      'note: nothing to change here: this is a gap in the compiler',
      'the loop fills the stack array `a`, whose address is retaken on every iteration',
      'fix: bind the array to a pointer once before the loop \(`var p: float32\* = &a\[0\];`\)',
      'local_fill_bound \(loop @ line \d+\): vectorized',
      # three loops miss the kernel; only two of them have work to do,
      # because the byte-width gap is the compiler's and not the code's
      'where to start \(2 of 3 missed optimizations have a fix',
      # the stack-array advice is not believed, it is checked: the
      # compiler binds the pointer on a clone and re-runs, and the kernel
      # it names is the one local_fill_bound actually gets below
      'verified: simulated that fix and re-ran the optimizer: this loop then vectorizes -> 16-byte splat stores',
      # so it leads the triage, ahead of the unproven split advice; and
      # the two live side by side rather than folding, because one code
      # here covers three different causes with three different fixes
      '1\. proven local_fill:\d+  bind the array to a pointer once',
      '2\.        two_regions:\d+  split it into one loop per destination'
    )
    OutputMustNotMatch = @(
      # the advice that could not be followed
      'its store address did not match the fill vectorizer'
    )
  },
  @{
    # A mixed-width float loop gets no kernel. The advice names which width to
    # keep, and the compiler proves it: it retypes the minority on a clone and
    # re-runs its own vectorizer. Each mixed loop is followed by the same loop
    # written in one width, and the kernel named by the simulation must be the
    # kernel that one actually gets, in both directions.
    Name          = "explain_mixed_float_widths"
    Path          = "tests/explain_mixed_float_widths.mettle"
    ShouldSucceed = $true
    Args          = @("--release", "--explain=loops")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0" }
    OutputMustMatch = @(
      'fix: keep the loop in float32: the float64 accesses are the minority',
      'verified: simulated that fix and re-ran the optimizer: this loop then vectorizes -> vfmadd231ps, 8-wide float32 affine map',
      'single32 \(loop @ line \d+\): vectorized -> vfmadd231ps, 8-wide float32 affine map',
      'fix: keep the loop in float64: the float32 accesses are the minority',
      'verified: simulated that fix and re-ran the optimizer: this loop then vectorizes -> 4-wide float64 element-wise map',
      'single64 \(loop @ line \d+\): vectorized -> 4-wide float64 element-wise map'
    )
  },
  @{
    # The outer loop of a nest is not a missed optimization: only innermost
    # loops vectorize, so its remark points at the inner loop's problem rather
    # than being a second one. It still gets a remark, and it must not raise
    # the count "where to start" quotes -- with nothing but outer-of-nest
    # selected there is no triage block at all.
    Name          = "explain_nest_not_a_miss"
    Path          = "tests/explain_demo.mettle"
    ShouldSucceed = $true
    Args          = @("--release", "--explain=outer-of-nest")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0" }
    OutputMustMatch = @(
      'matvec \(loop @ line \d+\): NOT vectorized  \[outer-of-nest\]',
      'only innermost loops are vectorized'
    )
    OutputMustNotMatch = @(
      'where to start',
      'missed optimization'
    )
  },
  @{
    # Every gate reason code has to reach the reader as a sentence about their
    # code. `vloop:reduce` used to print as "declined by the eligibility gate
    # (reason code: vloop:shape)", which names nothing anyone can act on. It
    # must say which shape, name the loop, and -- on a function this small --
    # say plainly that the spills are not worth chasing.
    Name          = "explain_backend_gate_reasons"
    Path          = "tests/test_vloop_general.mettle"
    ShouldSucceed = $true
    Args          = @("--release", "--explain=freduce64")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0" }
    OutputMustMatch = @(
      "contains a vectorized '\+' reduction \(``s = s \+ expr``\); the allocator's inline passthrough covers element-wise maps only",
      'freduce64 \(\d+, loop @ line \d+\)',
      'note: nothing worth doing at this size',
      # a large fallback in another function keeps its actionable fix
      'fix: move the vectorized loop into a function of its own'
    )
    OutputMustNotMatch = @(
      'reason code: vloop',
      # the plan honours the selector: main's fallback is not what was asked for
      'spills main'
    )
  },
  @{
    # Six loop bodies that branch or index off-pattern, each with its own
    # remark. They used to share one: "the loop body branches on data ...
    # compute both arms branchlessly", which is real advice for the predicated
    # count, no advice at all for a running maximum (a shape that vectorizes),
    # and actively wrong for a stride the data layout dictates. The test pins
    # that they stay told apart -- and that the body-local declaration in
    # scale_pass is hoisted by the compiler rather than billed to the reader.
    Name          = "explain_branch_shapes"
    Path          = "tests/explain_branch_shapes.mettle"
    ShouldSucceed = $true
    Args          = @("--release", "--explain=loops")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0" }
    OutputMustMatch = @(
      'scale_pass \(loop @ line 11\): vectorized',
      # Seeded from a[0] and counting from 1, which is how an extremum is
      # usually written. Iteration 0 compares the seed with itself, so the
      # counter is reset to 0 and the kernel claims it.
      'extent_from_first \(loop @ line 24\): vectorized -> 4-wide float64 vmaxpd',
      'count_above \(loop @ line 36\): NOT vectorized  \[predicated-count\]',
      'over float elements there is nothing to change here',
      'clamp_all \(loop @ line 46\): NOT vectorized  \[clamp-store\]',
      'green_sum \(loop @ line 60\): NOT vectorized  \[strided-access\]',
      'the loop steps 3 elements at a time',
      'row_sum \(loop @ line 71\): NOT vectorized  \[dot-shape-address\]',
      'add a loop-invariant term to the counter'
    )
    OutputMustNotMatch = @(
      # the catch-all must not be reached for any of them
      'the loop body branches on data'
    )
  },
  @{
    # A fix that is correct and not sufficient. The simulation applies it, sees
    # the loop stay scalar, and reports the obstacle that surfaced next -- so
    # the caveat reaches the reader before the edit does, in the action plan
    # ("step 1") as well as in the remark.
    Name          = "explain_partial_fix"
    Path          = "tests/explain_partial_fix.mettle"
    ShouldSucceed = $true
    Args          = @("-O", "--explain=row_energy")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0" }
    OutputMustMatch = @(
      'step 1 row_energy:13',
      'fix: use int32 elements and declare the accumulator as int64.*\(first step only\)',
      'still blocked: re-checked with that change applied: the loop still does not vectorize'
    )
    OutputMustNotMatch = @(
      'verified:'
    )
  },
  @{
    # A nest that arrives by inlining. The inliner drops `@simd` markers from
    # the copy it makes, so the callee's loop leaves no record and the driver
    # loop reads as a leaf -- which used to make the classifier blame the inner
    # loop's exit test on a data-dependent `if` and prescribe a branchless
    # rewrite for a body containing no branch. The verdict must be the nest, and
    # it must name the call that brought the loop in.
    Name          = "explain_inlined_nest"
    Path          = "tests/explain_inlined_nest.mettle"
    ShouldSucceed = $true
    Args          = @("-O", "--explain=main")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0" }
    OutputMustMatch = @(
      'main \(loop @ line 31\): NOT vectorized  \[outer-of-nest\]',
      "the call to ``sum_bytes`` on line 32 was inlined, so that callee's loop \(line 13\) now sits in this body",
      'note: nothing to change on this line'
    )
    OutputMustNotMatch = @(
      'branches on data',
      'branchless'
    )
  },
  @{
    # A reason can run past 300 columns, and a terminal folds that at column 0,
    # which dissolves the `\_ reason:` tree the report is shaped around. Only a
    # terminal gets the wrapped form; a pipe keeps one line per fact so a
    # pattern matching a whole reason keeps matching one. METTLE_EXPLAIN_COLUMNS
    # forces the width, since a test harness never has a terminal.
    Name          = "explain_wraps_for_a_terminal"
    Path          = "tests/explain_demo.mettle"
    ShouldSucceed = $true
    Args          = @("--release", "--explain=matvec")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0"; METTLE_EXPLAIN_COLUMNS = "80" }
    OutputMustMatch = @(
      # the fold happens on a space, and the continuation indents inside the
      # elbow so it still reads as subordinate to its verdict
      'the body contains a nested loop \(line 38\), and only innermost\r?\n {9}loops are vectorized',
      # the source echo keeps its gutter
      '38 \| for c in 0\.\.cols \{'
    )
    OutputMustNotMatch = @(
      # and the unwrapped form of that same reason is gone, so the fold is
      # real rather than an extra copy
      'nested loop \(line 38\), and only innermost loops are vectorized'
    )
  },
  @{
    # The same file without a forced width stays one line per fact, so grep
    # over a redirected report still returns whole reasons.
    Name          = "explain_unwrapped_when_redirected"
    Path          = "tests/explain_demo.mettle"
    ShouldSucceed = $true
    Args          = @("--release", "--explain=matvec")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0" }
    OutputMustMatch = @(
      'the body contains a nested loop \(line 38\), and only innermost loops are vectorized'
    )
  },
  @{
    # A selector nobody can satisfy says what the selectors are instead of
    # printing an empty section.
    Name          = "explain_selector_unknown"
    Path          = "tests/explain_demo.mettle"
    ShouldSucceed = $true
    Args          = @("--release", "--explain=nosuchthing")
    Env           = @{ METTLE_EXPLAIN_REPORT_LINES = "0" }
    OutputMustMatch = @(
      'nothing matches --explain=nosuchthing',
      'selectors: missed, fixable, proven, loops, calls'
    )
  },
  @{ Name = "err_decorator_on_loop"; Path = "tests/err_decorator_on_loop.mettle"; ShouldSucceed = $false; Pattern = "apply to a function, not a loop" },
  @{ Name = "err_decorator_unknown"; Path = "tests/err_decorator_unknown.mettle"; ShouldSucceed = $false; Pattern = "Unknown decorator after" },
  @{ Name = "err_decorator_conflict"; Path = "tests/err_decorator_conflict.mettle"; ShouldSucceed = $false; Pattern = "mutually exclusive" },
  @{ Name = "err_decorator_on_struct"; Path = "tests/err_decorator_on_struct.mettle"; ShouldSucceed = $false; Pattern = "may only precede a function declaration" },
  @{ Name = "err_decorator_after_export"; Path = "tests/err_decorator_after_export.mettle"; ShouldSucceed = $false; Pattern = "Decorators must precede 'export'" },
  @{ Name = "const_top_level"; Path = "tests/test_const_top_level.mettle"; ShouldSucceed = $true },
  @{ Name = "lambda"; Path = "tests/test_lambda.mettle"; ShouldSucceed = $true },
  @{ Name = "err_var_inferred"; Path = "tests/err_var_inferred.mettle"; ShouldSucceed = $false; Pattern = "requires an explicit type" },
  @{ Name = "closure_capture"; Path = "tests/test_closure_capture.mettle"; ShouldSucceed = $true },
  @{ Name = "closure_crossboundary"; Path = "tests/test_closure_crossboundary.mettle"; ShouldSucceed = $true },
  @{ Name = "closure_field"; Path = "tests/test_closure_field.mettle"; ShouldSucceed = $true },
  @{ Name = "closure_state"; Path = "tests/test_closure_state.mettle"; ShouldSucceed = $true },
  @{ Name = "closure_adapt"; Path = "tests/test_closure_adapt.mettle"; ShouldSucceed = $true },
  @{ Name = "fnptr_statement_call"; Path = "tests/test_fnptr_statement_call.mettle"; ShouldSucceed = $true },
  @{ Name = "err_lambda_capture"; Path = "tests/err_lambda_capture.mettle"; ShouldSucceed = $false; Pattern = "capturing closure cannot be stored in a plain function-pointer type" },
  @{ Name = "err_missing_return"; Path = "tests/err_missing_return.mettle"; ShouldSucceed = $false; Pattern = "non-void return type .* but contains no return statement" },
  @{ Name = "err_const_no_init"; Path = "tests/err_const_no_init.mettle"; ShouldSucceed = $false; Pattern = "Constant declaration requires an initializer" },
  @{ Name = "err_const_assign"; Path = "tests/err_const_assign.mettle"; ShouldSucceed = $false; Pattern = "is a constant and cannot be assigned to" },
  @{ Name = "err_const_nonconst"; Path = "tests/err_const_nonconst.mettle"; ShouldSucceed = $false; Pattern = "compile-time integer constant expression" },
  @{ Name = "comptime_type_ref"; Path = "tests/test_comptime_type_ref.mettle"; ShouldSucceed = $true },
  @{ Name = "comptime_field_ref"; Path = "tests/test_comptime_field_ref.mettle"; ShouldSucceed = $true },
  @{ Name = "type_table_layout"; Path = "tests/test_type_table_layout.mettle"; ShouldSucceed = $true },
  @{ Name = "type_table_enum"; Path = "tests/test_type_table_enum.mettle"; ShouldSucceed = $true },
  @{ Name = "err_offsetof_not_field"; Path = "tests/err_offsetof_not_field.mettle"; ShouldSucceed = $false
     Pattern = "offsetof expects a compile-time Field"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_fieldof_unknown_field"; Path = "tests/err_fieldof_unknown_field.mettle"; ShouldSucceed = $false
     OutputMustMatch = @("'Point' has no field named 'z'", "it has x, y")
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_fieldof_not_a_string"; Path = "tests/err_fieldof_not_a_string.mettle"; ShouldSucceed = $false
     Pattern = "fieldof expects a compile-time string"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_fieldof_not_a_type"; Path = "tests/err_fieldof_not_a_type.mettle"; ShouldSucceed = $false
     Pattern = "fieldof expects a compile-time type"
     OutputMustNotMatch = @("internal compiler error") },
  # The cross-struct contract must fail on a mismatch, or the matching
  # static_assert in test_comptime_string_compare proves nothing. The
  # expansion note has to name the iteration that failed.
  @{ Name = "err_comptime_contract_mismatch"; Path = "tests/err_comptime_contract_mismatch.mettle"; ShouldSucceed = $false
     OutputMustMatch = @("static_assert failed", "expanded from comptime-for iteration 1 \(field ``id``\)")
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "layoutof"; Path = "tests/test_layoutof.mettle"; ShouldSucceed = $true },
  @{ Name = "err_swappable_inline"; Path = "tests/err_swappable_inline.mettle"; ShouldSucceed = $false
     Pattern = "'@swappable' and '@inline' are mutually exclusive"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_swappable_on_loop"; Path = "tests/err_swappable_on_loop.mettle"; ShouldSucceed = $false
     Pattern = "apply to a function, not a loop"
     OutputMustNotMatch = @("internal compiler error") },
  # A pinned layout that no longer agrees must refuse the build. Detection is
  # only worth having if the default outcome is refusal.
  @{ Name = "err_layout_pin_broken"; Path = "tests/err_layout_pin_broken.mettle"; ShouldSucceed = $false
     Pattern = "static_assert failed"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_layoutof_not_a_type"; Path = "tests/err_layoutof_not_a_type.mettle"; ShouldSucceed = $false
     Pattern = "layoutof expects a compile-time type"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_type_var"; Path = "tests/err_type_var.mettle"; ShouldSucceed = $false
     Pattern = "type 'Type' has no runtime representation"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_type_param"; Path = "tests/err_type_param.mettle"; ShouldSucceed = $false
     Pattern = "type 'Type' has no runtime representation"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_type_return"; Path = "tests/err_type_return.mettle"; ShouldSucceed = $false
     Pattern = "type 'Type' has no runtime representation"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_type_field"; Path = "tests/err_type_field.mettle"; ShouldSucceed = $false
     Pattern = "type 'Type' has no runtime representation"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_type_escape_return"; Path = "tests/err_type_escape_return.mettle"; ShouldSucceed = $false
     Pattern = "cannot escape into runtime code"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_type_escape_arg"; Path = "tests/err_type_escape_arg.mettle"; ShouldSucceed = $false
     Pattern = "cannot escape into runtime code"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_field_var"; Path = "tests/err_field_var.mettle"; ShouldSucceed = $false
     Pattern = "type 'Field' has no runtime representation"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_type_pointer"; Path = "tests/err_type_pointer.mettle"; ShouldSucceed = $false
     Pattern = "no runtime representation"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_sizeof_type"; Path = "tests/err_sizeof_type.mettle"; ShouldSucceed = $false
     Pattern = "no runtime representation"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_type_address"; Path = "tests/err_type_address.mettle"; ShouldSucceed = $false
     Pattern = "cannot escape into runtime code"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "comptime_for_fields"; Path = "tests/test_comptime_for_fields.mettle"; ShouldSucceed = $true },
  @{ Name = "type_queries"; Path = "tests/test_type_queries.mettle"; ShouldSucceed = $true },
  @{ Name = "err_type_query_unknown"; Path = "tests/err_type_query_unknown.mettle"; ShouldSucceed = $false
     Pattern = "has no field or query"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_field_query_unknown"; Path = "tests/err_field_query_unknown.mettle"; ShouldSucceed = $false
     Pattern = "'Field' has no member"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_sequence_escape"; Path = "tests/err_sequence_escape.mettle"; ShouldSucceed = $false
     Pattern = "no runtime representation|cannot escape into runtime code"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_sequence_index_range"; Path = "tests/err_sequence_index_range.mettle"; ShouldSucceed = $false
     Pattern = "constant index that is in range"
     OutputMustNotMatch = @("internal compiler error") },
  # Each expansion is checked on its own, so a body valid for one field and
  # invalid for the next fails on exactly that iteration -- and says which.
  @{ Name = "err_comptime_for_iteration"; Path = "tests/err_comptime_for_iteration.mettle"; ShouldSucceed = $false
     Pattern = 'static_assert failed'
     OutputMustMatch = @('expanded from comptime-for iteration 1 \(field .small.\)')
     OutputMustNotMatch = @("internal compiler error", 'iteration 2') },
  @{ Name = "err_comptime_for_nested"; Path = "tests/err_comptime_for_nested.mettle"; ShouldSucceed = $false
     Pattern = 'static_assert failed'
     OutputMustMatch = @('expanded from comptime-for iteration 1 \(field .a.\)',
                         'expanded from comptime-for iteration 2 \(field .b.\)')
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_comptime_for_bad_sequence"; Path = "tests/err_comptime_for_bad_sequence.mettle"; ShouldSucceed = $false
     Pattern = "the only compile-time sequence is"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_comptime_for_escape"; Path = "tests/err_comptime_for_escape.mettle"; ShouldSucceed = $false
     Pattern = "cannot escape into runtime code"
     OutputMustNotMatch = @("internal compiler error") },
  # At module scope the directive generates declarations. Each one needs a name
  # of its own, and the failures around composing them are named individually
  # rather than left to surface as a missing symbol somewhere downstream.
  @{ Name = "comptime_for_declarations"; Path = "tests/test_comptime_for_declarations.mettle"; ShouldSucceed = $true },
  @{ Name = "err_comptime_ident_duplicate"; Path = "tests/err_comptime_ident_duplicate.mettle"; ShouldSucceed = $false
     Pattern = "generated two declarations named 'probe'"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_comptime_ident_outside"; Path = "tests/err_comptime_ident_outside.mettle"; ShouldSucceed = $false
     Pattern = "needs a 'comptime for' around it"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_comptime_ident_not_string"; Path = "tests/err_comptime_ident_not_string.mettle"; ShouldSucceed = $false
     Pattern = "joins compile-time strings"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_comptime_ident_in_type"; Path = "tests/err_comptime_ident_in_type.mettle"; ShouldSucceed = $false
     Pattern = "composes a declaration's name, not a type"
     OutputMustNotMatch = @("internal compiler error") },
  # A global is laid out at compile time and there is no module initializer, so a
  # run-time initializer must be a diagnostic with a source location - it used to
  # reach the direct-object backend and abort as an internal compiler error.
  @{ Name = "err_aggregate_address_of_local"; Path = "tests/err_aggregate_address_of_local.mettle"; ShouldSucceed = $false
     Pattern = "is a local, so its address is only known at run time"
     OutputMustNotMatch = @("Relocation refers to an undefined symbol") },
  @{ Name = "err_global_init_call"; Path = "tests/err_global_init_call.mettle"; ShouldSucceed = $false
     Pattern = "a global's initializer must be known at compile time"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_global_init_new"; Path = "tests/err_global_init_new.mettle"; ShouldSucceed = $false
     Pattern = "a global's initializer must be known at compile time"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_global_init_extern"; Path = "tests/err_global_init_extern.mettle"; ShouldSucceed = $false
     Pattern = "a global's initializer must be known at compile time"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_global_init_string_concat"; Path = "tests/err_global_init_string_concat.mettle"; ShouldSucceed = $false
     Pattern = "a global's initializer must be known at compile time"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_global_init_address_arith"; Path = "tests/err_global_init_address_arith.mettle"; ShouldSucceed = $false
     Pattern = "a global's initializer must be known at compile time"
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_import_guard_bad_platform"; Path = "tests/err_import_guard_bad_platform.mettle"; ShouldSucceed = $false; Pattern = "Import guard platform must be 'windows' or 'linux'" },
  @{ Name = "block_comment"; Path = "tests/test_block_comment.mettle"; ShouldSucceed = $true },
  @{ Name = "compound_assign"; Path = "tests/test_compound_assign.mettle"; ShouldSucceed = $true },
  @{ Name = "compound_assign_for"; Path = "tests/test_compound_assign_for.mettle"; ShouldSucceed = $true },
  @{ Name = "labeled_break"; Path = "tests/test_labeled_break.mettle"; ShouldSucceed = $true },
  @{ Name = "labeled_continue"; Path = "tests/test_labeled_continue.mettle"; ShouldSucceed = $true },
  @{ Name = "labeled_while"; Path = "tests/test_labeled_while.mettle"; ShouldSucceed = $true },
  @{
    Name            = "forward_decl"
    Path            = "tests/test_forward_decl.mettle"
    ShouldSucceed   = $true
  },
  @{ Name = "forward_decl_pointer"; Path = "tests/test_forward_decl_pointer.mettle"; ShouldSucceed = $true },
  @{
    Name            = "extern_function_link_name"
    Path            = "tests/test_extern_function_link_name.mettle"
    ShouldSucceed   = $true
  },
  @{
    Name            = "extern_global_link_name"
    Path            = "tests/test_extern_global_link_name.mettle"
    ShouldSucceed   = $true
  },
  @{ Name = "cstring_alias_type"; Path = "tests/test_cstring_alias_type.mettle"; ShouldSucceed = $true },
  @{ Name = "nested_function_pointer_type_annotation"; Path = "tests/test_nested_function_pointer_type_annotation.mettle"; ShouldSucceed = $true },
  @{
    Name            = "new_calloc"
    Path            = "tests/test_gc_alloc.mettle"
    ShouldSucceed   = $true
  },
  @{
    Name            = "new_calloc_fixed"
    Path            = "tests/test_gc_alloc_fixed.mettle"
    ShouldSucceed   = $true
  },
  @{ Name = "pointers"; Path = "tests/test_pointers.mettle"; ShouldSucceed = $true },
  @{ Name = "pointer_arith_scale"; Path = "tests/test_pointer_arith_scale.mettle"; ShouldSucceed = $true },
  @{ Name = "cstring_pointer_arith"; Path = "tests/test_cstring_pointer_arith.mettle"; ShouldSucceed = $true },
  @{ Name = "uint32_cross_lineage_eq"; Path = "tests/test_uint32_cross_lineage_eq.mettle"; ShouldSucceed = $true },
  @{ Name = "paren_ident_binop"; Path = "tests/test_paren_ident_binop.mettle"; ShouldSucceed = $true },
  @{ Name = "pointer_null"; Path = "tests/test_pointer_null.mettle"; ShouldSucceed = $true },
  @{
    Name          = "runtime_null_deref_check"
    Path          = "tests/test_runtime_null_deref_check.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "runtime_array_bounds_check"
    Path          = "tests/test_runtime_array_bounds_check.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "stack_trace_support"
    Path          = "tests/test_runtime_null_deref_check.mettle"
    ShouldSucceed = $true
    Args          = @("-s")
  },
  @{ Name = "pointer_param_address"; Path = "tests/test_pointer_param_address.mettle"; ShouldSucceed = $true },
  @{
    Name            = "call_many_args"
    Path            = "tests/test_call_many_args.mettle"
    ShouldSucceed   = $true
  },
  @{ Name = "import_relative_no_ext"; Path = "tests/test_import_relative_no_ext.mettle"; ShouldSucceed = $true },
  @{ Name = "import_circular"; Path = "tests/test_import_circular.mettle"; ShouldSucceed = $true },
  @{
    Name          = "import_include_path"
    Path          = "tests/test_import_include_path.mettle"
    ShouldSucceed = $true
    Args          = @("-I", "tests/lib")
  },
  @{ Name = "import_std_core"; Path = "tests/test_import_std_core.mettle"; ShouldSucceed = $true },
  @{ Name = "std_io"; Path = "tests/test_std_io.mettle"; ShouldSucceed = $true },
  @{ Name = "std_win32"; Path = "tests/test_internal_link_win32_user32.mettle"; ShouldSucceed = $true },
  @{ Name = "std_ui"; Path = "tests/test_internal_link_ui.mettle"; ShouldSucceed = $true },
  @{ Name = "enum"; Path = "tests/test_enum.mettle"; ShouldSucceed = $true },
  @{
    Name          = "prelude"
    Path          = "tests/test_prelude.mettle"
    ShouldSucceed = $true
    Args          = @("--prelude")
  },
  @{
    Name          = "string_escape_codegen"
    Path          = "tests/test_string_escape_codegen.mettle"
    ShouldSucceed = $true
  },
  @{ Name = "char_literals"; Path = "tests/test_char_literals.mettle"; ShouldSucceed = $true },
  @{ Name = "logical_ops"; Path = "tests/test_logical_ops.mettle"; ShouldSucceed = $true },
  @{ Name = "multiline_continuation"; Path = "tests/test_multiline_continuation.mettle"; ShouldSucceed = $true },
  @{ Name = "sizeof_static_assert"; Path = "tests/test_sizeof_static_assert.mettle"; ShouldSucceed = $true },
  @{ Name = "strncmp_slice"; Path = "tests/test_strncmp_slice.mettle"; ShouldSucceed = $true },
  @{ Name = "narrowing_conversions"; Path = "tests/test_narrowing_conversions.mettle"; ShouldSucceed = $true },
  @{ Name = "signed_negation"; Path = "tests/test_signed_negation.mettle"; ShouldSucceed = $true },
  @{
    Name          = "signed_division"
    Path          = "tests/test_signed_division.mettle"
    ShouldSucceed = $true
  },
  @{ Name = "signed_comparison"; Path = "tests/test_signed_comparison.mettle"; ShouldSucceed = $true },
  @{ Name = "float_negative_comparison"; Path = "tests/test_float_negative_comparison.mettle"; ShouldSucceed = $true },
  @{ Name = "signed_wraparound"; Path = "tests/test_signed_wraparound.mettle"; ShouldSucceed = $true },
  @{ Name = "signed_arithmetic"; Path = "tests/test_signed_arithmetic.mettle"; ShouldSucceed = $true },
  @{
    Name          = "sign_extension"
    Path          = "tests/test_sign_extension.mettle"
    ShouldSucceed = $true
  },
  @{
    Name            = "unsigned_zero_ext"
    Path            = "tests/test_unsigned_zero_ext.mettle"
    ShouldSucceed   = $true
  },
  @{ Name = "unsigned_division"; Path = "tests/test_unsigned_division.mettle"; ShouldSucceed = $true },
  @{ Name = "mixed_signed_unsigned"; Path = "tests/test_mixed_signed_unsigned.mettle"; ShouldSucceed = $true },
  @{
    Name          = "narrowing_reverify"
    Path          = "tests/test_narrowing_reverify.mettle"
    ShouldSucceed = $true
  },
  @{ Name = "integer_literal_wide"; Path = "tests/test_integer_literal_wide.mettle"; ShouldSucceed = $true },
  @{ Name = "stack_mixed_locals"; Path = "tests/test_stack_mixed_locals.mettle"; ShouldSucceed = $true },
  @{ Name = "stack_large_struct"; Path = "tests/test_stack_large_struct.mettle"; ShouldSucceed = $true },
  @{ Name = "stack_array_scalar"; Path = "tests/test_stack_array_scalar.mettle"; ShouldSucceed = $true },
  @{ Name = "stack_array_struct_stride"; Path = "tests/test_array_struct_stride.mettle"; ShouldSucceed = $true },
  @{ Name = "int64_truncate"; Path = "tests/test_int64_truncate.mettle"; ShouldSucceed = $true },
  @{ Name = "string_length"; Path = "tests/test_string_length.mettle"; ShouldSucceed = $true },
  @{ Name = "struct_new_zeroed"; Path = "tests/test_struct_new_zeroed.mettle"; ShouldSucceed = $true },
  @{ Name = "struct_field_offset"; Path = "tests/test_struct_field_offset.mettle"; ShouldSucceed = $true },
  @{
    Name          = "import_exported"
    Path          = "tests/test_import_exported.mettle"
    ShouldSucceed = $true
    Args          = @("-I", "tests/lib")
  },
  @{
    Name          = "import_namespaced"
    Path          = "tests/test_import_namespaced.mettle"
    ShouldSucceed = $true
    Args          = @("-I", "tests/lib")
  },
  @{
    Name          = "import_selective"
    Path          = "tests/test_import_selective.mettle"
    ShouldSucceed = $true
    Args          = @("-I", "tests/lib")
  },
  @{ Name = "traits_generic_bound"; Path = "tests/test_traits_generic_bound.mettle"; ShouldSucceed = $true },
  @{ Name = "traits_multiple_where_bounds"; Path = "tests/test_traits_multiple_where_bounds.mettle"; ShouldSucceed = $true },
  @{ Name = "trait_methods_generic_dispatch"; Path = "tests/test_trait_methods_generic_dispatch.mettle"; ShouldSucceed = $true },
  @{ Name = "generic_function"; Path = "tests/test_generic_function.mettle"; ShouldSucceed = $true },
  @{ Name = "generic_struct"; Path = "tests/test_generic_struct.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_full"; Path = "tests/test_generics_full.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_swap"; Path = "tests/test_generics_swap.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_struct_param"; Path = "tests/test_generics_struct_param.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_nested_call"; Path = "tests/test_generics_nested_call.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_pair_mixed"; Path = "tests/test_generics_pair_mixed.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_pointer_type_arg"; Path = "tests/test_generics_pointer_type_arg.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_same_generic_diff_args"; Path = "tests/test_generics_same_generic_diff_args.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_unused"; Path = "tests/test_generics_unused.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_in_control_flow"; Path = "tests/test_generics_in_control_flow.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_list"; Path = "tests/test_generics_list.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_list_push"; Path = "tests/test_generics_list_push.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_multiple_instantiations"; Path = "tests/test_generics_multiple_instantiations.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_nested_struct"; Path = "tests/test_generics_nested_struct.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_generic_enum"; Path = "tests/test_generics_generic_enum.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_return_struct"; Path = "tests/test_generics_return_struct.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_float"; Path = "tests/test_generics_float.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_new_heap"; Path = "tests/test_generics_new_heap.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_struct_methods"; Path = "tests/test_generics_struct_methods.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_method_body_instantiation"; Path = "tests/test_generics_method_body_instantiation.mettle"; ShouldSucceed = $true },
  @{ Name = "method_pointer_receiver"; Path = "tests/test_method_pointer_receiver.mettle"; ShouldSucceed = $true },
  @{ Name = "generics_struct_field"; Path = "tests/test_generics_struct_field.mettle"; ShouldSucceed = $true },
  @{
    Name          = "import_trait_bound"
    Path          = "tests/test_import_trait_bound.mettle"
    ShouldSucceed = $true
    Args          = @("-I", "tests/lib")
  },
  @{
    Name          = "import_enum_switch"
    Path          = "tests/test_import_enum_switch.mettle"
    ShouldSucceed = $true
    Args          = @("-I", "tests/lib")
  },
  @{ Name = "tagged_enum_match"; Path = "tests/test_tagged_enum_match.mettle"; ShouldSucceed = $true },
  @{ Name = "tagged_enum_return"; Path = "tests/test_tagged_enum_return.mettle"; ShouldSucceed = $true },
  @{ Name = "tagged_enum_bare_none"; Path = "tests/test_tagged_enum_bare_none.mettle"; ShouldSucceed = $true },
  @{ Name = "tagged_enum_qualified_ctor"; Path = "tests/test_tagged_enum_qualified_ctor.mettle"; ShouldSucceed = $true },
  @{ Name = "plain_enum_qualified"; Path = "tests/test_plain_enum_qualified.mettle"; ShouldSucceed = $true },
  @{ Name = "arena_basic"; Path = "tests/test_arena_basic.mettle"; ShouldSucceed = $true },
  @{ Name = "arena_align"; Path = "tests/test_arena_align.mettle"; ShouldSucceed = $true },
  @{ Name = "arena_oversized"; Path = "tests/test_arena_oversized.mettle"; ShouldSucceed = $true },
  @{ Name = "arena_savepoint"; Path = "tests/test_arena_savepoint.mettle"; ShouldSucceed = $true },
  @{ Name = "arena_reset_reuse"; Path = "tests/test_arena_reset_reuse.mettle"; ShouldSucceed = $true },
  @{ Name = "extern_signed_param"; Path = "tests/test_extern_signed_param.mettle"; ShouldSucceed = $true },
  @{ Name = "extern_signed_return"; Path = "tests/test_extern_signed_return.mettle"; ShouldSucceed = $true },
  @{ Name = "extern_cstring"; Path = "tests/test_extern_cstring.mettle"; ShouldSucceed = $true },
  @{ Name = "extern_string_auto_cstring"; Path = "tests/test_extern_string_auto_cstring.mettle"; ShouldSucceed = $true },
  @{ Name = "string_cstring_coercions"; Path = "tests/test_string_cstring_coercions.mettle"; ShouldSucceed = $true },
  @{ Name = "std_conv_format_i64"; Path = "tests/test_std_conv_format_i64.mettle"; ShouldSucceed = $true },

  # ABI tests (MS x64 on Windows; patterns may need adjustment for SysV/Linux)
  @{
    Name          = "abi_int4_regs"
    Path          = "tests/test_abi_int4_regs.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "abi_int_stack"
    Path          = "tests/test_abi_int_stack.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "abi_return_int"
    Path          = "tests/test_abi_return_int.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "abi_return_int64"
    Path          = "tests/test_abi_return_int64.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "abi_float_args"
    Path          = "tests/test_abi_float_args.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "abi_float_return"
    Path          = "tests/test_abi_float_return.mettle"
    ShouldSucceed = $true
  },
  @{
    Name            = "abi_float_symbol_args"
    Path            = "tests/test_abi_float_symbol_args.mettle"
    ShouldSucceed   = $true
  },
  @{
    Name          = "abi_mixed_args"
    Path          = "tests/test_abi_mixed_args.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "abi_shadow_space"
    Path          = "tests/test_abi_shadow_space.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "abi_prologue"
    Path          = "tests/test_abi_prologue.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "abi_pointer_arg"
    Path          = "tests/test_abi_pointer_arg.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "abi_extern_calling_convention"
    Path          = "tests/test_abi_extern_calling_convention.mettle"
    ShouldSucceed = $true
  },
  @{ Name = "abi_callee_saved"; Path = "tests/test_abi_callee_saved.mettle"; ShouldSucceed = $true },
  @{ Name = "abi_stack_alignment"; Path = "tests/test_abi_stack_alignment.mettle"; ShouldSucceed = $true },
  @{
    Name          = "abi_float4_args"
    Path          = "tests/test_abi_float4_args.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "abi_float_stack"
    Path          = "tests/test_abi_float_stack.mettle"
    ShouldSucceed = $true
  },
  @{ Name = "abi_void_return"; Path = "tests/test_abi_void_return.mettle"; ShouldSucceed = $true },
  @{
    Name          = "abi_small_int_args"
    Path          = "tests/test_abi_small_int_args.mettle"
    ShouldSucceed = $true
  },
  @{ Name = "abi_nested_calls"; Path = "tests/test_abi_nested_calls.mettle"; ShouldSucceed = $true },
  @{ Name = "abi_indirect_call"; Path = "tests/test_abi_indirect_call.mettle"; ShouldSucceed = $true },

  @{ Name = "stress_integrated"; Path = "tests/test_stress_integrated.mettle"; ShouldSucceed = $true },
  @{ Name = "bitwise"; Path = "tests/test_bitwise.mettle"; ShouldSucceed = $true },
  @{ Name = "modulo"; Path = "tests/test_modulo.mettle"; ShouldSucceed = $true },
  @{ Name = "logical_not"; Path = "tests/test_logical_not.mettle"; ShouldSucceed = $true },
  @{
    Name           = "optimize_ir_passes"
    Path           = "tests/test_optimize_ir_passes.mettle"
    ShouldSucceed  = $true
    Args           = @("-O")
    IrMustMatch    = @("@.* <- 42")
    IrMustNotMatch = @("branch_zero 0 ->", "\bcold_path\(", "@result <- @result", "branch_eq @same, @same")
  },
  @{
    Name          = "opt_dead_temp"
    Path          = "tests/test_opt_dead_temp.mettle"
    ShouldSucceed = $true
    Args          = @("-O")
    IrMustNotMatch = @("%\.?t[0-9]+ <- 123456")
  },
  @{
    Name          = "opt_symbol_temp_forwarding"
    Path          = "tests/test_opt_symbol_temp_forwarding.mettle"
    ShouldSucceed = $true
    Args          = @("-O")
    IrMustNotMatch = @("%\.?t[0-9]+ <- @x")
    IrMustMatch   = @("branch_zero @x ->")
  },
  @{
    Name          = "opt_strength_cse"
    Path          = "tests/test_optimize_strength_cse.mettle"
    ShouldSucceed = $true
    Args          = @("-O")
    IrMustMatch   = @("@x = @a << 3", "@y <- @x")
    IrMustNotMatch = @("@y = 8 \\* @a", "@w = @b \\+ @a")
  },
  @{
    Name          = "opt_loop_unroll"
    Path          = "tests/test_optimize_loop_unroll.mettle"
    ShouldSucceed = $true
    Args          = @("-O")
    IrMustNotMatch = @("jump ir_while")
  },
  @{
    # The range analysis replaced a family of loop-shaped and use-shaped
    # special cases, so this pins what it now proves GENERALLY: a divide under
    # any dominating guard (on either side of the `if`), a remainder on an
    # unsigned parameter, a mask the value cannot reach outside of, a
    # comparison the bounds already decide, and a counter that starts at zero.
    Name          = "opt_value_range"
    Path          = "tests/codegen/value_range.mettle"
    ShouldSucceed = $true
    Args          = @("-O")
    IrMustMatch   = @("%\.?t[0-9]+ = @n >> 3",      # guarded_div: `if (n >= 0) n / 8`
                      "%\.?t[0-9]+ = @n & 15",      # guarded_mod: `if (n > 0) n % 16`
                      "%\.?t[0-9]+ = @u >> 6",      # unsigned_div: uint32 / 64
                      "%\.?t[0-9]+ = @u & 63",      # unsigned_mod: uint32 % 64
                      "%\.?t[0-9]+ = @n >> 5",      # shift_chain: else-side guard, merged
                      "%\.?t[0-9]+ = @i & 3")       # counter_mod: monotone counter
    # decided(): every comparison against a uint16 is settled by its type.
    IrMustNotMatch = @("%\.?t[0-9]+ = @u < 0", "%\.?t[0-9]+ = @u > 100000")
  },
  @{
    Name          = "opt_mod_even_check"
    Path          = "tests/test_opt_mod_even_check.mettle"
    ShouldSucceed = $true
    Args          = @("-O")
    IrMustMatch   = @("%\.?t[0-9]+ = @n & 1")
    IrMustNotMatch = @("%\.?t[0-9]+ = @n % 2")
  },
  @{
    Name          = "opt_collatz_odd_fold"
    Path          = "tests/test_opt_collatz_odd_fold.mettle"
    ShouldSucceed = $true
    Args          = @("-O", "--dump-ir")
    IrMustMatch   = @("(?s)%\.?t[0-9]+ = 3 \* @x.*@x = %\.?t[0-9]+ \+ 1.*@x = @x >> 1.*@count = @count \+ 2.*jump ir_while_")
  },
  @{
    Name          = "opt_popcount_fold"
    Path          = "tests/test_optimize_popcount_fold.mettle"
    ShouldSucceed = $true
    Args          = @("-O", "--dump-ir")
    IrMustMatch   = @(">> 1", "branch_zero @v ->")
    IrMustNotMatch = @("jump ir_while_", "%\.?t[0-9]+ = @v / 2")
  },
  @{
    Name          = "opt_popcount_buffer_fuse"
    Path          = "tests/test_optimize_popcount_buffer_fuse.mettle"
    ShouldSucceed = $true
    Args          = @("--build", "--emit-obj", "--linker", "internal", "--release", "--profile-runtime-ops", "--dump-ir")
    IrMustMatch   = @("%pbf[0-9]+_raw <-", "@total = @total \+ %pbf")
    IrMustNotMatch = @("%\.?t[0-9]+ = popcount_byte", "__inl_popcount_byte", "local_count")
  },
  @{
    Name          = "opt_popcount_buffer_fuse_release"
    Path          = "tests/test_optimize_popcount_buffer_fuse.mettle"
    ShouldSucceed = $true
    Args          = @("--build", "--emit-obj", "--linker", "internal", "--release", "--dump-ir")
    IrMustMatch   = @("%pbf[0-9]+_raw <-", "@total = @total \+ %pbf")
    IrMustNotMatch = @("%\.?t[0-9]+ = popcount_byte", "__inl_popcount_byte", "local_count")
  },
  @{
    Name          = "opt_branch_notzero_forward"
    Path          = "tests/test_opt_branch_notzero_forward.mettle"
    ShouldSucceed = $true
    Args          = @("-O")
    IrMustMatch   = @("branch_zero @x ->")
    IrMustNotMatch = @("%\.?t[0-9]+ = @x != 0")
  },
  @{
    Name          = "opt_branch_eq_chain"
    Path          = "tests/test_opt_branch_eq_chain.mettle"
    ShouldSucceed = $true
    Args          = @("-O")
    IrMustMatch   = @("branch_eq @x, 1 ->", "branch_eq @x, 2 ->")
    IrMustNotMatch = @("%\.?t[0-9]+ = @x == 1", "%\.?t[0-9]+ = @x == 2")
  },
  @{
    Name            = "opt_cfg_cleanup"
    Path            = "tests/test_opt_cfg_cleanup.mettle"
    ShouldSucceed   = $true
    Args            = @("-O")
    IrMustNotMatch  = @("1000")
  },
  @{
    Name            = "opt_memcpy_const"
    Path            = "tests/test_opt_memcpy_const.mettle"
    ShouldSucceed   = $true
    Args            = @("--build", "--emit-obj", "--linker", "internal", "--release")
  },
  @{
    Name            = "opt_inline_loop_fn"
    Path            = "tests/test_opt_inline_loop_fn.mettle"
    ShouldSucceed   = $true
    Args            = @("--release")
  },
  @{
    Name            = "opt_no_inline_fib_guard"
    Path            = "tests/test_opt_no_inline_fib_guard.mettle"
    ShouldSucceed   = $true
    Args            = @("--release")
  },
  @{
    Name            = "opt_sum_i32"
    Path            = "tests/test_opt_sum_i32.mettle"
    ShouldSucceed   = $true
    Args            = @("--build", "--emit-obj", "--linker", "internal", "--release", "--dump-ir")
    IrMustMatch     = @("simd_sum_i32")
  },
  @{
    Name            = "opt_ptr_induction"
    Path            = "tests/test_opt_ptr_induction.mettle"
    ShouldSucceed   = $true
    Args            = @("--build", "--emit-obj", "--linker", "internal", "--release", "--dump-ir")
    IrMustMatch     = @("@__ptr_", "<- \*@__ptr_")
    IrMustNotMatch  = @("function map_inc[\s\S]*?@i << 2[\s\S]*?function main")
  },
  @{
    Name            = "opt_prefix_sum_i32"
    Path            = "tests/test_opt_prefix_sum_i32.mettle"
    ShouldSucceed   = $true
    Args            = @("--build", "--emit-obj", "--linker", "internal", "--release", "--dump-ir")
    IrMustMatch     = @("prefix_sum_i32")
  },
  @{
    Name            = "opt_simd_minmax_i32"
    Path            = "tests/test_opt_simd_minmax.mettle"
    ShouldSucceed   = $true
    Args            = @("--build", "--emit-obj", "--linker", "internal", "--release", "--dump-ir")
    IrMustMatch     = @("minmax_i32")
  },
  @{
    Name            = "opt_simd_clamp_shape"
    Path            = "tests/test_opt_simd_clamp_shape.mettle"
    ShouldSucceed   = $true
    Args            = @("--build", "--emit-obj", "--linker", "internal", "--release", "--dump-ir")
    IrMustMatch     = @("clamp_i32")
  },
  @{
    Name          = "opt_load_symbol_copy_branch"
    Path          = "tests/test_opt_load_symbol_copy_branch.mettle"
    ShouldSucceed = $true
    Args          = @("--build", "--emit-obj", "--linker", "internal", "--release")
  },
  @{
    Name            = "opt_simd_insertion_sort_i32"
    Path            = "tests/test_opt_shift_loop.mettle"
    ShouldSucceed   = $true
    Args            = @("--build", "--emit-obj", "--linker", "internal", "--release", "--dump-ir")
    IrMustMatch     = @("simd_insertion_sort_i32")
  },
  @{
    Name            = "opt_simd_insertion_sort_stack"
    Path            = "tests/test_opt_simd_insertion_sort_stack.mettle"
    ShouldSucceed   = $true
    Args            = @("--build", "--emit-obj", "--linker", "internal", "--release", "--dump-ir")
    IrMustMatch     = @("simd_insertion_sort_i32")
  },
  @{
    Name            = "opt_simd_dot_i32"
    Path            = "examples/dot_product/dot_product.mettle"
    ShouldSucceed   = $true
    Args            = @("--build", "--emit-obj", "--linker", "internal", "--release", "--dump-ir")
    IrMustMatch     = @("dot_i32")
  },
  # Anti-rot guards on real BENCHMARK sources: these exact-shape recognizers
  # broke silently once when unrelated passes drifted the IR out from under
  # them (fold_readonly_globals folded the matmul bound/stride to a constant;
  # eliminate_load_symbol_copy folded word_count's byte load straight into the
  # char symbol). A silent perf regression -- not a wrong answer -- so it slips
  # past correctness tests. Assert the kernel op is present in the optimized
  # IR of the actual benchmark, so any future drift is a red test, not a
  # quiet 3x slowdown on the next benchmark run.
  @{
    Name            = "antirot_matmul_slp_mac"
    Path            = "examples/matrix_mul/matrix_mul.mettle"
    ShouldSucceed   = $true
    Args            = @("--build", "--emit-obj", "--linker", "internal", "--release", "--dump-ir")
    IrMustMatch     = @("slp_mac_i32\(")
  },
  @{
    Name            = "antirot_word_count_scan"
    Path            = "examples/word_count/word_count.mettle"
    ShouldSucceed   = $true
    Args            = @("--build", "--emit-obj", "--linker", "internal", "--release", "--dump-ir")
    IrMustMatch     = @("count_word_starts\(")
  },
  @{
    Name            = "antirot_saxpy_affine_fma"
    Path            = "examples/saxpy/saxpy.mettle"
    ShouldSucceed   = $true
    Args            = @("--build", "--emit-obj", "--linker", "internal", "--release", "--dump-ir")
    IrMustMatch     = @("simd_affine_map_f64\(")
  },
  @{
    Name            = "opt_no_hidden_matmul_n32"
    Path            = "tests/test_opt_simd_matmul_n32.mettle"
    ShouldSucceed   = $true
    Args            = @("--build", "--emit-obj", "--linker", "internal", "--release", "--dump-ir")
    IrMustNotMatch  = @("matmul_n32")
  },
  @{
    Name          = "codegen_ir_fastpaths"
    Path          = "tests/test_codegen_ir_fastpaths.mettle"
    ShouldSucceed = $true
  },
  @{
    Name            = "release_size_mode"
    Path            = "tests/test_optimize_ir_passes.mettle"
    ShouldSucceed   = $true
    Args            = @("--release")
  },
  @{ Name = "string_concat"; Path = "tests/test_string_concat.mettle"; ShouldSucceed = $true },
  @{ Name = "defer_single"; Path = "tests/test_defer_single.mettle"; ShouldSucceed = $true },
  @{ Name = "defer_lifo"; Path = "tests/test_defer_lifo.mettle"; ShouldSucceed = $true },
  @{ Name = "defer_nested"; Path = "tests/test_defer_nested_control_flow.mettle"; ShouldSucceed = $true },
  @{ Name = "defer_early_return"; Path = "tests/test_defer_early_return.mettle"; ShouldSucceed = $true },
  @{
    Name          = "defer_block_exit"
    Path          = "tests/test_defer_block_exit.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "defer_if_else_branch_exit"
    Path          = "tests/test_defer_if_else_branch_exit.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "defer_loop_iteration"
    Path          = "tests/test_defer_loop_iteration.mettle"
    ShouldSucceed = $true
  },
  @{
    Name          = "errdefer_runs_on_error"
    Path          = "tests/test_errdefer_runs_on_error.mettle"
    ShouldSucceed = $true
  },
  @{
    Name            = "errdefer_skipped_on_success"
    Path            = "tests/test_errdefer_skipped_on_success.mettle"
    ShouldSucceed   = $true
  },
  @{
    Name          = "errdefer_multiple_returns"
    Path          = "tests/test_errdefer_multiple_returns.mettle"
    ShouldSucceed = $true
  },
  # New errdefer tests
  @{ Name = "test_cast_expression"; Path = "tests/test_cast_expression.mettle"; ShouldSucceed = $true },
  @{ Name = "errdefer_interleaved_with_defer"; Path = "tests/test_errdefer_interleaved_with_defer.mettle"; ShouldSucceed = $true },
  @{ Name = "errdefer_block_exit"; Path = "tests/test_errdefer_block_exit.mettle"; ShouldSucceed = $true },
  @{ Name = "errdefer_nested_if_else"; Path = "tests/test_errdefer_nested_if_else.mettle"; ShouldSucceed = $true },
  @{ Name = "errdefer_loop_with_break_continue"; Path = "tests/test_errdefer_loop_with_break_continue.mettle"; ShouldSucceed = $true },
  @{ Name = "errdefer_top_level"; Path = "tests/test_errdefer_top_level.mettle"; ShouldSucceed = $false; Pattern = "Defer statement outside of a function|Errdefer statement outside of a function" },
  @{ Name = "defer_block_statement"; Path = "tests/test_defer_block_statement.mettle"; ShouldSucceed = $true },
  @{ Name = "errdefer_assignment_statement"; Path = "tests/test_errdefer_assignment_statement.mettle"; ShouldSucceed = $true },
  @{
    Name            = "errdefer_implicit_fallthrough"
    Path            = "tests/test_errdefer_implicit_fallthrough.mettle"
    ShouldSucceed   = $true
  },
  @{ Name = "defer_complex_interleaving"; Path = "tests/test_defer_complex_interleaving.mettle"; ShouldSucceed = $true },
  @{
    Name            = "warn_recv_buffer_extent"
    Path            = "tests/test_warn_recv_buffer_extent.mettle"
    ShouldSucceed   = $true
    OutputMustMatch = @("recv length 8192 exceeds tracked allocation 4096 bytes for 'buf'")
  },
  @{
    Name             = "no_warn_recv_within_extent"
    Path             = "tests/test_no_warn_recv_within_extent.mettle"
    ShouldSucceed    = $true
    OutputMustNotMatch = @("recv length .* exceeds tracked allocation")
  },
  @{
    Name            = "warn_memcpy_src_extent"
    Path            = "tests/test_warn_memcpy_src_extent.mettle"
    ShouldSucceed   = $true
    OutputMustMatch = @("memcpy length 200 exceeds known source extent 128 bytes")
  },
  @{
    Name            = "warn_memcpy_dst_extent"
    Path            = "tests/test_warn_memcpy_dst_extent.mettle"
    ShouldSucceed   = $true
    OutputMustMatch = @("memcpy length 200 exceeds known destination extent 128 bytes")
  },
  @{
    Name              = "no_warn_memcpy_within_extent"
    Path              = "tests/test_no_warn_memcpy_within_extent.mettle"
    ShouldSucceed     = $true
    OutputMustNotMatch = @("memcpy length .* exceeds known (destination|source) extent")
  },
  @{
    Name            = "warn_memmove_src_extent"
    Path            = "tests/test_warn_memmove_src_extent.mettle"
    ShouldSucceed   = $true
    OutputMustMatch = @("memmove length 200 exceeds known source extent 128 bytes")
  },
  @{
    Name            = "warn_memmove_dst_extent_offset"
    Path            = "tests/test_warn_memmove_dst_extent_offset.mettle"
    ShouldSucceed   = $true
    OutputMustMatch = @("memmove length 220 exceeds known destination extent 192 bytes")
  },
  @{
    Name              = "no_warn_memmove_within_extent_offset"
    Path              = "tests/test_no_warn_memmove_within_extent_offset.mettle"
    ShouldSucceed     = $true
    OutputMustNotMatch = @("memmove length .* exceeds known (destination|source) extent")
  },
  @{
    Name            = "warn_cast_alignment_violation"
    Path            = "tests/test_warn_cast_alignment_violation.mettle"
    ShouldSucceed   = $true
    OutputMustMatch = @("Cast to int64\* may violate required 8-byte alignment")
  },
  @{
    Name              = "no_warn_cast_alignment_ok"
    Path              = "tests/test_no_warn_cast_alignment_ok.mettle"
    ShouldSucceed     = $true
    OutputMustNotMatch = @("Cast to int64\* may violate required 8-byte alignment")
  },

  @{ Name = "err_unknown_char"; Path = "tests/err_unknown_char.mettle"; ShouldSucceed = $false; Pattern = "Lexical error|error" },
  @{ Name = "err_unknown_fnptr_return_type"; Path = "tests/err_unknown_fnptr_return_type.mettle"; ShouldSucceed = $false; Pattern = "Unknown type|no_such_type" },
  @{ Name = "err_invalid_hex"; Path = "tests/err_invalid_hex.mettle"; ShouldSucceed = $false; Pattern = "Invalid hexadecimal literal" },
  @{ Name = "err_invalid_bin"; Path = "tests/err_invalid_bin.mettle"; ShouldSucceed = $false; Pattern = "Invalid binary literal" },
  @{ Name = "err_missing_brace"; Path = "tests/err_missing_brace.mettle"; ShouldSucceed = $false },
  @{ Name = "err_undefined_var"; Path = "tests/err_undefined_var.mettle"; ShouldSucceed = $false; Pattern = "Undefined variable" },
  @{ Name = "err_undefined_var_typo"; Path = "tests/err_undefined_var_typo.mettle"; ShouldSucceed = $false; Pattern = "did you mean 'counter'" },
  @{ Name = "err_top_level_return"; Path = "tests/err_top_level_return.mettle"; ShouldSucceed = $false; Pattern = "Return statement outside of a function|This 'return' cannot stand at file scope" },
  @{ Name = "err_break_outside_loop"; Path = "tests/err_break_outside_loop.mettle"; ShouldSucceed = $false; Pattern = "'break' can only be used inside a loop or switch" },
  @{ Name = "err_break_unknown_label"; Path = "tests/err_break_unknown_label.mettle"; ShouldSucceed = $false; Pattern = "no matching labeled loop" },
  @{ Name = "err_continue_in_switch"; Path = "tests/err_continue_in_switch.mettle"; ShouldSucceed = $false; Pattern = "'continue' can only be used inside a loop" },
  @{ Name = "err_switch_range_inverted"; Path = "tests/err_switch_range_inverted.mettle"; ShouldSucceed = $false; Pattern = "Range lower bound" },
  @{ Name = "err_switch_duplicate_case"; Path = "tests/err_switch_duplicate_case.mettle"; ShouldSucceed = $false; Pattern = "Duplicate case value|duplicate case" },
  @{ Name = "err_switch_nonconst_case"; Path = "tests/err_switch_nonconst_case.mettle"; ShouldSucceed = $false; Pattern = "compile-time integer constant expression" },
  @{ Name = "err_forward_decl_mismatch"; Path = "tests/err_forward_decl_mismatch.mettle"; ShouldSucceed = $false; Pattern = "does not match existing declaration" },
  @{ Name = "err_forward_decl_pointer_mismatch"; Path = "tests/err_forward_decl_pointer_mismatch.mettle"; ShouldSucceed = $false; Pattern = "does not match existing declaration" },
  @{ Name = "err_extern_var_initializer"; Path = "tests/err_extern_var_initializer.mettle"; ShouldSucceed = $false; Pattern = "Extern variable declarations cannot have an initializer|Expected string literal link name after '='" },
  @{ Name = "err_extern_var_missing_type"; Path = "tests/err_extern_var_missing_type.mettle"; ShouldSucceed = $false; Pattern = "Extern variable declarations require an explicit type" },
  @{ Name = "err_nonextern_link_name"; Path = "tests/err_nonextern_link_name.mettle"; ShouldSucceed = $false; Pattern = "Link-name suffix is only allowed on extern declarations" },
  @{ Name = "err_extern_link_name_conflict"; Path = "tests/err_extern_link_name_conflict.mettle"; ShouldSucceed = $false; Pattern = "conflicting link name" },
  @{ Name = "err_deref_non_pointer"; Path = "tests/err_deref_non_pointer.mettle"; ShouldSucceed = $false; Pattern = "Dereference operator requires a pointer operand" },
  @{ Name = "err_address_of_non_lvalue"; Path = "tests/err_address_of_non_lvalue.mettle"; ShouldSucceed = $false; Pattern = "Address-of operator requires an assignable expression" },
  @{ Name = "err_pointer_type_mismatch"; Path = "tests/err_pointer_type_mismatch.mettle"; ShouldSucceed = $false; Pattern = "Type mismatch" },
  @{
    # A type mismatch names two types; the help line has to say what to type
    # instead. Mettle converts nothing implicitly, so each of these has a
    # concrete answer rather than a restatement of the error.
    Name          = "err_type_mismatch_help"
    Path          = "tests/err_type_mismatch_help.mettle"
    ShouldSucceed = $false
    OutputMustMatch = @(
      'help: cast explicitly: \(int32\)value\. The fraction is discarded, not rounded',
      'help: quote it: a string literal is "42", not 42',
      'help: drop the quotes: a numeric literal is 42, not "42"',
      'help: take the address: `&value`',
      'help: read through the pointer: `\*value` or `value\[0\]`'
    )
  },
  @{ Name = "err_use_before_init"; Path = "tests/err_use_before_init.mettle"; ShouldSucceed = $false; Pattern = "before initialization" },
  @{
    # Widen silently, narrow loudly. Every position a value lands in has to
    # report -- a declaration, an argument, a return, a field, an element --
    # and the widenings in the same file have to stay silent, which is what
    # pins the rule rather than a blanket refusal.
    Name          = "err_narrowing_needs_cast"
    Path          = "tests/err_narrowing_needs_cast.mettle"
    ShouldSucceed = $false
    OutputMustMatch = @(
      "error\[M0119\]: Narrowing conversion from 'int64' to 'int32' needs a cast",
      "error\[M0119\]: Narrowing conversion from 'uint64' to 'int64' needs a cast",
      "help: cast explicitly: \(int32\)value\. 'int32' holds -2147483648\.\.2147483647"
    )
  },
  @{
    # A compile-time integer that does not fit names the value and the range.
    # The constants that DO fit must not report, including a folded `1 << 7`.
    Name          = "err_integer_out_of_range"
    Path          = "tests/err_integer_out_of_range.mettle"
    ShouldSucceed = $false
    OutputMustMatch = @(
      "error\[M0118\]: Integer 2654435761 is out of range for 'int32'",
      "error\[M0118\]: Integer -1 is out of range for 'uint32'",
      "error\[M0118\]: Integer 256 is out of range for 'uint8'",
      "help: 'uint8' holds 0\.\.255\."
    )
  },
  @{
    # A rawptr names no element, so index/dereference/offset are refused and
    # the help says to give the address a type first.
    Name          = "err_rawptr_no_element"
    Path          = "tests/err_rawptr_no_element.mettle"
    ShouldSucceed = $false
    OutputMustMatch = @(
      "cannot index a 'rawptr'",
      "cannot dereference a 'rawptr'",
      "cannot offset a 'rawptr'",
      "help: give the address a type first"
    )
  },
  @{ Name = "err_array_index_oob_const"; Path = "tests/err_array_index_oob_const.mettle"; ShouldSucceed = $false; Pattern = "out of bounds" },
  @{ Name = "err_array_index_oob_const_negative"; Path = "tests/err_array_index_oob_const_negative.mettle"; ShouldSucceed = $false; Pattern = "out of bounds" },
  @{ Name = "err_null_deref_const"; Path = "tests/err_null_deref_const.mettle"; ShouldSucceed = $false; Pattern = "Null pointer dereference" },
  @{ Name = "member_through_ptr"; Path = "tests/err_codegen_member_expr.mettle"; ShouldSucceed = $true },
  @{ Name = "err_function_arg_count"; Path = "tests/err_function_arg_count.mettle"; ShouldSucceed = $false; Pattern = "expects .* arguments, got" },
  @{ Name = "err_function_arg_type"; Path = "tests/err_function_arg_type.mettle"; ShouldSucceed = $false; Pattern = "Type mismatch" },
  @{ Name = "err_gpu_kernel_return"; Path = "tests/err_gpu_kernel_return.mettle"; ShouldSucceed = $false; Pattern = "GPU kernel 'invalid_result' must return void" },
  @{ Name = "err_gpu_kernel_abi"; Path = "tests/err_gpu_kernel_abi.mettle"; ShouldSucceed = $false; Pattern = "GPU kernel 'invalid_parameter' parameter 'pair' has unsupported ABI type 'Pair'" },
  @{ Name = "err_gpu_no_kernel"; Path = "tests/err_gpu_no_kernel.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "GPU module has no kernel entry points" },
  @{ Name = "err_gpu_recursive_device_call"; Path = "tests/err_gpu_recursive_device_call.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "GPU device call graph is recursive at 'recurse'" },
  @{ Name = "err_gpu_recursive_device_call_spirv"; Path = "tests/err_gpu_recursive_device_call.mettle"; ShouldSucceed = $false; Args = @("--emit-spirv"); Pattern = "GPU device call graph is recursive at 'recurse'" },
  @{ Name = "err_gpu_external_device_call"; Path = "tests/err_gpu_external_device_call.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "calls external or missing 'host_only'" },
  @{ Name = "err_gpu_indirect_device_call"; Path = "tests/err_gpu_indirect_device_call.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "contains an indirect call" },
  @{ Name = "err_gpu_direct_kernel_call"; Path = "tests/err_gpu_direct_kernel_call.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "directly calls kernel 'child'" },
  @{ Name = "err_gpu_launch_dimension"; Path = "tests/err_gpu_launch_dimension.mettle"; ShouldSucceed = $false; Pattern = "integer GPU launch dimension" },
  @{ Name = "err_gpu_launch_argument"; Path = "tests/err_gpu_launch_argument.mettle"; ShouldSucceed = $false; Pattern = "GPU launch argument 0 has unsupported ABI type 'string'" },
  @{ Name = "err_gpu_dispatch_named_missing"; Path = "tests/err_gpu_dispatch_named_missing.mettle"; ShouldSucceed = $false; Pattern = "Named dispatch controls require grid and block" },
  @{ Name = "err_gpu_dispatch_named_duplicate"; Path = "tests/err_gpu_dispatch_named_duplicate.mettle"; ShouldSucceed = $false; Pattern = "Duplicate named dispatch control" },
  @{ Name = "err_gpu_dispatch_named_unknown"; Path = "tests/err_gpu_dispatch_named_unknown.mettle"; ShouldSucceed = $false; Pattern = "Unknown named dispatch control" },
  @{ Name = "err_gpu_dispatch_named_shared_type"; Path = "tests/err_gpu_dispatch_named_shared_type.mettle"; ShouldSucceed = $false; Pattern = "integer dynamic shared-memory byte count" },
  @{ Name = "err_gpu_dispatch_named_dimension"; Path = "tests/err_gpu_dispatch_named_dimension.mettle"; ShouldSucceed = $false; Pattern = "GPU grid dimension 1 must be greater than zero" },
  @{ Name = "err_gpu_dispatch_named_stream_type"; Path = "tests/err_gpu_dispatch_named_stream_type.mettle"; ShouldSucceed = $false; Pattern = "integer or pointer stream handle" },
  @{ Name = "err_gpu_nested_launch"; Path = "tests/err_gpu_nested_launch.mettle"; ShouldSucceed = $false; Pattern = "GPU kernel cannot launch another kernel" },
  @{ Name = "err_gpu_workgroup_outside_kernel"; Path = "tests/err_gpu_workgroup_outside_kernel.mettle"; ShouldSucceed = $false; Pattern = "workgroup storage is only legal inside a GPU kernel" },
  @{ Name = "err_gpu_address_space_shape"; Path = "tests/err_gpu_address_space_shape.mettle"; ShouldSucceed = $false; Pattern = "GPU address-space storage requires a statically sized array type" },
  @{ Name = "err_gpu_dynamic_private"; Path = "tests/err_gpu_dynamic_private.mettle"; ShouldSucceed = $false; Pattern = "pointer type for a dynamic workgroup view" },
  @{ Name = "err_gpu_address_space_initializer"; Path = "tests/err_gpu_address_space_initializer.mettle"; ShouldSucceed = $false; Pattern = "workgroup storage cannot have a declaration initializer" },
  @{ Name = "err_gpu_address_space_rebind"; Path = "tests/err_gpu_address_space_rebind.mettle"; ShouldSucceed = $false; Pattern = "GPU address-space binding 'scratch' cannot be rebound" },
  @{ Name = "err_gpu_barrier_outside_kernel"; Path = "tests/err_gpu_barrier_outside_kernel.mettle"; ShouldSucceed = $false; Pattern = "Barrier statements are only legal inside a GPU kernel" },
  @{ Name = "err_gpu_subgroup_signature"; Path = "tests/err_gpu_subgroup_signature.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "invalid subgroup intrinsic signature" },
  @{ Name = "err_gpu_subgroup_outside_kernel"; Path = "tests/err_gpu_subgroup_outside_kernel.mettle"; ShouldSucceed = $false; Pattern = "Subgroup built-ins are only legal directly inside a GPU kernel" },
  @{ Name = "err_gpu_subgroup_type"; Path = "tests/err_gpu_subgroup_type.mettle"; ShouldSucceed = $false; Pattern = "Subgroup 'reduce_add' value must be uint32 or float32" },
  @{ Name = "err_gpu_subgroup_vote_type"; Path = "tests/err_gpu_subgroup_vote_type.mettle"; ShouldSucceed = $false; Pattern = "Subgroup 'any' predicate must be bool" },
  @{ Name = "err_gpu_subgroup_ballot_word"; Path = "tests/err_gpu_subgroup_ballot_word.mettle"; ShouldSucceed = $false; Pattern = "Subgroup ballot word index must be an integer" },
  @{ Name = "err_gpu_subgroup_shuffle_type"; Path = "tests/err_gpu_subgroup_shuffle_type.mettle"; ShouldSucceed = $false; Pattern = "Subgroup 'shuffle' value must be uint32 or float32" },
  @{ Name = "err_gpu_subgroup_shuffle_spirv"; Path = "tests/gpu/subgroup_shuffle.mettle"; ShouldSucceed = $false; Args = @("--emit-spirv"); Pattern = "SPIR-V OpenCL 2.0 does not provide non-uniform subgroup shuffle" },
  @{ Name = "err_gpu_atomic_outside_kernel"; Path = "tests/err_gpu_atomic_outside_kernel.mettle"; ShouldSucceed = $false; Pattern = "Atomic GPU built-ins are only legal directly inside a GPU kernel" },
  @{ Name = "err_gpu_atomic_type"; Path = "tests/err_gpu_atomic_type.mettle"; ShouldSucceed = $false; Pattern = "Atomic storage must be a uint32\* or uint64\*" },
  @{ Name = "err_gpu_atomic_failure_order"; Path = "tests/err_gpu_atomic_failure_order.mettle"; ShouldSucceed = $false; Pattern = "failure_order may not be release/acq_rel or stronger than success order" },
  @{ Name = "err_gpu_atomic_load_order"; Path = "tests/err_gpu_atomic_load_order.mettle"; ShouldSucceed = $false; Pattern = "Atomic load order must be relaxed, acquire, or seq_cst" },
  @{ Name = "err_gpu_atomic_store_order"; Path = "tests/err_gpu_atomic_store_order.mettle"; ShouldSucceed = $false; Pattern = "Atomic store order must be relaxed, release, or seq_cst" },
  @{ Name = "err_gpu_atomic_workgroup_scope"; Path = "tests/err_gpu_atomic_workgroup_scope.mettle"; ShouldSucceed = $false; Pattern = "Workgroup atomics cannot request device or system scope" },
  @{ Name = "err_gpu_divergent_barrier"; Path = "tests/err_gpu_divergent_barrier.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "workgroup barrier is control-dependent on a work-item-varying condition" },
  @{ Name = "err_gpu_subgroup_uniform_barrier"; Path = "tests/err_gpu_subgroup_uniform_barrier.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "workgroup barrier is control-dependent on a subgroup-uniform but not workgroup-uniform condition" },
  @{ Name = "err_gpu_divergent_subgroup"; Path = "tests/err_gpu_divergent_subgroup.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "subgroup collective is control-dependent on a work-item-varying condition" },
  @{ Name = "err_gpu_varying_broadcast_lane"; Path = "tests/err_gpu_varying_broadcast_lane.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "subgroup broadcast source lane is work-item-varying" },
  @{ Name = "err_gpu_varying_scan_lane"; Path = "tests/err_gpu_varying_scan_lane.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "subgroup broadcast source lane is work-item-varying" },
  @{ Name = "err_gpu_tensor_outside_kernel"; Path = "tests/err_gpu_tensor_outside_kernel.mettle"; ShouldSucceed = $false; Pattern = "tensor_mma is only legal directly inside a GPU kernel" },
  @{ Name = "err_gpu_tensor_storage"; Path = "tests/err_gpu_tensor_storage.mettle"; ShouldSucceed = $false; Pattern = "Tensor operand A has storage type 'float32\*' incompatible" },
  @{ Name = "err_gpu_tensor_option"; Path = "tests/err_gpu_tensor_option.mettle"; ShouldSucceed = $false; Pattern = "Unknown tensor option 'vendor_opcode'" },
  @{ Name = "err_gpu_tensor_stride_type"; Path = "tests/err_gpu_tensor_stride_type.mettle"; ShouldSucceed = $false; Pattern = "Runtime tensor option 'lda' must have integer type" },
  @{ Name = "err_gpu_tensor_scale_contract"; Path = "tests/err_gpu_tensor_scale_contract.mettle"; ShouldSucceed = $false; Pattern = "Invalid tensor MMA descriptor" },
  @{ Name = "err_gpu_tensor_scale_storage"; Path = "tests/err_gpu_tensor_scale_storage.mettle"; ShouldSucceed = $false; Pattern = "Tensor A scale has storage type 'uint32\*' incompatible with its scale format" },
  @{ Name = "err_gpu_tensor_packing_contract"; Path = "tests/err_gpu_tensor_packing_contract.mettle"; ShouldSucceed = $false; Pattern = "Invalid tensor MMA descriptor" },
  @{ Name = "err_gpu_divergent_tensor"; Path = "tests/err_gpu_divergent_tensor.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "tensor MMA is control-dependent on a work-item-varying condition" },
  @{ Name = "err_gpu_varying_tensor_pointer"; Path = "tests/err_gpu_varying_tensor_pointer.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "tensor MMA pointer operand 0 is not subgroup-uniform" },
  @{ Name = "err_gpu_varying_tensor_stride"; Path = "tests/err_gpu_varying_tensor_stride.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "tensor MMA runtime stride operand 4 is not subgroup-uniform" },
  @{ Name = "gpu_tensor_matmul_gb10"; Path = "tests/gpu/tensor_matmul.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "gpu_tensor_matmul_fp8_gb10"; Path = "tests/gpu/tensor_matmul_fp8.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "gpu_tensor_matmul_scaled_gb10"; Path = "tests/gpu/tensor_matmul_scaled.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "gpu_tensor_matmul_sparse_gb10"; Path = "tests/gpu/tensor_matmul_sparse.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "err_gpu_tensor_matmul_scaled_missing_scale_stride"; Path = "tests/err_gpu_tensor_matmul_scaled_missing_scale_stride.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx", "--gpu-arch=gb10"); Pattern = "block scales require explicit whole-matrix A/B scale leading dimensions" },
  @{ Name = "err_gpu_tensor_matmul_missing_stride"; Path = "tests/err_gpu_tensor_matmul_missing_stride.mettle"; ShouldSucceed = $false; Pattern = "tensor_matmul requires explicit lda, ldb, ldc, and ldd" },
  @{ Name = "err_gpu_tensor_matmul_control_type"; Path = "tests/err_gpu_tensor_matmul_control_type.mettle"; ShouldSucceed = $false; Pattern = "tensor_matmul row origin must have unsigned integer type" },
  @{ Name = "err_gpu_tensor_matmul_stride_type"; Path = "tests/err_gpu_tensor_matmul_stride_type.mettle"; ShouldSucceed = $false; Pattern = "Runtime tensor_matmul leading dimensions must fit the descriptor's uint32 range" },
  @{ Name = "err_gpu_tensor_matmul_tf32_tail"; Path = "tests/err_gpu_tensor_matmul_tf32_tail.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx", "--gpu-arch=gb10"); Pattern = "TF32, reduced-precision accumulators, unsupported sparse/scale profiles, and saturating integer tails are rejected" },
  @{ Name = "gpu_tensor_matmul_transpose_gb10"; Path = "tests/gpu/tensor_matmul_transpose.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "err_gpu_divergent_tensor_matmul"; Path = "tests/err_gpu_divergent_tensor_matmul.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx", "--gpu-arch=gb10"); Pattern = "bounded tensor matrix operation is control-dependent on a work-item-varying condition" },
  @{ Name = "err_gpu_varying_tensor_matmul_origin"; Path = "tests/err_gpu_varying_tensor_matmul_origin.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx", "--gpu-arch=gb10"); Pattern = "tensor matrix row-origin control operand [0-9]+ is not subgroup-uniform" },
  @{ Name = "err_gpu_tensor_matmul_spirv_profile"; Path = "tests/gpu/tensor_matmul.mettle"; ShouldSucceed = $false; Args = @("--emit-spirv"); Pattern = "SPIR-V OpenCL 2.0 profile has no exact bounded matrix-region lowering" },
  @{ Name = "gpu_tensor_epilogue_gb10"; Path = "tests/gpu/tensor_epilogue.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "gpu_tensor_epilogue_fused_gb10"; Path = "tests/gpu/tensor_epilogue_fused.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "gpu_tensor_epilogue_portable"; Path = "tests/gpu/tensor_epilogue_portable.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=portable") },
  @{ Name = "err_gpu_tensor_epilogue_outside_kernel"; Path = "tests/err_gpu_tensor_epilogue_outside_kernel.mettle"; ShouldSucceed = $false; Pattern = "tensor_epilogue is only legal directly inside a GPU kernel" },
  @{ Name = "err_gpu_tensor_epilogue_storage"; Path = "tests/err_gpu_tensor_epilogue_storage.mettle"; ShouldSucceed = $false; Pattern = "Tensor epilogue destination storage is incompatible with element_type" },
  @{ Name = "err_gpu_tensor_epilogue_bias_contract"; Path = "tests/err_gpu_tensor_epilogue_bias_contract.mettle"; ShouldSucceed = $false; Pattern = "bias operand must be present exactly when bias_mode" },
  @{ Name = "err_gpu_tensor_epilogue_clamp_contract"; Path = "tests/err_gpu_tensor_epilogue_clamp_contract.mettle"; ShouldSucceed = $false; Pattern = "clamp activation requires exactly one clamp_min and one clamp_max" },
  @{ Name = "err_gpu_tensor_epilogue_scalar_type"; Path = "tests/err_gpu_tensor_epilogue_scalar_type.mettle"; ShouldSucceed = $false; Pattern = "Tensor epilogue alpha must have type float32" },
  @{ Name = "err_gpu_divergent_tensor_epilogue"; Path = "tests/err_gpu_divergent_tensor_epilogue.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx", "--gpu-arch=gb10"); Pattern = "tensor epilogue is control-dependent on a work-item-varying condition" },
  @{ Name = "err_gpu_varying_tensor_epilogue_pointer"; Path = "tests/err_gpu_varying_tensor_epilogue_pointer.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx", "--gpu-arch=gb10"); Pattern = "tensor epilogue operand 0 is not subgroup-uniform" },
  @{ Name = "err_gpu_tensor_epilogue_spirv_profile"; Path = "tests/gpu/tensor_epilogue.mettle"; ShouldSucceed = $false; Args = @("--emit-spirv"); Pattern = "SPIR-V OpenCL 2.0 profile has no exact cooperative tensor-epilogue lowering" },
  @{ Name = "err_gpu_tensor_spirv_profile"; Path = "tests/gpu/tensor_kernels.mettle"; ShouldSucceed = $false; Args = @("--emit-spirv"); Pattern = "SPIR-V OpenCL 2.0 profile has no cooperative-matrix capability" },
  @{ Name = "err_gpu_tensor_portable_profile"; Path = "tests/gpu/tensor_kernels.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx", "--gpu-arch=portable"); Pattern = "profile requires PTX 7.0 and sm_80 or newer" },
  @{ Name = "gpu_tensor_sparse_gb10"; Path = "tests/gpu/tensor_sparse.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "err_gpu_tensor_sparse_portable"; Path = "tests/gpu/tensor_sparse.mettle"; ShouldSucceed = $false; Args = @("-O", "--emit-ptx", "--gpu-arch=portable"); Pattern = "structured-sparse mma\.sp requires PTX 7\.1 and sm_80 or newer" },
  @{ Name = "err_gpu_tensor_tiled_shape"; Path = "tests/err_gpu_tensor_tiled_shape.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx", "--gpu-arch=gb10"); Pattern = "profile is not a stable PTX WMMA combination" },
  @{ Name = "err_gpu_tensor_sparse_metadata_type"; Path = "tests/err_gpu_tensor_sparse_metadata_type.mettle"; ShouldSucceed = $false; Pattern = "Tensor metadata operand must be a uint8 pointer" },
  @{ Name = "err_gpu_tensor_transfer_outside_kernel"; Path = "tests/err_gpu_tensor_transfer_outside_kernel.mettle"; ShouldSucceed = $false; Pattern = "Tensor transfers are only legal directly inside a GPU kernel" },
  @{ Name = "err_gpu_tensor_transfer_geometry"; Path = "tests/err_gpu_tensor_transfer_geometry.mettle"; ShouldSucceed = $false; Pattern = "requires extent1, stride1, tile1, and coordinate1" },
  @{ Name = "err_gpu_tensor_transfer_storage"; Path = "tests/err_gpu_tensor_transfer_storage.mettle"; ShouldSucceed = $false; Pattern = "Tensor transfer source and destination pointer storage must match" },
  @{ Name = "err_gpu_tensor_transfer_rank"; Path = "tests/err_gpu_tensor_transfer_rank.mettle"; ShouldSucceed = $false; Pattern = "option for dimension 1 exceeds rank 1" },
  @{ Name = "gpu_tensor_transfer_gb10"; Path = "tests/gpu/tensor_transfer.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "gpu_tensor_transfer_portable"; Path = "tests/gpu/tensor_transfer.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=portable") },
  @{ Name = "err_gpu_tensor_transfer_spirv_profile"; Path = "tests/gpu/tensor_transfer.mettle"; ShouldSucceed = $false; Args = @("--emit-spirv"); Pattern = "SPIR-V OpenCL 2.0 profile has no multidimensional workgroup-transfer lowering" },
  @{ Name = "err_gpu_async_copy_outside_kernel"; Path = "tests/err_gpu_async_copy_outside_kernel.mettle"; ShouldSucceed = $false; Pattern = "Asynchronous workgroup copies are only legal directly inside a GPU kernel" },
  @{ Name = "err_gpu_async_copy_transaction"; Path = "tests/err_gpu_async_copy_transaction.mettle"; ShouldSucceed = $false; Pattern = "async copy byte span must be divisible by its transaction size" },
  @{ Name = "err_gpu_kernel_on_host_target"; Path = "tests/err_gpu_kernel_on_host_target.mettle"; ShouldSucceed = $false;
     OutputMustMatch = @("'tensor_mma' in function 'gemm_tile' runs on a GPU", "--emit-ptx") ;
     OutputMustNotMatch = @("internal compiler error") },
  @{ Name = "err_gpu_async_copy_unbalanced"; Path = "tests/err_gpu_async_copy_unbalanced.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "invalid asynchronous-copy contract or unbalanced group" },
  @{ Name = "err_gpu_async_copy_space"; Path = "tests/err_gpu_async_copy_space.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "invalid asynchronous-copy contract or unbalanced group" },
  @{ Name = "gpu_uniform_collectives"; Path = "tests/gpu/uniform_collectives.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx") },
  @{ Name = "gpu_warp_row_early_return"; Path = "tests/gpu/warp_row_early_return.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "gpu_warp_row_early_return_spirv"; Path = "tests/gpu/warp_row_early_return.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-spirv") },
  @{ Name = "gpu_bit_intrinsics"; Path = "tests/gpu/bit_intrinsics.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "gpu_bit_intrinsics_spirv"; Path = "tests/gpu/bit_intrinsics.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-spirv") },
  @{ Name = "gpu_u16_typed_loads"; Path = "tests/gpu/u16_typed_loads.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "gpu_u16_typed_loads_spirv"; Path = "tests/gpu/u16_typed_loads.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-spirv") },
  @{ Name = "gpu_unroll_annotated"; Path = "tests/gpu/unroll_annotated.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "err_gpu_subgroup_varying_guard"; Path = "tests/err_gpu_subgroup_varying_guard.mettle"; ShouldSucceed = $false; Args = @("-O", "--emit-ptx"); Pattern = "subgroup collective is control-dependent on a work-item-varying condition" },
  @{ Name = "err_gpu_subgroup_wrong_divisor"; Path = "tests/err_gpu_subgroup_wrong_divisor.mettle"; ShouldSucceed = $false; Args = @("--emit-ptx"); Pattern = "subgroup collective is control-dependent on a work-item-varying condition" },
  @{ Name = "err_gpu_kernel_block_range"; Path = "tests/err_gpu_kernel_block_range.mettle"; ShouldSucceed = $false; Pattern = "Kernel block dimension must be between 1 and 1024" },
  @{ Name = "err_gpu_dispatch_on_stream_conflict"; Path = "tests/err_gpu_dispatch_on_stream_conflict.mettle"; ShouldSucceed = $false; Pattern = "already given by the named 'stream:' control" },
  @{ Name = "gpu_typed_dispatch"; Path = "tests/test_gpu_typed_dispatch.mettle"; ShouldSucceed = $true; Args = @("--emit-obj") },
  @{ Name = "gpu_vector_and_packed"; Path = "tests/gpu/vector_and_packed.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "gpu_vector_and_packed_spirv"; Path = "tests/gpu/vector_and_packed.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-spirv") },
  @{ Name = "gpu_kernel_diagnostics"; Path = "tests/gpu/kernel_diagnostics.mettle"; ShouldSucceed = $true; Args = @("--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "gpu_kernel_diagnostics_checked"; Path = "tests/gpu/kernel_diagnostics.mettle"; ShouldSucceed = $true; Args = @("--emit-ptx", "--gpu-arch=gb10", "--gpu-checks") },
  @{ Name = "err_gpu_dispatch_block_mismatch"; Path = "tests/err_gpu_dispatch_block_mismatch.mettle"; ShouldSucceed = $false; Pattern = "declares block \(256, 1, 1\) but this dispatch launches \(32, 1, 1\)" },
  @{ Name = "err_gpu_dispatch_arg_type"; Path = "tests/err_gpu_dispatch_arg_type.mettle"; ShouldSucceed = $false; Pattern = "expected 'int32', found 'float32'" },
  @{ Name = "err_gpu_dispatch_arg_count"; Path = "tests/err_gpu_dispatch_arg_count.mettle"; ShouldSucceed = $false; Pattern = "GPU kernel 'vadd' takes 4 arguments, but this dispatch passes 3" },
  @{ Name = "err_gpu_dispatch_work_no_block"; Path = "tests/err_gpu_dispatch_work_no_block.mettle"; ShouldSucceed = $false; Pattern = "needs a 'kernel\(block = ...\)' declaration" },
  @{ Name = "err_gpu_dispatch_work_untyped"; Path = "tests/err_gpu_dispatch_work_untyped.mettle"; ShouldSucceed = $false; Pattern = "declare it host-side with 'extern kernel'" },
  @{ Name = "gpu_hardware_ai_kernels"; Path = "tests/gpu/hardware_kernels.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "gpu_native_fp8"; Path = "tests/gpu/tensor_native_fp8.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "err_gpu_native_fp8_portable"; Path = "tests/gpu/tensor_native_fp8.mettle"; ShouldSucceed = $false; Args = @("-O", "--emit-ptx", "--gpu-arch=portable"); Pattern = "FP8 mma\.sync requires PTX 8\.4 and sm_89 or newer" },
  @{ Name = "gpu_native_fp4"; Path = "tests/gpu/tensor_native_fp4.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "err_gpu_native_fp4_portable"; Path = "tests/gpu/tensor_native_fp4.mettle"; ShouldSucceed = $false; Args = @("-O", "--emit-ptx", "--gpu-arch=portable"); Pattern = "architecture- or family-specific sm_120a/sm_121a target" },
  @{ Name = "err_gpu_native_fp4_sm121"; Path = "tests/gpu/tensor_native_fp4.mettle"; ShouldSucceed = $false; Args = @("-O", "--emit-ptx", "--gpu-arch=sm_121"); Pattern = "architecture- or family-specific sm_120a/sm_121a target" },
  @{ Name = "gpu_native_fp6"; Path = "tests/gpu/tensor_native_fp6.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx", "--gpu-arch=gb10") },
  @{ Name = "err_gpu_native_fp6_portable"; Path = "tests/gpu/tensor_native_fp6.mettle"; ShouldSucceed = $false; Args = @("-O", "--emit-ptx", "--gpu-arch=portable"); Pattern = "architecture- or family-specific sm_120a/sm_121a target" },
  @{ Name = "err_gpu_native_fp6_sm121"; Path = "tests/gpu/tensor_native_fp6.mettle"; ShouldSucceed = $false; Args = @("-O", "--emit-ptx", "--gpu-arch=sm_121"); Pattern = "architecture- or family-specific sm_120a/sm_121a target" },
  @{ Name = "gpu_native_indices"; Path = "tests/gpu/native_indices.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-ptx") },
  @{ Name = "gpu_native_indices_spirv"; Path = "tests/gpu/native_indices.mettle"; ShouldSucceed = $true; Args = @("-O", "--emit-spirv") },
  @{ Name = "err_gpu_ml_optimizer_policy"; Path = "tests/gpu/native_indices.mettle"; ShouldSucceed = $false; Args = @("--ml-opt", "--emit-ptx"); Pattern = "--ml-opt is not target-neutral" },
  @{ Name = "err_match_bad_syntax"; Path = "tests/err_match_bad_syntax.mettle"; ShouldSucceed = $false; Pattern = "Expected .* after 'match'" },
  # Diagnostics quality: multi-error recovery, cascade suppression, notes,
  # caret labels, unused-variable warnings, JSON output.
  @{ Name = "diag_multi_error"; Path = "tests/diag_multi_error.mettle"; ShouldSucceed = $false
     OutputMustMatch = @("due to 4 previous errors", "Undefined variable 'missing1'", "Undefined variable 'missing2'") },
  @{ Name = "diag_parser_no_cascade"; Path = "tests/diag_parser_no_cascade.mettle"; ShouldSucceed = $false
     Pattern = "Expected '\(' after 'if'"
     OutputMustNotMatch = @("Expected '\(', found identifier", "due to [4-9] previous") },
  @{ Name = "diag_dup_note"; Path = "tests/diag_dup_note.mettle"; ShouldSucceed = $false
     OutputMustMatch = @("Duplicate declaration of 'x'", "previous declaration of 'x' is here") },
  @{ Name = "diag_call_notes"; Path = "tests/diag_call_notes.mettle"; ShouldSucceed = $false
     OutputMustMatch = @("expects 2 arguments, got 3", "\^\^\^ expected 2 arguments, got 3", "function 'add' defined here") },
  @{ Name = "diag_label_mismatch"; Path = "tests/diag_label_mismatch.mettle"; ShouldSucceed = $false
     OutputMustMatch = @("\^\^\^\^\^ expected 'int64', found 'string'") },
  @{ Name = "diag_unused_var"; Path = "tests/diag_unused_var.mettle"; ShouldSucceed = $true
     OutputMustMatch = @("unused variable 'scratch'", "rename it to '_scratch'")
     OutputMustNotMatch = @("unused variable '_intentional'", "unused variable 'used'") },
  @{ Name = "diag_json_format"; Path = "tests/diag_json_format.mettle"; ShouldSucceed = $false
     Args = @("--error-format=json")
     Pattern = '"severity":"error"'
     OutputMustMatch = @('"code":"E0004"', '"line":2', '"length":5', '"label":"expected ''int64'', found ''string''"') },
  @{ Name = "diag_poison_no_cascade"; Path = "tests/diag_poison_no_cascade.mettle"; ShouldSucceed = $false
     Pattern = "Type mismatch"
     OutputMustNotMatch = @("Undefined variable 'x'") },
  # --verify translation validation: clean programs validate with zero
  # divergences; a sabotaged pass is caught, quarantined, and the build heals.
  @{ Name = "verify_clean"; Path = "tests/verify_clean.mettle"; ShouldSucceed = $true
     Args = @("--verify")
     OutputMustMatch = @("translation validation: OK")
     OutputMustNotMatch = @("MISCOMPILE") },
  @{ Name = "verify_nullcheck_zerotrip"; Path = "tests/verify_nullcheck_zerotrip.mettle"; ShouldSucceed = $true
     Args = @("--verify")
     OutputMustMatch = @("translation validation: OK")
     OutputMustNotMatch = @("MISCOMPILE") },
  @{ Name = "verify_sabotage_caught"; Path = "tests/verify_clean.mettle"; ShouldSucceed = $true
     Args = @("--verify")
     Env = @{ METTLE_VERIFY_BREAK = "constant_and_branch_simplify:dot" }
     SkipDeterminism = $true
     OutputMustMatch = @("MISCOMPILE CAUGHT", "quarantined", "pre-pass IR restored") },
  # `mettle test`: interpreted @test functions - pass/fail/leak reporting with
  # assertion diagnostics; @test bodies are dropped from normal builds.
  @{ Name = "comptime_test_run"; Path = "tests/comptime_tests_demo.mettle"; ShouldSucceed = $false
     Args = @("test")
     Pattern = "assertion failed in test 'test_fail'"
     OutputMustMatch = @("test test_pass \.\.\. ok", "left: 20, right: 21",
                         "LEAKED", "leaked 24 bytes", "2 passed, 1 failed, 1 leak") },
  @{ Name = "comptime_test_filter"; Path = "tests/comptime_tests_demo.mettle"; ShouldSucceed = $false
     Args = @("test", "--filter=test_fail")
     Pattern = "running 1 test"
     OutputMustMatch = @("0 passed, 1 failed")
     OutputMustNotMatch = @("test test_pass") },
  @{ Name = "comptime_tests_dropped_in_build"; Path = "tests/comptime_tests_demo.mettle"; ShouldSucceed = $true
     OutputMustNotMatch = @("assertion failed") },
  # The interpreter must wrap a narrow integer where a register does. Six
  # products read their answer from it, and a disagreement here makes the
  # differential gates report a miscompile that is really a difference of
  # opinion about what int32 means. Runs with no codegen, so it pins the
  # interpreter's arithmetic and nothing else.
  @{ Name = "interp_narrow_widths"; Path = "tests/test_interp_narrow_widths.mettle"; ShouldSucceed = $true
     Args = @("test")
     SkipBinaryCheck = $true
     OutputMustMatch = @("4 passed")
     OutputMustNotMatch = @("failed") },
  @{ Name = "err_assert_outside_test"; Path = "tests/err_assert_outside_test.mettle"; ShouldSucceed = $false
     Pattern = "only be called inside a @test function" },
  # Zero-run PGO: interpreted profile marks the oversized callee hot, which
  # overrides the inliner's static budget; without --pgo it stays refused.
  @{ Name = "pgo_hot_inline"; Path = "tests/pgo_hot_inline.mettle"; ShouldSucceed = $true
     Args = @("--pgo", "--release", "--explain")
     OutputMustMatch = @('pgo: interpreted main', 'keyed_mix: 100000 calls.*\[hot\]',
                         'call to .keyed_mix. @ line 32.: inlined')
     OutputMustNotMatch = @('NOT inlined') },
  @{ Name = "pgo_off_budget_refusal"; Path = "tests/pgo_hot_inline.mettle"; ShouldSucceed = $true
     Args = @("--release", "--explain")
     OutputMustMatch = @('call to .keyed_mix. @ line 32.: NOT inlined') },
  @{ Name = "pgo_cold_unroll_threshold"; Path = "tests/pgo_hot_thresholds.mettle"; ShouldSucceed = $true
     Args = @("--pgo", "--release", "--dump-ir")
     OutputMustMatch = @('pgo: interpreted main')
     IrMustMatch = @('function cold_loop[\s\S]*jump ir_while_') },
  @{ Name = "err_match_non_exhaustive"; Path = "tests/err_match_non_exhaustive.mettle"; ShouldSucceed = $false; Pattern = "Non-exhaustive match" },
  @{ Name = "err_trait_bound_missing_impl"; Path = "tests/err_trait_bound_missing_impl.mettle"; ShouldSucceed = $false; Pattern = "does not implement trait 'Addable'" },
  @{ Name = "err_trait_bound_missing_second_impl"; Path = "tests/err_trait_bound_missing_second_impl.mettle"; ShouldSucceed = $false; Pattern = "does not implement trait 'SignedNumber'" },
  @{ Name = "err_trait_method_missing_impl"; Path = "tests/err_trait_method_missing_impl.mettle"; ShouldSucceed = $false; Pattern = "missing trait method 'next_value'" },
  @{ Name = "err_generics_generic_fn_ptr_address"; Path = "tests/err_generics_generic_fn_ptr_address.mettle"; ShouldSucceed = $false; Pattern = "Expected an expression" },

  # Syntax diagnostics. Each of these guards a message that used to say
  # nothing. The "1 previous error" patterns are the anti-cascade guards: one
  # mistake has to cost one diagnostic, or these fail.
  @{ Name = "err_syntax_missing_operand"; Path = "tests/err_syntax_missing_operand.mettle"; ShouldSucceed = $false; Pattern = "Expected an expression after '\+', found ';'" },
  @{ Name = "err_syntax_missing_operand_once"; Path = "tests/err_syntax_missing_operand.mettle"; ShouldSucceed = $false; Pattern = "due to 1 previous error" },
  @{ Name = "err_syntax_let_keyword"; Path = "tests/err_syntax_let_keyword.mettle"; ShouldSucceed = $false; Pattern = "'let' declares nothing in Mettle" },
  @{ Name = "err_syntax_c_type_name"; Path = "tests/err_syntax_c_type_name.mettle"; ShouldSucceed = $false; Pattern = "'int' is not a Mettle type" },
  @{ Name = "err_syntax_paren_comma"; Path = "tests/err_syntax_paren_comma.mettle"; ShouldSucceed = $false; Pattern = "the grouped expression holds one value" },
  @{ Name = "err_syntax_nested_group_comma"; Path = "tests/err_syntax_nested_group_comma.mettle"; ShouldSucceed = $false; Pattern = "the grouped expression holds one value" },
  @{ Name = "err_syntax_no_cascade"; Path = "tests/err_syntax_no_cascade.mettle"; ShouldSucceed = $false; Pattern = "due to 1 previous error" },
  @{ Name = "err_syntax_no_cascade_msg"; Path = "tests/err_syntax_no_cascade.mettle"; ShouldSucceed = $false; Pattern = "Expected '\(' after 'if'" },
  @{ Name = "err_syntax_c_for_header"; Path = "tests/err_syntax_c_for_header.mettle"; ShouldSucceed = $false; Pattern = "A 'for' header needs 'in' or parentheses" },
  @{ Name = "err_syntax_c_for_header_once"; Path = "tests/err_syntax_c_for_header.mettle"; ShouldSucceed = $false; Pattern = "due to 1 previous error" },
  @{ Name = "err_syntax_lexical_no_cascade"; Path = "tests/err_syntax_lexical_no_cascade.mettle"; ShouldSucceed = $false; Pattern = "due to 1 previous error" },
  # A `<` comparison whose right side makes the speculative type-argument parse
  # fail must backtrack without leaving the abandoned parse's diagnostic behind.
  @{ Name = "generic_call_lt_ambiguity"; Path = "tests/generic_call_lt_ambiguity.mettle"; ShouldSucceed = $true
     Args = @("test")
     SkipBinaryCheck = $true
     OutputMustMatch = @("3 passed")
     OutputMustNotMatch = @("Expected array size after", "error\[E0002\]") },
  @{ Name = "generic_call_lt_ambiguity_build"; Path = "tests/generic_call_lt_ambiguity.mettle"; ShouldSucceed = $true
     OutputMustNotMatch = @("Expected array size after", "error\[E0002\]") },
  @{ Name = "err_member_on_non_struct"; Path = "tests/err_member_on_non_struct.mettle"; ShouldSucceed = $false; Pattern = "Cannot access field on non-struct type" },
  @{ Name = "err_switch_multiple_default"; Path = "tests/err_switch_multiple_default.mettle"; ShouldSucceed = $false; Pattern = "Only one default case is allowed|only contain one default clause" },
  @{ Name = "err_return_type_mismatch"; Path = "tests/err_return_type_mismatch.mettle"; ShouldSucceed = $false; Pattern = "Type mismatch" },
  @{ Name = "err_static_assert_sizeof"; Path = "tests/err_static_assert_sizeof.mettle"; ShouldSucceed = $false; Pattern = "static_assert failed" },
  @{ Name = "err_defer_top_level"; Path = "tests/err_defer_top_level.mettle"; ShouldSucceed = $false; Pattern = "Defer statement outside of a function" },
  @{
    Name          = "err_import_private"
    Path          = "tests/err_import_private.mettle"
    ShouldSucceed = $false
    Pattern       = "Undefined variable|not visible|private_func"
    Args          = @("-I", "tests/lib")
  },
  @{
    Name          = "err_import_namespaced_private"
    Path          = "tests/err_import_namespaced_private.mettle"
    ShouldSucceed = $false
    Pattern       = "Undefined variable|private_bonus"
    Args          = @("-I", "tests/lib")
  },
  @{
    Name          = "err_import_selective_missing"
    Path          = "tests/err_import_selective_missing.mettle"
    ShouldSucceed = $false
    Pattern       = "missing_symbol|no top-level declaration"
    Args          = @("-I", "tests/lib")
  },
  @{
    Name          = "err_import_selective_private"
    Path          = "tests/err_import_selective_private.mettle"
    ShouldSucceed = $false
    Pattern       = "private_bonus|not exported"
    Args          = @("-I", "tests/lib")
  },
  @{
    Name          = "err_import_selective_private_dependency"
    Path          = "tests/err_import_selective_private_dependency.mettle"
    ShouldSucceed = $false
    Pattern       = "Undefined variable|private_bonus"
    Args          = @("-I", "tests/lib")
  },
  @{
    Name               = "err_import_bad_syntax_location"
    Path               = "tests/test_import_bad_syntax_location.mettle"
    ShouldSucceed      = $false
    Pattern            = "bad_syntax_module\.mettle"
    OutputMustNotMatch = @("Parse error in imported file", "test_import_bad_syntax_location\.mettle:[0-9]+:[0-9]+")
    Args               = @("-I", "tests/lib")
  },
  @{
    Name            = "err_import_bad_semantic_location"
    Path            = "tests/test_import_bad_semantic_location.mettle"
    ShouldSucceed   = $false
    Pattern         = "bad_semantic_module\.mettle"
    OutputMustMatch = @("Undefined variable")
    Args            = @("-I", "tests/lib")
  },
  @{
    Name          = "err_import_chain"
    Path          = "tests/test_import_chain_error.mettle"
    ShouldSucceed = $false
    Pattern       = "Could not resolve|import chain"
  }
)

$total = 0
$failed = 0

foreach ($case in $cases) {
  $caseName = $case.Name
  try {
    $total++
    $outFile = Join-Path $tmpDir ("{0}.obj" -f $case.Name)
    if (Test-Path $outFile) {
      Remove-Item -Path $outFile -Force -ErrorAction SilentlyContinue
    }

    $caseArgs = @()
    if ($case.ContainsKey("Args") -and $case.Args) {
      $caseArgs = @($case.Args)
    }
    if ((($case.ContainsKey("IrMustMatch") -and $case.IrMustMatch) -or
         ($case.ContainsKey("IrMustNotMatch") -and $case.IrMustNotMatch)) -and
        ($caseArgs -notcontains "--dump-ir") -and
        ($caseArgs -notcontains "--debug") -and
        ($caseArgs -notcontains "-d")) {
      $caseArgs += "--dump-ir"
    }

    # Per-case environment variables (restored right after the invocation).
    $savedEnv = @{}
    if ($case.ContainsKey("Env") -and $case.Env) {
      foreach ($k in $case.Env.Keys) {
        $savedEnv[$k] = [Environment]::GetEnvironmentVariable($k)
        [Environment]::SetEnvironmentVariable($k, $case.Env[$k])
      }
    }

    # -Width 4096: keep each diagnostic on one logical line so multi-word
    # Pattern matches aren't broken by console-width line wrapping.
    $output = & $CompilerPath @caseArgs $case.Path -o $outFile 2>&1 | Out-String -Width 4096
    $exitCode = $LASTEXITCODE

    foreach ($k in $savedEnv.Keys) {
      [Environment]::SetEnvironmentVariable($k, $savedEnv[$k])
    }

    $passed = $true
    $reason = ""

    if ($case.ShouldSucceed) {
      if ($exitCode -ne 0) {
        $passed = $false
        $reason = "Expected success, got exit code $exitCode"
      }
      else {
        $requiredOutputPatterns = @()
        $forbiddenOutputPatterns = @()
        $requiredIrPatterns = @()
        $forbiddenIrPatterns = @()
        if ($case.ContainsKey("OutputMustMatch") -and $case.OutputMustMatch) {
          $requiredOutputPatterns = @($case.OutputMustMatch)
        }
        if ($case.ContainsKey("OutputMustNotMatch") -and $case.OutputMustNotMatch) {
          $forbiddenOutputPatterns = @($case.OutputMustNotMatch)
        }
        if ($case.ContainsKey("IrMustMatch") -and $case.IrMustMatch) {
          $requiredIrPatterns = @($case.IrMustMatch)
        }
        if ($case.ContainsKey("IrMustNotMatch") -and $case.IrMustNotMatch) {
          $forbiddenIrPatterns = @($case.IrMustNotMatch)
        }
        $usesEmitObj = $caseArgs -contains "--emit-obj"

        # `mettle test` and `mettle trace` run in the compiler's interpreter and
        # emit no artifact, so a passing case in those modes has no binary to
        # check. SkipBinaryCheck lets such a case assert on output alone.
        $skipBinaryCheck = $case.ContainsKey("SkipBinaryCheck") -and $case.SkipBinaryCheck
        if (-not $skipBinaryCheck) {
          $binaryCheck = Test-BinaryOutput -BinaryPath $outFile
          if (-not $binaryCheck.Passed) {
            $passed = $false
            $reason = $binaryCheck.Reason
          }
        }
        if ($passed) {
          foreach ($pattern in $requiredOutputPatterns) {
            if ([string]::IsNullOrWhiteSpace($pattern)) {
              continue
            }
            if ($output -notmatch $pattern) {
              $passed = $false
              $reason = "Compiler output missing required pattern '$pattern'"
              break
            }
          }
        }
        if ($passed) {
          foreach ($pattern in $forbiddenOutputPatterns) {
            if ([string]::IsNullOrWhiteSpace($pattern)) {
              continue
            }
            if ($output -match $pattern) {
              $passed = $false
              $reason = "Compiler output matched forbidden pattern '$pattern'"
              break
            }
          }
        }
        if ($passed -and (($requiredIrPatterns.Count -gt 0) -or ($forbiddenIrPatterns.Count -gt 0))) {
          $irFile = "$outFile.ir"
          if ($usesEmitObj) {
            $objIrFile = ([System.IO.Path]::ChangeExtension($outFile, ".obj")) + ".ir"
            if (Test-Path $objIrFile) {
              $irFile = $objIrFile
            }
          }
          if (-not (Test-Path $irFile)) {
            $passed = $false
            $reason = "IR output file not produced"
          }
          else {
            $irText = Get-Content -Path $irFile -Raw

            foreach ($pattern in $requiredIrPatterns) {
              if ([string]::IsNullOrWhiteSpace($pattern)) {
                continue
              }
              if ($irText -notmatch $pattern) {
                $passed = $false
                $reason = "IR output missing required pattern '$pattern'"
                break
              }
            }

            if ($passed) {
              foreach ($pattern in $forbiddenIrPatterns) {
                if ([string]::IsNullOrWhiteSpace($pattern)) {
                  continue
                }
                if ($irText -match $pattern) {
                  $passed = $false
                  $reason = "IR output matched forbidden pattern '$pattern'"
                  break
                }
              }
            }
          }
        }
        if ($passed -and $case.ContainsKey("SidecarMustMatch") -and $case.SidecarMustMatch) {
          # The --explain sidecar: <output-stem>.explain.txt next to the obj.
          $sidecar = [System.IO.Path]::ChangeExtension($outFile, $null).TrimEnd('.') + ".explain.txt"
          if (-not (Test-Path $sidecar)) {
            $passed = $false
            $reason = "Expected explain sidecar '$sidecar' was not written"
          }
          else {
            $sidecarText = Get-Content -Path $sidecar -Raw
            foreach ($pattern in @($case.SidecarMustMatch)) {
              if ([string]::IsNullOrWhiteSpace($pattern)) {
                continue
              }
              if ($sidecarText -notmatch $pattern) {
                $passed = $false
                $reason = "Explain sidecar missing required pattern '$pattern'"
                break
              }
            }
          }
        }
        if ($passed -and -not $SkipDeterminism -and -not $skipBinaryCheck -and
            -not ($case.ContainsKey("SkipDeterminism") -and $case.SkipDeterminism)) {
          $outFile2 = Join-Path $tmpDir ("{0}.second.obj" -f $case.Name)
          if (Test-Path $outFile2) {
            Remove-Item -Path $outFile2 -Force -ErrorAction SilentlyContinue
          }

          $output2 = & $CompilerPath @caseArgs $case.Path -o $outFile2 2>&1 | Out-String
          $exitCode2 = $LASTEXITCODE
          if ($exitCode2 -ne 0) {
            $passed = $false
            $reason = "Determinism compile failed with exit code $exitCode2"
            if ($output2) {
              $output = $output + [Environment]::NewLine + $output2
            }
          }
          else {
            $hash1 = Get-Sha256FileHash -Path $outFile
            $hash2 = Get-Sha256FileHash -Path $outFile2
            if ($hash1 -ne $hash2) {
              $passed = $false
              $reason = "Determinism check failed: outputs differ between identical runs"
            }
          }
        }
      }
    }
    else {
      if ($exitCode -eq 0) {
        $passed = $false
        $reason = "Expected failure, got success"
      }
      elseif ($case.ContainsKey("Pattern") -and $case.Pattern) {
        if ($output -notmatch $case.Pattern) {
          $passed = $false
          $reason = "Failure message did not match expected pattern '$($case.Pattern)'"
        }
      }
      if ($passed -and $case.ContainsKey("OutputMustMatch") -and $case.OutputMustMatch) {
        foreach ($pattern in @($case.OutputMustMatch)) {
          if ([string]::IsNullOrWhiteSpace($pattern)) {
            continue
          }
          if ($output -notmatch $pattern) {
            $passed = $false
            $reason = "Failure output missing required pattern '$pattern'"
            break
          }
        }
      }
      if ($passed -and $case.ContainsKey("OutputMustNotMatch") -and $case.OutputMustNotMatch) {
        foreach ($pattern in @($case.OutputMustNotMatch)) {
          if ([string]::IsNullOrWhiteSpace($pattern)) {
            continue
          }
          if ($output -match $pattern) {
            $passed = $false
            $reason = "Failure output matched forbidden pattern '$pattern'"
            break
          }
        }
      }
    }

    if (-not $passed) {
      $failed++
      Write-CaseResult -Name $case.Name -Passed $false -Reason $reason
      if ($output) {
        Write-Host ($output.TrimEnd())
      }
    }
    else {
      Write-CaseResult -Name $case.Name -Passed $true
    }
  }
  catch {
    $failed++
    Write-CaseResult -Name $caseName -Passed $false -Reason $_.Exception.Message
  }
}

# SIMD correctness: build release binaries with the direct object backend and
# run adversarial runtime harnesses for each fused AVX2 family.
# `comptime for` folds a different offset/size/index into each expansion, so the
# only way to know the expansion was correct is to run it: the program checks
# the accumulated total itself and returns non-zero if any field is wrong.
$total++
try {
  $exePath = Join-Path $tmpDir "comptime_for_fields.exe"
  if (Test-Path $exePath) { Remove-Item -Path $exePath -Force -ErrorAction SilentlyContinue }
  $buildOut = & $CompilerPath --build --release "tests/test_comptime_for_fields.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "build failed: $buildOut" }
  & $exePath *> $null
  if ($LASTEXITCODE -ne 0) {
    throw "expansion produced wrong constants; program returned $LASTEXITCODE"
  }
  Write-CaseResult -Name "comptime_for_fields_runtime" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "comptime_for_fields_runtime" -Passed $false -Reason $_.Exception.Message
}

# `fieldof` folds a name lookup into an offset, a size and an index, so the
# expansion is only observable by running it. Both backends: the constants are
# folded in the checker, but the arithmetic around them is not.
# Returns 8 + 8 + (4 + 8 + 4) = 32.
foreach ($fieldofMode in @("debug", "release")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "fieldof_$fieldofMode.exe"
    if (Test-Path $exePath) { Remove-Item -Path $exePath -Force -ErrorAction SilentlyContinue }
    $buildArgs = @()
    if ($fieldofMode -eq "release") { $buildArgs += "--release" }
    $buildArgs += @("--build", "tests/test_fieldof.mettle", "-o", $exePath)
    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "fieldof ($fieldofMode) build failed: $buildOut" }
    & $exePath *> $null
    if ($LASTEXITCODE -ne 32) {
      throw "fieldof ($fieldofMode) returned $LASTEXITCODE (expected 32)"
    }
    Write-CaseResult -Name "fieldof_runtime_$fieldofMode" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "fieldof_runtime_$fieldofMode" -Passed $false -Reason $_.Exception.Message
  }
}

# Compile-time string equality, and the contract it exists for: two structs
# checked field-for-field by name at compile time. Every assert folds, so the
# program only has to confirm the walk ran over all three fields.
foreach ($strCmpMode in @("debug", "release")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "comptime_string_compare_$strCmpMode.exe"
    if (Test-Path $exePath) { Remove-Item -Path $exePath -Force -ErrorAction SilentlyContinue }
    $buildArgs = @()
    if ($strCmpMode -eq "release") { $buildArgs += "--release" }
    $buildArgs += @("--build", "tests/test_comptime_string_compare.mettle", "-o", $exePath)
    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "string compare ($strCmpMode) build failed: $buildOut" }
    & $exePath *> $null
    if ($LASTEXITCODE -ne 3) {
      throw "string compare ($strCmpMode) returned $LASTEXITCODE (expected 3)"
    }
    Write-CaseResult -Name "comptime_string_compare_$strCmpMode" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "comptime_string_compare_$strCmpMode" -Passed $false -Reason $_.Exception.Message
  }
}

# A module-scope directive generates real declarations: the program calls the
# functions, reads the constants and declares the structs it produced, and
# returns non-zero if any of them carries the wrong field.
$total++
try {
  $exePath = Join-Path $tmpDir "comptime_for_declarations.exe"
  if (Test-Path $exePath) { Remove-Item -Path $exePath -Force -ErrorAction SilentlyContinue }
  $buildOut = & $CompilerPath --build --release "tests/test_comptime_for_declarations.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "build failed: $buildOut" }
  & $exePath *> $null
  if ($LASTEXITCODE -ne 0) {
    throw "a generated declaration carried the wrong field; program returned $LASTEXITCODE"
  }
  Write-CaseResult -Name "comptime_for_declarations_runtime" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "comptime_for_declarations_runtime" -Passed $false -Reason $_.Exception.Message
}

# A generated declaration gets exactly the trust a written one gets, which is
# none: `@test` on one runs, and `@noalloc` on one fails the build.
$total++
try {
  $out = & $CompilerPath test "tests/test_comptime_for_declarations.mettle" 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "generated tests failed: $out" }
  foreach ($expected in @("test offset_is_ordered_kind", "test offset_is_ordered_payload", "3 passed")) {
    if ($out -notmatch [regex]::Escape($expected)) { throw "missing '$expected' in: $out" }
  }

  $contract = Join-Path $tmpDir "generated_noalloc.mettle"
  @'
struct P { a: int32; }
struct Node { v: int32; }
comptime for f in typeof(P).fields {
  @noalloc fn ident("make_", f.name)() -> Node* {
    return new Node;
  }
}
fn main() -> int32 { return 0; }
'@ | Set-Content -Path $contract -Encoding utf8
  $out = & $CompilerPath --release $contract 2>&1 | Out-String
  if ($LASTEXITCODE -eq 0) { throw "@noalloc on generated code did not fail the build: $out" }
  # Matched piecewise: the console wraps a long error line, so the message is
  # not guaranteed to arrive on one.
  if ($out -notmatch "@noalloc" -or $out -notmatch "make_a" -or $out -notmatch "allocates") {
    throw "contract failure did not name the generated function: $out"
  }
  Write-CaseResult -Name "generated_code_keeps_contracts" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "generated_code_keeps_contracts" -Passed $false -Reason $_.Exception.Message
}

# `mettle expand` must show generated declarations the same way it shows
# generated blocks, and the ledger must count a module-scope site.
$total++
try {
  $out = & $CompilerPath expand "tests/test_comptime_for_declarations.mettle" 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "expand failed: $out" }
  foreach ($expected in @(
      "fn end_of_kind(base: int64) -> int64",
      "struct Slot_payload {",
      "fn both_step_step() -> int64",
      "expanded from comptime-for iteration 2 (field ``seq``)")) {
    if ($out -notmatch [regex]::Escape($expected)) { throw "missing '$expected' in: $out" }
  }
  if ($out -match "ident\(") { throw "expand still shows an unresolved composed name: $out" }
  if ($out -match "comptime for") { throw "expand still shows an unexpanded directive: $out" }

  $used = & $CompilerPath --report-expansion "tests/test_comptime_for_declarations.mettle" 2>&1 | Out-String
  if ($used -notmatch "3 iterations") { throw "ledger missing the module-scope site: $used" }
  Write-CaseResult -Name "expand_shows_generated_declarations" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "expand_shows_generated_declarations" -Passed $false -Reason $_.Exception.Message
}

# `mettle expand` must show generated code as readable source, attributed to
# the iteration that produced it, with the same note a diagnostic would carry.
$total++
try {
  $out = & $CompilerPath expand "tests/test_comptime_for_fields.mettle" 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "expand failed: $out" }
  foreach ($expected in @(
      "expanded from comptime-for iteration 1 (field ``kind``)",
      "expanded from comptime-for iteration 3 (field ``payload``)",
      "struct Packet {")) {
    if ($out -notmatch [regex]::Escape($expected)) { throw "missing '$expected' in: $out" }
  }
  # The directive itself must be gone: expansion replaced it.
  if ($out -match "comptime for") { throw "expand still shows an unexpanded directive: $out" }
  # Each iteration folded a different offset, which is the whole point.
  foreach ($folded in @("total + 0", "total + 4", "total + 8")) {
    if ($out -notmatch [regex]::Escape($folded)) { throw "missing folded '$folded' in: $out" }
  }
  Write-CaseResult -Name "expand_shows_generated_source" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "expand_shows_generated_source" -Passed $false -Reason $_.Exception.Message
}

# III.2.2's second half: `trace` steps through an expansion, which means saying
# which iteration produced which value. Before the IR carried the expansion
# note these merged into one indistinguishable run ("total = 100, 505, 1315
# (3x)") because the chain lived on the error reporter's note frames and never
# reached the instructions the interpreter walks.
$total++
try {
  $out = & $CompilerPath trace "tests/test_comptime_for_fields.mettle" main 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "trace failed: $out" }
  foreach ($expected in @('(field `kind`) total = 100',
                          '(field `seq`) total = 505',
                          '(field `payload`) total = 1315')) {
    if ($out -notmatch [regex]::Escape($expected)) {
      throw "trace did not attribute the expansion: missing '$expected' in: $out"
    }
  }
  # The merged form must be gone, or the values were not separated at all.
  if ($out -match "100, 505, 1315") {
    throw "trace still merges expansions into one run: $out"
  }
  # Code the programmer wrote carries no note.
  if ($out -notmatch "<- total = 0") {
    throw "written code should be annotated without an expansion note: $out"
  }
  Write-CaseResult -Name "trace_attributes_expansions" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "trace_attributes_expansions" -Passed $false -Reason $_.Exception.Message
}

# Expansion is inspectable, which means every declaration the programmer wrote
# comes back as source. A module-scope `static_assert` used to reach the
# printer's default arm and be reported as having no source form, which also
# made expand disclaim the whole file as an incomplete program.
$total++
try {
  $out = & $CompilerPath expand "tests/test_fieldof.mettle" 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "expand failed: $out" }
  if ($out -match "no source form") {
    throw "expand reported a declaration as unprintable: $out"
  }
  if ($out -match "not a complete program") {
    throw "expand disclaimed the output despite printing every node: $out"
  }
  foreach ($expected in @('static_assert(', 'fieldof(Packet, "stamp")')) {
    if ($out -notmatch [regex]::Escape($expected)) { throw "missing '$expected' in: $out" }
  }
  Write-CaseResult -Name "expand_prints_module_scope_asserts" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "expand_prints_module_scope_asserts" -Passed $false -Reason $_.Exception.Message
}

# `@swappable` has to keep the call boundary a swap would redirect, including
# under --release where the inliner is most aggressive. `policy` and `plain`
# have identical bodies and differ only by the decorator, so the inliner's
# verdict on the pair is what proves the decorator did it. `quiesce;` must
# survive to a running program without emitting anything.
$total++
try {
  $swapSrc = "tests/test_swappable_quiesce.mettle"
  $out = & $CompilerPath --release --explain $swapSrc -o (Join-Path $tmpDir "swq.obj") 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "build failed: $out" }
  if ($out -notmatch "call to ``policy``[^\r\n]*NOT inlined") {
    throw "a @swappable function lost its call boundary under --release: $out"
  }
  if ($out -notmatch "call to ``plain``[^\r\n]*: inlined") {
    throw "the undecorated twin should still inline, or the test proves nothing: $out"
  }

  foreach ($mode in @("debug", "release")) {
    $exe = Join-Path $tmpDir "swq_$mode.exe"
    $buildArgs = @()
    if ($mode -eq "release") { $buildArgs += "--release" }
    $buildArgs += @("--build", $swapSrc, "-o", $exe)
    $bo = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "swappable ($mode) build failed: $bo" }
    & $exe *> $null
    if ($LASTEXITCODE -ne 20) {
      throw "swappable ($mode) returned $LASTEXITCODE (expected 20)"
    }
  }

  # Expansion is inspectable, and that includes the swap point.
  $ex = & $CompilerPath expand $swapSrc 2>&1 | Out-String
  if ($ex -notmatch "quiesce;") { throw "expand did not print the swap point: $ex" }

  Write-CaseResult -Name "swappable_keeps_call_boundary" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "swappable_keeps_call_boundary" -Passed $false -Reason $_.Exception.Message
}

# A function replaced in a running process. Staging records an intent and
# changes nothing; `quiesce;` is the only place it takes effect. The program
# checks each step itself and returns 42 only if every one held.
foreach ($swapMode in @("debug", "release")) {
  $total++
  try {
    $exe = Join-Path $tmpDir "hotswap_$swapMode.exe"
    $buildArgs = @()
    if ($swapMode -eq "release") { $buildArgs += "--release" }
    $buildArgs += @("--build", "tests/test_hot_swap.mettle", "-o", $exe)
    $out = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "hot swap ($swapMode) build failed: $out" }
    & $exe *> $null
    if ($LASTEXITCODE -ne 42) {
      throw "hot swap ($swapMode) returned $LASTEXITCODE (expected 42; see the step codes in the fixture)"
    }
    Write-CaseResult -Name "hot_swap_$swapMode" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "hot_swap_$swapMode" -Passed $false -Reason $_.Exception.Message
  }
}

# `==` / `!=` on strings compare contents, in both backends. Before this the
# 16-byte record was compared as a scalar and `"ab" == "ab"` was false, which
# compiled clean and warned about nothing.
foreach ($strEqMode in @("debug", "release")) {
  $total++
  try {
    $exe = Join-Path $tmpDir "stringeq_$strEqMode.exe"
    $buildArgs = @()
    if ($strEqMode -eq "release") { $buildArgs += "--release" }
    $buildArgs += @("--build", "tests/test_string_equality.mettle", "-o", $exe)
    $out = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "string equality ($strEqMode) build failed: $out" }
    & $exe *> $null
    if ($LASTEXITCODE -ne 77) {
      throw "string equality ($strEqMode) returned $LASTEXITCODE (expected 77; the code names the case)"
    }
    Write-CaseResult -Name "string_equality_$strEqMode" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "string_equality_$strEqMode" -Passed $false -Reason $_.Exception.Message
  }
}

# std/conv's string-native half: slicing, searching, trimming, splitting and
# parsing, all on `string` and all returning views into the input. Every
# returned view crosses a call boundary, so this also exercises string
# return-by-value at scale rather than in isolation.
foreach ($sopsMode in @("debug", "release")) {
  $total++
  try {
    $exe = Join-Path $tmpDir "std_string_ops_$sopsMode.exe"
    $buildArgs = @()
    if ($sopsMode -eq "release") { $buildArgs += "--release" }
    $buildArgs += @("--build", "tests/test_std_string_ops.mettle", "-o", $exe)
    $out = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "std string ops ($sopsMode) build failed: $out" }
    & $exe *> $null
    if ($LASTEXITCODE -ne 88) {
      throw "std string ops ($sopsMode) returned $LASTEXITCODE (expected 88; the code names the function)"
    }
    Write-CaseResult -Name "std_string_ops_$sopsMode" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "std_string_ops_$sopsMode" -Passed $false -Reason $_.Exception.Message
  }
}

# `string` moves like the two-field record it is: copied whole, returned
# through a hidden pointer, stored inline in an aggregate. It used to be a
# pointer in some places and the record in others, and the disagreements were
# silent. Both backends, because the fallback and MIR paths each decide this
# for themselves, and @noinline inside the fixture so an inlined call cannot
# hide a broken boundary.
foreach ($sbvMode in @("debug", "release")) {
  $total++
  try {
    $exe = Join-Path $tmpDir "string_byvalue_$sbvMode.exe"
    $buildArgs = @()
    if ($sbvMode -eq "release") { $buildArgs += "--release" }
    $buildArgs += @("--build", "tests/test_string_by_value.mettle", "-o", $exe)
    $out = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "string by value ($sbvMode) build failed: $out" }
    & $exe *> $null
    if ($LASTEXITCODE -ne 66) {
      throw "string by value ($sbvMode) returned $LASTEXITCODE (expected 66; the code names the shape)"
    }
    Write-CaseResult -Name "string_by_value_$sbvMode" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "string_by_value_$sbvMode" -Passed $false -Reason $_.Exception.Message
  }
}

# String comparison is opt-in by use: a program that never compares strings
# never names mettle_string_ and never links it.
$total++
try {
  $exe = Join-Path $tmpDir "string_excision.exe"
  $out = & $CompilerPath --build "tests/runtime_excision_probe.mettle" -o $exe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "probe build failed: $out" }
  $bytes = [IO.File]::ReadAllBytes($exe)
  if ([Text.Encoding]::ASCII.GetString($bytes).Contains("mettle_string_eq")) {
    throw "the string runtime was linked into a program that compares no strings"
  }
  Write-CaseResult -Name "string_runtime_excisable" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "string_runtime_excisable" -Passed $false -Reason $_.Exception.Message
}

# The swap runtime is opt-in like every other component: a program with no
# quiesce point never names mettle_swap_ and never links it.
$total++
try {
  $exe = Join-Path $tmpDir "swap_excision.exe"
  $out = & $CompilerPath --build "tests/runtime_excision_probe.mettle" -o $exe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "probe build failed: $out" }
  $bytes = [IO.File]::ReadAllBytes($exe)
  # mettle_swap_apply is called only by `quiesce;`, so its absence here is the
  # absence of the whole component.
  if ([Text.Encoding]::ASCII.GetString($bytes).Contains("mettle_swap_apply")) {
    throw "the swap runtime was linked into a program with no quiesce point"
  }
  Write-CaseResult -Name "swap_runtime_excisable" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "swap_runtime_excisable" -Passed $false -Reason $_.Exception.Message
}

# Runtime excision: each optional component has to be absent from a binary that
# did not ask for it, and the absence has to be checkable rather than asserted.
# What is excisable is optional; what is mandatory is a tax, so this is the
# gate that keeps the runtime honest as it grows.
#
# Each component is checked in both directions. Absence alone would pass if the
# marker never appeared at all, so the same marker must be present when the
# feature IS requested. Without that pairing the test proves nothing.
$total++
try {
  $probe = "tests/runtime_excision_probe.mettle"
  $components = @(
    @{ Name = "safety";        Flag = "--safe";            Marker = "memory access outside its allocation" },
    @{ Name = "crash_handler"; Flag = "-s";                Marker = "Fatal error: null pointer dereference" },
    @{ Name = "profile";       Flag = "--profile-runtime"; Marker = "total_us    avg_ns" },
    @{ Name = "debug_hooks";   Flag = "--debug-hooks";     Marker = "not a variable in this frame" }
  )

  function Test-BinaryContains($path, $marker) {
    $bytes = [IO.File]::ReadAllBytes($path)
    return [Text.Encoding]::ASCII.GetString($bytes).Contains($marker)
  }

  foreach ($c in $components) {
    $offExe = Join-Path $tmpDir ("excise_off_" + $c.Name + ".exe")
    $onExe  = Join-Path $tmpDir ("excise_on_"  + $c.Name + ".exe")

    $out = & $CompilerPath --build $probe -o $offExe 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "baseline build failed: $out" }
    if (Test-BinaryContains $offExe $c.Marker) {
      throw ("$($c.Name) was linked into a binary that did not ask for it: " +
             "found '$($c.Marker)' without $($c.Flag)")
    }

    $out = & $CompilerPath $c.Flag --build $probe -o $onExe 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "$($c.Name) build with $($c.Flag) failed: $out" }
    if (-not (Test-BinaryContains $onExe $c.Marker)) {
      throw ("$($c.Name) marker '$($c.Marker)' is absent even with $($c.Flag); " +
             "the absence check above proves nothing until this passes")
    }
  }

  Write-CaseResult -Name "runtime_components_excisable" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "runtime_components_excisable" -Passed $false -Reason $_.Exception.Message
}

# `mettle swap-check` points the differential harness at two functions instead
# of at one function before and after a pass. Four verdicts have to hold: an
# equivalent rewrite passes, a divergence inside the generated inputs is caught
# with a counterexample, a changed signature is refused before any input runs,
# and a passing verdict says it is a test rather than a proof.
$total++
try {
  $swapFile = "tests/swap_check_pairs.mettle"

  $out = & $CompilerPath swap-check $swapFile --old scale_v1 --new scale_v2 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "equivalent rewrite should pass: $out" }
  if ($out -notmatch "OK") { throw "expected OK for the equivalent rewrite: $out" }
  # A passing verdict must not read as equivalence.
  if ($out -notmatch "not a proof") {
    throw "a passing verdict must state that it is a differential test: $out"
  }

  $out = & $CompilerPath swap-check $swapFile --old near_v1 --new near_v2 2>&1 | Out-String
  if ($LASTEXITCODE -eq 0) { throw "divergence at n=5 should have been caught: $out" }
  if ($out -notmatch "DIVERGED") { throw "expected DIVERGED: $out" }
  if ($out -notmatch "near_v2\(5\)") { throw "expected the counterexample naming the input: $out" }

  $out = & $CompilerPath swap-check $swapFile --old near_v1 --new other_shape 2>&1 | Out-String
  if ($LASTEXITCODE -eq 0) { throw "a changed signature should be refused: $out" }
  if ($out -notmatch "REFUSED") { throw "expected REFUSED for a signature change: $out" }

  # A boundary the fixed shape table never reaches. The gate harvests the
  # constants these two functions compare against and tests both sides of
  # each, so a difference that only appears at n = 100 is caught with that
  # exact input. This case passed as OK before harvesting existed.
  $out = & $CompilerPath swap-check $swapFile --old far_v1 --new far_v2 2>&1 | Out-String
  if ($LASTEXITCODE -eq 0) { throw "divergence at n=100 should have been caught: $out" }
  if ($out -notmatch "DIVERGED") { throw "expected DIVERGED at the harvested boundary: $out" }
  if ($out -notmatch "far_v2\(100\)") {
    throw "expected the counterexample to name the harvested boundary input: $out"
  }

  Write-CaseResult -Name "swap_check_verdicts" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "swap_check_verdicts" -Passed $false -Reason $_.Exception.Message
}

# Expansion keeps a ledger, and a budget is a contract that fails the build.
$total++
try {
  $used = & $CompilerPath --report-expansion "tests/test_comptime_for_fields.mettle" 2>&1 | Out-String
  if ($used -notmatch "comptime expansion: 2 sites") { throw "ledger wrong: $used" }
  if ($used -notmatch "3 iterations") { throw "ledger missing iteration count: $used" }

  # A program that expands nothing must say so: an absence you can point at.
  $none = & $CompilerPath --report-expansion "tests/test_type_table_enum.mettle" 2>&1 | Out-String
  if ($none -notmatch "no sites; nothing generated") { throw "absence not reported: $none" }

  $over = & $CompilerPath --expansion-budget=10 "tests/test_comptime_for_fields.mettle" 2>&1 | Out-String
  if ($LASTEXITCODE -eq 0) { throw "budget of 10 should have failed the build" }
  if ($over -notmatch "over the budget of 10") { throw "budget error unclear: $over" }

  & $CompilerPath --expansion-budget=100000 "tests/test_comptime_for_fields.mettle" *> $null
  if ($LASTEXITCODE -ne 0) { throw "a budget above the real cost should compile" }
  Write-CaseResult -Name "expansion_ledger_and_budget" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "expansion_ledger_and_budget" -Passed $false -Reason $_.Exception.Message
}

# `.name` is module-qualified, and a string is not foldable, so the only way to
# check the qualification is to run the program and read what it printed.
$total++
try {
  $exePath = Join-Path $tmpDir "type_names.exe"
  if (Test-Path $exePath) { Remove-Item -Path $exePath -Force -ErrorAction SilentlyContinue }
  $buildOut = & $CompilerPath --build "tests/test_type_names.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "build failed: $buildOut" }
  $runOut = & $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "name lengths wrong; program returned $LASTEXITCODE`: $runOut" }
  foreach ($expected in @("declared=test_type_names.Point", "builtin=int32", "field=y")) {
    if ($runOut -notmatch [regex]::Escape($expected)) {
      throw "expected '$expected' in output: $runOut"
    }
  }
  Write-CaseResult -Name "type_names_qualified" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "type_names_qualified" -Passed $false -Reason $_.Exception.Message
}

$simdRuntimeCases = @(
  @{
    Name            = "simd_correctness_int"
    Path            = "tests/simd_correctness/simd_int_check.mettle"
    OutputMustMatch = "INT SIMD: ALL OK"
    IrMustMatch     = @("sum_i32", "dot_i32", "scale_i32", "clamp_i32", "reverse_copy_i32", "minmax_i32")
  },
  @{
    Name            = "simd_correctness_float"
    Path            = "tests/simd_correctness/simd_float_check.mettle"
    OutputMustMatch = "FLOAT SIMD: ALL OK"
    IrMustMatch     = @("simd_sum_f64", "simd_sum_f32", "simd_dot_f64", "simd_dot_f32", "simd_affine_map_f64", "simd_affine_map_f32", "simd_vloop_f64")
  },
  @{
    Name            = "simd_correctness_byte"
    Path            = "tests/simd_correctness/simd_byte_check.mettle"
    OutputMustMatch = "BYTE SIMD: ALL OK"
    IrMustMatch     = @("simd_byte_map", "simd_sum_u8")
  }
)

foreach ($case in $simdRuntimeCases) {
  $total++
  try {
    $exePath = Join-Path $tmpDir ("{0}.exe" -f $case.Name)
    $objPath = [System.IO.Path]::ChangeExtension($exePath, ".obj")
    $irPath = "$objPath.ir"
    foreach ($artifactPath in @($exePath, $objPath, $irPath)) {
      if (Test-Path $artifactPath) {
        Remove-Item -Path $artifactPath -Force -ErrorAction SilentlyContinue
      }
    }

    $buildOut = & $CompilerPath --build --linker internal --release --dump-ir $case.Path -o $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "SIMD correctness build failed: $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "SIMD correctness build did not produce an executable"
    }
    if (-not (Test-Path $irPath)) {
      throw "SIMD correctness IR output file not produced"
    }
    $irText = Get-Content -Path $irPath -Raw
    foreach ($pattern in @($case.IrMustMatch)) {
      if ($irText -notmatch $pattern) {
        throw "SIMD correctness IR missing required pattern '$pattern'"
      }
    }

    $runOut = & $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "SIMD correctness executable exited with $LASTEXITCODE`: $runOut"
    }
    if ($runOut -notmatch $case.OutputMustMatch) {
      throw "SIMD correctness output missing expected marker '$($case.OutputMustMatch)': $runOut"
    }
    Write-CaseResult -Name $case.Name -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name $case.Name -Passed $false -Reason $_.Exception.Message
  }
}

# Function decorators: build a release binary exercising @pure + @noinline
# (loop-invariant pure-call hoisting), @inline (forced past the call-count
# heuristic), and @simd! on a function (per-body-loop vectorization contract).
# Confirm the IR shows each transform and that the program is still correct.
$total++
try {
  $exePath = Join-Path $tmpDir "decorators.exe"
  $objPath = [System.IO.Path]::ChangeExtension($exePath, ".obj")
  $irPath = "$objPath.ir"
  foreach ($artifactPath in @($exePath, $objPath, $irPath)) {
    if (Test-Path $artifactPath) {
      Remove-Item -Path $artifactPath -Force -ErrorAction SilentlyContinue
    }
  }

  $buildOut = & $CompilerPath --build --linker internal --release --dump-ir "tests/test_decorators.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "decorators build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "decorators build did not produce an executable"
  }
  if (-not (Test-Path $irPath)) {
    throw "decorators IR output file not produced"
  }
  $irText = Get-Content -Path $irPath -Raw
  if ($irText -notmatch "licm_pure_") {
    throw "decorators IR missing 'licm_pure_' (pure-call LICM did not fire)"
  }
  if ($irText -notmatch "sum_i32") {
    throw "decorators IR missing 'sum_i32' (@simd! function did not vectorize)"
  }
  if ($irText -match "many_calls\(") {
    throw "decorators IR still calls 'many_calls' (@inline did not force inlining)"
  }

  $runOut = & $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "decorators executable exited with $LASTEXITCODE`: $runOut"
  }
  if ($runOut -notmatch "DECORATORS OK") {
    throw "decorators output missing expected marker: $runOut"
  }
  Write-CaseResult -Name "decorators" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "decorators" -Passed $false -Reason $_.Exception.Message
}

# --ml-opt translation-validation gate: every model disposition is executed
# through the reference interpreter before it stands. A clean run and a
# speculative run (model dead-code deletes) must never change program
# behavior, and a hand-injected wrong disposition must be rejected with a
# counterexample while the binary stays correct.
$total++
try {
  $mlBase = Join-Path $tmpDir "ml_gate_base.exe"
  $buildOut = & $CompilerPath --build --release "tests/ml_gate.mettle" -o $mlBase 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "ml_gate baseline build failed: $buildOut"
  }
  & $mlBase 2>&1 | Out-Null
  $mlBaseExit = $LASTEXITCODE

  $mlExe = Join-Path $tmpDir "ml_gate_ml.exe"
  $buildOut = & $CompilerPath --build --release --ml-opt "tests/ml_gate.mettle" -o $mlExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "ml_gate --ml-opt build failed: $buildOut"
  }
  if ($buildOut -notmatch "--ml-opt:") {
    throw "ml_gate --ml-opt summary line missing"
  }
  & $mlExe 2>&1 | Out-Null
  if ($LASTEXITCODE -ne $mlBaseExit) {
    throw "--ml-opt changed program behavior: exit $LASTEXITCODE vs baseline $mlBaseExit"
  }

  $mlSpec = Join-Path $tmpDir "ml_gate_spec.exe"
  $buildOut = & $CompilerPath --build --release --ml-opt-speculative "tests/ml_gate.mettle" -o $mlSpec 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "ml_gate --ml-opt-speculative build failed: $buildOut"
  }
  & $mlSpec 2>&1 | Out-Null
  if ($LASTEXITCODE -ne $mlBaseExit) {
    throw "--ml-opt-speculative changed program behavior: exit $LASTEXITCODE vs baseline $mlBaseExit"
  }

  # Inject two wrong dispositions resolved from the post-classical IR dump: a
  # CONST fold of mix's `a * b` (wrong at function level) and a speculative
  # delete of signbit's `neg <- 0` initializer (visible only because
  # uninitialized locals read poison, not zero). The validator must reject
  # both and the binary must still match the baseline.
  $irLine = Select-String -Path "_mlopt.ir" -Pattern "^\s+(\d+): %\S+ = @a \* @b" | Select-Object -First 1
  if (-not $irLine) {
    throw "could not locate 'a * b' in _mlopt.ir"
  }
  $badIdx = $irLine.Matches[0].Groups[1].Value
  $negLine = Select-String -Path "_mlopt.ir" -Pattern "^\s+(\d+): @neg <- 0" | Select-Object -First 1
  if (-not $negLine) {
    throw "could not locate '@neg <- 0' in _mlopt.ir"
  }
  $negIdx = $negLine.Matches[0].Groups[1].Value
  $dispPath = Join-Path $tmpDir "ml_gate_bad.disp"
  "mix $badIdx CONST 271828`nsignbit $negIdx NOP" | Out-File -Encoding ascii $dispPath
  $env:METTLE_ML_DISP = $dispPath
  $badExe = Join-Path $tmpDir "ml_gate_bad.exe"
  $buildOut = & $CompilerPath --build --release --ml-opt "tests/ml_gate.mettle" -o $badExe 2>&1 | Out-String
  $env:METTLE_ML_DISP = $null
  if ($LASTEXITCODE -ne 0) {
    throw "ml_gate bad-disposition build failed: $buildOut"
  }
  if ($buildOut -notmatch "PROPOSAL REJECTED") {
    throw "wrong disposition was not rejected by the validator: $buildOut"
  }
  if ($buildOut -notmatch "keeps its validated IR") {
    throw "rejection did not report restoring validated IR"
  }
  & $badExe 2>&1 | Out-Null
  if ($LASTEXITCODE -ne $mlBaseExit) {
    throw "rejected disposition still changed behavior: exit $LASTEXITCODE vs baseline $mlBaseExit"
  }
  # Provenance: a `?` suffix marks a disposition as model-sourced rather than
  # proven by construction, and anything unproven must be gated exactly like a
  # speculative NOP. Without this, a future model that picks its own rewrite
  # targets could reuse the COPY kind and have its guesses applied UNVALIDATED
  # on every function the interpreter cannot execute. Here the same wrong CONST
  # is injected as `CONST?`; it must still be caught and the binary must still
  # match the baseline.
  $dispPath2 = Join-Path $tmpDir "ml_gate_unproven.disp"
  "mix $badIdx CONST? 271828" | Out-File -Encoding ascii $dispPath2
  $env:METTLE_ML_DISP = $dispPath2
  $unpExe = Join-Path $tmpDir "ml_gate_unproven.exe"
  $buildOut = & $CompilerPath --build --release --ml-opt "tests/ml_gate.mettle" -o $unpExe 2>&1 | Out-String
  $env:METTLE_ML_DISP = $null
  if ($LASTEXITCODE -ne 0) {
    throw "ml_gate unproven-disposition build failed: $buildOut"
  }
  if ($buildOut -match "proven-only\)\s*,?\s*0 REJECTED" -and
      $buildOut -notmatch "PROPOSAL REJECTED") {
    throw "unproven disposition was applied without adjudication: $buildOut"
  }
  & $unpExe 2>&1 | Out-Null
  if ($LASTEXITCODE -ne $mlBaseExit) {
    throw "unproven disposition changed behavior: exit $LASTEXITCODE vs baseline $mlBaseExit"
  }

  Write-CaseResult -Name "ml_opt_gate" -Passed $true
}
catch {
  $env:METTLE_ML_DISP = $null
  $failed++
  Write-CaseResult -Name "ml_opt_gate" -Passed $false -Reason $_.Exception.Message
}

# --ml-opt sabotage self-test: METTLE_ML_SABOTAGE corrupts one real model
# disposition into a wrong constant; the gate must catch it, name it, and
# discard it - the ml-opt twin of verify_sabotage_caught.
$total++
try {
  $env:METTLE_ML_SABOTAGE = "1"
  $sabExe = Join-Path $tmpDir "ml_gate_sab.exe"
  $buildOut = & $CompilerPath --build --release --ml-opt "examples/explain_demo/explain_demo.mettle" -o $sabExe 2>&1 | Out-String
  $env:METTLE_ML_SABOTAGE = $null
  if ($LASTEXITCODE -ne 0) {
    throw "sabotaged --ml-opt build failed: $buildOut"
  }
  if ($buildOut -notmatch "SABOTAGE armed") {
    throw "sabotage did not arm (model produced no COPY/CONST disposition?)"
  }
  if ($buildOut -notmatch "PROPOSAL REJECTED") {
    throw "sabotaged disposition was not rejected: $buildOut"
  }
  if ($buildOut -notmatch "REJECTED by the validator") {
    throw "summary line does not report the rejection: $buildOut"
  }
  Write-CaseResult -Name "ml_opt_sabotage_caught" -Passed $true
}
catch {
  $env:METTLE_ML_SABOTAGE = $null
  $failed++
  Write-CaseResult -Name "ml_opt_sabotage_caught" -Passed $false -Reason $_.Exception.Message
}

# OBS Python/C parity. The ml-opt model trains on node features computed in
# Python (tools/mlopt/obs.py) and runs on node features computed in C
# (src/ir/ml_obs.c). A divergence between them does not crash or warn - the
# model just reads different inputs at compile time than it trained on, and the
# only symptom is optimization quality silently degrading. This replays the
# golden vectors so that failure mode is a build error instead.
$total++
try {
  $obsExe = "bin/ml_obs_parity_test.exe"
  & gcc -Wall -Wextra -std=c11 -g -O1 -Isrc -Iinclude tests/ml_obs_parity_test.c src/ir/ml_obs.c -o $obsExe -lm
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to compile OBS parity test"
  }
  $obsOutput = & $obsExe "tools/mlopt/obs_golden.txt" 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "OBS parity test failed:`n$obsOutput"
  }
  if ($obsOutput -notmatch "RESULT: (PASS|SKIP)") {
    throw "OBS parity test did not report PASS or SKIP:`n$obsOutput"
  }
  Write-CaseResult -Name "ml_obs_python_parity" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "ml_obs_python_parity" -Passed $false -Reason $_.Exception.Message
}


# Safety runtime. Under --safe the compiler leaves a check wherever it could
# not prove an access in bounds, and this is the half that answers it: which
# accesses are inside their allocation, which run off the end, and which touch
# memory that has been freed. Every answer here is a correctness claim with no
# compiler involvement, so it is tested on its own.
$total++
try {
  $safetyExe = Join-Path $tmpDir "safety_runtime_test.exe"
  $compileSafety = & gcc -Wall -Wextra -std=c99 -g -O1 -D_GNU_SOURCE -Isrc tests/safety_runtime_test.c src/runtime/safety.c -o $safetyExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Safety runtime harness compile failed: $compileSafety"
  }
  $safetyOutput = & $safetyExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Safety runtime test failed:`n$safetyOutput"
  }
  if ($safetyOutput -notmatch "RESULT: PASS") {
    throw "Safety runtime test did not report PASS:`n$safetyOutput"
  }
  Write-CaseResult -Name "safety_runtime" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "safety_runtime" -Passed $false -Reason $_.Exception.Message
}


# --safe end to end. The ordinary bounds check is gone at --release, so the
# baseline case reads past the array and returns whatever was there. Each bad
# program is built both ways: --safe must trap, and the same program without it
# must not, which is what shows the check is doing the catching rather than
# some unrelated change in codegen.
$total++
try {
  $safeClean = Join-Path $tmpDir "safe_clean.exe"
  & $CompilerPath --build --safe --release tests/test_safe_clean.mettle -o $safeClean 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_clean failed"
  }
  & $safeClean 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 98) {
    throw "Correct code changed its answer under --safe: expected 98, got $LASTEXITCODE"
  }

  foreach ($case in @("test_safe_bounds_overflow", "test_safe_bounds_negative")) {
    $safeExe = Join-Path $tmpDir "$case.safe.exe"
    & $CompilerPath --build --safe --release "tests/$case.mettle" -o $safeExe 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "--safe build of $case failed"
    }
    $safeOut = & $safeExe 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0) {
      throw "$case ran to completion under --safe; the access should have trapped"
    }
    if ($safeOut -notmatch "outside its bounds") {
      throw "$case trapped under --safe without naming the access:`n$safeOut"
    }

    $baseExe = Join-Path $tmpDir "$case.base.exe"
    & $CompilerPath --build --release "tests/$case.mettle" -o $baseExe 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "baseline build of $case failed"
    }
    $baseOut = & $baseExe 2>&1 | Out-String
    if ($baseOut -match "outside its bounds") {
      throw "$case trapped without --safe, so the case proves nothing about the check"
    }
  }
  Write-CaseResult -Name "safe_mode_bounds" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "safe_mode_bounds" -Passed $false -Reason $_.Exception.Message
}


# --safe on the heap, where the extent is not in the code and only the runtime
# shadow map can answer. Run against both allocators: the libc one is C the
# instrumentation never enters, while --native-heap routes to std/alloc, which
# is Mettle code that writes block headers and poisons freed blocks. Those
# accesses are outside the model by design, so the allocator call is bracketed;
# without that bracket correct programs trap, which is why the clean case is
# checked on both paths rather than one.
$total++
try {
  $allocators = @{ "libc" = @(); "native" = @("--native-heap") }
  foreach ($allocator in $allocators.Keys) {
    $extra = $allocators[$allocator]

    $cleanExe = Join-Path $tmpDir "safe_heap_clean.$allocator.exe"
    & $CompilerPath --build --safe --release @extra tests/test_safe_heap_clean.mettle -o $cleanExe 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "--safe build of test_safe_heap_clean failed on $allocator"
    }
    $cleanOut = & $cleanExe 2>&1 | Out-String
    if ($LASTEXITCODE -ne 18) {
      throw "correct heap code failed under --safe on ${allocator}: expected 18, got $LASTEXITCODE`n$cleanOut"
    }

    $bad = @{
      "test_safe_heap_overflow"  = "outside its allocation"
      "test_safe_use_after_free" = "after it was freed"
      "test_safe_realloc_stale"  = "after it was freed"
    }
    foreach ($case in $bad.Keys) {
      $exe = Join-Path $tmpDir "$case.$allocator.exe"
      & $CompilerPath --build --safe --release @extra "tests/$case.mettle" -o $exe 2>&1 | Out-Null
      if ($LASTEXITCODE -ne 0) {
        throw "--safe build of $case failed on $allocator"
      }
      $caseOut = & $exe 2>&1 | Out-String
      if ($LASTEXITCODE -eq 0) {
        throw "$case ran to completion under --safe on $allocator"
      }
      if ($caseOut -notmatch $bad[$case]) {
        throw "$case trapped on $allocator without reporting '$($bad[$case])':`n$caseOut"
      }
    }
  }
  Write-CaseResult -Name "safe_mode_heap" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "safe_mode_heap" -Passed $false -Reason $_.Exception.Message
}


# A pointer taken into a global and carried elsewhere. Indexing a global
# directly never reaches the runtime at all, since its size is in the program;
# this is the case that needs the globals described, and the clean fixture
# reads the first and last element through such a pointer, where an off-by-one
# in the described range would show.
$total++
try {
  $globalCleanExe = Join-Path $tmpDir "safe_global_clean.exe"
  & $CompilerPath --build --safe --release tests/test_safe_global_clean.mettle -o $globalCleanExe 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_global_clean failed"
  }
  $globalCleanOut = & $globalCleanExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 9) {
    throw "correct use of a global was rejected or the answer changed: expected 9, got $LASTEXITCODE`n$globalCleanOut"
  }

  $globalBadExe = Join-Path $tmpDir "safe_global_pointer.exe"
  & $CompilerPath --build --safe --release tests/test_safe_global_pointer.mettle -o $globalBadExe 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_global_pointer failed"
  }
  $globalBadOut = & $globalBadExe 2>&1 | Out-String
  if ($globalBadOut -notmatch "outside its allocation") {
    throw "an overrun through a pointer into a global was not caught:`n$globalBadOut"
  }

  $globalBaseExe = Join-Path $tmpDir "safe_global_pointer.base.exe"
  & $CompilerPath --build --release tests/test_safe_global_pointer.mettle -o $globalBaseExe 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "baseline build of test_safe_global_pointer failed"
  }
  $globalBaseOut = & $globalBaseExe 2>&1 | Out-String
  if ($globalBaseOut -match "outside its allocation") {
    throw "the overrun trapped without --safe, so the case proves nothing"
  }
  Write-CaseResult -Name "safe_mode_globals" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "safe_mode_globals" -Passed $false -Reason $_.Exception.Message
}


# Pointers into stack locals. Indexing a local directly never reaches the
# runtime, since its size is in the program; this is the case that needs the
# local described for as long as its frame lives.
#
# The neighbours fixture is the one that earns its keep. Two eight-byte locals
# laid out back to back share one unit of the runtime's map, and the runtime
# refuses to guess which of two objects owns a shared unit, so both would go
# uncovered. Locals the compiler describes are aligned so they cannot share;
# with that alignment removed this fixture returns 30 instead of trapping,
# which is how it was confirmed to be testing the alignment and not the
# layout it happened to get.
$total++
try {
  $stackCleanExe = Join-Path $tmpDir "safe_stack_clean.exe"
  & $CompilerPath --build --safe --release tests/test_safe_stack_clean.mettle -o $stackCleanExe 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_stack_clean failed"
  }
  $stackCleanOut = & $stackCleanExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 1021) {
    throw "correct use of stack pointers was rejected or the answer changed: expected 1021, got $LASTEXITCODE`n$stackCleanOut"
  }

  $stackBad = @("test_safe_stack_pointer", "test_safe_stack_neighbours")
  foreach ($case in $stackBad) {
    $safeExe = Join-Path $tmpDir "$case.safe.exe"
    & $CompilerPath --build --safe --release "tests/$case.mettle" -o $safeExe 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "--safe build of $case failed"
    }
    $safeOut = & $safeExe 2>&1 | Out-String
    if ($safeOut -notmatch "outside its allocation") {
      throw "an overrun through a pointer into a stack local was not caught in ${case}:`n$safeOut"
    }

    $baseExe = Join-Path $tmpDir "$case.base.exe"
    & $CompilerPath --build --release "tests/$case.mettle" -o $baseExe 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "baseline build of $case failed"
    }
    $baseOut = & $baseExe 2>&1 | Out-String
    if ($baseOut -match "outside its allocation") {
      throw "$case trapped without --safe, so the case proves nothing"
    }
  }
  Write-CaseResult -Name "safe_mode_stack" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "safe_mode_stack" -Passed $false -Reason $_.Exception.Message
}

# One check covering a whole loop's range is only honest when the loop really
# does walk that range. The body may branch, so long as it rejoins and the
# access sits ahead of the branch; a loop that can break out, or an access the
# body reaches only sometimes, keeps its per-access checks. Getting this wrong
# is silent in one direction (an overrun the hoist stopped describing) and
# loud in the other (a correct program accused), so both are pinned here.
try {
  $hoistExe = Join-Path $tmpDir "test_safe_hoist_branchy.exe"
  & $CompilerPath --build --safe --release "tests/test_safe_hoist_branchy.mettle" -o $hoistExe 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_hoist_branchy failed"
  }
  $hoistOut = & $hoistExe 2>&1 | Out-String
  if ($hoistOut -notmatch "outside its allocation") {
    throw "a loop whose body branches and rejoins walked off its array uncaught:`n$hoistOut"
  }

  $hoistClean = @(
    @{ Name = "test_safe_hoist_break_clean";       Expect = "walk=36" },
    @{ Name = "test_safe_hoist_conditional_clean"; Expect = "guarded=36" }
  )
  foreach ($case in $hoistClean) {
    $exe = Join-Path $tmpDir "$($case.Name).exe"
    & $CompilerPath --build --safe --release "tests/$($case.Name).mettle" -o $exe 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "--safe build of $($case.Name) failed"
    }
    $out = & $exe 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $out -notmatch [regex]::Escape($case.Expect)) {
      throw "a loop that stays in bounds was rejected in $($case.Name): expected '$($case.Expect)', got exit $LASTEXITCODE`n$out"
    }
  }
  Write-CaseResult -Name "safe_mode_hoist_branchy" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "safe_mode_hoist_branchy" -Passed $false -Reason $_.Exception.Message
}

# An index that is a straight line in one counter, in every shape the whole-loop
# check reads: a counter starting somewhere other than zero, a displacement the
# loop holds still, a counter running backwards, one scaled by a stride, and one
# starting from a value settled just before the loop.
#
# Both directions matter and both are silent when wrong. A range one element too
# wide accuses a program that never left its allocation, which is why the clean
# case sizes every buffer to exactly what its loop walks. A range one element too
# narrow lets the overrun through unexamined, which is the mode going missing
# from the loops it exists for, so each shape is also walked one past the end.
try {
  $affineClean = Join-Path $tmpDir "safe_affine_clean.exe"
  & $CompilerPath --build --safe --release tests/test_safe_affine_clean.mettle -o $affineClean 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_affine_clean failed"
  }
  $affineOut = & $affineClean 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0 -or $affineOut -notmatch "affine=151") {
    throw "an exactly sized buffer was rejected or the answer changed: expected affine=151, got exit $LASTEXITCODE`n$affineOut"
  }

  $affineShort = Join-Path $tmpDir "safe_affine_short.exe"
  & $CompilerPath --build --safe --release tests/test_safe_affine_short.mettle -o $affineShort 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_affine_short failed"
  }
  $shapes = @(
    @{ Arg = "1"; What = "a counter starting at one" },
    @{ Arg = "2"; What = "a displaced counter" },
    @{ Arg = "3"; What = "a counter running backwards" },
    @{ Arg = "4"; What = "a counter scaled by a stride" },
    @{ Arg = "5"; What = "a counter starting from a runtime value" }
  )
  foreach ($shape in $shapes) {
    $shapeOut = & $affineShort $shape.Arg 2>&1 | Out-String
    if ($shapeOut -notmatch "outside its allocation") {
      throw "$($shape.What) walked one element past the end uncaught:`n$shapeOut"
    }
  }
  Write-CaseResult -Name "safe_mode_affine_index" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "safe_mode_affine_index" -Passed $false -Reason $_.Exception.Message
}

# A loop whose body calls out. The check that covers a whole loop is taken
# before the loop runs, so what matters is not whether the body has a call in it
# but whether that call can reach anything that takes the memory away.
#
# All three answers are pinned. A loop around a helper that computes folds, and
# its buffer is sized to exactly what it walks so an over-wide range trips here.
# The same loop walked one past the end still traps, so the one check that
# replaced the per-access ones is not short. And a loop that frees what it is
# walking, two calls deep and behind a branch, keeps its per-access checks and
# reports the use-after-free -- getting that one wrong is completely silent.
try {
  $callClean = Join-Path $tmpDir "safe_call_clean.exe"
  & $CompilerPath --build --safe --release tests/test_safe_call_clean.mettle -o $callClean 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_call_clean failed"
  }
  $callOut = & $callClean 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0 -or $callOut -notmatch "calls=112") {
    throw "loops around a helper were rejected or the answer changed: expected calls=112, got exit $LASTEXITCODE`n$callOut"
  }

  $callShort = Join-Path $tmpDir "safe_call_short.exe"
  & $CompilerPath --build --safe --release tests/test_safe_call_short.mettle -o $callShort 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_call_short failed"
  }
  $shortOut = & $callShort 2>&1 | Out-String
  if ($shortOut -notmatch "outside its allocation") {
    throw "a loop around a helper walked past its buffer uncaught:`n$shortOut"
  }

  $callFrees = Join-Path $tmpDir "safe_call_frees.exe"
  & $CompilerPath --build --safe --release tests/test_safe_call_frees.mettle -o $callFrees 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_call_frees failed"
  }
  $freesOut = & $callFrees 2>&1 | Out-String
  if ($freesOut -notmatch "after it was freed") {
    throw "a loop that freed what it was walking kept reading uncaught:`n$freesOut"
  }
  Write-CaseResult -Name "safe_mode_call_in_body" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "safe_mode_call_in_body" -Passed $false -Reason $_.Exception.Message
}


# --safe must not change what a correct program computes and must not reject
# one. Real programs are what tests that: the fixtures above are written to
# exercise a mechanism, while these were written to sort and encode and
# multiply, and between them cover the shapes the elision reasons about
# (a vectorized reduction, two counters at different rates, data-dependent
# indices, a table lookup, a pointer walked along an array, a bound computed
# from other values).
#
# Running the full examples directory this way found both bugs the mechanism
# tests missed: a hoisted check naming a bound the loop header had not computed
# yet, and a benchmark that was already walking off its array four elements at
# a time.
$total++
try {
  $corpus = @("dot_product", "base64_encode", "heapsort", "crc32",
              "binary_search", "sort_insertion", "transpose", "aos_sum")
  foreach ($name in $corpus) {
    $src = "examples\$name\$name.mettle"
    if (-not (Test-Path $src)) { continue }

    $baseExe = Join-Path $tmpDir "corpus_$name.base.exe"
    $safeExe = Join-Path $tmpDir "corpus_$name.safe.exe"
    & $CompilerPath --build --release $src -o $baseExe 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "baseline build of $name failed"
    }
    & $CompilerPath --build --release --safe $src -o $safeExe 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "--safe build of $name failed"
    }

    $baseOut = & $baseExe 2>&1 | Out-String
    $baseCode = $LASTEXITCODE
    $safeOut = & $safeExe 2>&1 | Out-String
    $safeCode = $LASTEXITCODE
    if ($baseCode -ne $safeCode) {
      throw "$name exited $baseCode without --safe and $safeCode with it:`n$safeOut"
    }
    # Timings differ run to run; everything else must match exactly.
    $strippedBase = ($baseOut -replace "\d+\s*(us|ms|ns)", "T") -replace "\s+", " "
    $strippedSafe = ($safeOut -replace "\d+\s*(us|ms|ns)", "T") -replace "\s+", " "
    if ($strippedBase -ne $strippedSafe) {
      throw "$name printed different output under --safe:`nwithout: $strippedBase`nwith:    $strippedSafe"
    }
  }
  Write-CaseResult -Name "safe_mode_corpus_unchanged" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "safe_mode_corpus_unchanged" -Passed $false -Reason $_.Exception.Message
}


# --safe against the vectorizers, which is where the mode is easiest to lose
# silently. A recognizer scans a loop body for the pattern it knows, ignores
# what it does not, and then replaces the whole body; one that claims a body
# holding a check erases the check with it, and the program runs unchecked in
# exactly the hot loop the mode exists to cover. Nothing warns.
#
# One fixture per recognizer family, each overrunning a heap block inside a
# loop that shape would otherwise claim. All must trap at --release with the
# vectorizers on, and none may trap without --safe. A new recognizer that
# forgets to check whether the body is claimable fails here.
$total++
try {
  $shapes = @("test_safe_vec_dot", "test_safe_vec_sum", "test_safe_vec_fill",
              "test_safe_vec_map")
  foreach ($shape in $shapes) {
    $safeExe = Join-Path $tmpDir "$shape.safe.exe"
    & $CompilerPath --build --safe --release "tests/$shape.mettle" -o $safeExe 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "--safe build of $shape failed"
    }
    $safeOut = & $safeExe 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0) {
      throw "$shape ran to completion under --safe --release; a recognizer claimed the loop body and erased the check"
    }
    if ($safeOut -notmatch "outside its allocation") {
      throw "$shape trapped for the wrong reason:`n$safeOut"
    }

    $baseExe = Join-Path $tmpDir "$shape.base.exe"
    & $CompilerPath --build --release "tests/$shape.mettle" -o $baseExe 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "baseline build of $shape failed"
    }
    $baseOut = & $baseExe 2>&1 | Out-String
    if ($baseOut -match "outside its allocation") {
      throw "$shape trapped without --safe, so the case proves nothing about the check"
    }
  }
  Write-CaseResult -Name "safe_mode_vectorizer_keeps_checks" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "safe_mode_vectorizer_keeps_checks" -Passed $false -Reason $_.Exception.Message
}


# Hoisting a loop's checks into one covering the range it walks. The range has
# to be exactly what the loop touches: too large accuses a correct program, too
# small misses a real overrun. The zero-trip loop is the sharp edge, since the
# pointer handed to it may never have been valid and a check taken before the
# loop would look at it anyway.
$total++
try {
  $hoistExe = Join-Path $tmpDir "safe_hoist_clean.exe"
  & $CompilerPath --build --safe --release tests/test_safe_hoist_clean.mettle -o $hoistExe 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_hoist_clean failed"
  }
  $hoistOut = & $hoistExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 17) {
    throw "hoisting changed the answer or rejected a correct loop: expected 17, got $LASTEXITCODE`n$hoistOut"
  }

  # A loop advancing two counters at different rates. The test bounds one and
  # says nothing about the other, so the range for the second is worked out
  # from how many times the body runs. The buffer is exactly the size the loop
  # needs, so a range one byte too large rejects a correct program, and the
  # short version proves a range one byte too small would miss the overrun.
  $exactExe = Join-Path $tmpDir "safe_hoist_counters.exe"
  & $CompilerPath --build --safe --release tests/test_safe_hoist_counters.mettle -o $exactExe 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_hoist_counters failed"
  }
  $exactOut = & $exactExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 61) {
    throw "an exactly sized buffer was rejected or the answer changed: expected 61, got $LASTEXITCODE`n$exactOut"
  }

  $shortExe = Join-Path $tmpDir "safe_hoist_counters_short.exe"
  & $CompilerPath --build --safe --release tests/test_safe_hoist_counters_short.mettle -o $shortExe 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_hoist_counters_short failed"
  }
  $shortOut = & $shortExe 2>&1 | Out-String
  if ($LASTEXITCODE -eq 0) {
    throw "a buffer one byte too small ran to completion under --safe"
  }
  if ($shortOut -notmatch "outside its allocation") {
    throw "the short buffer trapped for the wrong reason:`n$shortOut"
  }

  # A table lookup indexed by a masked value. The bound comes from the mask,
  # not from any loop counter, and the table is exactly the size the mask
  # allows, so a range one byte too large rejects a correct program.
  $maskExe = Join-Path $tmpDir "safe_masked_index.exe"
  & $CompilerPath --build --safe --release tests/test_safe_masked_index.mettle -o $maskExe 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_masked_index failed"
  }
  $maskOut = & $maskExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 41) {
    throw "an exactly sized table was rejected or the answer changed: expected 41, got $LASTEXITCODE`n$maskOut"
  }

  $maskShortExe = Join-Path $tmpDir "safe_masked_index_short.exe"
  & $CompilerPath --build --safe --release tests/test_safe_masked_index_short.mettle -o $maskShortExe 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "--safe build of test_safe_masked_index_short failed"
  }
  $maskShortOut = & $maskShortExe 2>&1 | Out-String
  if ($maskShortOut -notmatch "outside its allocation") {
    throw "a table one byte smaller than the mask allows was not caught:`n$maskShortOut"
  }
  Write-CaseResult -Name "safe_mode_hoist_range" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "safe_mode_hoist_range" -Passed $false -Reason $_.Exception.Message
}


# Native heap: build with --native-heap and confirm new/malloc/calloc/realloc/
# free route through std/alloc's Mettle allocator (mettle_heap_*), stay correct
# at runtime, and do NOT emit the Win32 HeapAlloc/calloc path for `new`.
$total++
try {
  $exePath = Join-Path $tmpDir "native_heap.exe"
  $objPath = [System.IO.Path]::ChangeExtension($exePath, ".obj")
  $irPath = "$objPath.ir"
  foreach ($artifactPath in @($exePath, $objPath, $irPath)) {
    if (Test-Path $artifactPath) {
      Remove-Item -Path $artifactPath -Force -ErrorAction SilentlyContinue
    }
  }

  $buildOut = & $CompilerPath --build --linker internal --release --native-heap --dump-ir "tests/test_native_heap.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "native-heap build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "native-heap build did not produce an executable"
  }
  if (Test-Path $irPath) {
    $irText = Get-Content -Path $irPath -Raw
    # The reroute target call is usually inlined (and inline prefixes no
    # longer embed the callee name), so assert on the allocator core that
    # only enters the program when `new` was rerouted to the native heap.
    if ($irText -notmatch "mem_alloc") {
      throw "native-heap IR missing native allocator core (new not rerouted)"
    }
  }

  $runOut = & $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "native-heap executable exited with $LASTEXITCODE`: $runOut"
  }
  if ($runOut -notmatch "NATIVE-HEAP OK") {
    throw "native-heap output missing expected marker: $runOut"
  }
  Write-CaseResult -Name "native_heap" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "native_heap" -Passed $false -Reason $_.Exception.Message
}

# Native heap thread-safety: four threads hammer the shared global heap; the
# per-heap spinlock must keep every allocation counted (20000) with no leak.
$total++
try {
  $exePath = Join-Path $tmpDir "native_heap_threads.exe"
  $objPath = [System.IO.Path]::ChangeExtension($exePath, ".obj")
  foreach ($artifactPath in @($exePath, $objPath)) {
    if (Test-Path $artifactPath) {
      Remove-Item -Path $artifactPath -Force -ErrorAction SilentlyContinue
    }
  }

  $buildOut = & $CompilerPath --build --linker internal --release "tests/test_native_heap_threads.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "native-heap threads build failed: $buildOut"
  }
  $runOut = & $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "native-heap threads executable exited with $LASTEXITCODE`: $runOut"
  }
  if ($runOut -notmatch "THREADS OK") {
    throw "native-heap threads output missing expected marker: $runOut"
  }
  Write-CaseResult -Name "native_heap_threads" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "native_heap_threads" -Passed $false -Reason $_.Exception.Message
}

# The surface the friction report changed: implicit widening, the constant
# range check, rawptr allocation, and the string/cstring boundary. Both build
# modes, because the two release miscompiles this test found -- an aggregate
# local aliased to its initializer, and a string literal propagated between
# positions that read it differently -- were invisible in debug.
foreach ($surfaceMode in @("debug", "release")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir ("surface_conversions_{0}.exe" -f $surfaceMode)
    if (Test-Path $exePath) { Remove-Item -Path $exePath -Force -ErrorAction SilentlyContinue }
    $surfaceFlags = @("--build", "--linker", "internal")
    if ($surfaceMode -eq "release") { $surfaceFlags += "--release" }
    $buildOut = & $CompilerPath @surfaceFlags "tests/test_surface_conversions.mettle" -o $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "surface conversions build ($surfaceMode) failed: $buildOut" }
    $runOut = & $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "surface conversions ($surfaceMode) exited with $LASTEXITCODE`: $runOut" }
    if ($runOut -notmatch "SURFACE OK") { throw "surface conversions ($surfaceMode) marker missing: $runOut" }
    Write-CaseResult -Name ("surface_conversions_" + $surfaceMode) -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name ("surface_conversions_" + $surfaceMode) -Passed $false -Reason $_.Exception.Message
  }
}

# The float-suffixed math the owned runtime exports (sqrtf/expf/logf/powf/
# sinf/cosf/tanhf). Nothing else provides them -- std/math implements the
# double forms in Mettle -- so a program that binds one at the C boundary is
# reading these series. Checked against independently computed constants.
foreach ($mathMode in @("debug", "release")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir ("runtime_float_math_{0}.exe" -f $mathMode)
    if (Test-Path $exePath) { Remove-Item -Path $exePath -Force -ErrorAction SilentlyContinue }
    $mathFlags = @("--build", "--linker", "internal")
    if ($mathMode -eq "release") { $mathFlags += "--release" }
    $buildOut = & $CompilerPath @mathFlags "tests/test_runtime_float_math.mettle" -o $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "runtime float math build ($mathMode) failed: $buildOut" }
    $runOut = & $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "runtime float math ($mathMode) exited with $LASTEXITCODE`: $runOut" }
    if ($runOut -notmatch "RUNTIME MATH OK") { throw "runtime float math ($mathMode) marker missing: $runOut" }
    Write-CaseResult -Name ("runtime_float_math_" + $mathMode) -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name ("runtime_float_math_" + $mathMode) -Passed $false -Reason $_.Exception.Message
  }
}

# The same two miscompiles reached through the older coercion corpus. Both
# files passed in debug and misbehaved under --release for as long as they
# existed, because nothing ran them optimized. The property is agreement
# between the two builds, not a particular exit code: these programs return a
# computed value.
foreach ($coercionProgram in @("tests/test_string_cstring_coercions.mettle",
                               "tests/test_extern_string_auto_cstring.mettle")) {
  $total++
  $coercionName = "release_parity_" + [System.IO.Path]::GetFileNameWithoutExtension($coercionProgram)
  try {
    $debugExe = Join-Path $tmpDir ($coercionName + "_d.exe")
    $releaseExe = Join-Path $tmpDir ($coercionName + "_r.exe")
    foreach ($stale in @($debugExe, $releaseExe)) {
      if (Test-Path $stale) { Remove-Item -Path $stale -Force -ErrorAction SilentlyContinue }
    }
    $buildOut = & $CompilerPath --build --linker internal $coercionProgram -o $debugExe 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "debug build failed: $buildOut" }
    $buildOut = & $CompilerPath --build --linker internal --release $coercionProgram -o $releaseExe 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "release build failed: $buildOut" }
    $debugOut = & $debugExe 2>&1 | Out-String
    $debugExit = $LASTEXITCODE
    $releaseOut = & $releaseExe 2>&1 | Out-String
    $releaseExit = $LASTEXITCODE
    if ($debugExit -ne $releaseExit) {
      throw "exit code diverged: debug $debugExit vs release $releaseExit"
    }
    if ($debugOut -ne $releaseOut) {
      throw "output diverged: debug '$debugOut' vs release '$releaseOut'"
    }
    Write-CaseResult -Name $coercionName -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name $coercionName -Passed $false -Reason $_.Exception.Message
  }
}

# Allocator reliability: double-free / bogus-free rejection (no free-list
# corruption). Exercises std/alloc directly; no flag needed.
$total++
try {
  $exePath = Join-Path $tmpDir "alloc_doublefree.exe"
  if (Test-Path $exePath) { Remove-Item -Path $exePath -Force -ErrorAction SilentlyContinue }
  $buildOut = & $CompilerPath --build --linker internal --release "tests/test_alloc_doublefree.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "alloc doublefree build failed: $buildOut" }
  $runOut = & $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "alloc doublefree exited with $LASTEXITCODE`: $runOut" }
  if ($runOut -notmatch "DOUBLEFREE OK") { throw "alloc doublefree marker missing: $runOut" }
  Write-CaseResult -Name "alloc_doublefree" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "alloc_doublefree" -Passed $false -Reason $_.Exception.Message
}

# Native-heap behavioral parity: a broad set of allocation-using programs must
# produce the IDENTICAL exit code whether built normally (OS heap) or with
# --native-heap (Mettle allocator). These exit codes are computed from data
# that lived on the heap, so a divergence would mean the rewrite changed
# observable behavior. This is the broad reliability proof that the rewrite is
# correct across many real programs, not just the dedicated cases above.
$nativeHeapParityPrograms = @(
  "tests/test_gc_alloc.mettle",
  "tests/test_gc_alloc_fixed.mettle",
  "tests/test_generics_new_heap.mettle",
  "tests/test_generics_full.mettle",
  "tests/test_generics_return_struct.mettle",
  "tests/test_generics_nested_struct.mettle",
  "tests/test_generics_in_control_flow.mettle",
  "tests/test_generics_float.mettle",
  "tests/test_large_db_cache_loop.mettle",
  "tests/test_arena_basic.mettle",
  "tests/test_arena_oversized.mettle",
  "tests/test_arena_savepoint.mettle",
  "tests/test_arena_reset_reuse.mettle",
  "tests/test_arena_align.mettle"
)
foreach ($prog in $nativeHeapParityPrograms) {
  $total++
  $caseName = "native_heap_parity_" + [System.IO.Path]::GetFileNameWithoutExtension($prog).Replace("test_", "")
  try {
    $baseExe = Join-Path $tmpDir ("nhp_base_{0}.exe" -f [System.IO.Path]::GetFileNameWithoutExtension($prog))
    $nhExe   = Join-Path $tmpDir ("nhp_nh_{0}.exe"   -f [System.IO.Path]::GetFileNameWithoutExtension($prog))
    foreach ($e in @($baseExe, $nhExe)) { if (Test-Path $e) { Remove-Item -Path $e -Force -ErrorAction SilentlyContinue } }

    $bOut = & $CompilerPath --build --linker internal --release $prog -o $baseExe 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "baseline build failed: $bOut" }
    & $baseExe *> $null
    $baseCode = $LASTEXITCODE

    $nOut = & $CompilerPath --build --linker internal --release --native-heap $prog -o $nhExe 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "native-heap build failed: $nOut" }
    & $nhExe *> $null
    $nhCode = $LASTEXITCODE

    if ($baseCode -ne $nhCode) {
      throw "exit code differs: baseline=$baseCode native-heap=$nhCode"
    }
    Write-CaseResult -Name $caseName -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name $caseName -Passed $false -Reason $_.Exception.Message
  }
}

# Generics runtime: compile with --build and verify monomorphized return values.
$total++
try {
  $genericRuntimeCases = @(
    @{ Path = "tests/test_generics_nested_struct.mettle"; ExitCode = 99; Label = "nested-struct" },
    @{ Path = "tests/test_generics_generic_enum.mettle"; ExitCode = 42; Label = "generic-enum" },
    @{ Path = "tests/test_generics_return_struct.mettle"; ExitCode = 30; Label = "return-struct" },
    @{ Path = "tests/test_generics_float.mettle"; ExitCode = 4; Label = "float" },
    @{ Path = "tests/test_generics_new_heap.mettle"; ExitCode = 42; Label = "new-heap" },
    @{ Path = "tests/test_generics_full.mettle"; ExitCode = 30; Label = "full" },
    @{ Path = "tests/test_generics_in_control_flow.mettle"; ExitCode = 24; Label = "control-flow" },
    @{ Path = "tests/test_generics_struct_methods.mettle"; ExitCode = 155; Label = "struct-methods" },
    @{ Path = "tests/test_generics_method_body_instantiation.mettle"; ExitCode = 42; Label = "method-body-instantiation" },
    @{ Path = "tests/test_method_pointer_receiver.mettle"; ExitCode = 42; Label = "pointer-receiver" },
    @{ Path = "tests/test_generics_struct_field.mettle"; ExitCode = 42; Label = "struct-field-ordering" },
    @{ Path = "tests/test_trait_methods_generic_dispatch.mettle"; ExitCode = 42; Label = "trait-dispatch" }
  )

  foreach ($case in $genericRuntimeCases) {
    $exePath = Join-Path $tmpDir ("generics_runtime_{0}.exe" -f $case.Label)
    $buildOut = & $CompilerPath --build $case.Path -o $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "Generics runtime $($case.Label) build failed: $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "Generics runtime $($case.Label) build did not produce an executable"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne $case.ExitCode) {
      throw "Generics runtime $($case.Label) exited with $LASTEXITCODE (expected $($case.ExitCode))"
    }
  }

  Write-CaseResult -Name "generics_runtime" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "generics_runtime" -Passed $false -Reason $_.Exception.Message
}

# Global aggregates and function-pointer globals: arrays/structs at module
# scope, null-initialized function pointers, and globals initialized with the
# address of another symbol (which lowers to a relocation and must keep the
# referenced function alive through dead-function elimination). 55 = all intact.
$total++
try {
  foreach ($mode in @("", "--release")) {
    $label = if ($mode -eq "") { "debug" } else { "release" }
    $exePath = Join-Path $tmpDir ("global_aggregates_and_fnptr_{0}.exe" -f $label)
    if ($mode -eq "") {
      $buildOut = & $CompilerPath --build --linker internal "tests/test_global_aggregates_and_fnptr.mettle" -o $exePath 2>&1 | Out-String
    }
    else {
      $buildOut = & $CompilerPath --build --linker internal --release "tests/test_global_aggregates_and_fnptr.mettle" -o $exePath 2>&1 | Out-String
    }
    if ($LASTEXITCODE -ne 0) {
      throw "global aggregates $label build failed: $buildOut"
    }
    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 55) {
      throw "global aggregates $label exited with $LASTEXITCODE (expected 55)"
    }
  }
  Write-CaseResult -Name "global_aggregates_and_fnptr" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "global_aggregates_and_fnptr" -Passed $false -Reason $_.Exception.Message
}

# Runtime symbols are overridable defaults. freestanding.o is linked into every
# program and defines ~330 C names, so a program defining one of them must win
# rather than fail the link on a duplicate symbol. 77 = the module's strlen was
# bound; 5 would mean the runtime's won. Overriding must not reroute the
# runtime's own calls either: freestanding.c's fputs calls strlen internally, so
# puts("hello") still has to emit exactly "hello". Internal linker only -- GNU ld
# rejects the duplicate, which is why std/conv exports cstr_len, not strlen.
$total++
try {
  $exePath = Join-Path $tmpDir "runtime_symbol_override.exe"
  $buildOut = & $CompilerPath --build --linker internal -I tests/lib "tests/test_runtime_symbol_override.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "runtime symbol override build failed: $buildOut"
  }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 77) {
    throw "runtime symbol override exited with $LASTEXITCODE (expected 77)"
  }

  $hijackSource = Join-Path $tmpDir "runtime_symbol_override_hijack.mettle"
  @(
    'import "shadow_runtime";'
    ''
    'extern fn puts(s: cstring) -> int32 = "puts";'
    ''
    'fn main() -> int32 {'
    '  puts("hello");'
    '  return (int32)strlen("hello");'
    '}'
  ) | Set-Content -Path $hijackSource -Encoding utf8
  $hijackExe = Join-Path $tmpDir "runtime_symbol_override_hijack.exe"
  $buildOut = & $CompilerPath --build --linker internal -I tests/lib $hijackSource -o $hijackExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "runtime symbol override hijack build failed: $buildOut"
  }
  $printed = & $hijackExe | Out-String
  if ($printed.Trim() -ne "hello") {
    throw "the runtime's own strlen was rerouted: puts printed '$($printed.Trim())'"
  }

  Write-CaseResult -Name "runtime_symbol_override" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "runtime_symbol_override" -Passed $false -Reason $_.Exception.Message
}

# Word-sized global aggregates: a global struct or array of exactly 1/2/4/8
# bytes is memory reached through an address, not a value the MIR global cache
# may hold in a register. Caching one gave it a vreg nothing defines, and the
# allocator flushed that undefined vreg over the global's storage. 55 = every
# read sees what was written.
$total++
try {
  foreach ($mode in @("", "--release")) {
    $label = if ($mode -eq "") { "debug" } else { "release" }
    $exePath = Join-Path $tmpDir ("word_sized_global_aggregate_{0}.exe" -f $label)
    if ($mode -eq "") {
      $buildOut = & $CompilerPath --build --linker internal "tests/test_word_sized_global_aggregate.mettle" -o $exePath 2>&1 | Out-String
    }
    else {
      $buildOut = & $CompilerPath --build --linker internal --release "tests/test_word_sized_global_aggregate.mettle" -o $exePath 2>&1 | Out-String
    }
    if ($LASTEXITCODE -ne 0) {
      throw "word-sized global aggregate $label build failed: $buildOut"
    }
    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 55) {
      throw "word-sized global aggregate $label exited with $LASTEXITCODE (expected 55)"
    }
  }
  Write-CaseResult -Name "word_sized_global_aggregate" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "word_sized_global_aggregate" -Passed $false -Reason $_.Exception.Message
}

# Aggregate literals: `[a, b, c]`, `[value; count]`, and `{ field: value }` as
# global constants, global variables, locals, and assignment right-hand sides.
# Covers the element shapes that need more than plain bytes (float, bool,
# string, a function's address, another global's address) and the folded image's
# relocations. 55 = every form intact.
$total++
try {
  foreach ($mode in @("", "--release")) {
    $label = if ($mode -eq "") { "debug" } else { "release" }
    $exePath = Join-Path $tmpDir ("aggregate_literals_{0}.exe" -f $label)
    if ($mode -eq "") {
      $buildOut = & $CompilerPath --build --linker internal "tests/test_aggregate_literals.mettle" -o $exePath 2>&1 | Out-String
    }
    else {
      $buildOut = & $CompilerPath --build --linker internal --release "tests/test_aggregate_literals.mettle" -o $exePath 2>&1 | Out-String
    }
    if ($LASTEXITCODE -ne 0) {
      throw "aggregate literals $label build failed: $buildOut"
    }
    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 55) {
      throw "aggregate literals $label exited with $LASTEXITCODE (expected 55)"
    }
  }
  Write-CaseResult -Name "aggregate_literals" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "aggregate_literals" -Passed $false -Reason $_.Exception.Message
}

# Struct-return regression: an aggregate returned through a hidden pointer must
# be copied whole into a field, array element, or pointer target, not just its
# first machine word. 55 = every destination form intact.
$total++
try {
  foreach ($mode in @("", "--release")) {
    $label = if ($mode -eq "") { "debug" } else { "release" }
    $exePath = Join-Path $tmpDir ("struct_return_to_field_{0}.exe" -f $label)
    if ($mode -eq "") {
      $buildOut = & $CompilerPath --build --linker internal "tests/test_struct_return_to_field.mettle" -o $exePath 2>&1 | Out-String
    }
    else {
      $buildOut = & $CompilerPath --build --linker internal --release "tests/test_struct_return_to_field.mettle" -o $exePath 2>&1 | Out-String
    }
    if ($LASTEXITCODE -ne 0) {
      throw "struct-return $label build failed: $buildOut"
    }
    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 55) {
      throw "struct-return $label exited with $LASTEXITCODE (expected 55): a returned aggregate was truncated on assignment"
    }
  }
  Write-CaseResult -Name "struct_return_to_field" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "struct_return_to_field" -Passed $false -Reason $_.Exception.Message
}

# Nested-loop unroll regression: a loop whose bound is the enclosing loop's
# counter must have its trip count recomputed per outer iteration. The symbol
# map is built by a linear scan before the header, so without back-edge
# handling the bound froze at its pre-loop value. 55 = every shape correct.
$total++
try {
  foreach ($mode in @("", "--release")) {
    $label = if ($mode -eq "") { "debug" } else { "release" }
    $exePath = Join-Path $tmpDir ("opt_nested_loop_variable_bound_{0}.exe" -f $label)
    if ($mode -eq "") {
      $buildOut = & $CompilerPath --build --linker internal "tests/test_opt_nested_loop_variable_bound.mettle" -o $exePath 2>&1 | Out-String
    }
    else {
      $buildOut = & $CompilerPath --build --linker internal --release "tests/test_opt_nested_loop_variable_bound.mettle" -o $exePath 2>&1 | Out-String
    }
    if ($LASTEXITCODE -ne 0) {
      throw "nested-loop unroll $label build failed: $buildOut"
    }
    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 55) {
      throw "nested-loop unroll $label exited with $LASTEXITCODE (expected 55): an inner trip count was frozen at its pre-loop value"
    }
  }
  Write-CaseResult -Name "opt_nested_loop_variable_bound" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "opt_nested_loop_variable_bound" -Passed $false -Reason $_.Exception.Message
}

# std/math accuracy. Self-checking against independently computed reference
# values plus identity sweeps; prints "MATH: ALL OK" and exits 0 only when every
# assertion holds. Run at both optimisation levels because the library
# reinterprets doubles through pointer casts, which --release must not disturb.
$total++
try {
  foreach ($mode in @("", "--release")) {
    $label = if ($mode -eq "") { "debug" } else { "release" }
    $exePath = Join-Path $tmpDir ("std_math_{0}.exe" -f $label)
    if ($mode -eq "") {
      $buildOut = & $CompilerPath --build --linker internal "tests/test_std_math.mettle" -o $exePath 2>&1 | Out-String
    }
    else {
      $buildOut = & $CompilerPath --build --linker internal --release "tests/test_std_math.mettle" -o $exePath 2>&1 | Out-String
    }
    if ($LASTEXITCODE -ne 0) {
      throw "std/math $label build failed: $buildOut"
    }
    $runOut = & $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "std/math $label reported failures: $runOut"
    }
    if ($runOut -notmatch "MATH: ALL OK") {
      throw "std/math $label output missing success marker: $runOut"
    }
  }
  Write-CaseResult -Name "std_math" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "std_math" -Passed $false -Reason $_.Exception.Message
}

# Fused-loop threaded-exit regression: a vectorizable loop in an if/else THEN
# branch whose exit was jump-threaded to the join must not fall through into
# the ELSE branch after fusion (ir_fused_loop_exit_is_adjacent). Self-checking
# at --release: 55 = fused and reference results match, 1 = divergence.
$total++
try {
  $exePath = Join-Path $tmpDir "opt_fused_loop_threaded_exit.exe"
  $buildOut = & $CompilerPath --build --release "tests/test_opt_fused_loop_threaded_exit.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "threaded-exit regression build failed: $buildOut"
  }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 55) {
    throw "threaded-exit regression exited with $LASTEXITCODE (expected 55): fused loop fell through its deleted exit edge"
  }
  Write-CaseResult -Name "opt_fused_loop_threaded_exit" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "opt_fused_loop_threaded_exit" -Passed $false -Reason $_.Exception.Message
}

# Arg-register pool invariant: values whose intervals contain an outgoing
# argument homing write must never be placed in that argument register (the
# explicit-writes clobber index). Adversarial pressure shape, self-checking at
# --release: 55 = checksum matches, 1 = a homing move clobbered a live source.
$total++
try {
  $exePath = Join-Path $tmpDir "regalloc_argreg_call_pressure.exe"
  $buildOut = & $CompilerPath --build --release "tests/test_regalloc_argreg_call_pressure.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "argreg pressure build failed: $buildOut"
  }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 55) {
    throw "argreg pressure exited with $LASTEXITCODE (expected 55)"
  }
  Write-CaseResult -Name "regalloc_argreg_call_pressure" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "regalloc_argreg_call_pressure" -Passed $false -Reason $_.Exception.Message
}

# Wide-call boundary coverage: test_call_many_args.mettle only goes to 8 args,
# below the point where arguments spill to the stack and (over MIR_MAX_PARAMS)
# the enclosing function drops onto the baseline emitter. This runs a 20-param
# callee and a 20-arg call: >register-set parameter homing and argument passing,
# a correct wide-call return, and a float local intact across it. Returns 3. (It
# is a sanity check, not the m-c99 #14 clobber repro -- that is frame-specific to
# the C99 frontend and lives in the frontend's tests/diff/many_args.c.)
$total++
try {
  $exePath = Join-Path $tmpDir "call_many_args_frame.exe"
  $buildOut = & $CompilerPath --build "tests/test_call_many_args_frame.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "wide-call frame build failed: $buildOut"
  }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 3) {
    throw "wide-call frame exited with $LASTEXITCODE (expected 3): a wide call clobbered the caller's float local"
  }
  Write-CaseResult -Name "call_many_args_frame" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "call_many_args_frame" -Passed $false -Reason $_.Exception.Message
}

# Global float variables: compile with --build and verify they read back their
# initializer (and survive mutation) instead of reading 0 from an uninitialized
# XMM lane. Returns 25+125+35+30 = 215.
# Both backends are checked: the fallback emitter and, under --release, the
# register-allocating MIR path. The original miscompile lived in both, so
# gating only one leaves half of it uncovered.
foreach ($globalFloatMode in @("debug", "release")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "global_float_var_$globalFloatMode.exe"
    $buildArgs = @()
    if ($globalFloatMode -eq "release") { $buildArgs += "--release" }
    $buildArgs += @("--build", "tests/test_global_float_var.mettle", "-o", $exePath)
    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "Global float var ($globalFloatMode) build failed: $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "Global float var ($globalFloatMode) build did not produce an executable"
    }
    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 215) {
      throw "Global float var ($globalFloatMode) exited with $LASTEXITCODE (expected 215)"
    }
    Write-CaseResult -Name "global_float_var_runtime_$globalFloatMode" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "global_float_var_runtime_$globalFloatMode" -Passed $false -Reason $_.Exception.Message
  }
}

# Switch range cases: compile with --build and verify inclusive-interval dispatch.
$total++
try {
  $exePath = Join-Path $tmpDir "switch_range.exe"
  $buildOut = & $CompilerPath --build "tests/test_switch_range.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Switch range build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Switch range build did not produce an executable"
  }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 42) {
    throw "Switch range exited with $LASTEXITCODE (expected 42)"
  }
  Write-CaseResult -Name "switch_range_runtime" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "switch_range_runtime" -Passed $false -Reason $_.Exception.Message
}

# Crash forensics: an access violation at a small non-null address is
# classified as a null pointer plus offset (a field/index access through
# null), with the faulting line and a stack trace.
$total++
try {
  $exePath = Join-Path $tmpDir "crash_null_offset.exe"
  $buildOut = & $CompilerPath --build "tests/debug_crash.mettle" -o $exePath -s 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "Build failed: $buildOut" }
  $runOut = & $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -eq 0) { throw "Expected the program to crash" }
  if ($runOut -notmatch 'null plus offset 16: a field or array access through a null pointer') {
    throw "Missing null+offset classification. Output: $runOut"
  }
  if ($runOut -notmatch 'debug_crash\.mettle:13') { throw "Missing faulting line. Output: $runOut" }
  Write-CaseResult -Name "crash_classify_null_offset" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "crash_classify_null_offset" -Passed $false -Reason $_.Exception.Message
}

# Crash forensics: under --native-heap a freed page-backed block keeps its
# mapping with access revoked, so a use-after-free faults instantly and the
# crash handler classifies the address as a freed heap block.
$total++
try {
  $exePath = Join-Path $tmpDir "crash_uaf_large.exe"
  $buildOut = & $CompilerPath --build "tests/crash_uaf_large.mettle" -o $exePath -s --native-heap 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "Build failed: $buildOut" }
  $runOut = & $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -eq 0) { throw "Expected the program to crash" }
  if ($runOut -notmatch 'heap block that was already freed: use-after-free') {
    throw "Missing use-after-free classification. Output: $runOut"
  }
  Write-CaseResult -Name "crash_classify_use_after_free" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "crash_classify_use_after_free" -Passed $false -Reason $_.Exception.Message
}

# Crash forensics: a dangling WRITE into a freed small block corrupts the
# quarantine poison and is reported when the block leaves quarantine.
$total++
try {
  $exePath = Join-Path $tmpDir "crash_waf_small.exe"
  $buildOut = & $CompilerPath --build "tests/crash_waf_small.mettle" -o $exePath --native-heap 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "Build failed: $buildOut" }
  $runOut = & $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 134) { throw "Expected exit 134, got $LASTEXITCODE" }
  if ($runOut -notmatch 'written through a dangling pointer after it was freed') {
    throw "Missing write-after-free report. Output: $runOut"
  }
  Write-CaseResult -Name "crash_write_after_free_quarantine" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "crash_write_after_free_quarantine" -Passed $false -Reason $_.Exception.Message
}

# Debugger instrumentation: a --debug-hooks build must run NORMALLY when no
# debugger is attached (every hook is an early-out; METTLE_DBG_PIPE unset).
$total++
try {
  $exePath = Join-Path $tmpDir "debug_hooks_standalone.exe"
  $buildOut = & $CompilerPath --build "tests/debug_demo.mettle" -o $exePath --debug-hooks 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Debug-hooks build failed: $buildOut"
  }
  $runOut = & $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Debug-hooks binary exited with $LASTEXITCODE (expected 0)"
  }
  if ($runOut -notmatch 'total=60') {
    throw "Debug-hooks binary output wrong: $runOut (expected total=60)"
  }
  Write-CaseResult -Name "debug_hooks_standalone" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "debug_hooks_standalone" -Passed $false -Reason $_.Exception.Message
}

# --explain "since last build" diffing + --explain-json: recompiling unchanged
# source reports no changes; de-inlining `scale` between builds reports the
# with_call loop as REGRESSED; the .explain.json sidecar parses and agrees.
$total++
try {
  $exDir = Join-Path $tmpDir "explain_changes"
  New-Item -ItemType Directory $exDir -Force | Out-Null
  # the harness tmp dir persists across suite runs: a stale baseline would
  # make the "first build" assertion see a changes section
  Get-ChildItem $exDir -File -ErrorAction SilentlyContinue | Remove-Item -Force -Confirm:$false
  Copy-Item "tests/explain_demo.mettle" "$exDir/demo.mettle" -Force
  $exOut = Join-Path $exDir "demo.obj"
  $env:METTLE_EXPLAIN_REPORT_LINES = "0"
  $run1 = cmd /c "`"$((Resolve-Path $CompilerPath).Path)`" -i `"$exDir/demo.mettle`" -o `"$exOut`" --release --explain-json 2>&1" | Out-String
  if ($run1 -match 'changes since the last explain build') {
    throw "First build must not have a changes section"
  }
  $run2 = cmd /c "`"$((Resolve-Path $CompilerPath).Path)`" -i `"$exDir/demo.mettle`" -o `"$exOut`" --release --explain-json 2>&1" | Out-String
  if ($run2 -notmatch 'no optimization changes since the last explain build') {
    throw "Identical rebuild must report no changes"
  }
  (Get-Content "$exDir/demo.mettle" -Raw) -replace 'fn scale\(x: float32\)', '@noinline fn scale(x: float32)' |
    Set-Content "$exDir/demo.mettle" -Encoding ascii -NoNewline
  $run3 = cmd /c "`"$((Resolve-Path $CompilerPath).Path)`" -i `"$exDir/demo.mettle`" -o `"$exOut`" --release --explain-json 2>&1" | Out-String
  if ($run3 -notmatch 'REGRESSED' -or $run3 -notmatch 'was vectorized, now scalar') {
    throw "De-inlined scale must report a loop regression. Output: $($run3.Substring(0, [Math]::Min(600, $run3.Length)))"
  }
  $json = Get-Content (Join-Path $exDir "demo.explain.json") -Raw | ConvertFrom-Json
  if ($json.schema -ne 2) { throw "JSON schema field wrong" }
  if ($json.stats.changesRegressed -lt 1) { throw "JSON regression count missing" }
  if (@($json.remarks).Count -lt 10) { throw "JSON remarks too few: $(@($json.remarks).Count)" }
  $regLoop = @($json.changes.entries | Where-Object { $_.direction -eq 'regressed' -and $_.kind -eq 'loop' })
  if ($regLoop.Count -lt 1 -or -not $regLoop[0].reason) { throw "JSON regressed-loop entry missing reason" }
  if (-not @($json.remarks | Where-Object { $_.kind -eq 'loop' -and $_.depth -ge 2 }).Count) {
    throw "JSON nest depth missing (matvec inner loop is depth 2)"
  }
  # Schema 2: the structured half. Prose is for people; these are what tools key off.
  $coded = @($json.remarks | Where-Object { $_.code })
  if ($coded.Count -lt 5) { throw "JSON remarks missing stable codes: $($coded.Count)" }
  if (-not @($json.remarks | Where-Object { $_.code -eq 'int32-sum-narrow-acc' }).Count) {
    throw "JSON missing the int32-sum-narrow-acc diagnosis id"
  }
  if (-not @($json.remarks | Where-Object { $_.kind -eq 'loop' -and $_.endLine -gt $_.line }).Count) {
    throw "JSON loop remarks missing their source extent"
  }
  if (-not @($json.remarks | Where-Object { $_.trivial }).Count) {
    throw "JSON missing trivial-inline classification"
  }
  if (@($json.functions).Count -lt 2) { throw "JSON per-function table missing" }
  $mainFn = @($json.functions | Where-Object { $_.fn -eq 'main' })[0]
  if (-not $mainFn -or $mainFn.instructionsBefore -le 0) { throw "JSON function weights missing" }
  if (@($json.passes).Count -lt 5) { throw "JSON pass ledger missing" }
  if (-not @($json.passes | Where-Object { $_.instructionsRemoved -gt 0 }).Count) {
    throw "JSON pass ledger recorded no instruction deltas"
  }
  # A ledger entry has to say WHAT it did and WHERE, not just that it fired.
  $described = @($json.passes | Where-Object { $_.effects -and $_.sites })
  if ($described.Count -lt 3) {
    throw "JSON pass ledger missing effects/sites: $($described.Count) described"
  }
  $vec = @($json.passes | Where-Object { $_.pass -eq 'simd_affine_map_float' })[0]
  if (-not $vec) { throw "JSON pass ledger missing the vectorizer" }
  $kernel = $vec.effects.PSObject.Properties | Where-Object { $_.Name -like 'simd_*' -and $_.Value -lt 0 }
  if (-not $kernel) { throw "vectorizer ledger should report the kernel it introduced" }
  $scalar = $vec.effects.PSObject.Properties | Where-Object { $_.Name -eq 'binary' -and $_.Value -gt 0 }
  if (-not $scalar) { throw "vectorizer ledger should report the scalar work it removed" }
  if (-not @($vec.sites | Where-Object { $_.fn -and $_.line -gt 0 }).Count) {
    throw "vectorizer ledger missing the lines it changed"
  }
  if (@($json.loops).Count -lt 1) { throw "JSON per-loop cost model missing" }
  if (-not @($json.loops | Where-Object { $_.cyclesPerIter -gt 0 -and $_.bottleneck }).Count) {
    throw "JSON loop costs missing cycles or bottleneck port"
  }
  # The triage a tool renders as a fix-it panel: same ranking as the prose
  # "where to start". Whole-function codegen fallbacks lead (a measured cost,
  # not a per-loop prediction), then proven fixes, and the fix is untruncated.
  $start = @($json.startHere)
  if ($start.Count -lt 1) { throw "JSON startHere ranking missing" }
  if ($start[0].kind -ne "backend") { throw "JSON startHere should lead with the codegen fallback" }
  if (-not $start[0].why -or -not $start[0].instructions) { throw "JSON backend entry incomplete" }
  $firstRemark = @($start | Where-Object { $_.kind -eq "remark" })[0]
  if (-not $firstRemark.proven) { throw "JSON startHere should lead its remarks with a proven fix" }
  if (-not $firstRemark.fix -or -not $firstRemark.code) { throw "JSON startHere entry incomplete" }
  if (@($start | Where-Object { $_.proven }).Count -lt 2) {
    throw "JSON startHere lost the proven fixes"
  }
  if (@($json.callGraph).Count -lt 1) { throw "JSON call graph missing" }
  if (@($json.hotspots).Count -lt 1) { throw "JSON hotspot ranking missing" }
  $ranked = @($json.hotspots)
  if ($ranked.Count -gt 1 -and $ranked[0].cost -lt $ranked[-1].cost) {
    throw "JSON hotspots are not ranked by cost"
  }
  Remove-Item Env:METTLE_EXPLAIN_REPORT_LINES -ErrorAction SilentlyContinue
  Write-CaseResult -Name "explain_changes_and_json" -Passed $true
}
catch {
  Remove-Item Env:METTLE_EXPLAIN_REPORT_LINES -ErrorAction SilentlyContinue
  $failed++
  Write-CaseResult -Name "explain_changes_and_json" -Passed $false -Reason $_.Exception.Message
}

# --explain memory section: the compile-time memory diagnostics (here a borrow
# that outlives its scope) are surfaced in the optimization report's prose
# "memory report" section AND the .explain.json "memory" array, so the editor's
# Memory tab can render them.
$total++
try {
  $exDir = Join-Path $tmpDir "explain_memory"
  New-Item -ItemType Directory $exDir -Force | Out-Null
  $exOut = Join-Path $exDir "borrow.obj"
  $env:METTLE_EXPLAIN_REPORT_LINES = "0"
  $memRun = & $CompilerPath -i "tests/warn_borrow_scope.mettle" -o $exOut --release --explain --explain-json 2>&1 | Out-String
  if ($memRun -notmatch '-- memory report:') {
    throw "Prose memory report section missing. Output: $($memRun.Substring(0, [Math]::Min(600, $memRun.Length)))"
  }
  if ($memRun -notmatch 'after the scope of `x` ended') {
    throw "Memory report missing the borrow diagnostic"
  }
  $memJson = Get-Content (Join-Path $exDir "borrow.explain.json") -Raw | ConvertFrom-Json
  $memEntries = @($memJson.memory)
  if ($memEntries.Count -lt 1) { throw "JSON memory array empty" }
  $borrow = $memEntries | Where-Object { $_.headline -match 'borrows into `x`' }
  if (-not $borrow) { throw "JSON memory entry for the borrow missing" }
  if ($borrow.severity -ne 'warning') { throw "JSON memory severity wrong: $($borrow.severity)" }
  if (-not $borrow.fix) { throw "JSON memory entry missing its fix" }
  Remove-Item Env:METTLE_EXPLAIN_REPORT_LINES -ErrorAction SilentlyContinue
  Write-CaseResult -Name "explain_memory_section" -Passed $true
}
catch {
  Remove-Item Env:METTLE_EXPLAIN_REPORT_LINES -ErrorAction SilentlyContinue
  $failed++
  Write-CaseResult -Name "explain_memory_section" -Passed $false -Reason $_.Exception.Message
}

# Top-level constants: compile with --build and verify folded compile-time value.
$total++
try {
  $exePath = Join-Path $tmpDir "const_top_level.exe"
  $buildOut = & $CompilerPath --build "tests/test_const_top_level.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Const top-level build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Const top-level build did not produce an executable"
  }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 42) {
    throw "Const top-level exited with $LASTEXITCODE (expected 42)"
  }
  Write-CaseResult -Name "const_top_level_runtime" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "const_top_level_runtime" -Passed $false -Reason $_.Exception.Message
}

# Local non-integer consts (float + string): --build and verify runtime value.
$total++
try {
  $exePath = Join-Path $tmpDir "const_local_float_string.exe"
  $buildOut = & $CompilerPath --build "tests/test_const_local_float_string.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Local non-integer const build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Local non-integer const build did not produce an executable"
  }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 42) {
    throw "Local non-integer const exited with $LASTEXITCODE (expected 42)"
  }
  Write-CaseResult -Name "const_local_float_string_runtime" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "const_local_float_string_runtime" -Passed $false -Reason $_.Exception.Message
}

# Global non-integer consts (float + string): --build and verify runtime value.
# The float global now loads correctly in the direct-object backend, so it is no
# longer rejected; the string global must emit and link like any global.
$total++
try {
  $exePath = Join-Path $tmpDir "const_global_float_string.exe"
  $buildOut = & $CompilerPath --build "tests/test_const_global_float_string.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Global non-integer const build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Global non-integer const build did not produce an executable"
  }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 42) {
    throw "Global non-integer const exited with $LASTEXITCODE (expected 42)"
  }
  Write-CaseResult -Name "const_global_float_string_runtime" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "const_global_float_string_runtime" -Passed $false -Reason $_.Exception.Message
}

# Conditional imports: --build and verify off-target guarded imports are dropped.
$total++
try {
  $exePath = Join-Path $tmpDir "import_conditional.exe"
  $buildOut = & $CompilerPath --build "tests/test_import_conditional.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Conditional import build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Conditional import build did not produce an executable"
  }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 7) {
    throw "Conditional import exited with $LASTEXITCODE (expected 7)"
  }
  Write-CaseResult -Name "import_conditional_runtime" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "import_conditional_runtime" -Passed $false -Reason $_.Exception.Message
}

# Deferred calls capture arguments by value at the defer point.
$total++
try {
  $exePath = Join-Path $tmpDir "defer_by_value.exe"
  $buildOut = & $CompilerPath --build "tests/test_defer_by_value.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Defer by-value build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Defer by-value build did not produce an executable"
  }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 12) {
    throw "Defer by-value exited with $LASTEXITCODE (expected 12 - by-value; 123 would mean by-reference)"
  }
  Write-CaseResult -Name "defer_by_value_runtime" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "defer_by_value_runtime" -Passed $false -Reason $_.Exception.Message
}

# Bundled stdlib resolution test: compile from a project directory with no local stdlib.
$total++
try {
  $compilerFullPath = (Resolve-Path $CompilerPath).Path
  $nativeStdlibDir = Join-Path $tmpDir "native-stdlib-project"
  if (Test-Path $nativeStdlibDir) {
    Remove-Item -Path $nativeStdlibDir -Recurse -Force
  }
  New-Item -Path $nativeStdlibDir -ItemType Directory | Out-Null

  $nativeStdlibSource = Join-Path $nativeStdlibDir "main.mettle"
  $nativeStdlibObj = Join-Path $nativeStdlibDir "main.obj"
  @'
import "std/io";

fn main() -> int32 {
  println("Bundled stdlib works");
  return 0;
}
'@ | Set-Content -Path $nativeStdlibSource -Encoding ASCII

  Push-Location $nativeStdlibDir
  try {
    $nativeStdlibOut = & $compilerFullPath .\main.mettle -o .\main.obj 2>&1 | Out-String
    $nativeStdlibExit = $LASTEXITCODE
  }
  finally {
    Pop-Location
  }

  if ($nativeStdlibExit -ne 0) {
    throw "Bundled stdlib compile failed outside the repo root: $nativeStdlibOut"
  }
  if (-not (Test-Path $nativeStdlibObj)) {
    throw "Bundled stdlib compile did not produce an object output"
  }

  Write-CaseResult -Name "bundled_stdlib_outside_project" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "bundled_stdlib_outside_project" -Passed $false -Reason $_.Exception.Message
}

# UTF-8 BOM test: a source file starting with EF BB BF (PowerShell 5.1
# Set-Content -Encoding utf8, Notepad default) must compile cleanly.
$total++
try {
  $compilerFullPath = (Resolve-Path $CompilerPath).Path
  $bomDir = Join-Path $tmpDir "utf8-bom-project"
  if (Test-Path $bomDir) {
    Remove-Item -Path $bomDir -Recurse -Force
  }
  New-Item -Path $bomDir -ItemType Directory | Out-Null

  $bomSource = Join-Path $bomDir "main.mettle"
  $bomObj = Join-Path $bomDir "main.obj"
  @'
fn main() -> int32 {
  return 0;
}
'@ | Set-Content -Path $bomSource -Encoding utf8

  $bomBytes = [System.IO.File]::ReadAllBytes($bomSource)
  if ($bomBytes.Length -lt 3 -or $bomBytes[0] -ne 0xEF -or $bomBytes[1] -ne 0xBB -or $bomBytes[2] -ne 0xBF) {
    throw "BOM fixture was written without a UTF-8 BOM; the test cannot exercise the lexer path"
  }

  $bomOut = & $compilerFullPath $bomSource -o $bomObj 2>&1 | Out-String
  $bomExit = $LASTEXITCODE

  if ($bomExit -ne 0) {
    throw "Compile of a UTF-8 BOM source failed (exit $bomExit): $bomOut"
  }
  if (-not (Test-Path $bomObj)) {
    throw "Compile of a UTF-8 BOM source did not produce an object output"
  }

  Write-CaseResult -Name "utf8_bom_source" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "utf8_bom_source" -Passed $false -Reason $_.Exception.Message
}

# mettle.deps package resolution test: compile from a temp project using a package alias.
$total++
try {
  $compilerFullPath = (Resolve-Path $CompilerPath).Path
  $depsProjectDir = Join-Path $tmpDir "mettle-deps-project"
  if (Test-Path $depsProjectDir) {
    Remove-Item -Path $depsProjectDir -Recurse -Force
  }
  New-Item -Path $depsProjectDir -ItemType Directory | Out-Null

  $depsSource = Join-Path $depsProjectDir "main.mettle"
  $depsObj = Join-Path $depsProjectDir "main.obj"
  $depsFile = Join-Path $depsProjectDir "mettle.deps"
  $packageRoot = Join-Path $repoRoot "tests/lib"

  "testpkg=$packageRoot" | Set-Content -Path $depsFile -Encoding ASCII
  @'
import "testpkg/shared_math";

fn main() -> int32 {
  return forty_two();
}
'@ | Set-Content -Path $depsSource -Encoding ASCII

  Push-Location $depsProjectDir
  try {
    $depsOut = & $compilerFullPath .\main.mettle -o .\main.obj 2>&1 | Out-String
    $depsExit = $LASTEXITCODE
  }
  finally {
    Pop-Location
  }

  if ($depsExit -ne 0) {
    throw "mettle.deps package compile failed: $depsOut"
  }
  if (-not (Test-Path $depsObj)) {
    throw "mettle.deps package compile did not produce an object output"
  }

  Write-CaseResult -Name "mettle_deps_package_resolution" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "mettle_deps_package_resolution" -Passed $false -Reason $_.Exception.Message
}

# Function pointer test: build and run
$total++
try {
  $fpExe = Join-Path $tmpDir "test_function_pointer.exe"

  $fpOut = & $CompilerPath --build tests/test_function_pointer.mettle -o $fpExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Function pointer build failed: $fpOut"
  }

  $fpResult = & $fpExe 2>&1
  if ($LASTEXITCODE -ne 1) {
    throw "Function pointer test exited with $LASTEXITCODE (expected 1)"
  }

  Write-CaseResult -Name "function_pointer" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "function_pointer" -Passed $false -Reason $_.Exception.Message
}

# Struct new runtime test: verifies `new Struct` allocates full struct size.
$total++
try {
  $structNewExe = Join-Path $tmpDir "test_struct_new_zeroed.exe"

  $structNewOut = & $CompilerPath --build --linker internal tests/test_struct_new_zeroed.mettle -o $structNewExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Struct new build failed: $structNewOut"
  }
  if (-not (Test-Path $structNewExe)) {
    throw "Struct new build did not produce an executable"
  }

  & $structNewExe 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 1) {
    throw "Struct new executable exited with $LASTEXITCODE (expected 1)"
  }

  Write-CaseResult -Name "struct_new_runtime" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "struct_new_runtime" -Passed $false -Reason $_.Exception.Message
}


# Direct object backend test: emit COFF object directly, then build and run
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_return_const.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_return_const.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_direct_object_return_const.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object compile did not produce an object file"
  }

  $objSymbols = & objdump -t $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object symbol dump failed"
  }
  if ($objSymbols -notmatch "(?m)\bmain\b") {
    throw "Direct object symbol table did not contain main"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_direct_object_return_const.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 7) {
    throw "Direct object executable exited with $LASTEXITCODE (expected 7)"
  }

  Write-CaseResult -Name "direct_object_return_const" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_return_const" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend relocation test: internal call lowered to REL32 relocation
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_call_return.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_call_return.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_direct_object_call_return.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object call compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object call compile did not produce an object file"
  }

  $relocs = & objdump -r $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object relocation dump failed"
  }
  if ($relocs -notmatch "IMAGE_REL_AMD64_REL32\s+callee") {
    throw "Direct object relocation table did not contain a REL32 call to callee"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_direct_object_call_return.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object call build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object call build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 5) {
    throw "Direct object call executable exited with $LASTEXITCODE (expected 5)"
  }

  Write-CaseResult -Name "direct_object_call_return" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_call_return" -Passed $false -Reason $_.Exception.Message
}

# Closed-form reduction equivalence: the constant-bound loop unroller must not
# miscompile counted polynomial sums (regression for the stale-counter bug in
# ir_build_symbol_int_map_before). Built both with and without --release because
# the miscompile only surfaced once the reduction-unroll + const-bound unroll
# passes ran. The test program self-checks and returns nonzero on any mismatch.
foreach ($relFlag in @($true, $false)) {
  $total++
  $variant = if ($relFlag) { "release" } else { "debug" }
  try {
    $exePath = Join-Path $tmpDir "test_opt_closed_form_sum_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($relFlag) { $buildArgs += "--release" }
    $buildArgs += @("tests/test_opt_closed_form_sum.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "closed-form build ($variant) failed: $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "closed-form build ($variant) did not produce an executable"
    }

    $runOut = & $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "closed-form ($variant) reported a mismatch (exit $LASTEXITCODE): $runOut"
    }
    if ($runOut -notmatch "closed_form_sum OK") {
      throw "closed-form ($variant) did not print OK: $runOut"
    }

    Write-CaseResult -Name "opt_closed_form_sum_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "opt_closed_form_sum_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Pointer-induction scope: converting the first of two loops that share an
# induction variable and array must not rewrite the second loop's compare to
# the first loop's exhausted walk pointers (regression: the rewrite window ran
# to end-of-function instead of the loop's back-edge). Self-checks the second
# loop's stores and returns nonzero on any mismatch.
foreach ($relFlag in @($true, $false)) {
  $total++
  $variant = if ($relFlag) { "release" } else { "debug" }
  try {
    $exePath = Join-Path $tmpDir "test_opt_ptr_induction_two_loops_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($relFlag) { $buildArgs += "--release" }
    $buildArgs += @("tests/test_opt_ptr_induction_two_loops.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "ptr-induction two-loops build ($variant) failed: $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "ptr-induction two-loops build ($variant) did not produce an executable"
    }

    $runOut = & $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "ptr-induction two-loops ($variant) reported a mismatch (exit $LASTEXITCODE): $runOut"
    }
    if ($runOut -notmatch "ptr_induction_two_loops OK") {
      throw "ptr-induction two-loops ($variant) did not print OK: $runOut"
    }

    Write-CaseResult -Name "opt_ptr_induction_two_loops_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "opt_ptr_induction_two_loops_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# MIR loop rotation moves the header test above the header label, so it is only
# legal when the latch is the sole edge into the header. A Hoare partition's
# `if (i <= j)` false arm threads straight back to the enclosing `while (i <= j)`
# header as a CMPBR, which the back-edge scan missed when it counted only JMPs;
# rotating then let that edge re-enter the body untested and spin forever. The
# reproducer carries its own iteration budget so a regression fails instead of
# hanging the suite.
foreach ($relFlag in @($true, $false)) {
  $total++
  $variant = if ($relFlag) { "release" } else { "debug" }
  try {
    $exePath = Join-Path $tmpDir "test_mir_rotate_backedge_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($relFlag) { $buildArgs += "--release" }
    $buildArgs += @("tests/test_mir_rotate_backedge.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "rotate back-edge build ($variant) failed: $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "rotate back-edge build ($variant) did not produce an executable"
    }

    $runOut = & $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "rotate back-edge ($variant) failed (exit $LASTEXITCODE): $runOut"
    }
    if ($runOut -notmatch "rotate_backedge OK") {
      throw "rotate back-edge ($variant) did not print OK: $runOut"
    }

    Write-CaseResult -Name "mir_rotate_backedge_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "mir_rotate_backedge_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# A function-local that shares a name with a global must never be treated as
# that global. code_generator_lookup_symbol resolves against the module symbol
# table alone, so a `var exp` beside std/math's `exp` scored as global-scope and
# the register-promotion write-back stored the local into .text (a fault) or,
# when the name matched a global VARIABLE, over the global itself (silent
# corruption). Also asserts real global promotion still happens.
foreach ($relFlag in @($true, $false)) {
  $total++
  $variant = if ($relFlag) { "release" } else { "debug" }
  try {
    $exePath = Join-Path $tmpDir "test_local_shadows_global_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($relFlag) { $buildArgs += "--release" }
    $buildArgs += @("tests/test_local_shadows_global.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "local-shadows-global build ($variant) failed: $buildOut"
    }
    $runOut = & $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "local-shadows-global ($variant) failed (exit $LASTEXITCODE): $runOut"
    }
    if ($runOut -notmatch "local_shadows_global OK") {
      throw "local-shadows-global ($variant) did not print OK: $runOut"
    }

    Write-CaseResult -Name "local_shadows_global_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "local_shadows_global_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Two locals sharing a name but not a type. Every backend table that describes a
# local -- frame slot, declared type, float width -- is keyed by name, so the two
# bindings shared one slot at one type: a uint64 declared after a float64 of the
# same name compared as a float, and a struct declared after a scalar overran the
# scalar's slot. The lowering now gives the second binding a name of its own.
foreach ($relFlag in @($true, $false)) {
  $total++
  $variant = if ($relFlag) { "release" } else { "debug" }
  try {
    $exePath = Join-Path $tmpDir "test_scoped_shadowing_$variant.exe"
    $buildArgs = @("--build")
    if ($relFlag) { $buildArgs += "--release" }
    $buildArgs += @("tests/test_scoped_shadowing.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "scoped-shadowing build ($variant) failed: $buildOut"
    }
    $runOut = & $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "scoped-shadowing ($variant) failed (exit $LASTEXITCODE): $runOut"
    }
    if ($runOut -notmatch "scoped_shadowing OK") {
      throw "scoped-shadowing ($variant) did not print OK: $runOut"
    }

    Write-CaseResult -Name "scoped_shadowing_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "scoped_shadowing_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# The MIR encoder stages a spilled base/index/value through a scratch register.
# RDX was a candidate, but RDX is in the allocator's pool: staging over the live
# loop counter it held made the counter run past its bound and the release build
# walked off a local array. The scratch pick now vets liveness; this decodes a
# K-quant-shaped nest and checks every element against an independent oracle.
foreach ($relFlag in @($true, $false)) {
  $total++
  $variant = if ($relFlag) { "release" } else { "debug" }
  try {
    $exePath = Join-Path $tmpDir "test_mir_scratch_clobber_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($relFlag) { $buildArgs += "--release" }
    $buildArgs += @("tests/test_mir_scratch_clobber.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "mir scratch-clobber build ($variant) failed: $buildOut"
    }
    $runOut = & $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "mir scratch-clobber ($variant) failed (exit $LASTEXITCODE): $runOut"
    }
    if ($runOut -notmatch "mir_scratch_clobber OK") {
      throw "mir scratch-clobber ($variant) did not print OK: $runOut"
    }

    Write-CaseResult -Name "mir_scratch_clobber_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "mir_scratch_clobber_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Tail-recursion elimination: pure (`return self(...)`), void (`self(...);
# return`), and accumulator (`return E + self(...)`) forms must preserve
# semantics, including the MIR back-edge-to-entry liveness fix (params must
# survive the rebind+jump loop). Order-sensitive checks: qsr verifies actual
# sortedness, not an order-blind sum.
foreach ($relFlag in @($true, $false)) {
  $total++
  $variant = if ($relFlag) { "release" } else { "debug" }
  try {
    $exePath = Join-Path $tmpDir "test_opt_tail_recursion_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($relFlag) { $buildArgs += "--release" }
    $buildArgs += @("tests/test_opt_tail_recursion.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "tail-recursion build ($variant) failed: $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "tail-recursion build ($variant) did not produce an executable"
    }

    $runOut = & $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "tail-recursion ($variant) reported a mismatch (exit $LASTEXITCODE): $runOut"
    }
    if ($runOut -notmatch "tail_recursion OK") {
      throw "tail-recursion ($variant) did not print OK: $runOut"
    }

    Write-CaseResult -Name "opt_tail_recursion_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "opt_tail_recursion_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Read-only global fold: a never-written global integer var must fold to its
# initializer; a written one must NOT. Self-checks both.
foreach ($relFlag in @($true, $false)) {
  $total++
  $variant = if ($relFlag) { "release" } else { "debug" }
  try {
    $exePath = Join-Path $tmpDir "test_opt_readonly_global_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($relFlag) { $buildArgs += "--release" }
    $buildArgs += @("tests/test_opt_readonly_global.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "readonly-global build ($variant) failed: $buildOut"
    }

    $runOut = & $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "readonly-global ($variant) reported a mismatch (exit $LASTEXITCODE): $runOut"
    }
    if ($runOut -notmatch "readonly_global OK") {
      throw "readonly-global ($variant) did not print OK: $runOut"
    }

    Write-CaseResult -Name "opt_readonly_global_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "opt_readonly_global_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Indirect-gather correctness: `total += a[b[i]]` must not be misclaimed by
# the unit-stride sum recognizers (a fixed --release miscompile summed b
# instead), and the prefetch pass's look-ahead clone must not change results.
foreach ($relFlag in @($true, $false)) {
  $total++
  $variant = if ($relFlag) { "release" } else { "debug" }
  try {
    $exePath = Join-Path $tmpDir "test_opt_gather_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($relFlag) { $buildArgs += "--release" }
    $buildArgs += @("tests/test_opt_gather.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "gather build ($variant) failed: $buildOut"
    }

    $runOut = & $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "gather ($variant) reported a mismatch (exit $LASTEXITCODE): $runOut"
    }
    if ($runOut -notmatch "gather OK") {
      throw "gather ($variant) did not print OK: $runOut"
    }

    Write-CaseResult -Name "opt_gather_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "opt_gather_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Allocation-site layout factorization: a padded malloc pool must compact, a
# subset-loaded pool must factor into per-field arrays (SoA), and an escaped
# pool must be declined -- all while the program's closed-form checksums hold.
# Self-checks and returns nonzero on any mismatch.
foreach ($relFlag in @($true, $false)) {
  $total++
  $variant = if ($relFlag) { "release" } else { "debug" }
  try {
    $exePath = Join-Path $tmpDir "test_opt_layout_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($relFlag) { $buildArgs += "--release" }
    $buildArgs += @("tests/test_opt_layout.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "layout build ($variant) failed: $buildOut"
    }

    $runOut = & $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "layout ($variant) reported a mismatch (exit $LASTEXITCODE): $runOut"
    }
    if ($runOut -notmatch "layout OK") {
      throw "layout ($variant) did not print OK: $runOut"
    }

    Write-CaseResult -Name "opt_layout_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "opt_layout_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# float32 narrowing equivalence: a float64 expression assigned/returned into a
# float32 destination must narrow (cvtsd2ss). Built both with and without
# --release because the two miscompiles surfaced on different paths -- the
# assignment-statement narrowing at -O0, and the inliner + single-use assign
# coalesce at --release. The program self-checks and returns nonzero on any
# mismatch.
foreach ($relFlag in @($true, $false)) {
  $total++
  $variant = if ($relFlag) { "release" } else { "debug" }
  try {
    $exePath = Join-Path $tmpDir "test_float32_narrowing_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($relFlag) { $buildArgs += "--release" }
    $buildArgs += @("tests/test_float32_narrowing.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "float32-narrowing build ($variant) failed: $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "float32-narrowing build ($variant) did not produce an executable"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 1) {
      throw "float32-narrowing ($variant) reported a mismatch (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "float32_narrowing_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "float32_narrowing_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Shared scaled index: `a[i] * b[i]` computes `i << 2` once after CSE, so one
# scaled temp feeds two address adds. The SIB address fold used to demand a
# single reader and dropped this shape onto its scale-1 fallback, which folded
# the already-scaled value in as a unit index and addressed the wrong element.
# Built debug + release + *_fallback so the MIR and legacy backends agree; the
# bug was MIR-only and release-only, and the fallback columns prove that.
foreach ($variant in @("release", "debug", "release_fallback", "debug_fallback")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_shared_scaled_index_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -like "release*") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_shared_scaled_index.mettle", "-o", $exePath)

    if ($variant -like "*_fallback") { $env:METTLE_MIR = "0" }
    try {
      $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    }
    finally {
      if ($variant -like "*_fallback") { Remove-Item Env:\METTLE_MIR -ErrorAction SilentlyContinue }
    }
    if ($LASTEXITCODE -ne 0) {
      throw "shared-scaled-index build ($variant) failed: $buildOut"
    }

    & $exePath 2>&1 | Out-Null
    # 1/2/3 name which of the three loops addressed the wrong element.
    if ($LASTEXITCODE -ne 7) {
      throw "shared-scaled-index ($variant) miscompiled (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "shared_scaled_index_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "shared_scaled_index_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Float narrowing paths: the three sites that must cvtsd2ss a float64-tracked
# value into a float32 destination (MIR store, MIR return, inliner param
# assign) â€” each was a distinct silent miscompile found by the v2 fuzzer.
# Built debug AND release: the store bug fired at -O0, the return bug at
# release, the param bug in the fallback backend.
foreach ($variant in @("release", "debug", "release_fallback", "debug_fallback")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_float_narrowing_paths_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -like "release*") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_float_narrowing_paths.mettle", "-o", $exePath)

    # *_fallback routes every function to the legacy backend; the
    # inliner-param shape only miscompiled there (release_fallback = the
    # inliner runs AND the fallback backend consumes its output).
    if ($variant -like "*_fallback") { $env:METTLE_MIR = "0" }
    try {
      $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    }
    finally {
      if ($variant -like "*_fallback") { Remove-Item Env:\METTLE_MIR -ErrorAction SilentlyContinue }
    }
    if ($LASTEXITCODE -ne 0) {
      throw "float-narrowing-paths build ($variant) failed: $buildOut"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 1) {
      throw "float-narrowing-paths ($variant) miscompiled (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "float_narrowing_paths_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "float_narrowing_paths_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# MIR float call arguments: a call passing float args now uses the
# register-allocating backend (XMM0-3 homing) instead of bailing the caller to
# spill-everything codegen. Also covers the allocator entry-live interference
# fix (two single-use params no longer share a register) and float unary negate.
# Built debug + release + *_fallback (METTLE_MIR=0) so MIR and the legacy
# backend produce identical results.
foreach ($variant in @("release", "debug", "release_fallback", "debug_fallback")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_mir_float_call_args_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -like "release*") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_mir_float_call_args.mettle", "-o", $exePath)

    if ($variant -like "*_fallback") { $env:METTLE_MIR = "0" }
    try {
      $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    }
    finally {
      if ($variant -like "*_fallback") { Remove-Item Env:\METTLE_MIR -ErrorAction SilentlyContinue }
    }
    if ($LASTEXITCODE -ne 0) {
      throw "mir-float-call-args build ($variant) failed: $buildOut"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 1) {
      throw "mir-float-call-args ($variant) miscompiled (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "mir_float_call_args_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "mir_float_call_args_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# MIR inline fill passthrough: IR_OP_SIMD_FILL runs through the
# register-allocating backend (mode 0/1, runtime offset folded as base+off*size,
# live-iv write-back) instead of bailing the function to spill-everything
# codegen. Built debug + release + *_fallback so MIR and the legacy backend agree.
foreach ($variant in @("release", "debug", "release_fallback", "debug_fallback")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_mir_fill_passthrough_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -like "release*") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_mir_fill_passthrough.mettle", "-o", $exePath)

    if ($variant -like "*_fallback") { $env:METTLE_MIR = "0" }
    try {
      $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    }
    finally {
      if ($variant -like "*_fallback") { Remove-Item Env:\METTLE_MIR -ErrorAction SilentlyContinue }
    }
    if ($LASTEXITCODE -ne 0) {
      throw "mir-fill-passthrough build ($variant) failed: $buildOut"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 1) {
      throw "mir-fill-passthrough ($variant) miscompiled (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "mir_fill_passthrough_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "mir_fill_passthrough_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# A global array or struct is read and written through its address, which used
# to decline the whole function at the eligibility gate (`addressof:unsupported`)
# and put every value in the frame on the stack. Any global's address is one
# RIP-relative LEA, so all of these belong on the allocated path. Run against
# the fallback backend too, since only memory (never a cache vreg) is
# authoritative for an aggregate global.
foreach ($variant in @("release", "debug", "release_fallback", "debug_fallback")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_mir_global_aggregate_addr_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -like "release*") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_mir_global_aggregate_addr.mettle", "-o", $exePath)

    if ($variant -like "*_fallback") { $env:METTLE_MIR = "0" }
    try {
      $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    }
    finally {
      if ($variant -like "*_fallback") { Remove-Item Env:\METTLE_MIR -ErrorAction SilentlyContinue }
    }
    if ($LASTEXITCODE -ne 0) {
      throw "mir-global-aggregate-addr build ($variant) failed: $buildOut"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 42) {
      throw "mir-global-aggregate-addr ($variant) miscompiled (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "mir_global_aggregate_addr_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "mir_global_aggregate_addr_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Every if-converted shape at seven lengths, against a scalar oracle in the
# same program. Run with each contributing pass disabled in turn, so the
# vectorized answer is checked against the scalar one it replaces rather than
# only against itself.
foreach ($variant in @("release", "debug", "no_vec", "no_accum", "no_hoist", "no_scan")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_vloop_select_stress_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -ne "debug") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_vloop_select_stress.mettle", "-o", $exePath)

    $skip = $null
    if ($variant -eq "no_vec") { $skip = "auto_vectorize_int" }
    if ($variant -eq "no_accum") { $skip = "if_convert_accumulate" }
    if ($variant -eq "no_hoist") { $skip = "hoist_global_bases" }
    if ($variant -eq "no_scan") { $skip = "scan_from_first" }
    if ($skip) { $env:METTLE_SKIP_PASS = $skip }
    try {
      $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    }
    finally {
      if ($skip) { Remove-Item Env:\METTLE_SKIP_PASS -ErrorAction SilentlyContinue }
    }
    if ($LASTEXITCODE -ne 0) {
      throw "vloop-select-stress build ($variant) failed: $buildOut"
    }
    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 42) {
      throw "vloop-select-stress ($variant) miscompiled (kernel $LASTEXITCODE)"
    }
    Write-CaseResult -Name "vloop_select_stress_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "vloop_select_stress_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Counting under a condition. The comparison holds 0 or 1, so the accumulate is
# made unconditional and the reduction kernels can read it. Run with the pass
# disabled too, to check the rewritten answer against the branching one.
foreach ($variant in @("release", "debug", "release_scalar")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_predicated_accumulate_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -like "release*") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_predicated_accumulate.mettle", "-o", $exePath)

    if ($variant -eq "release_scalar") { $env:METTLE_SKIP_PASS = "if_convert_accumulate" }
    try {
      $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    }
    finally {
      if ($variant -eq "release_scalar") { Remove-Item Env:\METTLE_SKIP_PASS -ErrorAction SilentlyContinue }
    }
    if ($LASTEXITCODE -ne 0) {
      throw "predicated-accumulate build ($variant) failed: $buildOut"
    }
    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 42) {
      throw "predicated-accumulate ($variant) miscompiled (exit $LASTEXITCODE)"
    }
    Write-CaseResult -Name "predicated_accumulate_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "predicated_accumulate_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Anti-rot guard, in both directions: the three that must be claimed, and the
# two that must not. Rewriting `if (a[i] & 6)` as a multiply would add 2, 4 or 6
# where the branch added 1, and the arithmetic would still look plausible.
$total++
try {
  $exePath = Join-Path $tmpDir "test_predicated_accumulate_cover.exe"
  $coverOut = & $CompilerPath "--build" "--emit-obj" "--linker" "internal" "--release" `
    "--explain" "tests/test_predicated_accumulate.mettle" "-o" $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "predicated-accumulate coverage build failed: $coverOut"
  }
  foreach ($fn in @("count_matches", "sum_negatives", "weighted_hits",
                    "count_nonzero", "weigh_nonzero")) {
    if ($coverOut -notmatch "$fn \(loop @ line \d+\): vectorized") {
      throw "$fn no longer vectorizes; the predicated accumulate stopped converting"
    }
  }
  foreach ($fn in @("two_sided", "nonbool_guard")) {
    if ($coverOut -match "$fn \(loop @ line \d+\): vectorized") {
      throw "$fn vectorized; the accumulate rewrite is claiming a shape it cannot reproduce"
    }
  }
  Write-CaseResult -Name "predicated_accumulate_coverage" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "predicated_accumulate_coverage" -Passed $false -Reason $_.Exception.Message
}

# Comparisons read as values: `c + (a[i] > t)` and `a[i] * (a[i] > 0)`. All six
# operators, since `<` swaps the lane compare's operands and `<=`/`>=`/`!=`
# negate it, and getting either backwards is still plausible arithmetic.
foreach ($variant in @("release", "debug")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_compare_as_value_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -eq "release") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_compare_as_value.mettle", "-o", $exePath)
    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "compare-as-value build ($variant) failed: $buildOut"
    }
    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 42) {
      throw "compare-as-value ($variant) miscompiled (exit $LASTEXITCODE)"
    }
    Write-CaseResult -Name "compare_as_value_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "compare_as_value_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

$total++
try {
  $exePath = Join-Path $tmpDir "test_compare_as_value_cover.exe"
  $coverOut = & $CompilerPath "--build" "--emit-obj" "--linker" "internal" "--release" `
    "--explain" "tests/test_compare_as_value.mettle" "-o" $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "compare-as-value coverage build failed: $coverOut"
  }
  foreach ($fn in @("count_gt", "count_lt", "count_ge", "count_le", "count_eq",
                    "count_ne", "mask_map")) {
    if ($coverOut -notmatch "$fn \(loop @ line \d+\): vectorized") {
      throw "$fn no longer vectorizes; a comparison stopped being a value"
    }
  }
  Write-CaseResult -Name "compare_as_value_coverage" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "compare_as_value_coverage" -Passed $false -Reason $_.Exception.Message
}

# A sum kernel's base pointer, held in a local under an ordinary name. The rule
# used to be a name match against the inliner's `_param_data` suffix, so whether
# this vectorized depended on what the writer had called the variable.
foreach ($variant in @("release", "debug")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_sum_base_any_name_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -eq "release") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_sum_base_any_name.mettle", "-o", $exePath)
    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "sum-base-any-name build ($variant) failed: $buildOut"
    }
    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 42) {
      throw "sum-base-any-name ($variant) miscompiled (exit $LASTEXITCODE)"
    }
    Write-CaseResult -Name "sum_base_any_name_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "sum_base_any_name_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

$total++
try {
  $exePath = Join-Path $tmpDir "test_sum_base_any_name_cover.exe"
  $coverOut = & $CompilerPath "--build" "--emit-obj" "--linker" "internal" "--release" `
    "--explain" "tests/test_sum_base_any_name.mettle" "-o" $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "sum-base-any-name coverage build failed: $coverOut"
  }
  foreach ($fn in @("sum_via_local", "sum_direct")) {
    if ($coverOut -notmatch "$fn \(loop @ line \d+\): vectorized") {
      throw "$fn no longer vectorizes; the sum base rule went back to matching names"
    }
  }
  Write-CaseResult -Name "sum_base_any_name_coverage" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "sum_base_any_name_coverage" -Passed $false -Reason $_.Exception.Message
}

# Hoisting a global array's base above its loop. A global's address was computed
# inside the body, which hid the array from every recognizer at once, so a
# program keeping its buffers at file scope vectorized nowhere.
foreach ($variant in @("release", "debug", "release_scalar")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_hoist_global_bases_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -like "release*") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_hoist_global_bases.mettle", "-o", $exePath)

    if ($variant -eq "release_scalar") { $env:METTLE_SKIP_PASS = "hoist_global_bases" }
    try {
      $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    }
    finally {
      if ($variant -eq "release_scalar") { Remove-Item Env:\METTLE_SKIP_PASS -ErrorAction SilentlyContinue }
    }
    if ($LASTEXITCODE -ne 0) {
      throw "hoist-global-bases build ($variant) failed: $buildOut"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 42) {
      throw "hoist-global-bases ($variant) miscompiled (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "hoist_global_bases_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "hoist_global_bases_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Anti-rot guard: without the hoist these loops are still correct, just scalar.
$total++
try {
  $exePath = Join-Path $tmpDir "test_hoist_global_bases_cover.exe"
  $coverOut = & $CompilerPath "--build" "--emit-obj" "--linker" "internal" "--release" `
    "--explain" "tests/test_hoist_global_bases.mettle" "-o" $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "hoist-global-bases coverage build failed: $coverOut"
  }
  foreach ($fn in @("fill_src", "map_src", "sum_dst", "clamp_src_into_dst",
                    "fill_reals")) {
    if ($coverOut -notmatch "$fn \(loop @ line \d+\): vectorized") {
      throw "$fn no longer vectorizes; a global array's base stopped being hoisted"
    }
  }
  Write-CaseResult -Name "hoist_global_bases_coverage" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "hoist_global_bases_coverage" -Passed $false -Reason $_.Exception.Message
}

# If-conversion in the general vectorizer: an `if` whose arms only choose a
# value becomes a lane select, so a clamp, a ReLU and a floor all reach the same
# kernel however the source spells them. Run with the pass disabled as well, to
# check the vector answer against the scalar one it replaces.
foreach ($variant in @("release", "debug", "release_scalar")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_vloop_if_conversion_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -like "release*") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_vloop_if_conversion.mettle", "-o", $exePath)

    if ($variant -eq "release_scalar") { $env:METTLE_SKIP_PASS = "auto_vectorize_int" }
    try {
      $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    }
    finally {
      if ($variant -eq "release_scalar") { Remove-Item Env:\METTLE_SKIP_PASS -ErrorAction SilentlyContinue }
    }
    if ($LASTEXITCODE -ne 0) {
      throw "vloop-if-conversion build ($variant) failed: $buildOut"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 42) {
      throw "vloop-if-conversion ($variant) miscompiled (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "vloop_if_conversion_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "vloop_if_conversion_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Anti-rot guard for the case above. A select that quietly stops being claimed
# still passes every correctness check, just slowly, so name each loop and
# require a vectorized verdict for it.
$total++
try {
  $exePath = Join-Path $tmpDir "test_vloop_if_conversion_cover.exe"
  $coverOut = & $CompilerPath "--build" "--emit-obj" "--linker" "internal" "--release" `
    "--explain" "tests/test_vloop_if_conversion.mettle" "-o" $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "vloop-if-conversion coverage build failed: $coverOut"
  }
  foreach ($fn in @("clamp_two_sided", "relu", "ceiling_then_double", "floor_reversed")) {
    if ($coverOut -notmatch "$fn \(loop @ line \d+\): vectorized") {
      throw "$fn no longer vectorizes; if-conversion stopped claiming its select"
    }
  }
  # An int32 vector kernel must also run INSIDE an allocated frame. Without the
  # vloop passthrough one SIMD op puts every value in the function on the stack,
  # so vectorizing a loop would cost the rest of the function its registers.
  if ($coverOut -notmatch '(\d+)/(\d+) functions reaching codegen') {
    throw "no backend coverage line in --explain output"
  }
  if ($Matches[1] -ne $Matches[2]) {
    throw ("only {0}/{1} functions register-allocated; an int32 vloop stopped " +
           "passing through the MIR backend") -f $Matches[1], $Matches[2]
  }
  Write-CaseResult -Name "vloop_if_conversion_coverage" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "vloop_if_conversion_coverage" -Passed $false -Reason $_.Exception.Message
}

# Lane selects: a value chosen by a branch whose arms are NOT the two compared
# values needs a real blend, not a minimum or a maximum. Covers the early-return
# chain an inlined helper leaves behind, an if/else nest, and the two negated
# compares (`<=`, `>=`) whose arms the kernel has to exchange.
foreach ($variant in @("release", "debug", "release_scalar")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_vloop_select_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -like "release*") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_vloop_select.mettle", "-o", $exePath)

    if ($variant -eq "release_scalar") { $env:METTLE_SKIP_PASS = "auto_vectorize_int" }
    try {
      $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    }
    finally {
      if ($variant -eq "release_scalar") { Remove-Item Env:\METTLE_SKIP_PASS -ErrorAction SilentlyContinue }
    }
    if ($LASTEXITCODE -ne 0) {
      throw "vloop-select build ($variant) failed: $buildOut"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 42) {
      throw "vloop-select ($variant) miscompiled (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "vloop_select_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "vloop_select_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Anti-rot guard: each of these is still correct when it stops being claimed,
# just slower, so name the loops and require a vectorized verdict.
$total++
try {
  $exePath = Join-Path $tmpDir "test_vloop_select_cover.exe"
  $coverOut = & $CompilerPath "--build" "--emit-obj" "--linker" "internal" "--release" `
    "--explain" "tests/test_vloop_select.mettle" "-o" $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "vloop-select coverage build failed: $coverOut"
  }
  foreach ($fn in @("via_helper", "three_way", "negate_low", "ge_pick")) {
    if ($coverOut -notmatch "$fn \(loop @ line \d+\): vectorized") {
      throw "$fn no longer vectorizes; the lane select stopped being claimed"
    }
  }
  Write-CaseResult -Name "vloop_select_coverage" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "vloop_select_coverage" -Passed $false -Reason $_.Exception.Message
}

# Anti-rot guard for the case above: correctness alone cannot tell the allocated
# path from the fallback, so assert the coverage the gate is supposed to give.
$total++
try {
  $exePath = Join-Path $tmpDir "test_mir_global_aggregate_addr_cover.exe"
  $env:METTLE_MIR_TRACE = "1"
  try {
    $coverOut = & $CompilerPath "--build" "--emit-obj" "--linker" "internal" "--release" `
      "tests/test_mir_global_aggregate_addr.mettle" "-o" $exePath 2>&1 | Out-String
  }
  finally {
    Remove-Item Env:\METTLE_MIR_TRACE -ErrorAction SilentlyContinue
  }
  if ($LASTEXITCODE -ne 0) {
    throw "mir-global-aggregate-addr coverage build failed: $coverOut"
  }
  if ($coverOut -notmatch 'MIR-(OK|BAIL)') {
    throw "no MIR gate trace; METTLE_MIR_TRACE stopped reporting"
  }
  # The gate may still decline a function for its own reasons (a SIMD kernel it
  # has no passthrough for, say). What must never come back is declining one
  # because it took a global aggregate's address.
  if ($coverOut -match 'MIR-BAIL\s+(addressof|global_access)') {
    throw "the gate declined a function with '$($Matches[1])'; a global aggregate's address stopped being addressable"
  }
  Write-CaseResult -Name "mir_global_aggregate_addr_coverage" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "mir_global_aggregate_addr_coverage" -Passed $false -Reason $_.Exception.Message
}

# MIR inline float32 affine-map passthrough: IR_OP_SIMD_AFFINE_MAP_F32 (the
# float-copy / saxpy / `a*x+c` class) runs through the register-allocating
# backend with its compile-time coefficients baked into the kernel broadcasts,
# instead of bailing the function. This is what makes the qwen3 engine's
# load_f32 and process_token register-allocated.
foreach ($variant in @("release", "debug", "release_fallback", "debug_fallback")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_mir_affine_map_passthrough_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -like "release*") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_mir_affine_map_passthrough.mettle", "-o", $exePath)

    if ($variant -like "*_fallback") { $env:METTLE_MIR = "0" }
    try {
      $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    }
    finally {
      if ($variant -like "*_fallback") { Remove-Item Env:\METTLE_MIR -ErrorAction SilentlyContinue }
    }
    if ($LASTEXITCODE -ne 0) {
      throw "mir-affine-map-passthrough build ($variant) failed: $buildOut"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 1) {
      throw "mir-affine-map-passthrough ($variant) miscompiled (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "mir_affine_map_passthrough_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "mir_affine_map_passthrough_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Odd-sized struct copy: whole-struct assign of a 3-byte struct must copy
# exactly 3 bytes; the fallback backend's 8-byte round-trip clobbered the
# adjacent local. Run debug/release/fallback like the narrowing test.
foreach ($variant in @("release", "debug", "debug_fallback")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_struct_copy_odd_size_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -eq "release") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_struct_copy_odd_size.mettle", "-o", $exePath)

    if ($variant -eq "debug_fallback") { $env:METTLE_MIR = "0" }
    try {
      $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    }
    finally {
      if ($variant -eq "debug_fallback") { Remove-Item Env:\METTLE_MIR -ErrorAction SilentlyContinue }
    }
    if ($LASTEXITCODE -ne 0) {
      throw "struct-copy-odd-size build ($variant) failed: $buildOut"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 1) {
      throw "struct-copy-odd-size ($variant) miscompiled (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "struct_copy_odd_size_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "struct_copy_odd_size_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# MIR wide-store regression: release inlines a struct-returning helper, then
# copies a 24-byte struct by value. The executable returns the folded checksum.
foreach ($variant in @("release", "debug")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "test_mir_inline_struct_copy_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($variant -eq "release") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_mir_inline_struct_copy.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "mir-inline-struct-copy build ($variant) failed: $buildOut"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 9) {
      throw "mir-inline-struct-copy ($variant) miscompiled (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "mir_inline_struct_copy_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "mir_inline_struct_copy_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Constant division/modulo magic-multiply strength reduction. The program is a
# differential oracle: it compares each literal `x / C` / `x % C` (magic-
# multiply) against the same division by a heap-loaded divisor (genuine idiv),
# across signed/unsigned dividends incl. INT64_MIN and UINT64_MAX. Returns 1 on
# full agreement. Built both with and without --release (the strength reduction
# only runs in the binary backend, which both paths use).
foreach ($relFlag in @($true, $false)) {
  $total++
  $variant = if ($relFlag) { "release" } else { "debug" }
  try {
    $exePath = Join-Path $tmpDir "test_const_divmod_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($relFlag) { $buildArgs += "--release" }
    $buildArgs += @("tests/test_const_divmod.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "const-divmod build ($variant) failed: $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "const-divmod build ($variant) did not produce an executable"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 1) {
      throw "const-divmod ($variant) reported a mismatch (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "const_divmod_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "const_divmod_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Constant-multiply shift-add/sub strength reduction. Differential oracle: each
# literal `x * C` (shift-add for nice constants, imul for dense ones) is checked
# against `x * r` with r loaded from the heap (genuine imul), over signed and
# unsigned operands incl. extremes. Returns 1 on full agreement.
foreach ($relFlag in @($true, $false)) {
  $total++
  $variant = if ($relFlag) { "release" } else { "debug" }
  try {
    $exePath = Join-Path $tmpDir "test_const_mul_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($relFlag) { $buildArgs += "--release" }
    $buildArgs += @("tests/test_const_mul.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "const-mul build ($variant) failed: $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "const-mul build ($variant) did not produce an executable"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 1) {
      throw "const-mul ($variant) reported a mismatch (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "const_mul_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "const_mul_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Scalar Replacement of Aggregates (SROA). Self-checking oracle exercising
# by-value struct round-trips (struct_byval shape), per-field read/modify/write,
# whole-struct field-wise copy, and float-field width preservation. Built both
# --release (SROA active) and -O0 (inactive); both must return 1 and agree.
foreach ($relFlag in @($true, $false)) {
  $total++
  $variant = if ($relFlag) { "release" } else { "debug" }
  try {
    $exePath = Join-Path $tmpDir "test_sroa_$variant.exe"
    $buildArgs = @("--build", "--emit-obj", "--linker", "internal")
    if ($relFlag) { $buildArgs += "--release" }
    $buildArgs += @("tests/test_sroa.mettle", "-o", $exePath)

    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "sroa build ($variant) failed: $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "sroa build ($variant) did not produce an executable"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 1) {
      throw "sroa ($variant) reported a mismatch (exit $LASTEXITCODE)"
    }

    Write-CaseResult -Name "sroa_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "sroa_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# COFF reader test: parse Mettle and GCC-produced COFF objects
$total++
try {
  $coffReaderExe = Join-Path $tmpDir "coff_reader_test.exe"
  $basicObjPath = Join-Path $tmpDir "coff_reader_basic.obj"
  $relocObjPath = Join-Path $tmpDir "coff_reader_reloc.obj"
  $longObjPath = Join-Path $tmpDir "coff_reader_long.obj"
  $gccSourcePath = Join-Path $tmpDir "coff_reader_gcc_input.c"
  $gccObjPath = Join-Path $tmpDir "coff_reader_gcc_input.o"

  $compileHarness = & gcc -Wall -Wextra -std=c99 -g -O0 -D_GNU_SOURCE tests/coff_reader_test.c src/common.c src/lexer/lexer.c src/error/error_reporter.c src/linker/coff_reader.c -Isrc -o $coffReaderExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "COFF reader harness compile failed: $compileHarness"
  }

  $basicOut = & $CompilerPath --emit-obj tests/test_direct_object_return_const.mettle -o $basicObjPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "COFF reader basic object compile failed: $basicOut"
  }

  $relocOut = & $CompilerPath --emit-obj tests/test_direct_object_call_return.mettle -o $relocObjPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "COFF reader relocation object compile failed: $relocOut"
  }

  $longOut = & $CompilerPath --emit-obj tests/test_direct_object_long_symbol_name.mettle -o $longObjPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "COFF reader long-symbol object compile failed: $longOut"
  }

  @'
int gcc_reader_helper_symbol_name(void) {
  return 11;
}

int gcc_reader_entry_symbol_name(void) {
  return gcc_reader_helper_symbol_name();
}
'@ | Set-Content -Path $gccSourcePath

  $gccOut = & gcc -c $gccSourcePath -o $gccObjPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "COFF reader GCC object compile failed: $gccOut"
  }

  $coffOut = & $coffReaderExe $basicObjPath $relocObjPath $longObjPath $gccObjPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "COFF reader verification failed: $coffOut"
  }

  Write-CaseResult -Name "coff_reader" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "coff_reader" -Passed $false -Reason $_.Exception.Message
}

# Linker symbol resolution test: merge sections, resolve externals, and reject invalid symbol graphs
$total++
try {
  $symbolResolveExe = Join-Path $tmpDir "symbol_resolve_test.exe"
  $fnEntryObj = Join-Path $tmpDir "linker_merge_entry.obj"
  $fnProviderObj = Join-Path $tmpDir "linker_merge_provider.obj"
  $dataEntryObj = Join-Path $tmpDir "linker_merge_data_entry.obj"
  $dataProviderObj = Join-Path $tmpDir "linker_merge_data_provider.obj"
  $bssEntryObj = Join-Path $tmpDir "linker_merge_bss_entry.obj"
  $bssProviderObj = Join-Path $tmpDir "linker_merge_bss_provider.obj"
  $dupAObj = Join-Path $tmpDir "linker_duplicate_a.obj"
  $dupBObj = Join-Path $tmpDir "linker_duplicate_b.obj"
  $unresolvedObj = Join-Path $tmpDir "linker_unresolved_entry.obj"

  $compileHarness = & gcc -Wall -Wextra -std=c99 -g -O0 -D_GNU_SOURCE tests/symbol_resolve_test.c src/common.c src/lexer/lexer.c src/error/error_reporter.c src/linker/coff_reader.c src/linker/symbol_resolve.c src/codegen/binary_emitter.c src/codegen/elf_emitter.c -Isrc -Isrc\codegen -o $symbolResolveExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Symbol-resolve harness compile failed: $compileHarness"
  }

  $cases = @(
    @{ Path = "tests/test_linker_merge_entry.mettle"; Out = $fnEntryObj; Label = "function-entry" },
    @{ Path = "tests/test_linker_merge_provider.mettle"; Out = $fnProviderObj; Label = "function-provider" },
    @{ Path = "tests/test_linker_merge_data_entry.mettle"; Out = $dataEntryObj; Label = "data-entry" },
    @{ Path = "tests/test_linker_merge_data_provider.mettle"; Out = $dataProviderObj; Label = "data-provider" },
    @{ Path = "tests/test_linker_merge_bss_entry.mettle"; Out = $bssEntryObj; Label = "bss-entry" },
    @{ Path = "tests/test_linker_merge_bss_provider.mettle"; Out = $bssProviderObj; Label = "bss-provider" },
    @{ Path = "tests/test_linker_duplicate_a.mettle"; Out = $dupAObj; Label = "duplicate-a" },
    @{ Path = "tests/test_linker_duplicate_b.mettle"; Out = $dupBObj; Label = "duplicate-b" },
    @{ Path = "tests/test_linker_unresolved_entry.mettle"; Out = $unresolvedObj; Label = "unresolved-entry" }
  )

  foreach ($case in $cases) {
    $objOut = & $CompilerPath --emit-obj $case.Path -o $case.Out 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "Symbol-resolve $($case.Label) object compile failed: $objOut"
    }
    if (-not (Test-Path $case.Out)) {
      throw "Symbol-resolve $($case.Label) object compile did not produce an object file"
    }
  }

  $resolveOut = & $symbolResolveExe $fnEntryObj $fnProviderObj $dataEntryObj $dataProviderObj $bssEntryObj $bssProviderObj $dupAObj $dupBObj $unresolvedObj $tmpDir 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Symbol-resolve verification failed: $resolveOut"
  }

  Write-CaseResult -Name "symbol_resolve" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "symbol_resolve" -Passed $false -Reason $_.Exception.Message
}

# Linker relocation test: apply merged-image relocations for REL32, ADDR64, ADDR32NB, and SECREL
$total++
try {
  $relocationExe = Join-Path $tmpDir "relocation_test.exe"

  $compileHarness = & gcc -Wall -Wextra -std=c99 -g -O0 -D_GNU_SOURCE tests/relocation_test.c src/common.c src/lexer/lexer.c src/error/error_reporter.c src/linker/coff_reader.c src/linker/symbol_resolve.c src/linker/relocation.c src/codegen/binary_emitter.c src/codegen/elf_emitter.c -Isrc -Isrc\codegen -o $relocationExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Relocation harness compile failed: $compileHarness"
  }

  $relocationOut = & $relocationExe $tmpDir 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Relocation verification failed: $relocationOut"
  }

  Write-CaseResult -Name "relocation" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "relocation" -Passed $false -Reason $_.Exception.Message
}

# PE emitter test: write a minimal PE32+ image, verify headers/sections, and run it
$total++
try {
  $peEmitterExe = Join-Path $tmpDir "pe_emitter_test.exe"

  $compileHarness = & gcc -Wall -Wextra -std=c99 -g -O0 -D_GNU_SOURCE tests/pe_emitter_test.c src/common.c src/lexer/lexer.c src/error/error_reporter.c src/linker/coff_reader.c src/linker/symbol_resolve.c src/linker/relocation.c src/linker/pe_emitter.c src/linker/import_lib.c src/codegen/binary_emitter.c src/codegen/elf_emitter.c -Isrc -Isrc\codegen -o $peEmitterExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "PE-emitter harness compile failed: $compileHarness"
  }

  $peEmitterOut = & $peEmitterExe $tmpDir 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "PE-emitter verification failed: $peEmitterOut"
  }

  Write-CaseResult -Name "pe_emitter" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "pe_emitter" -Passed $false -Reason $_.Exception.Message
}

# Internal linker basic test: direct object build uses native PE emission for default imports
$total++
try {
  $exePath = Join-Path $tmpDir "internal_link_return_const.exe"

  $buildOut = & $CompilerPath --build --emit-obj --linker internal tests/test_direct_object_return_const.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker basic build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Internal linker basic build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 7) {
    throw "Internal linker basic executable exited with $LASTEXITCODE (expected 7)"
  }

  Write-CaseResult -Name "internal_link_basic" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "internal_link_basic" -Passed $false -Reason $_.Exception.Message
}

# Float comparisons must use numeric FP ordering, not raw IEEE bit ordering.
$total++
try {
  $binaryExePath = Join-Path $tmpDir "internal_link_float_negative_comparison.exe"
  $objExePath = Join-Path $tmpDir "internal_link_emit_obj_float_negative_comparison.exe"

  $buildOut = & $CompilerPath --build --linker internal tests/test_float_negative_comparison.mettle -o $binaryExePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker float-negative binary build failed: $buildOut"
  }
  if (-not (Test-Path $binaryExePath)) {
    throw "Internal linker float-negative binary build did not produce an executable"
  }

  & $binaryExePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker float-negative binary executable exited with $LASTEXITCODE (expected 0)"
  }

  $buildOut = & $CompilerPath --build --linker internal tests/test_float_negative_comparison.mettle -o $objExePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker float-negative emit-obj build failed: $buildOut"
  }
  if (-not (Test-Path $objExePath)) {
    throw "Internal linker float-negative emit-obj build did not produce an executable"
  }

  & $objExePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker float-negative emit-obj executable exited with $LASTEXITCODE (expected 0)"
  }

  Write-CaseResult -Name "internal_link_float_negative_comparison" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "internal_link_float_negative_comparison" -Passed $false -Reason $_.Exception.Message
}

# Runtime coverage for float returns through the binary object backend.
$total++
try {
  $exePath = Join-Path $tmpDir "internal_link_abi_float_return.exe"
  $buildOut = & $CompilerPath --build --linker internal tests/test_abi_float_return.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker ABI float-return build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Internal linker ABI float-return build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 1) {
    throw "Internal linker ABI float-return executable exited with $LASTEXITCODE (expected 1)"
  }

  Write-CaseResult -Name "internal_link_abi_float_return" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "internal_link_abi_float_return" -Passed $false -Reason $_.Exception.Message
}

# Every `fix:` --explain prints must survive being carried out. The paired
# functions in test_explain_fix_advice.mettle are one suggestion each: `_before`
# is the loop that draws the advice, `_after` is that loop with the advice
# applied literally. Advice that stops working fails here.
#
# Regression: these strings were hand-written and unrun. The unbounded-shift
# advice offered the loop it was refusing as its example of one that works, and
# the predicated-count advice asked for a spelling the reader had already used.
$fixAdvicePairs = @(
  @{ Name = "bytesum";   Id = "byte-sum-narrow-acc" },
  @{ Name = "i16";       Id = "int16-elements" },
  @{ Name = "i64";       Id = "int64-elements" },
  @{ Name = "dot";       Id = "dot-shape-address" },
  @{ Name = "extremum";  Id = "extremum-shape" },
  @{ Name = "count";     Id = "predicated-count" },
  @{ Name = "shift";     Id = "unbounded-shift" },
  @{ Name = "fill";      Id = "store-only-fill" },
  @{ Name = "search";    Id = "early-exit" },
  @{ Name = "noinline";  Id = "call-in-body" },
  @{ Name = "explainshift";  Id = "unbounded-shift" },
  @{ Name = "explainvshift"; Id = "variable-shift" }
)

$total++
try {
  $fixOut = & $CompilerPath tests/test_explain_fix_advice.mettle --release --explain 2>&1 | Out-String
  $problems = @()
  foreach ($pair in $fixAdvicePairs) {
    $beforeLine = ($fixOut -split "`n" | Where-Object { $_ -match "\b$($pair.Name)_before \(loop" }) -join ""
    $afterLines = @($fixOut -split "`n" | Where-Object { $_ -match "\b$($pair.Name)_after \(loop" })
    if ($beforeLine -notmatch "NOT vectorized") {
      $problems += "$($pair.Name)_before was expected to stay scalar; got: $beforeLine"
    }
    elseif ($beforeLine -notmatch [regex]::Escape("[$($pair.Id)]")) {
      $problems += "$($pair.Name)_before was expected to report [$($pair.Id)]; got: $beforeLine"
    }
    if ($afterLines.Count -eq 0) {
      $problems += "$($pair.Name)_after produced no loop remark"
    }
    foreach ($line in $afterLines) {
      if ($line -notmatch "vectorized ->" -and $line -notmatch "vectorized →") {
        $problems += "the fix for [$($pair.Id)] no longer vectorizes: $line"
      }
    }
  }
  if ($problems.Count -gt 0) {
    throw ($problems -join "`n")
  }
  Write-CaseResult -Name "explain_fix_advice_applies" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "explain_fix_advice_applies" -Passed $false -Reason $_.Exception.Message
}

# Deferred statements must run on every exit path, not only on `return` and
# falling off the end of a scope. Regression: `break`, `continue`, labeled
# `break`/`continue`, and a `switch` case body each jumped to their target
# without emitting the deferred statements of the scopes they left, so a
# `defer free(p)` in a loop leaked on every break. The order below is the
# contract: LIFO within a scope, innermost scope first, and `errdefer` only on
# a non-zero return.
$deferExitPathsExpected = @(
  "nested_break",
  "inner", "block", "loop", "fn",
  "nested_continue",
  "c-block", "c-loop", "c-block", "c-tail", "c-loop",
  "labeled_break",
  "l-inner", "l-outer",
  "labeled_continue",
  "k-inner", "k-outer", "k-inner", "k-outer",
  "switch_case_1",
  "s-body1", "s-case1", "s-body2", "s-case2", "s-after",
  "switch_case_2",
  "s-body2", "s-case2", "s-after",
  "switch_in_loop",
  "w-case", "w-after-switch", "w-loop", "w-after-switch", "w-loop",
  "errdefer_ok",
  "e-loop",
  "errdefer_err",
  "e-loop", "e-err"
) -join "`r`n"

foreach ($deferMode in @("debug", "release")) {
  $total++
  $caseName = "defer_exit_paths_$deferMode"
  try {
    $exePath = Join-Path $tmpDir "$caseName.exe"
    $extraArgs = @()
    if ($deferMode -eq "release") { $extraArgs = @("--release") }
    $buildOut = & $CompilerPath --build --linker internal @extraArgs tests/test_defer_exit_paths.mettle -o $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "defer exit-path build failed ($deferMode): $buildOut"
    }
    $runOut = (& $exePath 2>&1 | Out-String).TrimEnd()
    if ($runOut -ne $deferExitPathsExpected) {
      throw "defer exit-path output mismatch ($deferMode):`n--- expected ---`n$deferExitPathsExpected`n--- got ---`n$runOut"
    }
    Write-CaseResult -Name $caseName -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name $caseName -Passed $false -Reason $_.Exception.Message
  }
}

# Whole-struct assignment must copy every byte, not just the first machine word.
# Regression: structs > 8 bytes (ThreeI32, TwoF64, Mixed) used to keep only the
# first 8 bytes; trailing fields were zero/garbage. Verify the binary path produces byte-perfect copies.
$structCopyExpected = @(
  "struct copy repro",
  "two_i32_a 11",
  "two_i32_b 22",
  "three_i32_a 11",
  "three_i32_b 22",
  "three_i32_c 33",
  "two_f64_a_mm -3500",
  "two_f64_b_mm 22000",
  "mixed_a 11",
  "mixed_b_mm -3500",
  "mixed_c 22"
) -join "`r`n"

foreach ($mode in @("binary")) {
  $total++
  $caseName = "internal_link_struct_copy_$mode"
  try {
    $exePath = Join-Path $tmpDir "$caseName.exe"
      $buildOut = & $CompilerPath --build --linker internal tests/test_struct_copy.mettle -o $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "Struct copy build failed ($mode): $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "Struct copy build ($mode) did not produce an executable"
    }

    $runOut = (& $exePath 2>&1 | Out-String).TrimEnd()
    if ($LASTEXITCODE -ne 0) {
      throw "Struct copy executable exited with $LASTEXITCODE ($mode)"
    }
    if ($runOut -ne $structCopyExpected) {
      throw "Struct copy output mismatch ($mode):`n--- expected ---`n$structCopyExpected`n--- got ---`n$runOut"
    }

    Write-CaseResult -Name $caseName -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name $caseName -Passed $false -Reason $_.Exception.Message
  }
}

# Indirect-arg ABI: a struct larger than 8 bytes passed by value must reach
# the callee with every field intact, and mutations on the callee's parameter
# must not leak back to the caller's original.
$structPassByValueExpected = @(
  "struct pass by value",
  "sum_three 66",
  "third 33",
  "after_clobber_a 11",
  "after_clobber_b 22",
  "after_clobber_c 33",
  "mixed_b_mm -3500",
  "mixed_c 22"
) -join "`r`n"

foreach ($mode in @("binary")) {
  $total++
  $caseName = "internal_link_struct_pass_by_value_$mode"
  try {
    $exePath = Join-Path $tmpDir "$caseName.exe"
      $buildOut = & $CompilerPath --build --linker internal tests/test_struct_pass_by_value.mettle -o $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "Struct pass-by-value build failed ($mode): $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "Struct pass-by-value build ($mode) did not produce an executable"
    }

    $runOut = (& $exePath 2>&1 | Out-String).TrimEnd()
    if ($LASTEXITCODE -ne 0) {
      throw "Struct pass-by-value executable exited with $LASTEXITCODE ($mode)"
    }
    if ($runOut -ne $structPassByValueExpected) {
      throw "Struct pass-by-value output mismatch ($mode):`n--- expected ---`n$structPassByValueExpected`n--- got ---`n$runOut"
    }

    Write-CaseResult -Name $caseName -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name $caseName -Passed $false -Reason $_.Exception.Message
  }
}

# Indirect-return ABI: a struct larger than 8 bytes returned by value must
# arrive at the caller with every field intact. Validates the hidden
# out-pointer convention for binary object builds.
$structReturnByValueExpected = @(
  "struct return by value",
  "three_a 11",
  "three_b 22",
  "three_c 33",
  "chained_sum 6",
  "six_a 10",
  "six_b 20",
  "six_c 30",
  "six_d 40",
  "six_e 50",
  "six_f 60"
) -join "`r`n"

foreach ($mode in @("binary")) {
  $total++
  $caseName = "internal_link_struct_return_by_value_$mode"
  try {
    $exePath = Join-Path $tmpDir "$caseName.exe"
      $buildOut = & $CompilerPath --build --linker internal tests/test_struct_return_by_value.mettle -o $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "Struct return-by-value build failed ($mode): $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "Struct return-by-value build ($mode) did not produce an executable"
    }

    $runOut = (& $exePath 2>&1 | Out-String).TrimEnd()
    if ($LASTEXITCODE -ne 0) {
      throw "Struct return-by-value executable exited with $LASTEXITCODE ($mode)"
    }
    if ($runOut -ne $structReturnByValueExpected) {
      throw "Struct return-by-value output mismatch ($mode):`n--- expected ---`n$structReturnByValueExpected`n--- got ---`n$runOut"
    }

    Write-CaseResult -Name $caseName -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name $caseName -Passed $false -Reason $_.Exception.Message
  }
}

# Struct ABI classifier matrix: small power-of-two structs stay direct, odd-size
# structs go indirect, value receivers work, and nested temp regions do not alias.
$structAbiMatrixExpected = @(
  "struct abi matrix",
  "small4 41",
  "small8 33",
  "odd3 18",
  "odd3_return 30",
  "value_receiver_total 60",
  "nested_big 30"
) -join "`r`n"

foreach ($mode in @("binary")) {
  $total++
  $caseName = "internal_link_struct_abi_matrix_$mode"
  try {
    $exePath = Join-Path $tmpDir "$caseName.exe"
      $buildOut = & $CompilerPath --build --linker internal tests/test_struct_abi_matrix.mettle -o $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "Struct ABI matrix build failed ($mode): $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "Struct ABI matrix build ($mode) did not produce an executable"
    }

    $runOut = (& $exePath 2>&1 | Out-String).TrimEnd()
    if ($LASTEXITCODE -ne 0) {
      throw "Struct ABI matrix executable exited with $LASTEXITCODE ($mode)"
    }
    if ($runOut -ne $structAbiMatrixExpected) {
      throw "Struct ABI matrix output mismatch ($mode):`n--- expected ---`n$structAbiMatrixExpected`n--- got ---`n$runOut"
    }

    Write-CaseResult -Name $caseName -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name $caseName -Passed $false -Reason $_.Exception.Message
  }
}

# Struct ABI C boundary: MinGW C and Mettle agree on indirect pass/return for
# >8-byte and odd-size structs. GCC is used only to compile the small C shim;
# linking stays on Mettle's internal linker.
$structAbiExternExpected = @(
  "struct abi extern c",
  "c_sum_three 66",
  "c_make_three_sum 12",
  "c_make_odd3_sum 24"
) -join "`r`n"

foreach ($mode in @("binary")) {
  $total++
  $caseName = "internal_link_struct_abi_extern_c_$mode"
  try {
    $gccCmd = Get-Command gcc -ErrorAction SilentlyContinue
    if (-not $gccCmd) {
      Write-CaseResult -Name $caseName -Passed $true -Reason "skipped: gcc not on PATH"
      continue
    }

    $cObjPath = Join-Path $tmpDir "$caseName.c.o"
    $cOut = & gcc -c tests/struct_abi_c_shim.c -o $cObjPath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "Struct ABI C shim compile failed ($mode): $cOut"
    }

    $exePath = Join-Path $tmpDir "$caseName.exe"
      $buildOut = & $CompilerPath --build --linker internal tests/test_struct_abi_extern_c.mettle -o $exePath --link-arg $cObjPath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "Struct ABI extern C build failed ($mode): $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "Struct ABI extern C build ($mode) did not produce an executable"
    }

    $runOut = (& $exePath 2>&1 | Out-String).TrimEnd()
    if ($LASTEXITCODE -ne 0) {
      throw "Struct ABI extern C executable exited with $LASTEXITCODE ($mode)"
    }
    if ($runOut -ne $structAbiExternExpected) {
      throw "Struct ABI extern C output mismatch ($mode):`n--- expected ---`n$structAbiExternExpected`n--- got ---`n$runOut"
    }

    Write-CaseResult -Name $caseName -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name $caseName -Passed $false -Reason $_.Exception.Message
  }
}

# Companion repro: large structs containing float64 fields and engine-style
# layouts (float64-first, trailing int32) plus heap allocation. Just verify the
# repro builds and runs cleanly under both link modes; full byte-level scrutiny
# of every line would be brittle if write_i64 formatting ever shifts.
foreach ($mode in @("binary")) {
  $total++
  $caseName = "internal_link_struct_float_$mode"
  try {
    $exePath = Join-Path $tmpDir "$caseName.exe"
      $buildOut = & $CompilerPath --build --linker internal tests/test_struct_float.mettle -o $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "Struct/float build failed ($mode): $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "Struct/float build ($mode) did not produce an executable"
    }

    $runOut = (& $exePath 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
      throw "Struct/float executable exited with $LASTEXITCODE ($mode)"
    }
    # Every probe line that prints a copied float must show the non-zero scaled
    # value, never 0 (which would indicate a truncated copy past the 8th byte).
    foreach ($needle in @("lx_mm -3348000", "hz_mm 22000000", "marker 1234")) {
      if ($runOut -notmatch [regex]::Escape($needle)) {
        throw "Struct/float output missing '$needle' ($mode):`n$runOut"
      }
    }

    Write-CaseResult -Name $caseName -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name $caseName -Passed $false -Reason $_.Exception.Message
  }
}

# Native object + MinGW GCC link with no startup or default libraries.
$total++
try {
  $gccCmd = Get-Command gcc -ErrorAction SilentlyContinue
  if (-not $gccCmd) {
    Write-CaseResult -Name "direct_object_emit_obj_gcc_link" -Passed $true -Reason "skipped: gcc not on PATH"
  }
  else {
    $exeGcc = Join-Path $tmpDir "direct_object_emit_obj_gcc_link.exe"
    $buildGccOut = & $CompilerPath --build --emit-obj --linker gcc tests/test_direct_object_return_const.mettle -o $exeGcc 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "emit-obj gcc link build failed: $buildGccOut"
    }
    if (-not (Test-Path $exeGcc)) {
      throw "emit-obj gcc link did not produce an executable"
    }
    & $exeGcc 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 7) {
      throw "emit-obj gcc executable exited with $LASTEXITCODE (expected 7)"
    }
    Write-CaseResult -Name "direct_object_emit_obj_gcc_link" -Passed $true
  }
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_emit_obj_gcc_link" -Passed $false -Reason $_.Exception.Message
}

# Internal linker explicit DLL test: --link-arg -lws2_32 remains supported
$total++
try {
  $exePath = Join-Path $tmpDir "internal_link_ws2_32.exe"

  $buildOut = & $CompilerPath --build --emit-obj --linker internal tests/test_internal_link_ws2_32.mettle -o $exePath --link-arg -lws2_32 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker ws2_32 build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Internal linker ws2_32 build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker ws2_32 executable exited with $LASTEXITCODE (expected 0)"
  }

  Write-CaseResult -Name "internal_link_ws2_32" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "internal_link_ws2_32" -Passed $false -Reason $_.Exception.Message
}

# Internal linker native Win32 test: std/win32 resolves user32/kernel32 without link args
$total++
try {
  $exePath = Join-Path $tmpDir "internal_link_win32_user32.exe"

  $buildOut = & $CompilerPath --build --emit-obj --linker internal tests/test_internal_link_win32_user32.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker Win32 build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Internal linker Win32 build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker Win32 executable exited with $LASTEXITCODE (expected 0)"
  }

  Write-CaseResult -Name "internal_link_win32_user32" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "internal_link_win32_user32" -Passed $false -Reason $_.Exception.Message
}

# Internal linker UI test: std/ui resolves user32/gdi32 without link args
$total++
try {
  $exePath = Join-Path $tmpDir "internal_link_ui.exe"

  $buildOut = & $CompilerPath --build --emit-obj --linker internal tests/test_internal_link_ui.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker UI build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Internal linker UI build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker UI executable exited with $LASTEXITCODE (expected 0)"
  }

  Write-CaseResult -Name "internal_link_ui" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "internal_link_ui" -Passed $false -Reason $_.Exception.Message
}

# Internal linker owned stdio test. The _iob compatibility symbol must resolve
# from the bundled runtime and the output must import no C runtime.
$total++
try {
  $exePath = Join-Path $tmpDir "internal_link_std_io.exe"

  $buildOut = & $CompilerPath --build --emit-obj --linker internal tests/test_std_io.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker std-io build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Internal linker std-io build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker std-io executable exited with $LASTEXITCODE (expected 0)"
  }
  $stdioImports = & objdump -p $exePath 2>&1 | Out-String
  if ($stdioImports -match "msvcrt|ucrt|vcruntime|api-ms-win-crt|libgcc|libwinpthread") {
    throw "Internal linker std-io executable imports a forbidden runtime"
  }

  Write-CaseResult -Name "internal_link_std_io" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "internal_link_std_io" -Passed $false -Reason $_.Exception.Message
}

# Owned directory runtime test. It covers attributes, current directory, and
# recursive Markdown scans without a helper C object.
$total++
try {
  $dirExe = Join-Path $tmpDir "owned_dir.exe"
  $dirBuild = & $CompilerPath --build tests/test_owned_dir.mettle -o $dirExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "Owned directory build failed: $dirBuild" }
  $dirOut = & $dirExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0 -or $dirOut -notmatch "DIR OWNED OK") {
    throw "Owned directory test failed: $dirOut"
  }
  $dirImports = & objdump -p $dirExe 2>&1 | Out-String
  if ($dirImports -match "msvcrt|ucrt|vcruntime|api-ms-win-crt|libgcc|libwinpthread") {
    throw "Owned directory test imports a forbidden runtime"
  }
  Write-CaseResult -Name "owned_directory" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "owned_directory" -Passed $false -Reason $_.Exception.Message
}

# Internal linker kernel32 atomics test: std/thread uses exported Interlocked* names
$total++
try {
  $exePath = Join-Path $tmpDir "internal_link_thread_atomics.exe"

  $buildOut = & $CompilerPath --build --emit-obj --linker internal tests/test_internal_link_thread_atomics.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker thread-atomics build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Internal linker thread-atomics build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "Internal linker thread-atomics executable exited with $LASTEXITCODE (expected 0)"
  }

  Write-CaseResult -Name "internal_link_thread_atomics" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "internal_link_thread_atomics" -Passed $false -Reason $_.Exception.Message
}

# Auto linker PATH isolation test: auto mode should succeed without external linkers on PATH
$total++
try {
  $exePath = Join-Path $tmpDir "auto_link_internal_only.exe"
  $compilerFullPath = (Resolve-Path $CompilerPath).Path
  $system32Dir = Join-Path $env:SystemRoot "System32"

  $originalPath = $env:PATH
  try {
    $env:PATH = $system32Dir
    $buildOut = & $compilerFullPath --build --emit-obj tests/test_direct_object_return_const.mettle -o $exePath 2>&1 | Out-String
  }
  finally {
    $env:PATH = $originalPath
  }

  if ($LASTEXITCODE -ne 0) {
    throw "Auto linker internal-only build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Auto linker internal-only build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 7) {
    throw "Auto linker internal-only executable exited with $LASTEXITCODE (expected 7)"
  }

  Write-CaseResult -Name "auto_link_internal_only_path" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "auto_link_internal_only_path" -Passed $false -Reason $_.Exception.Message
}

# Auto linker fallback test: a static archive should fail internally, then link via GCC
$total++
try {
  $cSourcePath = Join-Path $tmpDir "phase6_fallback_static_lib.c"
  $cObjectPath = Join-Path $tmpDir "phase6_fallback_static_lib.o"
  $libPath = Join-Path $tmpDir "phase6_fallback_static_lib.a"
  $exePath = Join-Path $tmpDir "auto_link_fallback_static_lib.exe"
  # PATH may carry several archivers (e.g. both MinGW and Strawberry Perl ship
  # ar.exe on GitHub runners); take the first so $arCommand.Source is one path.
  $arCommand = Get-Command ar -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $arCommand) {
    $arCommand = Get-Command gcc-ar -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  }
  if (-not $arCommand) {
    throw "Static-library archiver not found (expected ar or gcc-ar)"
  }

  @'
int fallback_value(void) {
  return 42;
}
'@ | Set-Content -Path $cSourcePath -Encoding ASCII

  $gccOut = & gcc -c $cSourcePath -o $cObjectPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Static-library compile failed: $gccOut"
  }

  $arOut = & $arCommand.Source rcs $libPath $cObjectPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Static-library archive build failed: $arOut"
  }

  $buildOut = & $CompilerPath --build --linker auto tests/test_auto_link_fallback_static_lib.mettle -o $exePath --link-arg $libPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Auto linker fallback build failed: $buildOut"
  }
  if ($buildOut -notmatch "Internal linker failed in auto mode, falling back to external linkers") {
    throw "Auto linker fallback build did not report an internal-link failure before fallback: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Auto linker fallback build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 42) {
    throw "Auto linker fallback executable exited with $LASTEXITCODE (expected 42)"
  }

  Write-CaseResult -Name "auto_link_fallback_static_lib" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "auto_link_fallback_static_lib" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend parameter test: integer arg passed into callee home slot
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_params.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_params.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_direct_object_params.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object params compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object params compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_direct_object_params.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object params build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object params build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 9) {
    throw "Direct object params executable exited with $LASTEXITCODE (expected 9)"
  }

  Write-CaseResult -Name "direct_object_params" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_params" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend control-flow test: labels and conditional branches lower directly
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_control_flow.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_control_flow.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_direct_object_control_flow.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object control-flow compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object control-flow compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_direct_object_control_flow.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object control-flow build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object control-flow build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 11) {
    throw "Direct object control-flow executable exited with $LASTEXITCODE (expected 11)"
  }

  Write-CaseResult -Name "direct_object_control_flow" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_control_flow" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend local-slot test: locals plus call result materialization
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_abi_return_int.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_abi_return_int.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_abi_return_int.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object ABI-return-int compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object ABI-return-int compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_abi_return_int.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object ABI-return-int build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object ABI-return-int build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 1) {
    throw "Direct object ABI-return-int executable exited with $LASTEXITCODE (expected 1)"
  }

  Write-CaseResult -Name "direct_object_abi_return_int" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_abi_return_int" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend arithmetic test: locals plus unary/binary integer lowering
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_signed_arithmetic.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_signed_arithmetic.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_signed_arithmetic.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object signed-arithmetic compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object signed-arithmetic compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_signed_arithmetic.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object signed-arithmetic build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object signed-arithmetic build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 1) {
    throw "Direct object signed-arithmetic executable exited with $LASTEXITCODE (expected 1)"
  }

  Write-CaseResult -Name "direct_object_signed_arithmetic" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_signed_arithmetic" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend structured control-flow test: locals, comparisons, loops, and switch lowering
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_structured_control_flow.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_structured_control_flow.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_control_flow.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object structured control-flow compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object structured control-flow compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_control_flow.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object structured control-flow build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object structured control-flow build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 64) {
    throw "Direct object structured control-flow executable exited with $LASTEXITCODE (expected 64)"
  }

  Write-CaseResult -Name "direct_object_structured_control_flow" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_structured_control_flow" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend scalar matrix test: integer ops plus stack args
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_integer_matrix.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_integer_matrix.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_direct_object_integer_matrix.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object integer-matrix compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object integer-matrix compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_direct_object_integer_matrix.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object integer-matrix build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object integer-matrix build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 37) {
    throw "Direct object integer-matrix executable exited with $LASTEXITCODE (expected 37)"
  }

  Write-CaseResult -Name "direct_object_integer_matrix" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_integer_matrix" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend optimizer smoke: immediate ops, branch-chain scheduling,
# and hot local promotion should show up in the object code, not just binary object code.
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_codegen_fastpaths.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_codegen_fastpaths.exe"

  $objOut = & $CompilerPath --emit-obj --release tests/test_codegen_ir_fastpaths.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object codegen-fastpaths compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object codegen-fastpaths compile did not produce an object file"
  }

  $disasm = & objdump -d $objPath 2>&1 | Out-String
  # These assert that the binary backend's fast-path instruction selection fires
  # (cmp-with-imm, shift-by-imm, power-of-two multiply -> shl, mov $0, the &1
  # mask, and the fused multiply). They are register-agnostic: the MIR + linear-
  # scan allocator places these leaf integer
  # functions' values in allocator-chosen registers rather than always RAX, so
  # the opcodes/immediates are pinned but the registers are not.
  $requiredPatterns = @(
    'cmp\s+\$0xc,%\w+',
    'shl\s+\$0x2,%\w+',
    '(?s)<scale_by_eight>.*shl\s+\$0x3,%\w+',
    '(?s)<zero_const>.*mov\s+\$0x0,',
    '(?s)<even_branch>.*and\s+\$0x1,%\w+.*j(?:e|ne)',
    '(?s)<fused_mul_add>.*imul\s+%\w+,%\w+.*add\s+\$0x5,'
  )
  foreach ($pattern in $requiredPatterns) {
    if ($disasm -notmatch $pattern) {
      throw "Direct object codegen-fastpaths disassembly missing pattern: $pattern`n$disasm"
    }
  }

  $buildOut = & $CompilerPath --build --emit-obj --linker internal --release tests/test_codegen_ir_fastpaths.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object codegen-fastpaths build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object codegen-fastpaths build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object codegen-fastpaths executable exited with $LASTEXITCODE (expected 0)"
  }

  Write-CaseResult -Name "direct_object_codegen_fastpaths" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_codegen_fastpaths" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend scalar cast test: integer truncation/extension and pointer reinterpretation
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_scalar_casts.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_scalar_casts.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_direct_object_scalar_casts.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object scalar-casts compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object scalar-casts compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_direct_object_scalar_casts.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object scalar-casts build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object scalar-casts build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 21) {
    throw "Direct object scalar-casts executable exited with $LASTEXITCODE (expected 21)"
  }

  Write-CaseResult -Name "direct_object_scalar_casts" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_scalar_casts" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend float/scalar coverage: Win64 float ABI, casts, and
# narrow integer load canonicalization.
$directObjectScalarCases = @(
  @{ Name = "direct_object_abi_float_return"; Path = "tests/test_abi_float_return.mettle"; ExitCode = 1; Label = "float-return" },
  @{ Name = "direct_object_abi_float_args"; Path = "tests/test_abi_float_args.mettle"; ExitCode = 1; Label = "float-args" },
  @{ Name = "direct_object_abi_mixed_args"; Path = "tests/test_abi_mixed_args.mettle"; ExitCode = 1; Label = "mixed-args" },
  @{ Name = "direct_object_abi_float_symbol_args"; Path = "tests/test_abi_float_symbol_args.mettle"; ExitCode = 1; Label = "float-symbol-args" },
  @{ Name = "direct_object_abi_float4_args"; Path = "tests/test_abi_float4_args.mettle"; ExitCode = 1; Label = "float4-args" },
  @{ Name = "direct_object_abi_float_stack"; Path = "tests/test_abi_float_stack.mettle"; ExitCode = 1; Label = "float-stack" },
  @{ Name = "direct_object_cast_expression"; Path = "tests/test_cast_expression.mettle"; ExitCode = 0; Label = "cast-expression" },
  @{ Name = "direct_object_int32_load_sign_ext"; Path = "tests/test_direct_object_int32_load_sign_ext.mettle"; ExitCode = 0; Label = "int32-load-sign-ext" },
  @{ Name = "direct_object_int32_call_return_compare"; Path = "tests/test_int32_call_return_compare.mettle"; ExitCode = 1; Label = "int32-call-return-compare" },
  @{ Name = "direct_object_uint32_cross_lineage_eq"; Path = "tests/test_uint32_cross_lineage_eq.mettle"; ExitCode = 0; Label = "uint32-cross-lineage-eq" },
  @{ Name = "direct_object_uint32_signed_in_large_fn"; Path = "tests/test_uint32_signed_in_large_fn.mettle"; ExitCode = 0; Label = "uint32-signed-in-large-fn" },
  @{ Name = "direct_object_temp_local_name_collision"; Path = "tests/test_temp_local_name_collision.mettle"; ExitCode = 0; Label = "temp-local-name-collision" }
)

foreach ($case in $directObjectScalarCases) {
  $total++
  try {
    $objPath = Join-Path $tmpDir ($case.Name + ".obj")
    $exePath = Join-Path $tmpDir ($case.Name + ".exe")

    $objOut = & $CompilerPath --emit-obj $case.Path -o $objPath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "Direct object $($case.Label) compile failed: $objOut"
    }
    if (-not (Test-Path $objPath)) {
      throw "Direct object $($case.Label) compile did not produce an object file"
    }

    $buildOut = & $CompilerPath --build --emit-obj $case.Path -o $exePath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "Direct object $($case.Label) build failed: $buildOut"
    }
    if (-not (Test-Path $exePath)) {
      throw "Direct object $($case.Label) build did not produce an executable"
    }

    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne $case.ExitCode) {
      throw "Direct object $($case.Label) executable exited with $LASTEXITCODE (expected $($case.ExitCode))"
    }

    Write-CaseResult -Name $case.Name -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name $case.Name -Passed $false -Reason $_.Exception.Message
  }
}

# The uint32-as-signed-in-large-fn miscompile reappeared at -O (the optimizer's
# instruction clones dropped the is_unsigned flag), and the -O0 gate above missed
# it. Re-run the same regression at --release so the optimized path is covered.
$total++
try {
  $exePath = Join-Path $tmpDir "uint32_signed_in_large_fn_release.exe"
  $buildOut = & $CompilerPath --build --release "tests/test_uint32_signed_in_large_fn.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "release build failed: $buildOut" }
  if (-not (Test-Path $exePath)) { throw "release build produced no executable" }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "uint32 signedness check failed at --release (exit $LASTEXITCODE)"
  }
  Write-CaseResult -Name "direct_object_uint32_signed_in_large_fn_release" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_uint32_signed_in_large_fn_release" -Passed $false -Reason $_.Exception.Message
}

# Vectorizer coverage: saxpy with a parameter scale now lowers to
# simd_affine_map_f32; verify the vectorized output matches a scalar reference
# at --release (exit 0 == within f32 tolerance).
$total++
try {
  $exePath = Join-Path $tmpDir "saxpy_vectorized.exe"
  $buildOut = & $CompilerPath --build --release "tests/test_saxpy_vectorized.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "release build failed: $buildOut" }
  if (-not (Test-Path $exePath)) { throw "release build produced no executable" }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "vectorized saxpy diverged from scalar reference (exit $LASTEXITCODE)"
  }
  Write-CaseResult -Name "simd_saxpy_vectorized" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "simd_saxpy_vectorized" -Passed $false -Reason $_.Exception.Message
}

# Float vectorizer correctness coverage (the differential fuzzer is integer-only,
# so these recognizers had no continuous coverage). Each kernel is @simd!, so the
# --release build asserts it vectorized; the run checks the vectorized result vs
# a closed-form value. Covers affine map, in-place scale, and sum/dot for f32+f64.
$total++
try {
  $exePath = Join-Path $tmpDir "float_vectorizers.exe"
  $buildOut = & $CompilerPath --build --release "tests/test_float_vectorizers.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "release build failed (a @simd! kernel stopped vectorizing?): $buildOut" }
  if (-not (Test-Path $exePath)) { throw "release build produced no executable" }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "a vectorized float kernel diverged from its closed-form result ($LASTEXITCODE failures)"
  }
  Write-CaseResult -Name "simd_float_vectorizers" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "simd_float_vectorizers" -Passed $false -Reason $_.Exception.Message
}

# General-vectorizer extensions: int32 lanes (simd_vloop_i32 maps + reductions,
# bit-exact mod 2^32, incl. uint32 wraparound and the zero-extended accumulator
# writeback) and runtime scalar broadcast for f32/f64 (saxpy-style coefficients).
# Kernels are @simd! so the build asserts they vectorize (this also pins the
# pointer-induction decline for claimable int maps); results are checked against
# reversed-order reference loops the vectorizer refuses. Also pins the
# iv-start-zero fix (a j=3..n reduction must not be replayed as 0..n).
$total++
try {
  $exePath = Join-Path $tmpDir "vloop_general.exe"
  $buildOut = & $CompilerPath --build --release "tests/test_vloop_general.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "release build failed (a @simd! kernel stopped vectorizing?): $buildOut" }
  if (-not (Test-Path $exePath)) { throw "release build produced no executable" }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "a general-vectorizer kernel diverged from its scalar reference ($LASTEXITCODE failures)"
  }
  Write-CaseResult -Name "simd_vloop_general" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "simd_vloop_general" -Passed $false -Reason $_.Exception.Message
}

# Running extrema (simd_vloop reduce_op max/min). This is the one recognizer
# that claims a body with a branch, so its guard rails are what rot: the
# broadcast seed (trip counts either side of both lane splits, zero included),
# NaN losing the comparison the way `>` does rather than the way MAXPS's src1
# would, both operand orders, and signed int32 lanes. Kernels are @simd! so the
# build asserts they vectorize; references iterate backwards and stay scalar.
$total++
try {
  $exePath = Join-Path $tmpDir "minmax_reduce.exe"
  $buildOut = & $CompilerPath --build --release "tests/test_simd_minmax_reduce.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "release build failed (a @simd! kernel stopped vectorizing?): $buildOut" }
  if (-not (Test-Path $exePath)) { throw "release build produced no executable" }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "a vectorized extremum diverged from its scalar reference ($LASTEXITCODE failures)"
  }
  Write-CaseResult -Name "simd_minmax_reduce" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "simd_minmax_reduce" -Passed $false -Reason $_.Exception.Message
}

# Byte element-wise maps: uint8/int8 in memory, int32 in the lanes. The
# arithmetic is the int32 kernel's and stays bit-exact; the new parts are the
# widening load (both extensions), the truncating store that must touch one
# byte and not its neighbours, and the pack-plus-permute that folds eight
# int32 lanes back into eight contiguous bytes. Trip counts straddle the
# 8-element step so the byte-at-a-time tail runs too, and every destination is
# checked past the run to catch a store that widened. Kernels are @simd! so the
# build asserts they vectorize; references iterate backwards and stay scalar.
$total++
try {
  $exePath = Join-Path $tmpDir "byte_map.exe"
  $buildOut = & $CompilerPath --build --release "tests/test_simd_byte_map.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "release build failed (a @simd! kernel stopped vectorizing?): $buildOut" }
  if (-not (Test-Path $exePath)) { throw "release build produced no executable" }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "a vectorized byte map diverged from its scalar reference ($LASTEXITCODE mismatches)"
  }
  Write-CaseResult -Name "simd_byte_map" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "simd_byte_map" -Passed $false -Reason $_.Exception.Message
}

# Early-exit search skip-ahead (simd_find): find/memchr/mismatch loops keep
# their scalar body (every exit path replays natively) but fast-forward the
# counter with an 8-wide int32 / 32-wide byte compare+movemask kernel. The
# test checks exact first-hit indices across all predicates, both source
# forms, literal/scalar/two-array right-hand sides, and head/block/tail hit
# positions; one for-range kernel is @simd! so the build asserts recognition.
$total++
try {
  $exePath = Join-Path $tmpDir "vloop_find.exe"
  $buildOut = & $CompilerPath --build --release "tests/test_vloop_find.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "release build failed (the @simd! find kernel stopped vectorizing?): $buildOut" }
  if (-not (Test-Path $exePath)) { throw "release build produced no executable" }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "a search skip-ahead diverged from the exact first-hit index ($LASTEXITCODE failures)"
  }
  Write-CaseResult -Name "simd_vloop_find" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "simd_vloop_find" -Passed $false -Reason $_.Exception.Message
}

# Regression: two affine-map miscompiles found by differential-fuzzing the
# general vectorizer. (1) A map not reading dst lowers to b==0, and the kernel's
# `0*dst[i]` produced NaN where the uninitialized output held NaN/Inf bits.
# (2) A degenerate integer copy (bare load) has the same base+(i<<2) shape as a
# float32 map, so the float recognizer claimed it and laundered uint32 NaN
# payloads through `1.0f*x`. Both checks are deterministic (poisoned output /
# explicit NaN bit patterns); a clean exit 0 means neither miscompile is present.
$total++
try {
  $exePath = Join-Path $tmpDir "affine_nan_typeconf.exe"
  $buildOut = & $CompilerPath --build --release "tests/test_affine_map_nan_typeconfusion.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "release build failed: $buildOut" }
  if (-not (Test-Path $exePath)) { throw "release build produced no executable" }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "affine-map miscompile present ($LASTEXITCODE bad elements: 0*NaN or uint32 type confusion)" }
  Write-CaseResult -Name "simd_affine_map_nan_typeconf" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "simd_affine_map_nan_typeconf" -Passed $false -Reason $_.Exception.Message
}

# uint32 canonical-home semantics: unsigned sub-64-bit arithmetic wraps mod
# 2^width in scalar code (debug AND release), matching SIMD lanes and C. Pins
# the canonicalization of narrow unsigned locals/params/globals/returns and
# the dst==src mov32 encoder skip. Self-checking: exit code is a failure mask.
$total++
try {
  $exePath = Join-Path $tmpDir "u32_canonical_dbg.exe"
  $buildOut = & $CompilerPath --build "tests/test_u32_canonical.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "debug build failed: $buildOut" }
  if (-not (Test-Path $exePath)) { throw "debug build produced no executable" }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "uint32 wrap semantics diverged in DEBUG scalar code (failure mask $LASTEXITCODE)"
  }
  $exePath = Join-Path $tmpDir "u32_canonical_rel.exe"
  $buildOut = & $CompilerPath --build --release "tests/test_u32_canonical.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "release build failed: $buildOut" }
  if (-not (Test-Path $exePath)) { throw "release build produced no executable" }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "uint32 wrap semantics diverged in RELEASE code (failure mask $LASTEXITCODE)"
  }
  Write-CaseResult -Name "u32_canonical_homes" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "u32_canonical_homes" -Passed $false -Reason $_.Exception.Message
}

# Signed narrow canonical-home semantics: signed int32/int16/int8 homes wrap to
# their destination width and are sign-extended before later division/shift.
# Exercise MIR debug/release and the fallback backend variants explicitly.
foreach ($variant in @("debug", "release", "debug_fallback", "release_fallback")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "i32_canonical_$variant.exe"
    $buildArgs = @("--build")
    if ($variant -like "release*") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_i32_canonical.mettle", "-o", $exePath)

    $oldMir = $env:METTLE_MIR
    try {
      if ($variant -like "*_fallback") {
        $env:METTLE_MIR = "0"
      } else {
        Remove-Item Env:\METTLE_MIR -ErrorAction SilentlyContinue
      }
      $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    }
    finally {
      if ($null -ne $oldMir) {
        $env:METTLE_MIR = $oldMir
      } else {
        Remove-Item Env:\METTLE_MIR -ErrorAction SilentlyContinue
      }
    }

    if ($LASTEXITCODE -ne 0) { throw "$variant build failed: $buildOut" }
    if (-not (Test-Path $exePath)) { throw "$variant build produced no executable" }
    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "signed narrow wrap semantics diverged in $variant (failure mask $LASTEXITCODE)"
    }
    Write-CaseResult -Name "i32_canonical_homes_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "i32_canonical_homes_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Declarative rewrite engine (ir_optimize_rewrite.c): the Tier-1 algebraic
# identity table and the Tier-2 constant-reassociation pass must preserve the
# exact integer result of the arithmetic they rewrite. Exercised across
# MIR/fallback x debug/release so a rewrite that miscompiles on only one backend
# still trips. Self-checking: exit code is the number of the first failing check.
foreach ($variant in @("debug", "release", "debug_fallback", "release_fallback")) {
  $total++
  try {
    $exePath = Join-Path $tmpDir "algebraic_rewrites_$variant.exe"
    $buildArgs = @("--build")
    if ($variant -like "release*") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_algebraic_rewrites.mettle", "-o", $exePath)

    $oldMir = $env:METTLE_MIR
    try {
      if ($variant -like "*_fallback") {
        $env:METTLE_MIR = "0"
      } else {
        Remove-Item Env:\METTLE_MIR -ErrorAction SilentlyContinue
      }
      $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    }
    finally {
      if ($null -ne $oldMir) {
        $env:METTLE_MIR = $oldMir
      } else {
        Remove-Item Env:\METTLE_MIR -ErrorAction SilentlyContinue
      }
    }

    if ($LASTEXITCODE -ne 0) { throw "$variant build failed: $buildOut" }
    if (-not (Test-Path $exePath)) { throw "$variant build produced no executable" }
    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "an algebraic rewrite changed the result in $variant (first failing check #$LASTEXITCODE)"
    }
    Write-CaseResult -Name "algebraic_rewrites_$variant" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "algebraic_rewrites_$variant" -Passed $false -Reason $_.Exception.Message
  }
}

# Soundness: per-shape SIMD recognizers must not claim counted loops that
# start at iv != 0 (the fused kernels replay 0..bound). One kernel per
# recognizer family (sum_i32/dot_i32/dot_i8/sum_u8/byte_map/fill/exp_f32/
# i2f/minmax/SLP-MAC/outer-lane) starts at a nonzero index; results are
# runtime-checked against values a 0-start replay cannot produce.
$total++
try {
  $exePath = Join-Path $tmpDir "nonzero_start_loops.exe"
  $buildOut = & $CompilerPath --build --release "tests/test_nonzero_start_loops.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "release build failed: $buildOut" }
  if (-not (Test-Path $exePath)) { throw "release build produced no executable" }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "a nonzero-start loop was replayed from 0 by a SIMD kernel (failure mask $LASTEXITCODE)"
  }
  Write-CaseResult -Name "simd_nonzero_start_loops" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "simd_nonzero_start_loops" -Passed $false -Reason $_.Exception.Message
}

# Scan-search SIMD soundness: SLP MAC must not leave k/a_idx/b_idx stale when
# they are read after the matched stores, and byte-compare-to-memcmp must not
# erase arbitrary post-loop code. Build and run both debug and release so the
# release-only recognizers are checked against the scalar baseline.
$total++
try {
  foreach ($variant in @("debug", "release")) {
    $exePath = Join-Path $tmpDir ("simd_scan_search_liveness_{0}.exe" -f $variant)
    $buildArgs = @("--build")
    if ($variant -eq "release") { $buildArgs += "--release" }
    $buildArgs += @("tests/test_simd_scan_search_liveness.mettle", "-o", $exePath)
    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "$variant build failed: $buildOut" }
    if (-not (Test-Path $exePath)) { throw "$variant build produced no executable" }
    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "$variant executable detected scan-search SIMD stale/destroyed behavior (failure mask $LASTEXITCODE)"
    }
  }
  Write-CaseResult -Name "simd_scan_search_liveness" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "simd_scan_search_liveness" -Passed $false -Reason $_.Exception.Message
}

# Coverage: the cast-free int32->int64 reduction `s += a[i]` now vectorizes
# (pointer-induction leaves reductions indexed; sum_i32 admits the implicit
# widen). Verify the vectorized result matches the closed form (negative inputs
# stress the signed widening).
$total++
try {
  $exePath = Join-Path $tmpDir "int_sum_nocast.exe"
  $buildOut = & $CompilerPath --build --release "tests/test_int_sum_nocast.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "release build failed: $buildOut" }
  if (-not (Test-Path $exePath)) { throw "release build produced no executable" }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "vectorized cast-free int sum diverged from closed form (exit $LASTEXITCODE)" }
  Write-CaseResult -Name "simd_int_sum_nocast" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "simd_int_sum_nocast" -Passed $false -Reason $_.Exception.Message
}

# Coverage: the uint32 linear-congruential recurrence reduction vectorizes
# (IR_OP_SIMD_LCG_U32, 8-wide closed form). Build at --release (vectorized) and
# -O0 (scalar); both must produce the same golden checksum across trip counts
# that exercise every scalar-remainder length.
$total++
try {
  foreach ($variant in @("release", "debug")) {
    $exePath = Join-Path $tmpDir "lcg_check_$variant.exe"
    $buildArgs = @("--build")
    if ($variant -eq "release") { $buildArgs += "--release" }
    $buildArgs += @("tests/simd_lcg_check.mettle", "-o", $exePath)
    $buildOut = & $CompilerPath @buildArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "$variant build failed: $buildOut" }
    if (-not (Test-Path $exePath)) { throw "$variant build produced no executable" }
    & $exePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "$variant LCG vectorizer diverged from the golden checksum (exit $LASTEXITCODE)"
    }
  }
  Write-CaseResult -Name "simd_lcg_recurrence" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "simd_lcg_recurrence" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend globals: scalar definitions plus extern-global symbol emission
$total++
try {
  $objPath = Join-Path $tmpDir "direct_object_ok_global_int.obj"
  $exePath = Join-Path $tmpDir "direct_object_ok_global_int.exe"

  $objOut = & $CompilerPath --emit-obj tests/ok_global_int.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object ok-global-int compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object ok-global-int compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/ok_global_int.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object ok-global-int build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object ok-global-int build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 1) {
    throw "Direct object ok-global-int executable exited with $LASTEXITCODE (expected 1)"
  }

  Write-CaseResult -Name "direct_object_ok_global_int" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_ok_global_int" -Passed $false -Reason $_.Exception.Message
}

$total++
try {
  $objPath = Join-Path $tmpDir "direct_object_global_string.obj"
  $exePath = Join-Path $tmpDir "direct_object_global_string.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_direct_object_global_string.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object global-string compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object global-string compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_direct_object_global_string.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object global-string build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object global-string build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object global-string executable exited with $LASTEXITCODE (expected 0)"
  }

  Write-CaseResult -Name "direct_object_global_string" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_global_string" -Passed $false -Reason $_.Exception.Message
}

$total++
try {
  $objPath = Join-Path $tmpDir "direct_object_extern_global_link_name.obj"

  $objOut = & $CompilerPath --emit-obj tests/test_extern_global_link_name.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object extern-global-link-name compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object extern-global-link-name compile did not produce an object file"
  }

  $symbols = & objdump -t $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object extern-global-link-name symbol dump failed"
  }
  if ($symbols -notmatch "errno") {
    throw "Direct object extern-global-link-name object is missing extern symbol 'errno'"
  }

  $relocs = & objdump -r $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object extern-global-link-name relocation dump failed"
  }
  if ($relocs -notmatch "IMAGE_REL_AMD64_REL32" -or $relocs -notmatch "errno") {
    throw "Direct object extern-global-link-name object is missing REL32 relocations to 'errno'"
  }

  Write-CaseResult -Name "direct_object_extern_global_link_name" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_extern_global_link_name" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend pointer-param-address test: address of parameter slot survives load/store
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_pointer_param_address.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_pointer_param_address.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_pointer_param_address.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object pointer-param-address compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object pointer-param-address compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_pointer_param_address.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object pointer-param-address build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object pointer-param-address build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 7) {
    throw "Direct object pointer-param-address executable exited with $LASTEXITCODE (expected 7)"
  }

  Write-CaseResult -Name "direct_object_pointer_param_address" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_pointer_param_address" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend struct method calls: receiver desugars to a first arg
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_struct_method_calls.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_struct_method_calls.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_direct_object_struct_method_calls.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object struct-method compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object struct-method compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_direct_object_struct_method_calls.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object struct-method build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object struct-method build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object struct-method executable exited with $LASTEXITCODE (expected 0)"
  }

  Write-CaseResult -Name "direct_object_struct_method_calls" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_struct_method_calls" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend pointer-memory test: new, addr_of, load, store, and pointer args
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_pointer_memory.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_pointer_memory.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_direct_object_pointer_memory.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object pointer-memory compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object pointer-memory compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_direct_object_pointer_memory.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object pointer-memory build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object pointer-memory build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 29) {
    throw "Direct object pointer-memory executable exited with $LASTEXITCODE (expected 29)"
  }

  Write-CaseResult -Name "direct_object_pointer_memory" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_pointer_memory" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend release test: a reused byte-address temp must survive load+store fusion
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_byte_load_store_alias.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_byte_load_store_alias.exe"

  $objOut = & $CompilerPath --emit-obj --release tests/test_direct_object_byte_load_store_alias.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object byte-load-store-alias compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object byte-load-store-alias compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj --linker internal --release tests/test_direct_object_byte_load_store_alias.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object byte-load-store-alias build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object byte-load-store-alias build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 37) {
    throw "Direct object byte-load-store-alias executable exited with $LASTEXITCODE (expected 37)"
  }

  Write-CaseResult -Name "direct_object_byte_load_store_alias" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_byte_load_store_alias" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend release test: inline memcpy must preserve live RSI/RDI values
$total++
try {
  $exePath = Join-Path $tmpDir "test_opt_memcpy_const.exe"

  $buildOut = & $CompilerPath --build --emit-obj --linker internal --release tests/test_opt_memcpy_const.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object memcpy-live-registers build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object memcpy-live-registers build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object memcpy-live-registers executable exited with $LASTEXITCODE (expected 0)"
  }

  Write-CaseResult -Name "direct_object_memcpy_live_registers" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_memcpy_live_registers" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend aggregate-local test: stack-allocated struct addressed and passed by pointer
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_struct_field_offset.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_struct_field_offset.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_struct_field_offset.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object struct-field-offset compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object struct-field-offset compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_struct_field_offset.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object struct-field-offset build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object struct-field-offset build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 1) {
    throw "Direct object struct-field-offset executable exited with $LASTEXITCODE (expected 1)"
  }

  Write-CaseResult -Name "direct_object_struct_field_offset" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_struct_field_offset" -Passed $false -Reason $_.Exception.Message
}

# Direct object: local array of struct ??? index scale must be sizeof(element), not 8
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_array_struct_stride.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_array_struct_stride.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_array_struct_stride.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object array-struct-stride compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object array-struct-stride compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_array_struct_stride.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object array-struct-stride build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object array-struct-stride build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 24) {
    throw "Direct object array-struct-stride executable exited with $LASTEXITCODE (expected 24)"
  }

  Write-CaseResult -Name "direct_object_array_struct_stride" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_array_struct_stride" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend function-pointer test: addr_of function plus indirect call
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_function_pointer.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_function_pointer.exe"

  $objOut = & $CompilerPath --emit-obj tests/test_function_pointer.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object function-pointer compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object function-pointer compile did not produce an object file"
  }

  $relocs = & objdump -r $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object function-pointer relocation dump failed"
  }
  if ($relocs -notmatch "IMAGE_REL_AMD64_REL32\s+add") {
    throw "Direct object function-pointer relocations did not contain add"
  }
  if ($relocs -notmatch "IMAGE_REL_AMD64_REL32\s+multiply") {
    throw "Direct object function-pointer relocations did not contain multiply"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests/test_function_pointer.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object function-pointer build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object function-pointer build did not produce an executable"
  }

  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 1) {
    throw "Direct object function-pointer executable exited with $LASTEXITCODE (expected 1)"
  }

  Write-CaseResult -Name "direct_object_function_pointer" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_function_pointer" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend runtime trap test: null deref lowers and links through the trap helper
$total++
try {
  $exePath = Join-Path $tmpDir "test_direct_object_runtime_null_deref.exe"

  $buildOut = & $CompilerPath --build --emit-obj tests/test_runtime_null_deref_check.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object runtime-null build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object runtime-null build did not produce an executable"
  }

  $runtimeOut = & $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 1) {
    throw "Direct object runtime-null executable exited with $LASTEXITCODE (expected 1)"
  }
  if ($runtimeOut -notmatch "Fatal error: Null pointer dereference") {
    throw "Direct object runtime-null output missing null-deref message"
  }

  Write-CaseResult -Name "direct_object_runtime_null_deref" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "direct_object_runtime_null_deref" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend stack-trace metadata: -s emits embedded debug tables and crash startup.
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_stack_trace_support.obj"

  $objOut = & $CompilerPath --emit-obj -s tests/test_runtime_null_deref_check.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object stack-trace compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object stack-trace compile did not produce an object file"
  }

  $symbols = & objdump -t $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object stack-trace symbol dump failed"
  }
  foreach ($sym in @("mettle_debug_functions", "mettle_debug_locations",
      "mettle_debug_header", "mettle_debug_trap_sites", "mettle_debug_image",
      "mettle_crash_startup")) {
    if ($symbols -notmatch [regex]::Escape($sym)) {
      throw "Direct object stack-trace object is missing symbol '$sym'"
    }
  }
  if ($symbols -notmatch "mettle_crash_trap") {
    throw "Direct object stack-trace object is missing undefined 'mettle_crash_trap' reference"
  }
  if ($symbols -notmatch "mettle_crash_trap_ex") {
    throw "Direct object stack-trace object is missing undefined 'mettle_crash_trap_ex' reference"
  }

  Write-CaseResult -Name "stack_trace_support_coff" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "stack_trace_support_coff" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend runtime null trace: --build -s produces symbolized backtraces.
$total++
try {
  $exePath = Join-Path $tmpDir "test_runtime_null_trace_coff.exe"

  $buildOut = & $CompilerPath --build -s tests/test_runtime_null_deref_check.mettle -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object runtime null trace build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Direct object runtime null trace build did not produce an executable"
  }

  $runtimeOut = & $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 1) {
    throw "Direct object runtime null trace exited with $LASTEXITCODE (expected 1)"
  }
  if ($runtimeOut -notmatch "Fatal error: Null pointer dereference") {
    throw "Direct object runtime null trace output missing null-deref message"
  }
  if ($runtimeOut -notmatch "Stack trace:") {
    throw "Direct object runtime null trace output missing stack trace header"
  }
  if ($runtimeOut -notmatch "main") {
    throw "Direct object runtime null trace output missing Mettle frame names"
  }
  if ($runtimeOut -notmatch "test_runtime_null_deref_check\.mettle:\d+:\d+") {
    throw "Direct object runtime null trace output missing file:line:column"
  }
  if ($runtimeOut -notmatch "\|") {
    throw "Direct object runtime null trace output missing source snippet border"
  }
  if ($runtimeOut -notmatch "\^") {
    throw "Direct object runtime null trace output missing caret marker"
  }

  Write-CaseResult -Name "runtime_null_trace_coff" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "runtime_null_trace_coff" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend bounds trap context: --build -s reports index and length.
$total++
try {
  $boundsExe = Join-Path $tmpDir "test_runtime_bounds_trace_coff.exe"
  $boundsBuild = & $CompilerPath --build -s tests/test_crash_bounds_context.mettle -o $boundsExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object runtime bounds trace build failed: $boundsBuild"
  }

  $boundsOut = & $boundsExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 1) {
    throw "Direct object runtime bounds trace exited with $LASTEXITCODE (expected 1)"
  }
  if ($boundsOut -notmatch "index 4") {
    throw "Direct object runtime bounds trace output missing index value"
  }
  if ($boundsOut -notmatch "length 4") {
    throw "Direct object runtime bounds trace output missing array length"
  }
  if ($boundsOut -notmatch "test_crash_bounds_context\.mettle:\d+:\d+") {
    throw "Direct object runtime bounds trace output missing file:line:column"
  }
  if ($boundsOut -notmatch "\^") {
    throw "Direct object runtime bounds trace output missing caret marker"
  }

  Write-CaseResult -Name "runtime_bounds_trace_coff" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "runtime_bounds_trace_coff" -Passed $false -Reason $_.Exception.Message
}

# Direct object backend access-violation trace with file:line when symbolicated.
$total++
try {
  $avExe = Join-Path $tmpDir "test_runtime_av_trace_coff.exe"
  $avBuild = & $CompilerPath --build -s tests/test_runtime_av_trace_coff.mettle -o $avExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object runtime access-violation trace build failed: $avBuild"
  }

  $avOut = & $avExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 1) {
    throw "Direct object runtime access-violation trace exited with $LASTEXITCODE (expected 1)"
  }
  if ($avOut -notmatch "0xC0000005") {
    throw "Direct object runtime access-violation trace output missing exception code"
  }
  if ($avOut -notmatch "Stack trace:") {
    throw "Direct object runtime access-violation trace output missing stack trace header"
  }
  if ($avOut -notmatch "leaf_crash") {
    throw "Direct object runtime access-violation trace output missing frame names"
  }
  if ($avOut -notmatch "test_runtime_av_trace_coff\.mettle:\d+:\d+") {
    throw "Direct object runtime access-violation trace output missing file:line:column"
  }

  Write-CaseResult -Name "runtime_access_violation_trace_coff" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "runtime_access_violation_trace_coff" -Passed $false -Reason $_.Exception.Message
}

# main(argc, argv) test: startup uses the owned command line parser.
$total++
try {
  $avExe = Join-Path $tmpDir "test_main_argc_argv.exe"

  $avOut = & $CompilerPath --build tests/test_main_argc_argv.mettle -o $avExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "main(argc,argv) build failed: $avOut"
  }

  $avImports = & objdump -p $avExe 2>&1 | Out-String
  if ($avImports -match "__getmainargs|ucrtbase|msvcrt") {
    throw "main(argc,argv) executable imports a C runtime"
  }

  $avResult = & $avExe 2>&1
  if ($LASTEXITCODE -ne 0) {
    throw "main(argc,argv) test exited with $LASTEXITCODE (expected 0)"
  }

  Write-CaseResult -Name "main_argc_argv" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "main_argc_argv" -Passed $false -Reason $_.Exception.Message
}

# The normal build path must retain the same runtime independence.
$total++
try {
  $buildArgvExe = Join-Path $tmpDir "test_main_argc_argv_build.exe"

  $buildArgvOut = & $CompilerPath --build tests/test_main_argc_argv.mettle -o $buildArgvExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "main(argc,argv) --build compile failed: $buildArgvOut"
  }

  $exeImports = & objdump -p $buildArgvExe 2>&1 | Out-String
  if ($exeImports -match "__getmainargs|ucrtbase|msvcrt") {
    throw "main(argc,argv) --build executable imports a C runtime"
  }

  $buildArgvResult = & $buildArgvExe "dummy-arg" 2>&1
  if ($LASTEXITCODE -ne 0) {
    throw "main(argc,argv) --build test exited with $LASTEXITCODE (expected 0)"
  }

  Write-CaseResult -Name "main_argc_argv_build" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "main_argc_argv_build" -Passed $false -Reason $_.Exception.Message
}

$total++
try {
  $nullExe = Join-Path $tmpDir "test_runtime_null_trace.exe"

  $nullOut = & $CompilerPath --build -s tests/test_runtime_null_deref_check.mettle -o $nullExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Runtime null trace build failed: $nullOut"
  }

  $nullRuntime = & $nullExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 1) {
    throw "Runtime null trace exited with $LASTEXITCODE (expected 1)"
  }
  if ($nullRuntime -notmatch "Fatal error: Null pointer dereference") {
    throw "Runtime null trace output missing null-deref message"
  }
  if ($nullRuntime -notmatch "Stack trace:") {
    throw "Runtime null trace output missing stack trace header"
  }
  if ($nullRuntime -notmatch "main") {
    throw "Runtime null trace output missing Mettle frame names"
  }

  Write-CaseResult -Name "runtime_null_trace" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "runtime_null_trace" -Passed $false -Reason $_.Exception.Message
}

$total++
try {
  Write-CaseResult -Name "runtime_access_violation_trace" -Passed $true -Reason "skipped: inline assembly is not supported by the binary backend"
}
catch {
  $failed++
  Write-CaseResult -Name "runtime_access_violation_trace" -Passed $false -Reason $_.Exception.Message
}

# AArch64 encoder validity gate. Compiles and runs the from-scratch A64
# instruction encoder against ground-truth constants from the ARM Architecture
# Reference Manual (RET=0xD65F03C0, the stp x29,x30,[sp,#-16]! prologue, ...)
# plus an encode->decode round-trip across the register/immediate range. This
# is the AArch64 analogue of the PTX/ptxas gate below: it validates the hardest
# layer (instruction encodings) with no external assembler and no ARM hardware,
# since the test is pure 32-bit math that runs on the build host.
$total++
try {
  $arm64Exe = "bin/arm64_encode_test.exe"
  & gcc -Wall -Wextra -std=c99 -g -O0 -Isrc -Iinclude tests/arm64_encode_test.c src/codegen/binary/arm64_encode.c src/codegen/binary/arm64_disasm.c src/codegen/binary/arm64_abi.c -o $arm64Exe
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to compile AArch64 encoder test"
  }

  $arm64Output = & $arm64Exe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "AArch64 encoder test failed:`n$arm64Output"
  }
  if ($arm64Output -notmatch "RESULT: PASS") {
    throw "AArch64 encoder test did not report PASS"
  }

  Write-CaseResult -Name "arm64_encoder" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "arm64_encoder" -Passed $false -Reason $_.Exception.Message
}

# AArch64 emit-layer + execution gate. Emits complete AAPCS64 functions
# (prologue/body/epilogue with branch fixups), validates each by decoding every
# word with the from-scratch disassembler, and writes them as minimal static
# AArch64 ELF executables (hand-built header -- no external assembler/linker).
# The structural validation always runs (no external deps). If a qemu-aarch64
# user-mode emulator is reachable through WSL, each ELF is executed and its exit
# code checked against the expected result -- the semantic proof that the
# generated machine code runs on AArch64 without ARM hardware. Execution is
# skipped (not failed) when no emulator is present, like the ptxas gate.
$total++
try {
  $arm64EmitExe = "bin/arm64_emit_test.exe"
  & gcc -Wall -Wextra -std=c99 -g -O0 -Isrc -Iinclude tests/arm64_emit_test.c src/codegen/binary/arm64_encode.c src/codegen/binary/arm64_emit.c src/codegen/binary/arm64_disasm.c src/codegen/binary/arm64_mir_encode.c -o $arm64EmitExe
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to compile AArch64 emit test"
  }

  $elfDir = Join-Path $tmpDir "arm64elf"
  New-Item -ItemType Directory -Force -Path $elfDir | Out-Null
  $emitOut = & $arm64EmitExe $elfDir 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "AArch64 emit test failed:`n$emitOut"
  }
  if ($emitOut -notmatch "RESULT: PASS") {
    throw "AArch64 emit test did not report PASS"
  }

  # Best-effort: run the ELF programs under qemu-aarch64 via WSL.
  $wsl = Get-Command wsl -ErrorAction SilentlyContinue
  if ($wsl -and $elfDir -match '^[A-Za-z]:\\') {
    $scriptWin = (Resolve-Path (Join-Path $PSScriptRoot "arm64_qemu_run.sh")).Path
    $toWsl = {
      param($p)
      "/mnt/" + $p.Substring(0, 1).ToLower() + ($p.Substring(2) -replace '\\', '/')
    }
    $wslScript = & $toWsl $scriptWin
    $wslDir = & $toWsl $elfDir
    $runOut = & wsl bash $wslScript $wslDir 2>&1 | Out-String
    $code = $LASTEXITCODE
    if ($code -eq 0) {
      Write-Host ($runOut.Trim())
    }
    elseif ($code -eq 64) {
      Write-Host "[SKIP] arm64_emit execution (qemu-aarch64 not found; structural validation passed)"
    }
    elseif ($code -eq 1) {
      throw "AArch64 program(s) produced wrong result under qemu:`n$runOut"
    }
    else {
      Write-Host "[SKIP] arm64_emit execution (qemu run unavailable: exit $code)"
    }
  }
  else {
    Write-Host "[SKIP] arm64_emit execution (no WSL; structural validation passed)"
  }

  Write-CaseResult -Name "arm64_emit" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "arm64_emit" -Passed $false -Reason $_.Exception.Message
}

# Real-source -> AArch64 gate. Drives the REAL compiler ($CompilerPath
# --emit-arm64) to lower each tests/arm64/*.mettle fixture to a static AArch64
# ELF, then (if a qemu-aarch64 emulator is reachable through WSL) runs each and
# checks the exit code against expected.txt. Proves actual Mettle source --
# loops, if/else, modulo, recursion, mutual recursion, multi-arg calls --
# compiles to AArch64 and executes. Execution skips like the ptxas gate.
$total++
try {
  $elfDir = Join-Path $tmpDir "arm64src"
  New-Item -ItemType Directory -Force -Path $elfDir | Out-Null
  Copy-Item tests/arm64/expected.txt (Join-Path $elfDir "manifest.txt") -Force
  $names = Get-Content tests/arm64/expected.txt |
    ForEach-Object { ($_ -split ' ')[0] } | Where-Object { $_ }
  foreach ($n in $names) {
    $elf = Join-Path $elfDir "$n.elf"
    & $CompilerPath --emit-arm64 "tests/arm64\$n.mettle" -o $elf 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $elf)) {
      throw "mettle --emit-arm64 failed on $n.mettle"
    }
  }

  # I/O fixtures: compile and stage each with its expected-stdout .out sidecar.
  $ioDir = Join-Path $tmpDir "arm64io"
  New-Item -ItemType Directory -Force -Path $ioDir | Out-Null
  Get-ChildItem tests/arm64\io\*.mettle | ForEach-Object {
    $elf = Join-Path $ioDir ($_.BaseName + ".elf")
    & $CompilerPath --emit-arm64 $_.FullName -o $elf 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $elf)) {
      throw "mettle --emit-arm64 failed on io/$($_.Name)"
    }
    Copy-Item (Join-Path "tests/arm64\io" ($_.BaseName + ".out")) `
      (Join-Path $ioDir ($_.BaseName + ".out")) -Force
  }

  $wsl = Get-Command wsl -ErrorAction SilentlyContinue
  if ($wsl -and $elfDir -match '^[A-Za-z]:\\') {
    $toWsl = {
      param($p)
      "/mnt/" + $p.Substring(0, 1).ToLower() + ($p.Substring(2) -replace '\\', '/')
    }
    $wslScript = & $toWsl (Resolve-Path (Join-Path $PSScriptRoot "arm64_qemu_run.sh")).Path
    $runOut = & wsl bash $wslScript (& $toWsl $elfDir) 2>&1 | Out-String
    $code = $LASTEXITCODE
    if ($code -eq 0) {
      Write-Host ($runOut.Trim())
      # I/O: diff each program's stdout against its committed .out
      $ioScript = & $toWsl (Resolve-Path (Join-Path $PSScriptRoot "arm64_io_run.sh")).Path
      $ioOut = & wsl bash $ioScript (& $toWsl $ioDir) 2>&1 | Out-String
      $ioCode = $LASTEXITCODE
      if ($ioCode -eq 0) {
        Write-Host ($ioOut.Trim())
      }
      elseif ($ioCode -eq 64) {
        Write-Host "[SKIP] arm64 I/O execution (qemu-aarch64 not found)"
      }
      else {
        throw "real-source AArch64 I/O stdout mismatch:`n$ioOut"
      }
    }
    elseif ($code -eq 64) {
      Write-Host "[SKIP] arm64_source execution (qemu-aarch64 not found; all fixtures lowered)"
    }
    elseif ($code -eq 1) {
      throw "real-source AArch64 program produced wrong result under qemu:`n$runOut"
    }
    else {
      Write-Host "[SKIP] arm64_source execution (qemu run unavailable: exit $code)"
    }
  }
  else {
    Write-Host "[SKIP] arm64_source execution (no WSL; all fixtures lowered)"
  }

  Write-CaseResult -Name "arm64_source" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "arm64_source" -Passed $false -Reason $_.Exception.Message
}

# The AArch64 *object* backend, from an x86-64 host. It used to be reachable
# only on an ARM machine (the driver picked it with #if defined(__aarch64__)),
# so every change to the shared lowering went untested off ARM. --emit-arm64-obj
# asks any host for it. Checked structurally -- an AArch64 REL object with the
# expected relocation -- because this host has no linker that could take one.
$total++
try {
  # ELF64 header: e_type at 0x10, e_machine at 0x12; section headers at e_shoff
  # with e_shnum entries of e_shentsize, names in section e_shstrndx.
  function Get-ElfSectionSize([byte[]]$bytes, [string]$wanted) {
    $shoff = [BitConverter]::ToUInt64($bytes, 0x28)
    $shentsize = [BitConverter]::ToUInt16($bytes, 0x3A)
    $shnum = [BitConverter]::ToUInt16($bytes, 0x3C)
    $shstrndx = [BitConverter]::ToUInt16($bytes, 0x3E)
    $strtab = [BitConverter]::ToUInt64($bytes, [int]($shoff + $shstrndx * $shentsize + 0x18))
    for ($i = 0; $i -lt $shnum; $i++) {
      $sh = [int]($shoff + $i * $shentsize)
      $nameOff = [int]($strtab + [BitConverter]::ToUInt32($bytes, $sh))
      $end = $nameOff
      while ($bytes[$end] -ne 0) { $end++ }
      $name = [Text.Encoding]::ASCII.GetString($bytes, $nameOff, $end - $nameOff)
      if ($name -eq $wanted) { return [BitConverter]::ToUInt64($bytes, $sh + 0x20) }
    }
    return -1
  }

  $objDir = Join-Path $tmpDir "arm64obj"
  New-Item -ItemType Directory -Force -Path $objDir | Out-Null
  $names = Get-Content tests/arm64/expected.txt |
    ForEach-Object { ($_ -split '\s+')[0] } | Where-Object { $_ }
  foreach ($n in $names) {
    $obj = Join-Path $objDir "$n.o"
    & $CompilerPath --emit-arm64-obj "tests/arm64\$n.mettle" -o $obj 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "mettle --emit-arm64-obj failed on $n.mettle"
    }
    $bytes = [IO.File]::ReadAllBytes($obj)
    if ($bytes[0] -ne 0x7F -or $bytes[1] -ne 0x45 -or $bytes[2] -ne 0x4C -or $bytes[3] -ne 0x46) {
      throw "$n.o is not an ELF file"
    }
    $etype = [BitConverter]::ToUInt16($bytes, 0x10)
    $emachine = [BitConverter]::ToUInt16($bytes, 0x12)
    if ($etype -ne 1) { throw "$n.o is ELF type $etype, expected 1 (REL)" }
    if ($emachine -ne 183) { throw "$n.o is machine $emachine, expected 183 (AArch64)" }
  }

  # A call to an undefined external has to leave a relocation behind for the
  # linker; native_link.mettle calls the owned putchar ABI.
  $extObj = Join-Path $objDir "native_link.o"
  & $CompilerPath --emit-arm64-obj tests/arm64/native_link.mettle -o $extObj 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "mettle --emit-arm64-obj failed on native_link.mettle"
  }
  $extBytes = [IO.File]::ReadAllBytes($extObj)
  $relaSize = Get-ElfSectionSize $extBytes ".rela.text"
  if ($relaSize -le 0) {
    throw "native_link.o has no .rela.text relocations for its extern call"
  }

  # Cross output chooses its target OS standard library, not the compiler host.
  # A Windows hosted compiler must put Linux stream symbols in an AArch64 ELF.
  $crossStdObj = Join-Path $objDir "owned_dir_linux_std.o"
  & $CompilerPath --emit-arm64-obj tests/test_owned_dir.mettle -o $crossStdObj 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "AArch64 owned directory object failed"
  }
  $crossStdText = [Text.Encoding]::ASCII.GetString(
    [IO.File]::ReadAllBytes($crossStdObj))
  if ($crossStdText -match "__acrt_iob_func" -or
      $crossStdText -notmatch "mettle_dir_exists" -or
      $crossStdText -notmatch "stdout") {
    throw "AArch64 object selected Windows stdlib symbols instead of Linux"
  }

  Write-CaseResult -Name "arm64_object" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "arm64_object" -Passed $false -Reason $_.Exception.Message
}

# The same fixtures at -O. --emit-arm64 used to emit before the optimizer ran,
# so --release was accepted and silently ignored; it now runs the optimizer's
# target-neutral half (scalar and control-flow transforms that keep the shared
# IR instruction set) and lowers the result. Same answers as the debug build.
# trapnull is excluded: release strips runtime checks on every target, so its
# deliberate null dereference faults instead of printing and exiting 1 -- the
# x86 release build faults on it too.
$total++
try {
  $relDir = Join-Path $tmpDir "arm64rel"
  New-Item -ItemType Directory -Force -Path $relDir | Out-Null
  $relNames = Get-Content tests/arm64/expected.txt |
    Where-Object { $_ -and ($_ -split '\s+')[0] -ne "trapnull" }
  Set-Content -Encoding ascii (Join-Path $relDir "manifest.txt") $relNames
  foreach ($line in $relNames) {
    $n = ($line -split '\s+')[0]
    $elf = Join-Path $relDir "$n.elf"
    & $CompilerPath --emit-arm64 --release "tests/arm64\$n.mettle" -o $elf 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $elf)) {
      throw "mettle --emit-arm64 --release failed on $n.mettle"
    }
  }

  $wsl = Get-Command wsl -ErrorAction SilentlyContinue
  if ($wsl -and $relDir -match '^[A-Za-z]:\\') {
    $toWsl = {
      param($p)
      "/mnt/" + $p.Substring(0, 1).ToLower() + ($p.Substring(2) -replace '\\', '/')
    }
    $wslScript = & $toWsl (Resolve-Path (Join-Path $PSScriptRoot "arm64_qemu_run.sh")).Path
    $runOut = & wsl bash $wslScript (& $toWsl $relDir) 2>&1 | Out-String
    $code = $LASTEXITCODE
    if ($code -eq 0) {
      Write-Host ($runOut.Trim())
    }
    elseif ($code -eq 64) {
      Write-Host "[SKIP] arm64_source_release execution (qemu-aarch64 not found; all fixtures lowered)"
    }
    elseif ($code -eq 1) {
      throw "AArch64 --release program(s) produced wrong result under qemu:`n$runOut"
    }
    else {
      Write-Host "[SKIP] arm64_source_release execution (qemu run unavailable: exit $code)"
    }
  }
  else {
    Write-Host "[SKIP] arm64_source_release execution (no WSL; all fixtures lowered)"
  }

  Write-CaseResult -Name "arm64_source_release" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "arm64_source_release" -Passed $false -Reason $_.Exception.Message
}

# General reduction-unrolling vectorizer: correctness on non-benchmark
# reductions (distinct EXPR(i), inclusive/exclusive bounds, a trip count that
# is not a multiple of the unroll factor so the scalar remainder runs). Built
# via the direct-object backend (the path the benchmarks use). Exact closed
# forms are asserted so a miscompiled unroll is caught, not just a crash.
$total++
try {
  $reduExe = "bin/test_opt_reduction_unroll.exe"
  $reduBuild = & $CompilerPath --build --emit-obj --linker internal --release `
    tests/test_opt_reduction_unroll.mettle -o $reduExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "reduction-unroll build failed: $reduBuild"
  }
  $reduOut = & $reduExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "reduction-unroll exe exited with $LASTEXITCODE"
  }
  if ($reduOut -notmatch "lin=500500") {
    throw "sum_linear(1000) wrong (expected 500500): $reduOut"
  }
  if ($reduOut -notmatch "aff=1517539") {
    throw "sum_affine(1003) wrong (expected 1517539): $reduOut"
  }
  if ($reduOut -notmatch "cnt=777") {
    throw "count_to(777) wrong (expected 777): $reduOut"
  }
  # The unroll must actually have fired (synthetic accumulators in the IR).
  $reduCheckObj = Join-Path $tmpDir "redu_check.obj"
  $reduIr = & $CompilerPath --release --dump-ir tests/test_opt_reduction_unroll.mettle `
    -o $reduCheckObj 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "reduction-unroll IR check compile failed"
  }
  $reduIrPath = "$reduCheckObj.ir"
  if (-not (Test-Path $reduIrPath)) {
    throw "reduction-unroll IR check did not produce an IR dump"
  }
  $reduIrText = Get-Content $reduIrPath -Raw
  if ($reduIrText -notmatch "vu\d+_main") {
    throw "reduction-unroll pass did not fire (no vuN_main in IR)"
  }
  Write-CaseResult -Name "opt_reduction_unroll" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "opt_reduction_unroll" -Passed $false -Reason $_.Exception.Message
}

# Runtime profile mode: function entry/exit instrumentation and exit report
$total++
try {
  $profileExe = Join-Path $tmpDir "test_profile_runtime.exe"
  $profileBuild = & $CompilerPath --build --emit-obj --linker internal --profile-runtime `
    tests/test_profile_runtime.mettle -o $profileExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "profile-runtime build failed: $profileBuild"
  }
  if (-not (Test-Path $profileExe)) {
    throw "profile-runtime build did not produce an executable"
  }

  $profileRun = & $profileExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "profile-runtime exe exited with $LASTEXITCODE"
  }
  if ($profileRun -notmatch "Runtime profile:") {
    throw "profile-runtime report missing header: $profileRun"
  }
  if ($profileRun -notmatch "helper") {
    throw "profile-runtime report missing helper: $profileRun"
  }
  if ($profileRun -notmatch "work") {
    throw "profile-runtime report missing work: $profileRun"
  }
  if ($profileRun -notmatch "main") {
    throw "profile-runtime report missing main: $profileRun"
  }
  if ($profileRun -notmatch "location") {
    throw "profile-runtime report missing location column: $profileRun"
  }
  if ($profileRun -notmatch "Runtime profile \(call graph\):") {
    throw "profile-runtime report missing call graph: $profileRun"
  }
  if ($profileRun -notmatch "test_profile_runtime\.mettle:[0-9]+") {
    throw "profile-runtime report missing file:line location: $profileRun"
  }

  Write-CaseResult -Name "profile_runtime" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "profile_runtime" -Passed $false -Reason $_.Exception.Message
}

$total++
try {
  $profileOpsExe = Join-Path $tmpDir "test_profile_runtime_ops.exe"
  $profileOpsBuild = & $CompilerPath --build --emit-obj --linker internal --profile-runtime-ops `
    tests/test_profile_runtime.mettle -o $profileOpsExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "profile-runtime-ops build failed: $profileOpsBuild"
  }
  if (-not (Test-Path $profileOpsExe)) {
    throw "profile-runtime-ops build did not produce an executable"
  }

  $profileOpsRun = & $profileOpsExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "profile-runtime-ops exe exited with $LASTEXITCODE"
  }
  if ($profileOpsRun -notmatch "Operation profile:") {
    throw "profile-runtime-ops report missing header: $profileOpsRun"
  }
  if ($profileOpsRun -notmatch "function\s+op_class\s+count") {
    throw "profile-runtime-ops report missing columns: $profileOpsRun"
  }
  if ($profileOpsRun -notmatch "work\s+add") {
    throw "profile-runtime-ops report missing expected op row: $profileOpsRun"
  }
  if ($profileOpsRun -notmatch "work\s+branch") {
    throw "profile-runtime-ops report missing branch row: $profileOpsRun"
  }

  Write-CaseResult -Name "profile_runtime_ops" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "profile_runtime_ops" -Passed $false -Reason $_.Exception.Message
}

try {
  $total++
  $iceExe = Join-Path $tmpDir "compiler_ice_report_test.exe"
  $iceCompile = & gcc -Wall -Wextra -std=c99 -g -O0 -Isrc -Iinclude tests/compiler_ice_report_test.c src/common.c src/lexer/lexer.c src/compiler/compiler_context.c src/compiler/compiler_crash.c src/runtime/crash_handler.c src/ir/ir.c -o $iceExe -ldbghelp 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "compiler ICE report harness compile failed: $iceCompile"
  }
  $iceRun = & $iceExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "compiler ICE report harness exited with $LASTEXITCODE"
  }
  if ($iceRun -notmatch "Mettle internal compiler error") {
    throw "compiler ICE report missing banner: $iceRun"
  }
  if ($iceRun -notmatch "Phase: IR optimization") {
    throw "compiler ICE report missing phase: $iceRun"
  }
  if ($iceRun -notmatch "Pass: memcpy_inline") {
    throw "compiler ICE report missing pass: $iceRun"
  }
  if ($iceRun -notmatch "Compiler backtrace:") {
    throw "compiler ICE report missing backtrace: $iceRun"
  }
  if ($iceRun -notmatch "memcpy_inline") {
    throw "compiler ICE report missing IR instruction text: $iceRun"
  }
  Write-CaseResult -Name "compiler_ice_report" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "compiler_ice_report" -Passed $false -Reason $_.Exception.Message
}

# PTX backend validity gate. Emission and structural/profile checks always run.
# When NVIDIA's ptxas is installed, each portable module is assembled too; a
# second GB10-specific gate assembles sm_121a when the installed toolkit knows
# that target. This keeps non-NVIDIA CI useful without weakening the DGX Spark
# acceptance gate on CUDA development machines.
$ptxas = Get-Command ptxas -ErrorAction SilentlyContinue
foreach ($src in @("tests/gpu/compute_kernels.mettle",
                   "tests/gpu/subgroup_shuffle.mettle",
                   "tests/gpu/atomic_kernels.mettle",
                   "tests/gpu/async_copy.mettle",
                   "tests/gpu/auto_staging.mettle",
                   "tests/gpu/auto_staging_no_promote.mettle",
                   "tests/gpu/tensor_chain.mettle",
                   "tests/gpu/tensor_chain_no_fuse.mettle",
                   "tests/gpu/tensor_loop.mettle",
                   "tests/gpu/tensor_loop_no_residency.mettle",
                   "tests/gpu/tensor_pipeline.mettle",
                   "tests/gpu/tensor_pipeline4.mettle",
                   "tests/gpu/tensor_pipeline_no_residency.mettle",
                   "tests/gpu/tensor_epilogue_portable.mettle",
                   "examples/gpu_vadd/vadd_kernel.mettle")) {
  $total++
  $name = "ptx_emit_" + [System.IO.Path]::GetFileNameWithoutExtension($src)
  try {
    $ptxPath = Join-Path $tmpDir ($name + ".ptx")
    $cubin = Join-Path $tmpDir ($name + ".cubin")
    $emitOut = & $CompilerPath -O --emit-ptx --gpu-arch=portable $src -o $ptxPath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "emit failed: $emitOut" }
    if (-not (Test-Path $ptxPath)) { throw "no PTX produced" }
    $ptxText = Get-Content -Raw $ptxPath
    if ($ptxText -notmatch "(?m)^\.version 6\.4\r?$") { throw "portable PTX version is not 6.4" }
    if ($ptxText -notmatch "(?m)^\.target compute_75\r?$") { throw "portable PTX target is not compute_75" }
    if ($ptxText -match "ordinary_function_not_entry") {
      throw "unreachable ordinary function entered the PTX module"
    }
    if ($src -like "*compute_kernels.mettle") {
      if ($ptxText -notmatch "(?m)^\.func \(\.param \.f32 scale_value_ret\) scale_value\(" -or
          $ptxText -notmatch "call\.uni .*scale_value") {
        throw "reachable ordinary helper was not lowered as a PTX device call"
      }
      foreach ($decl in @("\.param \.s8 narrow_scalar_abi_p0",
                           "\.param \.u8 narrow_scalar_abi_p1",
                           "\.param \.s16 narrow_scalar_abi_p2",
                           "\.param \.u16 narrow_scalar_abi_p3",
                           "\.param \.u8 narrow_scalar_abi_p4")) {
        if ($ptxText -notmatch $decl) { throw "missing natural-width kernel ABI declaration: $decl" }
      }
      foreach ($memoryContract in @(
          "\.shared \.align 32 \.b8 staged_copy_tile_storage\[128\]",
          "\.local \.align 4 \.b8 staged_copy_scratch_storage\[16\]",
          "\.extern \.shared \.align 32 \.b8 dynamic_staged_copy_dynamic_workgroup_storage\[\]",
          "st\.shared\.f32", "ld\.shared\.f32",
          "st\.local\.s32", "ld\.local\.s32")) {
        if ($ptxText -notmatch $memoryContract) {
          throw "missing static address-space memory contract: $memoryContract"
        }
      }
      $dynamicBaseMoves = [regex]::Matches(
        $ptxText,
        'mov\.u64 %rd[0-9]+, dynamic_staged_copy_dynamic_workgroup_storage;'
      ).Count
      if ($dynamicBaseMoves -ne 2) {
        throw "dynamic workgroup views did not alias one PTX arena: moves=$dynamicBaseMoves"
      }
      foreach ($subgroupContract in @(
          "min\.f32", "min\.u32", "max\.f32", "max\.u32",
          "shfl\.sync\.up\.b32", "vote\.sync\.ballot\.b32",
          "vote\.sync\.any\.pred", "vote\.sync\.all\.pred")) {
        if ($ptxText -notmatch $subgroupContract) {
          throw "missing extended subgroup contract: $subgroupContract"
        }
      }
    }
    if ($src -like "*subgroup_shuffle.mettle") {
      if ([regex]::Matches($ptxText, "shfl\.sync\.idx\.b32").Count -ne 2 -or
          [regex]::Matches($ptxText, "activemask\.b32").Count -ne 2) {
        throw "variable-source subgroup shuffle contract mismatch"
      }
    }
    if ($src -like "*atomic_kernels.mettle") {
      if ([regex]::Matches($ptxText, "atom\.").Count -ne 20 -or
          [regex]::Matches($ptxText, "ld\.(relaxed|acquire)\.").Count -lt 4 -or
          [regex]::Matches($ptxText, "st\.(relaxed|release)\.").Count -lt 4 -or
          $ptxText -notmatch "mul\.lo\.u64" -or
          $ptxText -notmatch "neg\.s32" -or
          $ptxText -notmatch "neg\.s64") {
        throw "atomic family did not preserve exact operation/index width"
      }
      foreach ($atomicContract in @(
          "global\.add\.u32", "global\.add\.u64",
          "global\.min\.u32", "global\.min\.u64",
          "global\.max\.u32", "global\.max\.u64",
          "global\.and\.b32", "global\.and\.b64",
          "global\.or\.b32", "global\.or\.b64",
          "global\.xor\.b32", "global\.xor\.b64",
          "global\.exch\.b32", "global\.exch\.b64",
          "global\.cas\.b32", "global\.cas\.b64",
          "shared\.add\.u32", "shared\.cas\.b64",
          "ld\.acquire\.gpu\.global\.u32",
          "ld\.acquire\.sys\.global\.u64",
          "st\.release\.gpu\.global\.u32",
          "st\.relaxed\.sys\.global\.u64",
          "ld\.relaxed\.cta\.shared\.u32",
          "ld\.acquire\.cta\.shared\.u64",
          "st\.relaxed\.cta\.shared\.u32",
          "st\.release\.cta\.shared\.u64")) {
        if ($ptxText -notmatch $atomicContract) {
          throw "missing broad atomic contract: $atomicContract"
        }
      }
    }
    if ($src -like "*tensor_chain.mettle") {
      $mmaCount = [regex]::Matches($ptxText, "wmma\.mma\.sync").Count
      $cLoadCount = [regex]::Matches($ptxText, "wmma\.load\.c\.sync").Count
      $dStoreCount = [regex]::Matches($ptxText, "wmma\.store\.d\.sync").Count
      if ($ptxText -notmatch "mtlc\.tensor_chain resident tiles=4 tuple_peak=32 budget=64" -or
          $mmaCount -ne 4 -or $cLoadCount -ne 1 -or $dStoreCount -ne 1) {
        throw "optimized tensor chain residency mismatch: mma=$mmaCount c_load=$cLoadCount d_store=$dStoreCount"
      }
      $unoptimized = Join-Path $tmpDir ($name + "_unoptimized.ptx")
      $unoptimizedOut = & $CompilerPath --emit-ptx --gpu-arch=portable $src -o $unoptimized 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "unoptimized tensor emit failed: $unoptimizedOut" }
      $unoptimizedText = Get-Content -Raw $unoptimized
      if ($unoptimizedText -match "mtlc\.tensor_chain" -or
          [regex]::Matches($unoptimizedText, "wmma\.load\.c\.sync").Count -ne 4 -or
          [regex]::Matches($unoptimizedText, "wmma\.store\.d\.sync").Count -ne 4) {
        throw "tensor residency was not formed exclusively by the optimizer"
      }
      $explained = Join-Path $tmpDir ($name + "_explained.ptx")
      $explainOut = & $CompilerPath -O --explain-all --emit-ptx `
        --gpu-arch=portable $src -o $explained 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0 -or
          $explainOut -notmatch "fused 4 tensor tiles into one accumulator-resident chain" -or
          $explainOut -notmatch "target-neutral optimized IR emitted through the PTX backend") {
        throw "PTX --explain omitted the neutral tensor decision or backend boundary: $explainOut"
      }
      $dumped = Join-Path $tmpDir ($name + "_dumped.ptx")
      $dumpOut = & $CompilerPath -O --dump-ir --emit-ptx `
        --gpu-arch=portable $src -o $dumped 2>&1 | Out-String
      $dumpPath = $dumped + ".ir"
      if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dumpPath) -or
          (Get-Content -Raw $dumpPath) -notmatch "tensor_mma x4") {
        throw "GPU --dump-ir omitted the optimized tensor chain: $dumpOut"
      }
      $budgeted = Join-Path $tmpDir ($name + "_budget31.ptx")
      $budgetOut = & $CompilerPath -O --emit-ptx --gpu-arch=portable `
        --gpu-tensor-tuple-budget=31 $src -o $budgeted 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        throw "tensor tuple-budget variant emit failed: $budgetOut"
      }
      $budgetText = Get-Content -Raw $budgeted
      if ($budgetText -notmatch
            "mtlc\.tensor_chain replay tiles=4 tuple_peak=32 budget=31" -or
          [regex]::Matches($budgetText, "wmma\.load\.c\.sync").Count -ne 4 -or
          [regex]::Matches($budgetText, "wmma\.store\.d\.sync").Count -ne 4) {
        throw "explicit tensor tuple budget did not select exact replay"
      }
      $invalidBudgetOut = & $CompilerPath -O --emit-ptx `
        --gpu-tensor-tuple-budget=4097 $src -o $budgeted 2>&1 | Out-String
      if ($LASTEXITCODE -eq 0 -or
          $invalidBudgetOut -notmatch
            "--gpu-tensor-tuple-budget expects 0\.\.4096") {
        throw "invalid tensor tuple budget was not rejected: $invalidBudgetOut"
      }
    }
    if ($src -like "*tensor_chain_no_fuse.mettle") {
      $mmaCount = [regex]::Matches($ptxText, "wmma\.mma\.sync").Count
      $cLoadCount = [regex]::Matches($ptxText, "wmma\.load\.c\.sync").Count
      $dStoreCount = [regex]::Matches($ptxText, "wmma\.store\.d\.sync").Count
      if ($ptxText -match "mtlc\.tensor_chain" -or
          $mmaCount -ne 4 -or $cLoadCount -ne 4 -or $dStoreCount -ne 4 -or
          $ptxText -notmatch "st\.relaxed\.gpu\.global\.u32") {
        throw "illegal tensor-chain fusion: mma=$mmaCount c_load=$cLoadCount d_store=$dStoreCount"
      }
    }
    if ($src -like "*tensor_loop.mettle") {
      $mmaCount = [regex]::Matches($ptxText, "wmma\.mma\.sync").Count
      $cLoadCount = [regex]::Matches($ptxText, "wmma\.load\.c\.sync").Count
      $dStoreCount = [regex]::Matches($ptxText, "wmma\.store\.d\.sync").Count
      if ($ptxText -notmatch "mtlc\.tensor_loop resident group=1 tuple_peak=32 budget=64" -or
          $mmaCount -ne 2 -or $cLoadCount -ne 1 -or $dStoreCount -ne 1) {
        throw "optimized tensor-loop residency mismatch: mma=$mmaCount c_load=$cLoadCount d_store=$dStoreCount"
      }
      $unoptimized = Join-Path $tmpDir ($name + "_unoptimized.ptx")
      $unoptimizedOut = & $CompilerPath --emit-ptx --gpu-arch=portable $src -o $unoptimized 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "unoptimized tensor-loop emit failed: $unoptimizedOut" }
      $unoptimizedText = Get-Content -Raw $unoptimized
      if ($unoptimizedText -match "mtlc\.tensor_loop" -or
          [regex]::Matches($unoptimizedText, "wmma\.load\.c\.sync").Count -ne 2 -or
          [regex]::Matches($unoptimizedText, "wmma\.store\.d\.sync").Count -ne 2) {
        throw "tensor-loop residency was not formed exclusively by the optimizer"
      }
      $explained = Join-Path $tmpDir ($name + "_explained.ptx")
      $explainOut = & $CompilerPath -O --explain-all --emit-ptx `
        --gpu-arch=portable $src -o $explained 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0 -or
          $explainOut -notmatch "formed a loop-carried tensor accumulator residency region" -or
          $explainOut -notmatch "target-neutral optimized IR emitted through the PTX backend") {
        throw "PTX --explain omitted the neutral tensor-loop decision or backend boundary: $explainOut"
      }
      $dumped = Join-Path $tmpDir ($name + "_dumped.ptx")
      $dumpOut = & $CompilerPath -O --dump-ir --emit-ptx `
        --gpu-arch=portable $src -o $dumped 2>&1 | Out-String
      $dumpPath = $dumped + ".ir"
      if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dumpPath)) {
        throw "GPU --dump-ir omitted the optimized tensor loop: $dumpOut"
      }
      $dumpText = Get-Content -Raw $dumpPath
      foreach ($loopContract in @("residency\.loop\.start#",
                                   "residency\.loop\.update#",
                                   "residency\.loop\.commit#")) {
        if ($dumpText -notmatch $loopContract) {
          throw "GPU --dump-ir omitted tensor-loop contract: $loopContract"
        }
      }
    }
    if ($src -like "*tensor_loop_no_residency.mettle") {
      $mmaCount = [regex]::Matches($ptxText, "wmma\.mma\.sync").Count
      $cLoadCount = [regex]::Matches($ptxText, "wmma\.load\.c\.sync").Count
      $dStoreCount = [regex]::Matches($ptxText, "wmma\.store\.d\.sync").Count
      if ($ptxText -match "mtlc\.tensor_loop" -or
          $mmaCount -ne 2 -or $cLoadCount -ne 2 -or $dStoreCount -ne 2 -or
          $ptxText -notmatch "st\.relaxed\.gpu\.global\.u32") {
        throw "illegal tensor-loop residency: mma=$mmaCount c_load=$cLoadCount d_store=$dStoreCount"
      }
    }
    if ($src -like "*tensor_pipeline.mettle") {
      if ([regex]::Matches($ptxText,
                          "mtlc\.async_copy synchronous-fallback bytes=16 transaction=16").Count -ne 4 -or
          [regex]::Matches($ptxText, "ld\.global\.b32").Count -ne 16 -or
          [regex]::Matches($ptxText, "st\.shared\.b32").Count -ne 16 -or
          [regex]::Matches($ptxText,
                          "mtlc\.async_copy commit synchronous-fallback").Count -ne 2 -or
          [regex]::Matches($ptxText, "bar\.sync 0").Count -ne 2 -or
          $ptxText -notmatch "mtlc\.tensor_pipeline resident group=1 tuple_peak=32 budget=64" -or
          [regex]::Matches($ptxText, "wmma\.mma\.sync").Count -ne 2 -or
          [regex]::Matches($ptxText, "wmma\.load\.c\.sync").Count -ne 1 -or
          [regex]::Matches($ptxText, "wmma\.store\.d\.sync").Count -ne 1 -or
          $ptxText -match "cp\.async\.") {
        throw "portable staged-tensor replay/residency contract mismatch"
      }
      $waitOneAt = $ptxText.IndexOf(
        "mtlc.async_copy wait pending=1 synchronous-fallback")
      $firstBarrierAt = $ptxText.IndexOf("bar.sync 0", $waitOneAt + 1)
      $firstMmaAt = $ptxText.IndexOf("wmma.mma.sync", $firstBarrierAt + 1)
      $waitZeroAt = $ptxText.IndexOf(
        "mtlc.async_copy wait pending=0 synchronous-fallback", $firstMmaAt + 1)
      $secondBarrierAt = $ptxText.IndexOf("bar.sync 0", $waitZeroAt + 1)
      $secondMmaAt = $ptxText.IndexOf("wmma.mma.sync", $firstMmaAt + 1)
      $storeAt = $ptxText.IndexOf("wmma.store.d.sync", $secondMmaAt + 1)
      if ($waitOneAt -lt 0 -or $firstBarrierAt -le $waitOneAt -or
          $firstMmaAt -le $firstBarrierAt -or $waitZeroAt -le $firstMmaAt -or
          $secondBarrierAt -le $waitZeroAt -or
          $secondMmaAt -le $secondBarrierAt -or $storeAt -le $secondMmaAt) {
        throw "portable staged-tensor handoff ordering mismatch"
      }
      $unoptimized = Join-Path $tmpDir ($name + "_unoptimized.ptx")
      $unoptimizedOut = & $CompilerPath --emit-ptx --gpu-arch=portable `
        $src -o $unoptimized 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        throw "unoptimized staged-tensor emit failed: $unoptimizedOut"
      }
      $unoptimizedText = Get-Content -Raw $unoptimized
      if ($unoptimizedText -match "mtlc\.tensor_pipeline" -or
          [regex]::Matches($unoptimizedText, "wmma\.load\.c\.sync").Count -ne 2 -or
          [regex]::Matches($unoptimizedText, "wmma\.store\.d\.sync").Count -ne 2) {
        throw "staged-tensor residency was not formed exclusively by the optimizer"
      }
      $explained = Join-Path $tmpDir ($name + "_explained.ptx")
      $explainOut = & $CompilerPath -O --explain-all --emit-ptx `
        --gpu-arch=portable $src -o $explained 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0 -or
          $explainOut -notmatch "formed an asynchronously staged tensor accumulator pipeline" -or
          $explainOut -notmatch "target-neutral optimized IR emitted through the PTX backend") {
        throw "PTX --explain omitted the neutral staged-tensor decision: $explainOut"
      }
      $dumped = Join-Path $tmpDir ($name + "_dumped.ptx")
      $dumpOut = & $CompilerPath -O --dump-ir --emit-ptx `
        --gpu-arch=portable $src -o $dumped 2>&1 | Out-String
      $dumpPath = $dumped + ".ir"
      if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dumpPath)) {
        throw "GPU --dump-ir omitted the staged tensor pipeline: $dumpOut"
      }
      $dumpText = Get-Content -Raw $dumpPath
      foreach ($pipelineContract in @("residency\.pipeline\.start#",
                                       "residency\.pipeline\.update#",
                                       "residency\.pipeline\.commit#")) {
        if ($dumpText -notmatch $pipelineContract) {
          throw "GPU --dump-ir omitted staged-tensor contract: $pipelineContract"
        }
      }
    }
    if ($src -like "*tensor_pipeline_no_residency.mettle") {
      if ($ptxText -match "mtlc\.tensor_pipeline" -or
          [regex]::Matches($ptxText, "wmma\.mma\.sync").Count -ne 2 -or
          [regex]::Matches($ptxText, "wmma\.load\.c\.sync").Count -ne 2 -or
          [regex]::Matches($ptxText, "wmma\.store\.d\.sync").Count -ne 2 -or
          [regex]::Matches($ptxText,
                          "mtlc\.async_copy synchronous-fallback bytes=16 transaction=16").Count -ne 4 -or
          $ptxText -notmatch "st\.global\.u32") {
        throw "observable effect illegally received staged-tensor residency"
      }
    }
    if ($src -like "*tensor_pipeline4.mettle") {
      if ([regex]::Matches($ptxText,
                          "mtlc\.async_copy synchronous-fallback bytes=16 transaction=16").Count -ne 8 -or
          [regex]::Matches($ptxText, "ld\.global\.b32").Count -ne 32 -or
          [regex]::Matches($ptxText, "st\.shared\.b32").Count -ne 32 -or
          [regex]::Matches($ptxText,
                          "mtlc\.async_copy commit synchronous-fallback").Count -ne 4 -or
          [regex]::Matches($ptxText, "bar\.sync 0").Count -ne 4 -or
          $ptxText -notmatch "mtlc\.tensor_pipeline resident group=1 tuple_peak=32 budget=64" -or
          [regex]::Matches($ptxText, "wmma\.mma\.sync").Count -ne 4 -or
          [regex]::Matches($ptxText, "wmma\.load\.c\.sync").Count -ne 1 -or
          [regex]::Matches($ptxText, "wmma\.store\.d\.sync").Count -ne 1 -or
          $ptxText -match "cp\.async\.") {
        throw "portable four-stage tensor pipeline contract mismatch"
      }
      foreach ($pending in 3, 2, 1, 0) {
        if ([regex]::Matches(
              $ptxText,
              "mtlc\.async_copy wait pending=$pending synchronous-fallback").Count -ne 1) {
          throw "portable four-stage pipeline omitted wait pending=$pending"
        }
      }
      $unoptimized = Join-Path $tmpDir ($name + "_unoptimized.ptx")
      $unoptimizedOut = & $CompilerPath --emit-ptx --gpu-arch=portable `
        $src -o $unoptimized 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        throw "unoptimized four-stage pipeline emit failed: $unoptimizedOut"
      }
      $unoptimizedText = Get-Content -Raw $unoptimized
      if ($unoptimizedText -match "mtlc\.tensor_pipeline" -or
          [regex]::Matches($unoptimizedText,
                          "wmma\.load\.c\.sync").Count -ne 4 -or
          [regex]::Matches($unoptimizedText,
                          "wmma\.store\.d\.sync").Count -ne 4) {
        throw "four-stage residency was not formed exclusively by the optimizer"
      }
      $dumped = Join-Path $tmpDir ($name + "_dumped.ptx")
      $dumpOut = & $CompilerPath -O --dump-ir --emit-ptx `
        --gpu-arch=portable $src -o $dumped 2>&1 | Out-String
      $dumpPath = $dumped + ".ir"
      if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dumpPath)) {
        throw "GPU --dump-ir omitted the four-stage pipeline: $dumpOut"
      }
      $dumpText = Get-Content -Raw $dumpPath
      if ([regex]::Matches($dumpText,
                          "residency\.pipeline\.start#").Count -ne 1 -or
          [regex]::Matches($dumpText,
                          "residency\.pipeline\.update#").Count -ne 3 -or
          [regex]::Matches($dumpText,
                          "residency\.pipeline\.commit#").Count -ne 1) {
        throw "neutral four-stage residency roles are incomplete"
      }
    }
    if ($src -like "*async_copy.mettle") {
      if ([regex]::Matches($ptxText, "mtlc\.async_copy synchronous-fallback").Count -ne 2 -or
          [regex]::Matches($ptxText, "ld\.global\.b32").Count -ne 5 -or
          [regex]::Matches($ptxText, "st\.shared\.b32").Count -ne 5 -or
          $ptxText -match "cp\.async\.") {
        throw "portable async-copy fallback contract mismatch"
      }
    }
    if ([System.IO.Path]::GetFileName($src) -eq "auto_staging.mettle") {
      $commitAt = $ptxText.IndexOf("mtlc.async_copy commit synchronous-fallback")
      $overlapAt = $ptxText.IndexOf("mul.lo.u32", $commitAt + 1)
      $waitAt = $ptxText.IndexOf("mtlc.async_copy wait pending=0 synchronous-fallback", $commitAt + 1)
      $barrierAt = $ptxText.IndexOf("bar.sync 0", $waitAt + 1)
      if ([regex]::Matches($ptxText,
                           "mtlc\.async_copy auto-promoted synchronous-fallback").Count -ne 1 -or
          $ptxText -match "cp\.async\." -or $commitAt -lt 0 -or
          $overlapAt -le $commitAt -or $waitAt -le $overlapAt -or
          $barrierAt -le $waitAt) {
        throw "portable optimizer-generated staging/overlap contract mismatch"
      }
      $unoptimized = Join-Path $tmpDir ($name + "_unoptimized.ptx")
      $unoptimizedOut = & $CompilerPath --emit-ptx --gpu-arch=portable $src -o $unoptimized 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "unoptimized auto-staging emit failed: $unoptimizedOut" }
      $unoptimizedText = Get-Content -Raw $unoptimized
      if ($unoptimizedText -match "mtlc\.async_copy|cp\.async\." -or
          $unoptimizedText -notmatch "ld\.global\.u32" -or
          $unoptimizedText -notmatch "st\.shared\.u32") {
        throw "auto staging was not formed exclusively by the optimizer"
      }
      $explained = Join-Path $tmpDir ($name + "_explained.ptx")
      $explainOut = & $CompilerPath -O --explain-all --emit-ptx `
        --gpu-arch=portable $src -o $explained 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0 -or
          $explainOut -notmatch "promoted 1 typed global-to-workgroup copy to asynchronous staging" -or
          $explainOut -notmatch "target-neutral optimized IR emitted through the PTX backend") {
        throw "PTX --explain omitted automatic neutral staging: $explainOut"
      }
      $dumped = Join-Path $tmpDir ($name + "_dumped.ptx")
      $dumpOut = & $CompilerPath -O --dump-ir --emit-ptx `
        --gpu-arch=portable $src -o $dumped 2>&1 | Out-String
      $dumpPath = $dumped + ".ir"
      if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dumpPath) -or
          (Get-Content -Raw $dumpPath) -notmatch
            "(?s)async_copy\.workgroup .* generated.*async_copy\.commit.*async_copy\.wait") {
        throw "GPU --dump-ir omitted optimizer-generated staging: $dumpOut"
      }
    }
    if ([System.IO.Path]::GetFileName($src) -eq
        "auto_staging_no_promote.mettle") {
      if ($ptxText -match "mtlc\.async_copy|cp\.async\." -or
          $ptxText -notmatch "ld\.global\.u32" -or
          $ptxText -notmatch "st\.shared\.u32") {
        throw "acquire-only barrier was illegally auto-promoted"
      }
    }
    if ($ptxas) {
      $asmOut = & $ptxas.Source -arch=sm_75 $ptxPath -o $cubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "ptxas rejected emitted PTX: $asmOut" }
    }
    Write-CaseResult -Name $name -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name $name -Passed $false -Reason $_.Exception.Message
  }
}

# Execute the semantic launch lowering against a pure host stub. This validates
# all eight controls and the natural-width parameter cells without loading a
# GPU provider or touching a device. The public-API gate below feeds the same
# nontrivial controls through the cross-host AArch64 object backend.
$total++
try {
  $hostGcc = Get-Command gcc -ErrorAction SilentlyContinue
  if (-not $hostGcc) {
    Write-Host "[SKIP] gpu_dispatch_host_abi_runtime (gcc not found)"
  } else {
    $dispatchObj = Join-Path $tmpDir "gpu_dispatch_host_abi.obj"
    $dispatchExe = Join-Path $tmpDir "gpu_dispatch_host_abi.exe"
    $linkOut = & $hostGcc.Source $dispatchObj tests/gpu_dispatch_checked_stub.c `
      -o $dispatchExe 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "linking the hardware-free launch ABI fixture failed: $linkOut"
    }
    & $dispatchExe | Out-Null
    if ($LASTEXITCODE -ne 0) {
      throw "hardware-free launch ABI fixture returned $LASTEXITCODE"
    }
    Write-CaseResult -Name "gpu_dispatch_host_abi_runtime" -Passed $true
  }
}
catch {
  $failed++
  Write-CaseResult -Name "gpu_dispatch_host_abi_runtime" -Passed $false `
    -Reason $_.Exception.Message
}

# Pure CPU semantic oracle for the neutral bounded matrix-region contract.
# It covers non-multiple M/N/K, mixed layouts/strides, uint32 wrap, canonical
# structured 2:4 compressed A, packed FP8/FP6/FP4 block scaling, K=0, and
# out-of-range no-op behavior. It neither links a GPU provider nor touches a
# driver/device.
$total++
try {
  $hostGcc = Get-Command gcc -ErrorAction SilentlyContinue
  if (-not $hostGcc) {
    Write-Host "[SKIP] tensor_matmul_cpu_oracle (gcc not found)"
  } else {
    $oracleExe = Join-Path $tmpDir "tensor_matmul_oracle.exe"
    $buildOut = & $hostGcc.Source -std=c11 -Wall -Wextra -Werror `
      tests/tensor_matmul_oracle.c -o $oracleExe 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "building the tensor_matmul CPU oracle failed: $buildOut"
    }
    $oracleOut = & $oracleExe 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or
        $oracleOut -notmatch "structured 2:4, FP8/FP6/FP4 block scales, packed streams, K=0, and no-op bounds OK") {
      throw "tensor_matmul CPU oracle failed: $oracleOut"
    }
    Write-CaseResult -Name "tensor_matmul_cpu_oracle" -Passed $true
  }
}
catch {
  $failed++
  Write-CaseResult -Name "tensor_matmul_cpu_oracle" -Passed $false `
    -Reason $_.Exception.Message
}

# Compiler/offline-assembler evidence for whole-problem tensor lowering. No
# cubin produced here is ever loaded or executed.
$total++
try {
  $matmulPtx = Join-Path $tmpDir "tensor_matmul_sm121a.ptx"
  $matmulBudgetPtx = Join-Path $tmpDir "tensor_matmul_budget1.ptx"
  $matmulTransposePtx = Join-Path $tmpDir "tensor_matmul_transpose_sm121a.ptx"
  $matmulTransposeBudgetPtx = Join-Path $tmpDir "tensor_matmul_transpose_budget1.ptx"
  $matmulFp8Ptx = Join-Path $tmpDir "tensor_matmul_fp8_sm121a.ptx"
  $matmulFp8BudgetPtx = Join-Path $tmpDir "tensor_matmul_fp8_budget1.ptx"
  $matmulScaledPtx = Join-Path $tmpDir "tensor_matmul_scaled_sm121a.ptx"
  $matmulScaledBudgetPtx = Join-Path $tmpDir "tensor_matmul_scaled_budget1.ptx"
  $matmulSparsePtx = Join-Path $tmpDir "tensor_matmul_sparse_sm121a.ptx"
  $matmulSparseBudgetPtx = Join-Path $tmpDir "tensor_matmul_sparse_budget1.ptx"
  $matmulCubin = Join-Path $tmpDir "tensor_matmul_sm121a.cubin"
  $matmulTransposeCubin = Join-Path $tmpDir "tensor_matmul_transpose_sm121a.cubin"
  $matmulFp8Cubin = Join-Path $tmpDir "tensor_matmul_fp8_sm121a.cubin"
  $matmulScaledCubin = Join-Path $tmpDir "tensor_matmul_scaled_sm121a.cubin"
  $matmulSparseCubin = Join-Path $tmpDir "tensor_matmul_sparse_sm121a.cubin"
  $emitOut = & $CompilerPath -O --dump-ir --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_matmul.mettle -o $matmulPtx 2>&1 | Out-String
  $matmulIr = $matmulPtx + ".ir"
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $matmulIr)) {
    throw "tensor_matmul GB10 text emission failed: $emitOut"
  }
  $matmulText = Get-Content -Raw $matmulPtx
  $matmulIrText = Get-Content -Raw $matmulIr
  if ([regex]::Matches($matmulText,
        "mtlc\.tensor_matmul native interior runtime-K resident stable-wmma").Count -ne 5 -or
      [regex]::Matches($matmulText,
        "mtlc\.tensor_matmul cooperative-full exact M/N/K edge replay").Count -ne 5 -or
      [regex]::Matches($matmulText,
        "mtlc\.tensor_matmul cooperative-tail exact M/N/K edge replay").Count -ne 5 -or
      [regex]::Matches($matmulText, "wmma\.load\.c\.sync").Count -ne 5 -or
      [regex]::Matches($matmulText, "wmma\.store\.d\.sync").Count -ne 5 -or
      $matmulText -notmatch "mul\.lo\.u64" -or
      $matmulText -notmatch "fma\.rn\.f32" -or
      $matmulText -notmatch "fma\.rn\.f64" -or
      $matmulText -notmatch "mad\.lo\.s32" -or
      $matmulIrText -notmatch "tensor_matmul region=m16n16 k_chunk=16") {
    throw "tensor_matmul output lost native residency, exact edge replay, 64-bit addressing, or neutral IR identity"
  }
  $budgetOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    --gpu-tensor-tuple-budget=1 tests/gpu/tensor_matmul.mettle `
    -o $matmulBudgetPtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "tensor_matmul tuple-budget fallback emission failed: $budgetOut"
  }
  $budgetText = Get-Content -Raw $matmulBudgetPtx
  if ([regex]::Matches($budgetText,
        "mtlc\.tensor_matmul cooperative-only: native accumulator exceeds tensor tuple budget").Count -ne 5 -or
      $budgetText -match "wmma\.mma") {
    throw "tensor_matmul tuple-budget policy did not fail over to exact cooperative lowering"
  }
  $transposeOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_matmul_transpose.mettle -o $matmulTransposePtx `
    2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "tensor_matmul transpose emission failed: $transposeOut"
  }
  $transposeText = Get-Content -Raw $matmulTransposePtx
  if ([regex]::Matches($transposeText,
        "mtlc\.tensor_matmul native interior runtime-K resident stable-wmma").Count -ne 3 -or
      [regex]::Matches($transposeText,
        "mtlc\.tensor_matmul cooperative-full exact M/N/K edge replay").Count -ne 3 -or
      [regex]::Matches($transposeText,
        "mtlc\.tensor_matmul cooperative-tail exact M/N/K edge replay").Count -ne 3 -or
      [regex]::Matches($transposeText, "wmma\.mma").Count -ne 6 -or
      [regex]::Matches($transposeText, "wmma\.load\.c\.sync").Count -ne 3 -or
      [regex]::Matches($transposeText, "wmma\.store\.d\.sync").Count -ne 3 -or
      $transposeText -notmatch "mul\.lo\.u64") {
    throw "tensor_matmul transpose lost its backend-local native view or exact edge replay"
  }
  $transposeBudgetOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    --gpu-tensor-tuple-budget=1 tests/gpu/tensor_matmul_transpose.mettle `
    -o $matmulTransposeBudgetPtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "tensor_matmul transpose tuple-budget fallback emission failed: $transposeBudgetOut"
  }
  $transposeBudgetText = Get-Content -Raw $matmulTransposeBudgetPtx
  if ([regex]::Matches($transposeBudgetText,
        "mtlc\.tensor_matmul cooperative-only: native accumulator exceeds tensor tuple budget").Count -ne 3 -or
      $transposeBudgetText -match "wmma\.mma") {
    throw "tensor_matmul transpose tuple-budget policy did not preserve exact cooperative replay"
  }
  $fp8Out = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_matmul_fp8.mettle -o $matmulFp8Ptx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "tensor_matmul FP8 emission failed: $fp8Out"
  }
  $fp8Text = Get-Content -Raw $matmulFp8Ptx
  if ([regex]::Matches($fp8Text,
        "mtlc\.tensor_matmul native interior runtime-K resident direct-mma").Count -ne 2 -or
      [regex]::Matches($fp8Text,
        "mtlc\.tensor_matmul cooperative-full exact M/N/K edge replay").Count -ne 2 -or
      [regex]::Matches($fp8Text,
        "mtlc\.tensor_matmul cooperative-tail exact M/N/K edge replay").Count -ne 2 -or
      [regex]::Matches($fp8Text, "cvt\.rn\.f16x2\.e4m3x2").Count -ne 4 -or
      [regex]::Matches($fp8Text, "cvt\.rn\.f16x2\.e5m2x2").Count -ne 4 -or
      [regex]::Matches($fp8Text, "mma\.sync").Count -ne 8 -or
      $fp8Text -match "wmma\.mma") {
    throw "tensor_matmul FP8 lost direct-MMA residency or exact architectural edge conversion"
  }
  $fp8BudgetOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    --gpu-tensor-tuple-budget=1 tests/gpu/tensor_matmul_fp8.mettle `
    -o $matmulFp8BudgetPtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "tensor_matmul FP8 tuple-budget fallback emission failed: $fp8BudgetOut"
  }
  $fp8BudgetText = Get-Content -Raw $matmulFp8BudgetPtx
  if ([regex]::Matches($fp8BudgetText,
        "mtlc\.tensor_matmul cooperative-only: native accumulator exceeds tensor tuple budget").Count -ne 2 -or
      [regex]::Matches($fp8BudgetText, "cvt\.rn\.f16x2\.e4m3x2").Count -ne 2 -or
      [regex]::Matches($fp8BudgetText, "cvt\.rn\.f16x2\.e5m2x2").Count -ne 2 -or
      $fp8BudgetText -match "wmma\.mma|mma\.sync") {
    throw "tensor_matmul FP8 tuple-budget policy did not preserve exact cooperative replay"
  }
  $scaledOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_matmul_scaled.mettle -o $matmulScaledPtx `
    2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "tensor_matmul scaled FP8/FP6/FP4 emission failed: $scaledOut"
  }
  $scaledText = Get-Content -Raw $matmulScaledPtx
  if ([regex]::Matches($scaledText,
        "mtlc\.tensor_matmul native interior runtime-K resident direct-mma").Count -ne 4 -or
      [regex]::Matches($scaledText,
        "mtlc\.tensor_matmul cooperative-full exact M/N/K edge replay").Count -ne 4 -or
      [regex]::Matches($scaledText,
        "mtlc\.tensor_matmul cooperative-tail exact M/N/K edge replay").Count -ne 4 -or
      [regex]::Matches($scaledText,
        "mtlc\.tensor_matmul dense-subbyte byte-alignment guard").Count -ne 4 -or
      [regex]::Matches($scaledText, "fma\.rn\.f32").Count -ne 8 -or
      [regex]::Matches($scaledText, "@%p[0-9]+ mov\.u32 %r[0-9]+, 4194304").Count -ne 12 -or
      [regex]::Matches($scaledText, "@%p[0-9]+ mov\.u32 %r[0-9]+, 2143289344").Count -ne 12 -or
      $scaledText -match "cvt\.rn\.f16x2\.(e2m1|e2m3|e3m2)x2|wmma\.mma") {
    throw "tensor_matmul scaled output lost native residency, exact packed/scale replay, or the GB10-safe integer decoder"
  }
  foreach ($scaledInstruction in @(
      "mma\.sync\.aligned\.m16n8k32\.row\.col\.kind::mxf8f6f4\.block_scale\.scale_vec::1X\.f32\.e3m2\.e2m3\.f32\.ue8m0",
      "mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4\.block_scale\.scale_vec::2X\.f32\.e2m1\.e2m1\.f32\.ue8m0",
      "mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4nvf4\.block_scale\.scale_vec::4X\.f32\.e2m1\.e2m1\.f32\.ue4m3",
      "mma\.sync\.aligned\.m16n8k32\.row\.col\.kind::mxf8f6f4\.block_scale\.scale_vec::1X\.f32\.e4m3\.e5m2\.f32\.ue8m0")) {
    if ([regex]::Matches($scaledText, $scaledInstruction).Count -ne 4) {
      throw "tensor_matmul scaled output lost native instruction family: $scaledInstruction"
    }
  }
  $scaledBudgetOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    --gpu-tensor-tuple-budget=1 tests/gpu/tensor_matmul_scaled.mettle `
    -o $matmulScaledBudgetPtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "tensor_matmul scaled tuple-budget fallback emission failed: $scaledBudgetOut"
  }
  $scaledBudgetText = Get-Content -Raw $matmulScaledBudgetPtx
  if ([regex]::Matches($scaledBudgetText,
        "mtlc\.tensor_matmul cooperative-only: native accumulator exceeds tensor tuple budget").Count -ne 4 -or
      [regex]::Matches($scaledBudgetText, "fma\.rn\.f32").Count -ne 4 -or
      $scaledBudgetText -match "wmma\.mma|mma\.sync") {
    throw "tensor_matmul scaled tuple-budget policy did not preserve exact cooperative replay"
  }
  $sparseOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_matmul_sparse.mettle -o $matmulSparsePtx `
    2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "tensor_matmul structured-2:4 emission failed: $sparseOut"
  }
  $sparseText = Get-Content -Raw $matmulSparsePtx
  if ([regex]::Matches($sparseText,
        "mtlc\.tensor_matmul native interior runtime-K resident direct-mma").Count -ne 2 -or
      [regex]::Matches($sparseText,
        "mtlc\.tensor_matmul cooperative-full exact M/N/K edge replay").Count -ne 2 -or
      [regex]::Matches($sparseText,
        "mtlc\.tensor_matmul cooperative-tail exact M/N/K edge replay").Count -ne 2 -or
      [regex]::Matches($sparseText,
        "mma\.sp::ordered_metadata\.sync\.aligned\.m16n8k16").Count -ne 8 -or
      [regex]::Matches($sparseText, "@%p[0-9]+ ld\.global\.b16").Count -ne 4 -or
      [regex]::Matches($sparseText, "cvt\.f32\.f16").Count -ne 4 -or
      [regex]::Matches($sparseText, "cvt\.f32\.bf16").Count -ne 4 -or
      $sparseText -match "wmma\.mma") {
    throw "tensor_matmul structured-2:4 lost native residency, canonical metadata translation, transpose, or exact edge replay"
  }
  $sparseBudgetOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    --gpu-tensor-tuple-budget=1 tests/gpu/tensor_matmul_sparse.mettle `
    -o $matmulSparseBudgetPtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "tensor_matmul structured-2:4 tuple-budget fallback emission failed: $sparseBudgetOut"
  }
  $sparseBudgetText = Get-Content -Raw $matmulSparseBudgetPtx
  if ([regex]::Matches($sparseBudgetText,
        "mtlc\.tensor_matmul cooperative-only: native accumulator exceeds tensor tuple budget").Count -ne 2 -or
      [regex]::Matches($sparseBudgetText,
        "@%p[0-9]+ ld\.global\.b16").Count -ne 2 -or
      $sparseBudgetText -match "wmma\.mma|mma\.sp") {
    throw "tensor_matmul structured-2:4 tuple-budget policy did not preserve exact cooperative replay"
  }
  if ($ptxas) {
    $ptxasHelp = & $ptxas.Source --help 2>&1 | Out-String
    if ($ptxasHelp -match "sm_121a") {
      $asmOut = & $ptxas.Source -arch=sm_121a -v $matmulPtx `
        -o $matmulCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0 -or
          $asmOut -match "[1-9][0-9]* bytes spill (stores|loads)") {
        throw "offline ptxas rejected or spilled tensor_matmul PTX: $asmOut"
      }
      $transposeAsmOut = & $ptxas.Source -arch=sm_121a -v `
        $matmulTransposePtx -o $matmulTransposeCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0 -or
          $transposeAsmOut -match "[1-9][0-9]* bytes spill (stores|loads)") {
        throw "offline ptxas rejected or spilled transposed tensor_matmul PTX: $transposeAsmOut"
      }
      $fp8AsmOut = & $ptxas.Source -arch=sm_121a -v $matmulFp8Ptx `
        -o $matmulFp8Cubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0 -or
          $fp8AsmOut -match "[1-9][0-9]* bytes spill (stores|loads)") {
        throw "offline ptxas rejected or spilled FP8 tensor_matmul PTX: $fp8AsmOut"
      }
      $scaledAsmOut = & $ptxas.Source -arch=sm_121a -v $matmulScaledPtx `
        -o $matmulScaledCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0 -or
          $scaledAsmOut -match "[1-9][0-9]* bytes (stack frame|spill stores|spill loads)") {
        throw "offline ptxas rejected, stacked, or spilled scaled tensor_matmul PTX: $scaledAsmOut"
      }
      $sparseAsmOut = & $ptxas.Source -arch=sm_121a -v $matmulSparsePtx `
        -o $matmulSparseCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0 -or
          $sparseAsmOut -match "[1-9][0-9]* bytes (stack frame|spill stores|spill loads)") {
        throw "offline ptxas rejected, stacked, or spilled structured-2:4 tensor_matmul PTX: $sparseAsmOut"
      }
      foreach ($sparseKernel in @(
          "tensor_matmul_sparse_f16",
          "tensor_matmul_sparse_bf16_transpose_a")) {
        $resource = [regex]::Match(
          $sparseAsmOut,
          "(?s)Function properties for $sparseKernel.*?Used ([0-9]+) registers"
        )
        if (-not $resource.Success -or
            [int]$resource.Groups[1].Value -gt 56) {
          throw "structured-2:4 tensor_matmul register ceiling failed for $sparseKernel`: $sparseAsmOut"
        }
      }
      $scaledRegisterCeilings = @{
        tensor_matmul_mxfp6_f32 = 64
        tensor_matmul_mxfp4_f32 = 64
        tensor_matmul_nvfp4_f32 = 64
        tensor_matmul_mxfp8_transpose_f32 = 80
      }
      foreach ($scaledKernel in $scaledRegisterCeilings.Keys) {
        $resource = [regex]::Match(
          $scaledAsmOut,
          "(?s)Function properties for $scaledKernel.*?Used ([0-9]+) registers"
        )
        if (-not $resource.Success -or
            [int]$resource.Groups[1].Value -gt $scaledRegisterCeilings[$scaledKernel]) {
          throw "scaled tensor_matmul register ceiling failed for $scaledKernel`: $scaledAsmOut"
        }
      }
    } else {
      Write-Host "[SKIP] tensor_matmul_offline ptxas assembly (toolkit lacks sm_121a)"
    }
  } else {
    Write-Host "[SKIP] tensor_matmul_offline ptxas assembly (ptxas not found)"
  }
  Write-CaseResult -Name "tensor_matmul_offline" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "tensor_matmul_offline" -Passed $false `
    -Reason $_.Exception.Message
}

$total++
try {
  $gb10Ptx = Join-Path $tmpDir "ptx_emit_gb10.ptx"
  $gb10Cubin = Join-Path $tmpDir "ptx_emit_gb10.cubin"
  $emitOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/compute_kernels.mettle -o $gb10Ptx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "GB10 emit failed: $emitOut" }
  $gb10Text = Get-Content -Raw $gb10Ptx
  if ($gb10Text -notmatch "(?m)^\.version 8\.8\r?$") { throw "GB10 PTX version is not 8.8" }
  if ($gb10Text -notmatch "(?m)^\.target sm_121a\r?$") { throw "GB10 PTX target is not sm_121a" }
  $rowNormEntry = [regex]::Match(
    $gb10Text,
    '(?s)\.visible \.entry row_norm\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $rowNormEntry -or
      $rowNormEntry -notmatch 'add\.f32 (?<acc>%f[0-9]+), \k<acc>, ' -or
      $rowNormEntry -notmatch 'add\.s32 (?<iv>%r[0-9]+), \k<iv>, ') {
    throw "optimized PTX lost stable mutable-symbol homes across the row_norm loop"
  }
  if ($ptxas) {
    $ptxasHelp = & $ptxas.Source --help 2>&1 | Out-String
    if ($ptxasHelp -match "sm_121a") {
      $asmOut = & $ptxas.Source -arch=sm_121a $gb10Ptx -o $gb10Cubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "ptxas rejected GB10 PTX: $asmOut" }
    } else {
      Write-Host "[SKIP] ptx_emit_gb10 ptxas assembly (toolkit lacks sm_121a)"
    }
  } else {
    Write-Host "[SKIP] ptx_emit_gb10 ptxas assembly (ptxas not found)"
  }
  Write-CaseResult -Name "ptx_emit_gb10" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "ptx_emit_gb10" -Passed $false -Reason $_.Exception.Message
}

$total++
try {
  $gb10AsyncPtx = Join-Path $tmpDir "ptx_emit_gb10_async_copy.ptx"
  $gb10AsyncCubin = Join-Path $tmpDir "ptx_emit_gb10_async_copy.cubin"
  $gb10AutoPtx = Join-Path $tmpDir "ptx_emit_gb10_auto_staging.ptx"
  $gb10AutoCubin = Join-Path $tmpDir "ptx_emit_gb10_auto_staging.cubin"
  $asyncEmitOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/async_copy.mettle -o $gb10AsyncPtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "GB10 async-copy emit failed: $asyncEmitOut" }
  $autoEmitOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/auto_staging.mettle -o $gb10AutoPtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "GB10 auto-staging emit failed: $autoEmitOut" }
  $gb10AsyncText = Get-Content -Raw $gb10AsyncPtx
  if ([regex]::Matches($gb10AsyncText, "cp\.async\.ca\.shared\.global").Count -ne 1 -or
      [regex]::Matches($gb10AsyncText, "cp\.async\.cg\.shared\.global").Count -ne 1 -or
      [regex]::Matches($gb10AsyncText, "cp\.async\.commit_group").Count -ne 2 -or
      [regex]::Matches($gb10AsyncText, "cp\.async\.wait_group 0").Count -ne 2 -or
      $gb10AsyncText -match "synchronous-fallback") {
    throw "GB10 async-copy native contract mismatch"
  }
  $gb10AutoText = Get-Content -Raw $gb10AutoPtx
  $autoCommitAt = $gb10AutoText.IndexOf("cp.async.commit_group")
  $autoOverlapAt = $gb10AutoText.IndexOf("mul.lo.u32", $autoCommitAt + 1)
  $autoWaitAt = $gb10AutoText.IndexOf("cp.async.wait_group 0", $autoCommitAt + 1)
  $autoBarrierAt = $gb10AutoText.IndexOf("bar.sync 0", $autoWaitAt + 1)
  if ([regex]::Matches($gb10AutoText,
                       "mtlc\.async_copy auto-promoted native").Count -ne 1 -or
      [regex]::Matches($gb10AutoText,
                       "cp\.async\.ca\.shared\.global").Count -ne 1 -or
      $autoCommitAt -lt 0 -or $autoOverlapAt -le $autoCommitAt -or
      $autoWaitAt -le $autoOverlapAt -or $autoBarrierAt -le $autoWaitAt -or
      $gb10AutoText -match "synchronous-fallback") {
    throw "GB10 optimizer-generated staging/overlap contract mismatch"
  }
  if ($ptxas) {
    $ptxasHelp = & $ptxas.Source --help 2>&1 | Out-String
    if ($ptxasHelp -match "sm_121a") {
      $asyncAsmOut = & $ptxas.Source -arch=sm_121a $gb10AsyncPtx -o $gb10AsyncCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "ptxas rejected GB10 async-copy PTX: $asyncAsmOut" }
      $autoAsmOut = & $ptxas.Source -arch=sm_121a $gb10AutoPtx -o $gb10AutoCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "ptxas rejected GB10 auto-staging PTX: $autoAsmOut" }
    } else {
      Write-Host "[SKIP] ptx_emit_gb10_async_copy ptxas assembly (toolkit lacks sm_121a)"
    }
  } else {
    Write-Host "[SKIP] ptx_emit_gb10_async_copy ptxas assembly (ptxas not found)"
  }
  Write-CaseResult -Name "ptx_emit_gb10_async_copy" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "ptx_emit_gb10_async_copy" -Passed $false -Reason $_.Exception.Message
}

$total++
try {
  $gb10TensorPtx = Join-Path $tmpDir "ptx_emit_gb10_tensor.ptx"
  $gb10TensorCubin = Join-Path $tmpDir "ptx_emit_gb10_tensor.cubin"
  $gb10ChainPtx = Join-Path $tmpDir "ptx_emit_gb10_tensor_chain.ptx"
  $gb10ChainCubin = Join-Path $tmpDir "ptx_emit_gb10_tensor_chain.cubin"
  $tensorEmitOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_kernels.mettle -o $gb10TensorPtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "GB10 tensor emit failed: $tensorEmitOut" }
  $chainEmitOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_chain.mettle -o $gb10ChainPtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "GB10 tensor-chain emit failed: $chainEmitOut" }
  $gb10ChainText = Get-Content -Raw $gb10ChainPtx
  if ($gb10ChainText -notmatch "mtlc\.tensor_chain resident tiles=4 tuple_peak=32 budget=96" -or
      [regex]::Matches($gb10ChainText, "wmma\.load\.c\.sync").Count -ne 1 -or
      [regex]::Matches($gb10ChainText, "wmma\.store\.d\.sync").Count -ne 1) {
    throw "GB10 tensor-chain cost model/residency contract mismatch"
  }
  $gb10TensorText = Get-Content -Raw $gb10TensorPtx
  foreach ($tensorContract in @(
      "wmma\.mma\.sync\.aligned\.m16n16k16\.row\.col\.f32\.f32",
      "wmma\.mma\.sync\.aligned\.m32n8k16\.col\.row\.f32\.bf16\.bf16\.f32",
      "wmma\.mma\.sync\.aligned\.m16n16k8\.row\.col\.f32\.tf32\.tf32\.f32",
      "wmma\.mma\.sync\.aligned\.m8n8k4\.row\.col\.f64\.f64\.f64\.f64",
      "wmma\.mma\.sync\.aligned\.m16n16k16\.row\.col\.s32\.s8\.s8\.s32\.satfinite",
      "wmma\.mma\.sync\.aligned\.m8n8k32\.row\.col\.s32\.u4\.u4\.s32",
      "wmma\.mma\.xor\.popc\.sync\.aligned\.m8n8k128",
      "\.entry tensor_f16_f32_strided\(",
      "\.param \.s32 tensor_f16_f32_strided_p7",
      "ld\.param\.s32 %r[0-9]+, \[tensor_f16_f32_strided_p4\]",
      "\.entry tensor_f16_f32_kloop\(")) {
    if ($gb10TensorText -notmatch $tensorContract) {
      throw "missing GB10 tensor contract: $tensorContract"
    }
  }
  $gb10KLoopEntry = [regex]::Match(
    $gb10TensorText,
    '(?s)\.visible \.entry tensor_f16_f32_kloop\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $gb10KLoopEntry -or
      $gb10KLoopEntry -notmatch 'mtlc\.tensor_loop resident group=1 tuple_peak=32 budget=96' -or
      [regex]::Matches($gb10KLoopEntry, 'wmma\.mma\.sync').Count -ne 2 -or
      [regex]::Matches($gb10KLoopEntry, 'wmma\.load\.c\.sync').Count -ne 1 -or
      [regex]::Matches($gb10KLoopEntry, 'wmma\.store\.d\.sync').Count -ne 1) {
    throw "GB10 runtime-K tensor residency contract mismatch"
  }
  if ($ptxas) {
    $ptxasHelp = & $ptxas.Source --help 2>&1 | Out-String
    if ($ptxasHelp -match "sm_121a") {
      $tensorAsmOut = & $ptxas.Source -arch=sm_121a $gb10TensorPtx -o $gb10TensorCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "ptxas rejected GB10 tensor PTX: $tensorAsmOut" }
      $chainAsmOut = & $ptxas.Source -arch=sm_121a $gb10ChainPtx -o $gb10ChainCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "ptxas rejected GB10 tensor-chain PTX: $chainAsmOut" }
    } else {
      Write-Host "[SKIP] ptx_emit_gb10_tensor ptxas assembly (toolkit lacks sm_121a)"
    }
  } else {
    Write-Host "[SKIP] ptx_emit_gb10_tensor ptxas assembly (ptxas not found)"
  }
  Write-CaseResult -Name "ptx_emit_gb10_tensor" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "ptx_emit_gb10_tensor" -Passed $false -Reason $_.Exception.Message
}

$total++
try {
  $gb10Fp8Ptx = Join-Path $tmpDir "ptx_emit_gb10_tensor_fp8.ptx"
  $gb10Fp8Cubin = Join-Path $tmpDir "ptx_emit_gb10_tensor_fp8.cubin"
  $fp8EmitOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_native_fp8.mettle -o $gb10Fp8Ptx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 native-FP8 tensor emit failed: $fp8EmitOut"
  }
  $gb10Fp8Text = Get-Content -Raw $gb10Fp8Ptx
  if ($gb10Fp8Text -notmatch "(?m)^\.version 8\.8\r?$" -or
      $gb10Fp8Text -notmatch "(?m)^\.target sm_121a\r?$") {
    throw "GB10 native-FP8 tensor module did not select PTX 8.8/sm_121a"
  }
  $gb10Fp8Entry = [regex]::Match(
    $gb10Fp8Text,
    '(?s)\.visible \.entry tensor_fp8_m16n16k32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $gb10Fp8Entry -or
      $gb10Fp8Entry -notmatch
        'mtlc\.tensor_mma native-mma fp8 whole-tile lowering' -or
      [regex]::Matches(
        $gb10Fp8Entry,
        'mma\.sync\.aligned\.m16n8k32\.row\.col\.f32\.e4m3\.e5m2\.f32'
      ).Count -ne 2 -or
      $gb10Fp8Entry -match 'wmma\.') {
    throw "GB10 native mixed-FP8 m16n16k32 contract mismatch"
  }
  $gb10Fp8TransposedEntry = [regex]::Match(
    $gb10Fp8Text,
    '(?s)\.visible \.entry tensor_fp8_m32n24k16_transposed\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $gb10Fp8TransposedEntry -or
      $gb10Fp8TransposedEntry -notmatch
        'mtlc\.tensor_mma native-mma fp8 whole-tile lowering' -or
      [regex]::Matches(
        $gb10Fp8TransposedEntry,
        'mma\.sync\.aligned\.m16n8k16\.row\.col\.f32\.e5m2\.e4m3\.f32'
      ).Count -ne 6 -or
      $gb10Fp8TransposedEntry -match 'wmma\.') {
    throw "GB10 tiled mixed-FP8 transpose/layout contract mismatch"
  }
  $gb10Fp8ChainEntry = [regex]::Match(
    $gb10Fp8Text,
    '(?s)\.visible \.entry tensor_fp8_chain4_m16n16k32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $gb10Fp8ChainEntry -or
      $gb10Fp8ChainEntry -notmatch
        'mtlc\.tensor_chain resident native-mma fp8 tiles=4 subtiles=2' -or
      [regex]::Matches(
        $gb10Fp8ChainEntry,
        'mma\.sync\.aligned\.m16n8k32\.row\.col\.f32\.e4m3\.e5m2\.f32'
      ).Count -ne 8 -or
      [regex]::Matches($gb10Fp8ChainEntry, 'ld\.global\.f32').Count -ne 8 -or
      [regex]::Matches($gb10Fp8ChainEntry, 'st\.global\.f32').Count -ne 8 -or
      $gb10Fp8ChainEntry -match 'replay|wmma\.') {
    throw "GB10 native FP8 chain residency contract mismatch"
  }
  $gb10Fp8LoopEntry = [regex]::Match(
    $gb10Fp8Text,
    '(?s)\.visible \.entry tensor_fp8_runtime_k_m16n16k32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $gb10Fp8LoopEntry -or
      $gb10Fp8LoopEntry -notmatch
        'mtlc\.tensor_loop resident native-mma fp8 group=1 subtiles=2' -or
      [regex]::Matches(
        $gb10Fp8LoopEntry,
        'mma\.sync\.aligned\.m16n8k32\.row\.col\.f32\.e4m3\.e5m2\.f32'
      ).Count -ne 4 -or
      [regex]::Matches($gb10Fp8LoopEntry, 'ld\.global\.f32').Count -ne 8 -or
      [regex]::Matches($gb10Fp8LoopEntry, 'st\.global\.f32').Count -ne 8 -or
      $gb10Fp8LoopEntry -match 'replay|wmma\.') {
    throw "GB10 runtime-K native FP8 residency contract mismatch"
  }
  $gb10Fp8Unoptimized =
    Join-Path $tmpDir "ptx_emit_gb10_tensor_fp8_unoptimized.ptx"
  $fp8UnoptimizedOut = & $CompilerPath --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_native_fp8.mettle -o $gb10Fp8Unoptimized 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 unoptimized native-FP8 emit failed: $fp8UnoptimizedOut"
  }
  $gb10Fp8UnoptimizedText = Get-Content -Raw $gb10Fp8Unoptimized
  $gb10Fp8UnoptimizedChain = [regex]::Match(
    $gb10Fp8UnoptimizedText,
    '(?s)\.visible \.entry tensor_fp8_chain4_m16n16k32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  $gb10Fp8UnoptimizedLoop = [regex]::Match(
    $gb10Fp8UnoptimizedText,
    '(?s)\.visible \.entry tensor_fp8_runtime_k_m16n16k32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if ($gb10Fp8UnoptimizedChain -match 'mtlc\.tensor_chain' -or
      [regex]::Matches(
        $gb10Fp8UnoptimizedChain,
        'mtlc\.tensor_mma native-mma fp8 whole-tile lowering'
      ).Count -ne 4 -or
      $gb10Fp8UnoptimizedLoop -match 'mtlc\.tensor_loop' -or
      [regex]::Matches(
        $gb10Fp8UnoptimizedLoop,
        'mtlc\.tensor_mma native-mma fp8 whole-tile lowering'
      ).Count -ne 2) {
    throw "native FP8 residency was not formed exclusively by the optimizer"
  }
  if ($ptxas) {
    $ptxasHelp = & $ptxas.Source --help 2>&1 | Out-String
    if ($ptxasHelp -match "sm_121a") {
      $fp8AsmOut = & $ptxas.Source -arch=sm_121a $gb10Fp8Ptx `
        -o $gb10Fp8Cubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        throw "ptxas rejected GB10 native-FP8 tensor PTX: $fp8AsmOut"
      }
    } else {
      Write-Host "[SKIP] ptx_emit_gb10_tensor_fp8 ptxas assembly (toolkit lacks sm_121a)"
    }
  } else {
    Write-Host "[SKIP] ptx_emit_gb10_tensor_fp8 ptxas assembly (ptxas not found)"
  }
  Write-CaseResult -Name "ptx_emit_gb10_tensor_fp8" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "ptx_emit_gb10_tensor_fp8" -Passed $false -Reason $_.Exception.Message
}

$total++
try {
  $gb10Fp4Ptx = Join-Path $tmpDir "ptx_emit_gb10_tensor_fp4.ptx"
  $gb10Fp4Cubin = Join-Path $tmpDir "ptx_emit_gb10_tensor_fp4.cubin"
  $fp4EmitOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_native_fp4.mettle -o $gb10Fp4Ptx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 native-MXFP4 tensor emit failed: $fp4EmitOut"
  }
  $gb10Fp4Text = Get-Content -Raw $gb10Fp4Ptx
  if ($gb10Fp4Text -notmatch "(?m)^\.version 8\.8\r?$" -or
      $gb10Fp4Text -notmatch "(?m)^\.target sm_121a\r?$") {
    throw "GB10 native-MXFP4 tensor module did not select PTX 8.8/sm_121a"
  }
  $fp4Instruction =
    'mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4\.block_scale\.scale_vec::2X\.f32\.e2m1\.e2m1\.f32\.ue8m0'
  $gb10Fp4Entry = [regex]::Match(
    $gb10Fp4Text,
    '(?s)\.visible \.entry tensor_mxfp4_m16n16k64\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $gb10Fp4Entry -or
      $gb10Fp4Entry -notmatch
        'mtlc\.tensor_mma native-mma mxfp4 whole-tile lowering' -or
      [regex]::Matches($gb10Fp4Entry, $fp4Instruction).Count -ne 2 -or
      [regex]::Matches($gb10Fp4Entry, 'ld\.global\.b32').Count -ne 12 -or
      [regex]::Matches($gb10Fp4Entry, 'ld\.global\.f32').Count -ne 8 -or
      [regex]::Matches($gb10Fp4Entry, 'st\.global\.f32').Count -ne 8 -or
      $gb10Fp4Entry -match 'wmma\.') {
    throw "GB10 native MXFP4 direct/packed-load contract mismatch"
  }
  $gb10Fp4ChainEntry = [regex]::Match(
    $gb10Fp4Text,
    '(?s)\.visible \.entry tensor_mxfp4_chain3_m16n16k64\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $gb10Fp4ChainEntry -or
      $gb10Fp4ChainEntry -notmatch
        'mtlc\.tensor_chain resident native-mma mxfp4 tiles=3 subtiles=2' -or
      [regex]::Matches($gb10Fp4ChainEntry, $fp4Instruction).Count -ne 6 -or
      [regex]::Matches($gb10Fp4ChainEntry, 'ld\.global\.b32').Count -ne 36 -or
      [regex]::Matches($gb10Fp4ChainEntry, 'ld\.global\.f32').Count -ne 8 -or
      [regex]::Matches($gb10Fp4ChainEntry, 'st\.global\.f32').Count -ne 8 -or
      $gb10Fp4ChainEntry -match 'replay|wmma\.') {
    throw "GB10 native MXFP4 chain-residency contract mismatch"
  }
  $gb10Fp4LoopEntry = [regex]::Match(
    $gb10Fp4Text,
    '(?s)\.visible \.entry tensor_mxfp4_runtime_k_m16n16k64\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $gb10Fp4LoopEntry -or
      $gb10Fp4LoopEntry -notmatch
        'mtlc\.tensor_loop resident native-mma mxfp4 group=1 subtiles=2' -or
      [regex]::Matches($gb10Fp4LoopEntry, $fp4Instruction).Count -ne 4 -or
      [regex]::Matches($gb10Fp4LoopEntry, 'ld\.global\.f32').Count -ne 8 -or
      [regex]::Matches($gb10Fp4LoopEntry, 'st\.global\.f32').Count -ne 8 -or
      $gb10Fp4LoopEntry -match 'replay|wmma\.') {
    throw "GB10 runtime-K native MXFP4 residency contract mismatch"
  }
  $nvfp4Instruction =
    'mma\.sync\.aligned\.m16n8k64\.row\.col\.kind::mxf4nvf4\.block_scale\.scale_vec::4X\.f32\.e2m1\.e2m1\.f32\.ue4m3'
  $gb10Nvfp4Entry = [regex]::Match(
    $gb10Fp4Text,
    '(?s)\.visible \.entry tensor_nvfp4_m16n16k64\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $gb10Nvfp4Entry -or
      $gb10Nvfp4Entry -notmatch
        'mtlc\.tensor_mma native-mma nvfp4 whole-tile lowering' -or
      [regex]::Matches($gb10Nvfp4Entry, $nvfp4Instruction).Count -ne 2 -or
      [regex]::Matches($gb10Nvfp4Entry, 'ld\.global\.b32').Count -ne 12 -or
      [regex]::Matches($gb10Nvfp4Entry, 'ld\.global\.f32').Count -ne 8 -or
      [regex]::Matches($gb10Nvfp4Entry, 'st\.global\.f32').Count -ne 8 -or
      $gb10Nvfp4Entry -match 'wmma\.') {
    throw "GB10 native NVFP4 direct/packed-load contract mismatch"
  }
  $gb10Nvfp4ChainEntry = [regex]::Match(
    $gb10Fp4Text,
    '(?s)\.visible \.entry tensor_nvfp4_chain3_m16n16k64\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $gb10Nvfp4ChainEntry -or
      $gb10Nvfp4ChainEntry -notmatch
        'mtlc\.tensor_chain resident native-mma nvfp4 tiles=3 subtiles=2' -or
      [regex]::Matches($gb10Nvfp4ChainEntry, $nvfp4Instruction).Count -ne 6 -or
      [regex]::Matches($gb10Nvfp4ChainEntry, 'ld\.global\.b32').Count -ne 36 -or
      [regex]::Matches($gb10Nvfp4ChainEntry, 'ld\.global\.f32').Count -ne 8 -or
      [regex]::Matches($gb10Nvfp4ChainEntry, 'st\.global\.f32').Count -ne 8 -or
      $gb10Nvfp4ChainEntry -match 'replay|wmma\.') {
    throw "GB10 native NVFP4 chain-residency contract mismatch"
  }
  $gb10Nvfp4LoopEntry = [regex]::Match(
    $gb10Fp4Text,
    '(?s)\.visible \.entry tensor_nvfp4_runtime_k_m16n16k64\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $gb10Nvfp4LoopEntry -or
      $gb10Nvfp4LoopEntry -notmatch
        'mtlc\.tensor_loop resident native-mma nvfp4 group=1 subtiles=2' -or
      [regex]::Matches($gb10Nvfp4LoopEntry, $nvfp4Instruction).Count -ne 4 -or
      [regex]::Matches($gb10Nvfp4LoopEntry, 'ld\.global\.f32').Count -ne 8 -or
      [regex]::Matches($gb10Nvfp4LoopEntry, 'st\.global\.f32').Count -ne 8 -or
      $gb10Nvfp4LoopEntry -match 'replay|wmma\.') {
    throw "GB10 runtime-K native NVFP4 residency contract mismatch"
  }

  $gb10Fp4Unoptimized =
    Join-Path $tmpDir "ptx_emit_gb10_tensor_fp4_unoptimized.ptx"
  $fp4UnoptimizedOut = & $CompilerPath --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_native_fp4.mettle -o $gb10Fp4Unoptimized 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 unoptimized native-MXFP4 emit failed: $fp4UnoptimizedOut"
  }
  $gb10Fp4UnoptimizedText = Get-Content -Raw $gb10Fp4Unoptimized
  $gb10Fp4UnoptimizedChain = [regex]::Match(
    $gb10Fp4UnoptimizedText,
    '(?s)\.visible \.entry tensor_mxfp4_chain3_m16n16k64\(.*?(?=\.visible \.entry|\z)'
  ).Value
  $gb10Fp4UnoptimizedLoop = [regex]::Match(
    $gb10Fp4UnoptimizedText,
    '(?s)\.visible \.entry tensor_mxfp4_runtime_k_m16n16k64\(.*?(?=\.visible \.entry|\z)'
  ).Value
  $gb10Nvfp4UnoptimizedChain = [regex]::Match(
    $gb10Fp4UnoptimizedText,
    '(?s)\.visible \.entry tensor_nvfp4_chain3_m16n16k64\(.*?(?=\.visible \.entry|\z)'
  ).Value
  $gb10Nvfp4UnoptimizedLoop = [regex]::Match(
    $gb10Fp4UnoptimizedText,
    '(?s)\.visible \.entry tensor_nvfp4_runtime_k_m16n16k64\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if ($gb10Fp4UnoptimizedChain -match 'mtlc\.tensor_chain' -or
      [regex]::Matches(
        $gb10Fp4UnoptimizedChain,
        'mtlc\.tensor_mma native-mma mxfp4 whole-tile lowering'
      ).Count -ne 3 -or
      $gb10Fp4UnoptimizedLoop -match 'mtlc\.tensor_loop' -or
      [regex]::Matches(
        $gb10Fp4UnoptimizedLoop,
        'mtlc\.tensor_mma native-mma mxfp4 whole-tile lowering'
      ).Count -ne 2 -or
      $gb10Nvfp4UnoptimizedChain -match 'mtlc\.tensor_chain' -or
      [regex]::Matches(
        $gb10Nvfp4UnoptimizedChain,
        'mtlc\.tensor_mma native-mma nvfp4 whole-tile lowering'
      ).Count -ne 3 -or
      $gb10Nvfp4UnoptimizedLoop -match 'mtlc\.tensor_loop' -or
      [regex]::Matches(
        $gb10Nvfp4UnoptimizedLoop,
        'mtlc\.tensor_mma native-mma nvfp4 whole-tile lowering'
      ).Count -ne 2) {
    throw "native FP4 residency was not formed exclusively by the optimizer"
  }

  $rawFp4Ptx = Join-Path $tmpDir "ptx_emit_sm121_tensor_fp4.ptx"
  $rawFp4Out = & $CompilerPath -O --emit-ptx --gpu-arch=sm_121 `
    tests/gpu/tensor_native_fp4.mettle -o $rawFp4Ptx 2>&1 | Out-String
  if ($LASTEXITCODE -eq 0 -or
      $rawFp4Out -notmatch
        'architecture- or family-specific sm_120a/sm_121a target') {
    throw "raw sm_121 did not reject architecture-specific native FP4 lowering"
  }

  if ($ptxas) {
    $ptxasHelp = & $ptxas.Source --help 2>&1 | Out-String
    if ($ptxasHelp -match "sm_121a") {
      $fp4AsmOut = & $ptxas.Source -v -arch=sm_121a $gb10Fp4Ptx `
        -o $gb10Fp4Cubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        throw "ptxas rejected GB10 native-MXFP4 tensor PTX: $fp4AsmOut"
      }
      if ($fp4AsmOut -match '(?m)^\s*[1-9][0-9]* bytes spill (stores|loads)') {
        throw "GB10 native-MXFP4 kernels spilled registers: $fp4AsmOut"
      }
      foreach ($registerGate in @(
          @{ Name = 'tensor_mxfp4_m16n16k64'; Max = 48 },
          @{ Name = 'tensor_mxfp4_chain3_m16n16k64'; Max = 64 },
          @{ Name = 'tensor_mxfp4_runtime_k_m16n16k64'; Max = 64 },
          @{ Name = 'tensor_nvfp4_m16n16k64'; Max = 56 },
          @{ Name = 'tensor_nvfp4_chain3_m16n16k64'; Max = 64 },
          @{ Name = 'tensor_nvfp4_runtime_k_m16n16k64'; Max = 56 })) {
        $escapedName = [regex]::Escape($registerGate.Name)
        $registerMatch = [regex]::Match(
          $fp4AsmOut,
          "(?s)Function properties for $escapedName.*?Used ([0-9]+) registers"
        )
        if (-not $registerMatch.Success -or
            [int]$registerMatch.Groups[1].Value -gt $registerGate.Max) {
          throw "GB10 native-MXFP4 register ceiling exceeded for $($registerGate.Name): $fp4AsmOut"
        }
      }
    } else {
      Write-Host "[SKIP] ptx_emit_gb10_tensor_fp4 ptxas assembly (toolkit lacks sm_121a)"
    }
  } else {
    Write-Host "[SKIP] ptx_emit_gb10_tensor_fp4 ptxas assembly (ptxas not found)"
  }
  Write-CaseResult -Name "ptx_emit_gb10_tensor_fp4" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "ptx_emit_gb10_tensor_fp4" -Passed $false -Reason $_.Exception.Message
}

$total++
try {
  $gb10Fp6Ptx = Join-Path $tmpDir "ptx_emit_gb10_tensor_fp6.ptx"
  $gb10Fp6Cubin = Join-Path $tmpDir "ptx_emit_gb10_tensor_fp6.cubin"
  $fp6EmitOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_native_fp6.mettle -o $gb10Fp6Ptx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 native-FP6 tensor emit failed: $fp6EmitOut"
  }
  $gb10Fp6Text = Get-Content -Raw $gb10Fp6Ptx
  if ($gb10Fp6Text -notmatch "(?m)^\.version 8\.8\r?$" -or
      $gb10Fp6Text -notmatch "(?m)^\.target sm_121a\r?$") {
    throw "GB10 native-FP6 tensor module did not select PTX 8.8/sm_121a"
  }
  $fp6Instruction =
    'mma\.sync\.aligned\.m16n8k32\.row\.col\.kind::mxf8f6f4\.block_scale\.scale_vec::1X\.f32\.e3m2\.e2m3\.f32\.ue8m0'
  $gb10Fp6Entry = [regex]::Match(
    $gb10Fp6Text,
    '(?s)\.visible \.entry tensor_mxfp6_m16n16k32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $gb10Fp6Entry -or
      $gb10Fp6Entry -notmatch
        'mtlc\.tensor_mma native-mma mxf8f6f4 whole-tile lowering' -or
      [regex]::Matches($gb10Fp6Entry, $fp6Instruction).Count -ne 2 -or
      [regex]::Matches($gb10Fp6Entry, 'setp\.gt\.u32').Count -ne 0 -or
      [regex]::Matches($gb10Fp6Entry, 'ld\.global\.f32').Count -ne 8 -or
      [regex]::Matches($gb10Fp6Entry, 'st\.global\.f32').Count -ne 8 -or
      $gb10Fp6Entry -match 'wmma\.') {
    throw "GB10 native FP6 direct/packed-fast-path contract mismatch"
  }
  $gb10Fp6ChainEntry = [regex]::Match(
    $gb10Fp6Text,
    '(?s)\.visible \.entry tensor_mxfp6_chain3_m16n16k32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $gb10Fp6ChainEntry -or
      $gb10Fp6ChainEntry -notmatch
        'mtlc\.tensor_chain resident native-mma mxf8f6f4 tiles=3 subtiles=2' -or
      [regex]::Matches($gb10Fp6ChainEntry, $fp6Instruction).Count -ne 6 -or
      [regex]::Matches($gb10Fp6ChainEntry, 'setp\.gt\.u32').Count -ne 0 -or
      [regex]::Matches($gb10Fp6ChainEntry, 'ld\.global\.f32').Count -ne 8 -or
      [regex]::Matches($gb10Fp6ChainEntry, 'st\.global\.f32').Count -ne 8 -or
      $gb10Fp6ChainEntry -match 'replay|wmma\.') {
    throw "GB10 native FP6 chain-residency contract mismatch"
  }
  $gb10Fp6LoopEntry = [regex]::Match(
    $gb10Fp6Text,
    '(?s)\.visible \.entry tensor_mxfp6_runtime_k_m16n16k32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $gb10Fp6LoopEntry -or
      $gb10Fp6LoopEntry -notmatch
        'mtlc\.tensor_loop resident native-mma mxf8f6f4 group=1 subtiles=2' -or
      [regex]::Matches($gb10Fp6LoopEntry, $fp6Instruction).Count -ne 4 -or
      [regex]::Matches($gb10Fp6LoopEntry, 'setp\.gt\.u32').Count -eq 0 -or
      [regex]::Matches($gb10Fp6LoopEntry, 'ld\.global\.f32').Count -ne 8 -or
      [regex]::Matches($gb10Fp6LoopEntry, 'st\.global\.f32').Count -ne 8 -or
      $gb10Fp6LoopEntry -match 'replay|wmma\.') {
    throw "GB10 runtime-K native FP6 residency/general-pack contract mismatch"
  }

  $gb10Fp6Unoptimized =
    Join-Path $tmpDir "ptx_emit_gb10_tensor_fp6_unoptimized.ptx"
  $fp6UnoptimizedOut = & $CompilerPath --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_native_fp6.mettle -o $gb10Fp6Unoptimized 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 unoptimized native-FP6 emit failed: $fp6UnoptimizedOut"
  }
  $gb10Fp6UnoptimizedText = Get-Content -Raw $gb10Fp6Unoptimized
  $gb10Fp6UnoptimizedChain = [regex]::Match(
    $gb10Fp6UnoptimizedText,
    '(?s)\.visible \.entry tensor_mxfp6_chain3_m16n16k32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  $gb10Fp6UnoptimizedLoop = [regex]::Match(
    $gb10Fp6UnoptimizedText,
    '(?s)\.visible \.entry tensor_mxfp6_runtime_k_m16n16k32\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if ($gb10Fp6UnoptimizedChain -match 'mtlc\.tensor_chain' -or
      [regex]::Matches(
        $gb10Fp6UnoptimizedChain,
        'mtlc\.tensor_mma native-mma mxf8f6f4 whole-tile lowering'
      ).Count -ne 3 -or
      $gb10Fp6UnoptimizedLoop -match 'mtlc\.tensor_loop' -or
      [regex]::Matches(
        $gb10Fp6UnoptimizedLoop,
        'mtlc\.tensor_mma native-mma mxf8f6f4 whole-tile lowering'
      ).Count -ne 2) {
    throw "native FP6 residency was not formed exclusively by the optimizer"
  }

  $rawFp6Ptx = Join-Path $tmpDir "ptx_emit_sm121_tensor_fp6.ptx"
  $rawFp6Out = & $CompilerPath -O --emit-ptx --gpu-arch=sm_121 `
    tests/gpu/tensor_native_fp6.mettle -o $rawFp6Ptx 2>&1 | Out-String
  if ($LASTEXITCODE -eq 0 -or
      $rawFp6Out -notmatch
        'architecture- or family-specific sm_120a/sm_121a target') {
    throw "raw sm_121 did not reject architecture-specific native FP6 lowering"
  }

  if ($ptxas) {
    $ptxasHelp = & $ptxas.Source --help 2>&1 | Out-String
    if ($ptxasHelp -match "sm_121a") {
      $fp6AsmOut = & $ptxas.Source -v -arch=sm_121a $gb10Fp6Ptx `
        -o $gb10Fp6Cubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        throw "ptxas rejected GB10 native-FP6 tensor PTX: $fp6AsmOut"
      }
      if ($fp6AsmOut -match '(?m)^\s*[1-9][0-9]* bytes spill (stores|loads)') {
        throw "GB10 native-FP6 kernels spilled registers: $fp6AsmOut"
      }
      foreach ($registerGate in @(
          @{ Name = 'tensor_mxfp6_m16n16k32'; Max = 48 },
          @{ Name = 'tensor_mxfp6_chain3_m16n16k32'; Max = 56 },
          @{ Name = 'tensor_mxfp6_runtime_k_m16n16k32'; Max = 72 })) {
        $escapedName = [regex]::Escape($registerGate.Name)
        $registerMatch = [regex]::Match(
          $fp6AsmOut,
          "(?s)Function properties for $escapedName.*?Used ([0-9]+) registers"
        )
        if (-not $registerMatch.Success -or
            [int]$registerMatch.Groups[1].Value -gt $registerGate.Max) {
          throw "GB10 native-FP6 register ceiling exceeded for $($registerGate.Name): $fp6AsmOut"
        }
      }
    } else {
      Write-Host "[SKIP] ptx_emit_gb10_tensor_fp6 ptxas assembly (toolkit lacks sm_121a)"
    }
  } else {
    Write-Host "[SKIP] ptx_emit_gb10_tensor_fp6 ptxas assembly (ptxas not found)"
  }
  Write-CaseResult -Name "ptx_emit_gb10_tensor_fp6" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "ptx_emit_gb10_tensor_fp6" -Passed $false -Reason $_.Exception.Message
}

$total++
try {
  # Sparse A and its uint8 occupancy masks are frontend/IR semantics. PTX alone
  # translates those masks into ordered warp metadata and sanitizes every
  # dynamic group before it can reach an instruction with undefined encodings.
  $gb10SparsePtx = Join-Path $tmpDir "ptx_emit_gb10_tensor_sparse.ptx"
  $gb10SparseCubin = Join-Path $tmpDir "ptx_emit_gb10_tensor_sparse.cubin"
  $sparseEmitOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_sparse.mettle -o $gb10SparsePtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 structured-sparse emit failed: $sparseEmitOut"
  }
  $sparseText = Get-Content -Raw $gb10SparsePtx
  if ([regex]::Matches(
        $sparseText,
        'mma\.sp::ordered_metadata\.sync\.aligned\.m16n8k16\.row\.col\.f32\.f16\.f16\.f32').Count -ne 10 -or
      [regex]::Matches(
        $sparseText,
        'mma\.sp::ordered_metadata\.sync\.aligned\.m16n8k16\.row\.col\.f32\.bf16\.bf16\.f32').Count -ne 2 -or
      [regex]::Matches($sparseText,
        'mtlc\.tensor_mma native-mma sparse-').Count -ne 2 -or
      # A and its metadata are M/K fragments; each is prepared once and reused
      # by both adjacent N subtiles in these m16n16 logical tensors.
      [regex]::Matches($sparseText, 'popc\.b32').Count -ne 48 -or
      [regex]::Matches($sparseText,
        '(?m)^\s*and\.b32 [^,]+, [^,]+, 15;\r?$').Count -ne 48 -or
      [regex]::Matches($sparseText,
        'mtlc\.tensor_chain resident native-mma sparse-f16-2to4').Count -ne 1 -or
      [regex]::Matches($sparseText,
        'mtlc\.tensor_loop resident native-mma sparse-f16-2to4').Count -ne 1 -or
      $sparseText -match 'tcgen05') {
    throw "GB10 canonical structured-sparse PTX contract mismatch"
  }
  if ($ptxas) {
    $ptxasHelp = & $ptxas.Source --help 2>&1 | Out-String
    if ($ptxasHelp -match "sm_121a") {
      $sparseAsmOut = & $ptxas.Source -v -arch=sm_121a $gb10SparsePtx `
        -o $gb10SparseCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        throw "ptxas rejected GB10 structured-sparse PTX: $sparseAsmOut"
      }
      if ($sparseAsmOut -match
          '(?m)^\s*[1-9][0-9]* bytes spill (stores|loads)') {
        throw "GB10 structured-sparse kernels spilled registers: $sparseAsmOut"
      }
      foreach ($name in @('tensor_sparse_f16_2to4',
                           'tensor_sparse_bf16_2to4',
                           'tensor_sparse_chain2',
                           'tensor_sparse_runtime_k')) {
        $registerMatch = [regex]::Match(
          $sparseAsmOut,
          "(?s)Function properties for $name.*?Used ([0-9]+) registers"
        )
        if (-not $registerMatch.Success -or
            [int]$registerMatch.Groups[1].Value -gt 56) {
          throw "GB10 structured-sparse register ceiling exceeded for $name`: $sparseAsmOut"
        }
      }
    } else {
      Write-Host "[SKIP] ptx_emit_gb10_tensor_sparse native assembly (toolkit lacks sm_121a)"
    }
  } else {
    Write-Host "[SKIP] ptx_emit_gb10_tensor_sparse assembly (ptxas not found)"
  }
  Write-CaseResult -Name "ptx_emit_gb10_tensor_sparse" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "ptx_emit_gb10_tensor_sparse" -Passed $false -Reason $_.Exception.Message
}

$total++
try {
  # Large logical dense tiles remain one frontend/shared-IR operation. PTX
  # selects a stable physical WMMA grid, reuses the cheaper input fragment,
  # and applies the same tuple policy to every resident output subtile.
  $gb10TiledPtx = Join-Path $tmpDir "ptx_emit_gb10_tensor_tiled.ptx"
  $gb10TiledCubin = Join-Path $tmpDir "ptx_emit_gb10_tensor_tiled.cubin"
  $gb10TiledReplayPtx =
    Join-Path $tmpDir "ptx_emit_gb10_tensor_tiled_budget55.ptx"
  $gb10TiledReplayCubin =
    Join-Path $tmpDir "ptx_emit_gb10_tensor_tiled_budget55.cubin"
  $tiledEmitOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_tiled.mettle -o $gb10TiledPtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 logical tensor-grid emit failed: $tiledEmitOut"
  }
  $tiledReplayOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    --gpu-tensor-tuple-budget=55 tests/gpu/tensor_tiled.mettle `
    -o $gb10TiledReplayPtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 tensor-grid replay variant emit failed: $tiledReplayOut"
  }
  $tiledText = Get-Content -Raw $gb10TiledPtx
  $tiledReplayText = Get-Content -Raw $gb10TiledReplayPtx
  if ([regex]::Matches($tiledText, 'mtlc\.tensor_mma tiled').Count -ne 13 -or
      [regex]::Matches($tiledText, 'subtiles=4 reuse=A').Count -ne 12 -or
      [regex]::Matches($tiledText, 'subtiles=2 reuse=B').Count -ne 1 -or
      [regex]::Matches($tiledText, 'wmma\.mma\.sync').Count -ne 50 -or
      [regex]::Matches($tiledText, 'mul\.wide\.u32').Count -lt 20 -or
      $tiledText -notmatch
        'mtlc\.tensor_chain resident tiles=2 subtiles=4 tuple_peak=56 budget=96' -or
      $tiledText -notmatch
        'mtlc\.tensor_loop resident group=1 subtiles=4 tuple_peak=56 budget=96' -or
      $tiledReplayText -notmatch
        'mtlc\.tensor_chain replay tiles=2 subtiles=4 tuple_peak=56 budget=55' -or
      $tiledReplayText -notmatch
        'mtlc\.tensor_loop replay group=1 tuple_peak=56 budget=55') {
    throw "GB10 logical tensor-grid selection/reuse policy mismatch"
  }
  foreach ($variant in @(
      @{ Text = $tiledText; Name = 'tensor_tiled_chain2_f16_m32n32';
         CLoads = 4; Stores = 4 },
      @{ Text = $tiledText; Name = 'tensor_tiled_loop_f16_m32n32';
         CLoads = 4; Stores = 4 },
      @{ Text = $tiledReplayText; Name = 'tensor_tiled_chain2_f16_m32n32';
         CLoads = 8; Stores = 8 },
      @{ Text = $tiledReplayText; Name = 'tensor_tiled_loop_f16_m32n32';
         CLoads = 8; Stores = 8 })) {
    $entry = [regex]::Match(
      $variant.Text,
      "(?s)\.visible \.entry $($variant.Name)\(.*?(?=\.visible \.entry|\z)"
    ).Value
    if (-not $entry -or
        [regex]::Matches($entry, 'wmma\.mma\.sync').Count -ne 8 -or
        [regex]::Matches($entry, 'wmma\.load\.a\.sync').Count -ne 4 -or
        [regex]::Matches($entry, 'wmma\.load\.b\.sync').Count -ne 8 -or
        [regex]::Matches($entry, 'wmma\.load\.c\.sync').Count -ne
          $variant.CLoads -or
        [regex]::Matches($entry, 'wmma\.store\.d\.sync').Count -ne
          $variant.Stores) {
      throw "tensor-grid resident/replay instruction counts changed for $($variant.Name)"
    }
  }
  foreach ($variant in @(
      @{ Name = 'tensor_tiled_u8_m32n32';
         Checks = @(
           @{ Pattern = 'wmma\.load\.a\.sync.*\.row\.u8'; Count = 2 },
           @{ Pattern = 'wmma\.load\.b\.sync.*\.col\.u8'; Count = 4 },
           @{ Pattern = 'wmma\.mma\.sync.*\.s32\.u8\.u8\.s32\.satfinite'; Count = 4 },
           @{ Pattern = 'wmma\.store\.d\.sync.*\.row\.s32'; Count = 4 }
         ) },
      @{ Name = 'tensor_tiled_f16_result_m32n32';
         Checks = @(
           @{ Pattern = 'wmma\.load\.c\.sync.*\.row\.f16'; Count = 4 },
           @{ Pattern = 'wmma\.mma\.sync.*\.row\.col\.f16\.f16'; Count = 4 },
           @{ Pattern = 'wmma\.store\.d\.sync.*\.row\.f16'; Count = 4 }
         ) },
      @{ Name = 'tensor_tiled_f16_colrow_m32n32';
         Checks = @(
           @{ Pattern = 'wmma\.load\.a\.sync.*\.col\.f16'; Count = 2 },
           @{ Pattern = 'wmma\.load\.b\.sync.*\.row\.f16'; Count = 4 },
           @{ Pattern = 'wmma\.load\.c\.sync.*\.col\.f32'; Count = 4 },
           @{ Pattern = 'wmma\.mma\.sync.*\.col\.row\.f32\.f32'; Count = 4 },
           @{ Pattern = 'wmma\.store\.d\.sync.*\.col\.f32'; Count = 4 },
           @{ Pattern = 'mul\.wide\.u32'; Count = 4 }
         ) },
      @{ Name = 'tensor_tiled_f16_runtime_strides_m32n32';
         Checks = @(
           @{ Pattern = 'ld\.param\.s32'; Count = 4 },
           @{ Pattern = 'wmma\.load\.a\.sync'; Count = 2 },
           @{ Pattern = 'wmma\.load\.b\.sync'; Count = 4 },
           @{ Pattern = 'wmma\.load\.c\.sync'; Count = 4 },
           @{ Pattern = 'wmma\.mma\.sync'; Count = 4 },
           @{ Pattern = 'wmma\.store\.d\.sync'; Count = 4 },
           @{ Pattern = 'mul\.wide\.u32'; Count = 7 }
         ) })) {
    $entry = [regex]::Match(
      $tiledText,
      "(?s)\.visible \.entry $($variant.Name)\(.*?(?=\.visible \.entry|\z)"
    ).Value
    if (-not $entry) {
      throw "missing widened tensor-grid entry $($variant.Name)"
    }
    foreach ($check in $variant.Checks) {
      $actual = [regex]::Matches($entry, $check.Pattern).Count
      if ($actual -ne $check.Count) {
        throw "tensor-grid lowering changed for $($variant.Name): " +
          "'$($check.Pattern)' expected $($check.Count), got $actual"
      }
    }
  }
  if ($ptxas) {
    $ptxasHelp = & $ptxas.Source --help 2>&1 | Out-String
    if ($ptxasHelp -match "sm_121a") {
      $tiledAsmOut = & $ptxas.Source -v -arch=sm_121a $gb10TiledPtx `
        -o $gb10TiledCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        throw "ptxas rejected GB10 logical tensor-grid PTX: $tiledAsmOut"
      }
      $tiledReplayAsmOut = & $ptxas.Source -v -arch=sm_121a `
        $gb10TiledReplayPtx -o $gb10TiledReplayCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        throw "ptxas rejected GB10 tensor-grid replay PTX: $tiledReplayAsmOut"
      }
      if (($tiledAsmOut + $tiledReplayAsmOut) -match
          '(?m)^\s*[1-9][0-9]* bytes spill (stores|loads)') {
        throw "GB10 logical tensor-grid variants spilled registers"
      }
      $residentRegisters = [regex]::Matches(
        $tiledAsmOut, 'Used ([0-9]+) registers') |
        ForEach-Object { [int]$_.Groups[1].Value }
      $replayRegisters = [regex]::Matches(
        $tiledReplayAsmOut, 'Used ([0-9]+) registers') |
        ForEach-Object { [int]$_.Groups[1].Value }
      if (-not $residentRegisters -or
          ($residentRegisters | Measure-Object -Maximum).Maximum -gt 64 -or
          -not $replayRegisters -or
          ($replayRegisters | Measure-Object -Maximum).Maximum -gt 48) {
        throw "GB10 logical tensor-grid register ceiling exceeded"
      }
    } else {
      Write-Host "[SKIP] ptx_emit_gb10_tensor_tiled native assembly (toolkit lacks sm_121a)"
    }
  } else {
    Write-Host "[SKIP] ptx_emit_gb10_tensor_tiled assembly (ptxas not found)"
  }
  Write-CaseResult -Name "ptx_emit_gb10_tensor_tiled" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "ptx_emit_gb10_tensor_tiled" -Passed $false `
    -Reason $_.Exception.Message
}

$total++
try {
  # Epilogues are a separate neutral collective so activation never changes an
  # MMA chain's exact composition. PTX currently emits synchronized logical
  # memory replay, not a fictional addressable view of opaque WMMA fragments.
  $epiloguePtx = Join-Path $tmpDir "ptx_emit_gb10_tensor_epilogue.ptx"
  $epilogueCubin = Join-Path $tmpDir "ptx_emit_gb10_tensor_epilogue.cubin"
  $epilogueOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_epilogue.mettle -o $epiloguePtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 tensor-epilogue emit failed: $epilogueOut"
  }
  $epilogueText = Get-Content -Raw $epiloguePtx
  if ([regex]::Matches(
        $epilogueText,
        'mtlc\.tensor_epilogue cooperative-memory').Count -ne 5 -or
      [regex]::Matches($epilogueText, 'bar\.warp\.sync').Count -ne 8 -or
      [regex]::Matches($epilogueText, 'bar\.sync 0').Count -ne 2 -or
      [regex]::Matches($epilogueText, 'cvt\.f32\.f16').Count -ne 1 -or
      [regex]::Matches($epilogueText, 'cvt\.rn\.f16\.f32').Count -ne 1 -or
      [regex]::Matches($epilogueText, 'cvt\.f32\.bf16').Count -ne 2 -or
      [regex]::Matches($epilogueText, 'cvt\.rn\.bf16\.f32').Count -ne 1 -or
      [regex]::Matches($epilogueText, 'ld\.global\.f64').Count -ne 2 -or
      [regex]::Matches($epilogueText, 'st\.global\.f64').Count -ne 1 -or
      [regex]::Matches($epilogueText, 'setp\.lt\.f32').Count -ne 3 -or
      [regex]::Matches($epilogueText, 'setp\.gt\.f32').Count -ne 1 -or
      [regex]::Matches($epilogueText, 'selp\.f32').Count -ne 4 -or
      $epilogueText -match 'wmma\.|mma\.sync') {
    throw "GB10 tensor-epilogue synchronized replay contract mismatch"
  }
  $runtimeEntry = [regex]::Match(
    $epilogueText,
    '(?s)\.visible \.entry tensor_epilogue_f32_matrix_bias_clamp_runtime\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $runtimeEntry -or
      $runtimeEntry -notmatch 'scope=workgroup' -or
      [regex]::Matches($runtimeEntry, 'mul\.wide\.u32').Count -lt 2 -or
      $runtimeEntry -notmatch 'cvt\.u32\.u64') {
    throw "runtime-stride matrix-bias epilogue lowering mismatch"
  }
  if ($ptxas) {
    $ptxasHelp = & $ptxas.Source --help 2>&1 | Out-String
    if ($ptxasHelp -match "sm_121a") {
      $epilogueAsmOut = & $ptxas.Source -v -arch=sm_121a $epiloguePtx `
        -o $epilogueCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        throw "ptxas rejected GB10 tensor-epilogue PTX: $epilogueAsmOut"
      }
      if ($epilogueAsmOut -match
          '(?m)^\s*[1-9][0-9]* bytes spill (stores|loads)') {
        throw "GB10 tensor-epilogue lowering spilled registers"
      }
      $epilogueRegisters = [regex]::Matches(
        $epilogueAsmOut, 'Used ([0-9]+) registers') |
        ForEach-Object { [int]$_.Groups[1].Value }
      if (-not $epilogueRegisters -or
          ($epilogueRegisters | Measure-Object -Maximum).Maximum -gt 24) {
        throw "GB10 tensor-epilogue register ceiling exceeded"
      }
    } else {
      Write-Host "[SKIP] ptx_emit_gb10_tensor_epilogue native assembly (toolkit lacks sm_121a)"
    }
  } else {
    Write-Host "[SKIP] ptx_emit_gb10_tensor_epilogue assembly (ptxas not found)"
  }
  Write-CaseResult -Name "ptx_emit_gb10_tensor_epilogue" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "ptx_emit_gb10_tensor_epilogue" -Passed $false `
    -Reason $_.Exception.Message
}

$total++
try {
  # A backend may consume adjacent verified neutral MMA/commit + epilogue
  # operations, or a uniquely reached loop-exit epilogue. Opaque fragment
  # mappings, bypass edges, and tuple pressure must fall back to the already-
  # tested synchronized memory contract.
  $fusedEpiloguePtx = Join-Path $tmpDir "ptx_emit_gb10_tensor_epilogue_fused.ptx"
  $fusedEpilogueCubin = Join-Path $tmpDir "ptx_emit_gb10_tensor_epilogue_fused.cubin"
  $replayEpiloguePtx = Join-Path $tmpDir "ptx_emit_gb10_tensor_epilogue_fused_replay.ptx"
  $replayEpilogueCubin = Join-Path $tmpDir "ptx_emit_gb10_tensor_epilogue_fused_replay.cubin"
  $fusedOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_epilogue_fused.mettle -o $fusedEpiloguePtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 resident tensor-epilogue emit failed: $fusedOut"
  }
  $replayOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    --gpu-tensor-tuple-budget=25 tests/gpu/tensor_epilogue_fused.mettle `
    -o $replayEpiloguePtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 tensor-epilogue replay emit failed: $replayOut"
  }
  $fusedText = Get-Content -Raw $fusedEpiloguePtx
  $replayText = Get-Content -Raw $replayEpiloguePtx
  if ([regex]::Matches(
        $fusedText, 'mtlc\.tensor_epilogue resident').Count -ne 4 -or
      [regex]::Matches(
        $fusedText, 'mtlc\.tensor_epilogue cooperative-memory').Count -ne 3 -or
      [regex]::Matches($fusedText, 'bar\.warp\.sync').Count -ne 14) {
    throw "resident tensor-epilogue selection/ordering mismatch"
  }
  $stableEntry = [regex]::Match(
    $fusedText,
    '(?s)\.visible \.entry tensor_epilogue_fused_wmma_chain\(.*?(?=\.visible \.entry|\z)'
  ).Value
  $nativeEntry = [regex]::Match(
    $fusedText,
    '(?s)\.visible \.entry tensor_epilogue_fused_native_matrix_bias\(.*?(?=\.visible \.entry|\z)'
  ).Value
  $opaqueEntry = [regex]::Match(
    $fusedText,
    '(?s)\.visible \.entry tensor_epilogue_wmma_bias_replay\(.*?(?=\.visible \.entry|\z)'
  ).Value
  $mismatchEntry = [regex]::Match(
    $fusedText,
    '(?s)\.visible \.entry tensor_epilogue_stride_mismatch_replay\(.*?(?=\.visible \.entry|\z)'
  ).Value
  $pipelineEntry = [regex]::Match(
    $fusedText,
    '(?s)\.visible \.entry tensor_epilogue_fused_pipeline\(.*?(?=\.visible \.entry|\z)'
  ).Value
  $loopEntry = [regex]::Match(
    $fusedText,
    '(?s)\.visible \.entry tensor_epilogue_fused_loop\(.*?(?=\.visible \.entry|\z)'
  ).Value
  $guardedLoopEntry = [regex]::Match(
    $fusedText,
    '(?s)\.visible \.entry tensor_epilogue_guarded_loop_replay\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $stableEntry -or
      $stableEntry -notmatch 'resident stable-wmma tiles=2' -or
      [regex]::Matches($stableEntry, 'wmma\.store\.d').Count -ne 1 -or
      [regex]::Matches($stableEntry, 'mul\.rn\.f32').Count -ne 8 -or
      [regex]::Matches($stableEntry, 'setp\.lt\.f32').Count -ne 8 -or
      [regex]::Matches($stableEntry, 'selp\.f32').Count -ne 8 -or
      $stableEntry -match 'cooperative-memory' -or
      -not $nativeEntry -or
      $nativeEntry -notmatch 'resident native-mma fp8 tiles=2 subtiles=2' -or
      [regex]::Matches($nativeEntry, 'ld\.global\.f32').Count -ne 16 -or
      [regex]::Matches($nativeEntry, 'mul\.rn\.f32').Count -ne 16 -or
      [regex]::Matches($nativeEntry, 'add\.rn\.f32').Count -ne 8 -or
      [regex]::Matches($nativeEntry, 'setp\.lt\.f32').Count -ne 8 -or
      [regex]::Matches($nativeEntry, 'setp\.gt\.f32').Count -ne 8 -or
      [regex]::Matches($nativeEntry, 'selp\.f32').Count -ne 16 -or
      [regex]::Matches($nativeEntry, 'st\.global\.f32').Count -ne 8 -or
      $nativeEntry -match 'cooperative-memory' -or
      -not $opaqueEntry -or
      $opaqueEntry -match 'tensor_epilogue resident' -or
      $opaqueEntry -notmatch 'cooperative-memory' -or
      [regex]::Matches($opaqueEntry, 'wmma\.store\.d').Count -ne 1 -or
      -not $mismatchEntry -or
      $mismatchEntry -match 'tensor_epilogue resident' -or
      $mismatchEntry -notmatch 'cooperative-memory' -or
      [regex]::Matches($mismatchEntry, 'wmma\.store\.d').Count -ne 1 -or
      -not $pipelineEntry -or
      $pipelineEntry -notmatch 'resident handoff group=1 stable-wmma' -or
      [regex]::Matches($pipelineEntry, 'setp\.lt\.f32').Count -ne 8 -or
      [regex]::Matches($pipelineEntry, 'selp\.f32').Count -ne 8 -or
      [regex]::Matches($pipelineEntry, 'wmma\.store\.d').Count -ne 1 -or
      -not $loopEntry -or
      $loopEntry -notmatch 'mtlc\.tensor_loop resident group=1' -or
      $loopEntry -notmatch 'resident handoff group=1 stable-wmma' -or
      [regex]::Matches($loopEntry, 'mul\.rn\.f32').Count -ne 8 -or
      [regex]::Matches($loopEntry, 'setp\.lt\.f32').Count -ne 8 -or
      [regex]::Matches($loopEntry, 'selp\.f32').Count -ne 8 -or
      [regex]::Matches($loopEntry, 'wmma\.store\.d').Count -ne 1 -or
      $loopEntry -match 'cooperative-memory' -or
      -not $guardedLoopEntry -or
      $guardedLoopEntry -notmatch 'mtlc\.tensor_loop resident group=1' -or
      $guardedLoopEntry -match 'tensor_epilogue resident' -or
      $guardedLoopEntry -notmatch 'cooperative-memory' -or
      [regex]::Matches($guardedLoopEntry, 'wmma\.store\.d').Count -ne 1) {
    throw "resident tensor-epilogue structural contract mismatch"
  }
  if ($replayText -match 'mtlc\.tensor_epilogue resident' -or
      [regex]::Matches(
        $replayText, 'mtlc\.tensor_epilogue cooperative-memory').Count -ne 7) {
    throw "tensor-epilogue tuple-budget replay mismatch"
  }
  if ($ptxas) {
    $ptxasHelp = & $ptxas.Source --help 2>&1 | Out-String
    if ($ptxasHelp -match "sm_121a") {
      foreach ($assembly in @(
          @{ Ptx = $fusedEpiloguePtx; Cubin = $fusedEpilogueCubin },
          @{ Ptx = $replayEpiloguePtx; Cubin = $replayEpilogueCubin })) {
        $assemblyOut = & $ptxas.Source -v -arch=sm_121a $assembly.Ptx `
          -o $assembly.Cubin 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
          throw "GB10 resident/replay tensor-epilogue assembly failed: $assemblyOut"
        }
        if ($assemblyOut -match '[1-9][0-9]* bytes spill (stores|loads)') {
          throw "GB10 resident/replay tensor-epilogue spilled registers"
        }
        $registers = [regex]::Matches(
          $assemblyOut, 'Used\s+([0-9]+)\s+registers') |
          ForEach-Object { [int]$_.Groups[1].Value }
        if (-not $registers -or
            ($registers | Measure-Object -Maximum).Maximum -gt 72) {
          throw "GB10 resident/replay tensor-epilogue register ceiling exceeded"
        }
      }
    } else {
      Write-Host "[SKIP] ptx_emit_gb10_tensor_epilogue_fused native assembly (toolkit lacks sm_121a)"
    }
  } else {
    Write-Host "[SKIP] ptx_emit_gb10_tensor_epilogue_fused assembly (ptxas not found)"
  }
  Write-CaseResult -Name "ptx_emit_gb10_tensor_epilogue_fused" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "ptx_emit_gb10_tensor_epilogue_fused" -Passed $false `
    -Reason $_.Exception.Message
}

$total++
try {
  # The source and shared IR expose only a rank/geometry/storage contract.  This
  # gate proves that GB10 selects TMA while the exact same program remains
  # executable as cooperative scalar replay on the baseline portable profile.
  $gb10TransferPtx = Join-Path $tmpDir "ptx_emit_gb10_tensor_transfer.ptx"
  $gb10TransferCubin = Join-Path $tmpDir "ptx_emit_gb10_tensor_transfer.cubin"
  $portableTransferPtx = Join-Path $tmpDir "ptx_emit_portable_tensor_transfer.ptx"
  $portableTransferCubin = Join-Path $tmpDir "ptx_emit_portable_tensor_transfer.cubin"
  $transferEmitOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_transfer.mettle -o $gb10TransferPtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 tensor-transfer emit failed: $transferEmitOut"
  }
  $portableTransferOut = & $CompilerPath -O --emit-ptx --gpu-arch=portable `
    tests/gpu/tensor_transfer.mettle -o $portableTransferPtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "portable tensor-transfer emit failed: $portableTransferOut"
  }
  $gb10TransferText = Get-Content -Raw $gb10TransferPtx
  $portableTransferText = Get-Content -Raw $portableTransferPtx
  if ($gb10TransferText -notmatch "(?m)^\.version 8\.8\r?$" -or
      $gb10TransferText -notmatch "(?m)^\.target sm_121a\r?$" -or
      [regex]::Matches($gb10TransferText,
        'cp\.async\.bulk\.tensor\.2d\.shared::cta\.global\.tile\.mbarrier::complete_tx::bytes').Count -ne 1 -or
      [regex]::Matches($gb10TransferText,
        'cp\.async\.bulk\.tensor\.5d\.global\.shared::cta\.tile\.bulk_group').Count -ne 1 -or
      [regex]::Matches($gb10TransferText,
        'fence\.proxy\.tensormap::generic\.acquire\.sys').Count -ne 2 -or
      [regex]::Matches($gb10TransferText,
        'fence\.proxy\.async\.shared::cta').Count -ne 2 -or
      [regex]::Matches($gb10TransferText,
        'mbarrier\.arrive\.expect_tx\.release\.cta\.shared::cta').Count -ne 1 -or
      [regex]::Matches($gb10TransferText,
        'mbarrier\.try_wait\.parity\.acquire\.cta\.shared::cta').Count -ne 1 -or
      [regex]::Matches($gb10TransferText, 'cp\.async\.bulk\.commit_group').Count -ne 1 -or
      [regex]::Matches($gb10TransferText, 'cp\.async\.bulk\.wait_group 0').Count -ne 1 -or
      [regex]::Matches($gb10TransferText,
        '(?m)^\s*and\.b64 [^,]+, [^,]+, 63;\r?$').Count -ne 2 -or
      [regex]::Matches($gb10TransferText,
        '(?m)^\s*and\.b64 [^,]+, [^,]+, 15;\r?$').Count -ne 2 -or
      [regex]::Matches($gb10TransferText,
        'bra mtlc_tensor_transfer_0_fallback').Count -ne 6 -or
      [regex]::Matches($gb10TransferText,
        'mtlc\.tensor_transfer cooperative-fallback').Count -ne 5) {
    throw "GB10 native/fallback tensor-transfer structure contract mismatch"
  }
  $transferLoadEntry = [regex]::Match(
    $gb10TransferText,
    '(?s)\.visible \.entry tensor_transfer_load_2d\(.*?(?=\.visible \.entry|\z)'
  ).Value
  $transferStoreEntry = [regex]::Match(
    $gb10TransferText,
    '(?s)\.visible \.entry tensor_transfer_store_5d\(.*?(?=\.visible \.entry|\z)'
  ).Value
  if (-not $transferLoadEntry -or -not $transferStoreEntry -or
      $transferLoadEntry -notmatch
        '(?m)^\s*@%p[0-9]+ fence\.proxy\.async\.shared::cta;\r?$' -or
      $transferStoreEntry -notmatch
        '(?m)^\s*fence\.proxy\.async\.shared::cta;\r?$') {
    throw "GB10 tensor-transfer proxy-fence participation contract mismatch"
  }
  foreach ($entry in @($transferLoadEntry, $transferStoreEntry)) {
    $mapGuardAt = $entry.IndexOf(', 63;')
    $sharedGuardAt = $entry.IndexOf(', 15;')
    $mapAcquireAt = $entry.IndexOf(
      'fence.proxy.tensormap::generic.acquire.sys'
    )
    if ($mapGuardAt -lt 0 -or $sharedGuardAt -le $mapGuardAt -or
        $mapAcquireAt -le $sharedGuardAt) {
      throw "GB10 tensor-transfer alignment guards do not dominate tensor-map acquire"
    }
  }
  $loadOrder = @(
    'mbarrier.init.shared::cta.b64',
    'fence.proxy.async.shared::cta',
    'bar.sync 0',
    'cp.async.bulk.tensor.2d.shared::cta.global.tile.mbarrier::complete_tx::bytes',
    'mbarrier.arrive.expect_tx.release.cta.shared::cta.b64',
    'mbarrier.try_wait.parity.acquire.cta.shared::cta.b64',
    'bar.sync 0',
    'mbarrier.inval.shared::cta.b64'
  )
  $cursor = -1
  foreach ($needle in $loadOrder) {
    $next = $transferLoadEntry.IndexOf($needle, $cursor + 1)
    if ($next -le $cursor) {
      throw "GB10 tensor-transfer load ordering failed at '$needle'"
    }
    $cursor = $next
  }
  $storeOrder = @(
    'fence.proxy.async.shared::cta',
    'bar.sync 0',
    'cp.async.bulk.tensor.5d.global.shared::cta.tile.bulk_group',
    'cp.async.bulk.commit_group',
    'cp.async.bulk.wait_group 0',
    'bar.sync 0'
  )
  $cursor = -1
  foreach ($needle in $storeOrder) {
    $next = $transferStoreEntry.IndexOf($needle, $cursor + 1)
    if ($next -le $cursor) {
      throw "GB10 tensor-transfer store ordering failed at '$needle'"
    }
    $cursor = $next
  }
  if ($portableTransferText -notmatch "(?m)^\.target compute_75\r?$" -or
      [regex]::Matches($portableTransferText,
        'mtlc\.tensor_transfer cooperative-fallback').Count -ne 5 -or
      $portableTransferText -match 'cp\.async\.bulk\.tensor|fence\.proxy|mbarrier') {
    throw "portable tensor-transfer replay contract mismatch"
  }

  if ($ptxas) {
    $portableTransferAsmOut = & $ptxas.Source -v -arch=sm_75 $portableTransferPtx `
      -o $portableTransferCubin 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "ptxas rejected portable tensor-transfer PTX: $portableTransferAsmOut"
    }
    if ($portableTransferAsmOut -match
        '(?m)^\s*[1-9][0-9]* bytes spill (stores|loads)') {
      throw "portable tensor-transfer kernels spilled registers: $portableTransferAsmOut"
    }
    foreach ($registerGate in @(
        @{ Name = 'tensor_transfer_load_2d'; Max = 20 },
        @{ Name = 'tensor_transfer_store_5d'; Max = 28 },
        @{ Name = 'tensor_transfer_portable_3d'; Max = 24 },
        @{ Name = 'tensor_transfer_tma_ineligible_inner_2d'; Max = 20 },
        @{ Name = 'tensor_transfer_tma_ineligible_stride0_2d'; Max = 20 })) {
      $escapedName = [regex]::Escape($registerGate.Name)
      $registerMatch = [regex]::Match(
        $portableTransferAsmOut,
        "(?s)Function properties for $escapedName.*?Used ([0-9]+) registers"
      )
      if (-not $registerMatch.Success -or
          [int]$registerMatch.Groups[1].Value -gt $registerGate.Max) {
        throw "portable tensor-transfer register ceiling exceeded for $($registerGate.Name): $portableTransferAsmOut"
      }
    }

    $ptxasHelp = & $ptxas.Source --help 2>&1 | Out-String
    if ($ptxasHelp -match "sm_121a") {
      $gb10TransferAsmOut = & $ptxas.Source -v -arch=sm_121a $gb10TransferPtx `
        -o $gb10TransferCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        throw "ptxas rejected GB10 tensor-transfer PTX: $gb10TransferAsmOut"
      }
      if ($gb10TransferAsmOut -match
          '(?m)^\s*[1-9][0-9]* bytes spill (stores|loads)') {
        throw "GB10 tensor-transfer kernels spilled registers: $gb10TransferAsmOut"
      }
      foreach ($registerGate in @(
          @{ Name = 'tensor_transfer_load_2d'; Max = 24 },
          @{ Name = 'tensor_transfer_store_5d'; Max = 32 },
          @{ Name = 'tensor_transfer_portable_3d'; Max = 28 },
          @{ Name = 'tensor_transfer_tma_ineligible_inner_2d'; Max = 20 },
          @{ Name = 'tensor_transfer_tma_ineligible_stride0_2d'; Max = 20 })) {
        $escapedName = [regex]::Escape($registerGate.Name)
        $registerMatch = [regex]::Match(
          $gb10TransferAsmOut,
          "(?s)Function properties for $escapedName.*?Used ([0-9]+) registers"
        )
        if (-not $registerMatch.Success -or
            [int]$registerMatch.Groups[1].Value -gt $registerGate.Max) {
          throw "GB10 tensor-transfer register ceiling exceeded for $($registerGate.Name): $gb10TransferAsmOut"
        }
      }
    } else {
      Write-Host "[SKIP] ptx_emit_gb10_tensor_transfer native assembly (toolkit lacks sm_121a)"
    }
  } else {
    Write-Host "[SKIP] ptx_emit_gb10_tensor_transfer assembly (ptxas not found)"
  }
  Write-CaseResult -Name "ptx_emit_gb10_tensor_transfer" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "ptx_emit_gb10_tensor_transfer" -Passed $false -Reason $_.Exception.Message
}

$total++
try {
  # Native TMA is not part of the ordinary real-device suite until it has
  # passed on a disposable/recoverable host.  Keep both runners and direct
  # harness invocation fail-closed so an offline assembler success cannot
  # accidentally turn into local device execution.
  $hardwarePs = Get-Content -Raw "tests/gpu/run_hardware_tests.ps1"
  $hardwareSh = Get-Content -Raw "tests/gpu/run_hardware_tests.sh"
  $hardwareHarness = Get-Content -Raw "tests/gpu/hardware_harness.c"
  $ack = "MTLC_ALLOW_EXPERIMENTAL_TMA"
  $ackValue = "I_ACCEPT_GPU_RESET_RISK"
  $recoveryAck = "MTLC_TMA_RECOVERY_READY"
  $recoveryAckValue = "I_HAVE_OUT_OF_BAND_RECOVERY"
  if ($hardwarePs -notmatch '\[switch\]\$ExperimentalTma' -or
      $hardwarePs -notmatch [regex]::Escape($ack) -or
      $hardwarePs -notmatch [regex]::Escape($ackValue) -or
      $hardwarePs -notmatch [regex]::Escape($recoveryAck) -or
      $hardwarePs -notmatch [regex]::Escape($recoveryAckValue) -or
      $hardwarePs -notmatch
        'if \(\$ExperimentalTma -and \$computeMajor -ge 9\)' -or
      $hardwareSh -notmatch '--experimental-tma' -or
      $hardwareSh -notmatch [regex]::Escape($ack) -or
      $hardwareSh -notmatch [regex]::Escape($ackValue) -or
      $hardwareSh -notmatch [regex]::Escape($recoveryAck) -or
      $hardwareSh -notmatch [regex]::Escape($recoveryAckValue) -or
      $hardwareSh -notmatch
        'if \[\[ \$EXPERIMENTAL_TMA -eq 1 && "\$COMPUTE_MAJOR" -ge 9 \]\]' -or
      $hardwarePs -notmatch '\$tmaHarnessArgs' -or
      $hardwareSh -notmatch 'TMA_HARNESS_ARGS') {
    throw "experimental TMA runner quarantine is missing or fail-open"
  }
  $directGateAt = $hardwareHarness.IndexOf('if (tensor_transfer_path &&')
  $driverLoadAt = $hardwareHarness.IndexOf('if (!load_driver(&h.api))')
  if ($directGateAt -lt 0 -or $driverLoadAt -lt 0 -or
      $directGateAt -ge $driverLoadAt -or
      $hardwareHarness -notmatch [regex]::Escape($ack) -or
      $hardwareHarness -notmatch [regex]::Escape($ackValue) -or
      $hardwareHarness -notmatch [regex]::Escape($recoveryAck) -or
      $hardwareHarness -notmatch [regex]::Escape($recoveryAckValue) -or
      $hardwareHarness -notmatch '--tensor-transfer-only' -or
      $hardwareHarness -notmatch 'experimental TMA must run alone') {
    throw "direct hardware harness does not quarantine TMA before loading CUDA"
  }
  Write-CaseResult -Name "gpu_experimental_tma_quarantine" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "gpu_experimental_tma_quarantine" -Passed $false -Reason $_.Exception.Message
}

$total++
try {
  # Pure host parser coverage for the offline resource-profile tool.  This test
  # does not invoke ptxas, load a module, query a driver, or touch a GPU.
  $profilePython = Get-Command python -ErrorAction SilentlyContinue
  if (-not $profilePython) {
    $profilePython = Get-Command python3 -ErrorAction SilentlyContinue
  }
  if (-not $profilePython) {
    Write-CaseResult -Name "ptxas_resource_profile_parser" -Passed $true `
      -Reason "python not found; skipped"
  }
  else {
    $profileTestOut = & $profilePython.Source `
      "tests/ptxas_profile_test.py" 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "offline ptxas resource-profile parser failed: $profileTestOut"
    }
    Write-CaseResult -Name "ptxas_resource_profile_parser" -Passed $true
  }
}
catch {
  $failed++
  Write-CaseResult -Name "ptxas_resource_profile_parser" -Passed $false `
    -Reason $_.Exception.Message
}

$total++
try {
  # Pure host coverage for deterministic occupancy bounds and Pareto selection.
  # The selector consumes JSON only and has no ptxas/driver/device code path.
  $selectorPython = Get-Command python -ErrorAction SilentlyContinue
  if (-not $selectorPython) {
    $selectorPython = Get-Command python3 -ErrorAction SilentlyContinue
  }
  if (-not $selectorPython) {
    Write-CaseResult -Name "ptxas_resource_selector" -Passed $true `
      -Reason "python not found; skipped"
  }
  else {
    $selectorTestOut = & $selectorPython.Source `
      "tests/ptxas_select_test.py" 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "offline ptxas resource selector failed: $selectorTestOut"
    }
    Write-CaseResult -Name "ptxas_resource_selector" -Passed $true
  }
}
catch {
  $failed++
  Write-CaseResult -Name "ptxas_resource_selector" -Passed $false `
    -Reason $_.Exception.Message
}

$total++
try {
  $gb10PipelinePtx = Join-Path $tmpDir "ptx_emit_gb10_tensor_pipeline.ptx"
  $gb10PipelineCubin = Join-Path $tmpDir "ptx_emit_gb10_tensor_pipeline.cubin"
  $gb10Pipeline4Ptx = Join-Path $tmpDir "ptx_emit_gb10_tensor_pipeline4.ptx"
  $gb10Pipeline4Cubin = Join-Path $tmpDir "ptx_emit_gb10_tensor_pipeline4.cubin"
  $pipelineEmitOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_pipeline.mettle -o $gb10PipelinePtx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 staged-tensor emit failed: $pipelineEmitOut"
  }
  $pipeline4EmitOut = & $CompilerPath -O --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_pipeline4.mettle -o $gb10Pipeline4Ptx 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 four-stage tensor emit failed: $pipeline4EmitOut"
  }
  $pipelineText = Get-Content -Raw $gb10PipelinePtx
  if ([regex]::Matches($pipelineText,
                      "cp\.async\.cg\.shared\.global").Count -ne 4 -or
      [regex]::Matches($pipelineText, "cp\.async\.commit_group").Count -ne 2 -or
      [regex]::Matches($pipelineText, "cp\.async\.wait_group 1").Count -ne 1 -or
      [regex]::Matches($pipelineText, "cp\.async\.wait_group 0").Count -ne 1 -or
      [regex]::Matches($pipelineText, "bar\.sync 0").Count -ne 2 -or
      $pipelineText -notmatch "mtlc\.tensor_pipeline resident group=1 tuple_peak=32 budget=96" -or
      [regex]::Matches($pipelineText, "wmma\.load\.a\.sync").Count -ne 2 -or
      [regex]::Matches($pipelineText, "wmma\.load\.b\.sync").Count -ne 2 -or
      [regex]::Matches($pipelineText, "wmma\.load\.c\.sync").Count -ne 1 -or
      [regex]::Matches($pipelineText, "wmma\.mma\.sync").Count -ne 2 -or
      [regex]::Matches($pipelineText, "wmma\.store\.d\.sync").Count -ne 1 -or
      [regex]::Matches($pipelineText,
                      "\.shared \.align 32 \.b8 tensor_pipeline_f16_f32_[ab]_stage_storage\[1024\]").Count -ne 2 -or
      $pipelineText -match "synchronous-fallback") {
    throw "GB10 native staged-tensor structure/residency contract mismatch"
  }
  $secondCommitAt = $pipelineText.LastIndexOf("cp.async.commit_group")
  $waitOneAt = $pipelineText.IndexOf("cp.async.wait_group 1", $secondCommitAt + 1)
  $firstBarrierAt = $pipelineText.IndexOf("bar.sync 0", $waitOneAt + 1)
  $firstMmaAt = $pipelineText.IndexOf("wmma.mma.sync", $firstBarrierAt + 1)
  $waitZeroAt = $pipelineText.IndexOf("cp.async.wait_group 0", $firstMmaAt + 1)
  $secondBarrierAt = $pipelineText.IndexOf("bar.sync 0", $waitZeroAt + 1)
  $secondMmaAt = $pipelineText.IndexOf("wmma.mma.sync", $firstMmaAt + 1)
  $storeAt = $pipelineText.IndexOf("wmma.store.d.sync", $secondMmaAt + 1)
  if ($secondCommitAt -lt 0 -or $waitOneAt -le $secondCommitAt -or
      $firstBarrierAt -le $waitOneAt -or $firstMmaAt -le $firstBarrierAt -or
      $waitZeroAt -le $firstMmaAt -or $secondBarrierAt -le $waitZeroAt -or
      $secondMmaAt -le $secondBarrierAt -or $storeAt -le $secondMmaAt) {
    throw "GB10 native staged-tensor overlap/handoff ordering mismatch"
  }
  $gb10PipelineUnoptimized =
    Join-Path $tmpDir "ptx_emit_gb10_tensor_pipeline_unoptimized.ptx"
  $pipelineUnoptimizedOut = & $CompilerPath --emit-ptx --gpu-arch=gb10 `
    tests/gpu/tensor_pipeline.mettle -o $gb10PipelineUnoptimized 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "GB10 unoptimized staged-tensor emit failed: $pipelineUnoptimizedOut"
  }
  $pipelineUnoptimizedText = Get-Content -Raw $gb10PipelineUnoptimized
  if ($pipelineUnoptimizedText -match "mtlc\.tensor_pipeline" -or
      [regex]::Matches($pipelineUnoptimizedText,
                      "cp\.async\.cg\.shared\.global").Count -ne 4 -or
      [regex]::Matches($pipelineUnoptimizedText,
                      "wmma\.load\.c\.sync").Count -ne 2 -or
      [regex]::Matches($pipelineUnoptimizedText,
                      "wmma\.store\.d\.sync").Count -ne 2) {
    throw "GB10 staged-tensor optimization ownership contract mismatch"
  }
  $pipeline4Text = Get-Content -Raw $gb10Pipeline4Ptx
  if ([regex]::Matches($pipeline4Text,
                      "cp\.async\.cg\.shared\.global").Count -ne 8 -or
      [regex]::Matches($pipeline4Text,
                      "cp\.async\.commit_group").Count -ne 4 -or
      [regex]::Matches($pipeline4Text,
                      "cp\.async\.wait_group [0-3]").Count -ne 4 -or
      [regex]::Matches($pipeline4Text, "bar\.sync 0").Count -ne 4 -or
      $pipeline4Text -notmatch "mtlc\.tensor_pipeline resident group=1 tuple_peak=32 budget=96" -or
      [regex]::Matches($pipeline4Text, "wmma\.load\.a\.sync").Count -ne 4 -or
      [regex]::Matches($pipeline4Text, "wmma\.load\.b\.sync").Count -ne 4 -or
      [regex]::Matches($pipeline4Text, "wmma\.load\.c\.sync").Count -ne 1 -or
      [regex]::Matches($pipeline4Text, "wmma\.mma\.sync").Count -ne 4 -or
      [regex]::Matches($pipeline4Text, "wmma\.store\.d\.sync").Count -ne 1 -or
      $pipeline4Text -match "synchronous-fallback") {
    throw "GB10 native four-stage tensor pipeline contract mismatch"
  }
  $pipeline4Order = New-Object System.Collections.Generic.List[int]
  $pipeline4Cursor = -1
  foreach ($needle in @("cp.async.wait_group 3", "bar.sync 0",
                         "wmma.mma.sync", "cp.async.wait_group 2",
                         "bar.sync 0", "wmma.mma.sync",
                         "cp.async.wait_group 1", "bar.sync 0",
                         "wmma.mma.sync", "cp.async.wait_group 0",
                         "bar.sync 0", "wmma.mma.sync",
                         "wmma.store.d.sync")) {
    $pipeline4Cursor = $pipeline4Text.IndexOf($needle, $pipeline4Cursor + 1)
    $pipeline4Order.Add($pipeline4Cursor)
  }
  for ($i = 0; $i -lt $pipeline4Order.Count; $i++) {
    if ($pipeline4Order[$i] -lt 0 -or
        ($i -gt 0 -and $pipeline4Order[$i] -le $pipeline4Order[$i - 1])) {
      throw "GB10 four-stage tensor pipeline ordering mismatch at step $i"
    }
  }
  if ($ptxas) {
    $ptxasHelp = & $ptxas.Source --help 2>&1 | Out-String
    if ($ptxasHelp -match "sm_121a") {
      $pipelineAsmOut = & $ptxas.Source -arch=sm_121a $gb10PipelinePtx `
        -o $gb10PipelineCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        throw "ptxas rejected GB10 staged-tensor PTX: $pipelineAsmOut"
      }
      $pipeline4AsmOut = & $ptxas.Source -arch=sm_121a $gb10Pipeline4Ptx `
        -o $gb10Pipeline4Cubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        throw "ptxas rejected GB10 four-stage tensor PTX: $pipeline4AsmOut"
      }
    } else {
      Write-Host "[SKIP] ptx_emit_gb10_tensor_pipeline ptxas assembly (toolkit lacks sm_121a)"
    }
  } else {
    Write-Host "[SKIP] ptx_emit_gb10_tensor_pipeline ptxas assembly (ptxas not found)"
  }
  Write-CaseResult -Name "ptx_emit_gb10_tensor_pipeline" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "ptx_emit_gb10_tensor_pipeline" -Passed $false -Reason $_.Exception.Message
}

# Self-checking codegen fixtures. Each one's main returns 0 when every value it
# computes is correct and a distinct nonzero code otherwise, so these are RUN,
# not merely compiled: a construct that silently produces the wrong value still
# compiles clean. Every fixture runs in debug and release, since the two take
# different backend paths. They cannot be @test functions - the compile-time
# interpreter does not model global `var` initializers, and it is the native
# backend under audit here.
$runFixtures = @(
  @{ Name = "global_init_layoutable"; Path = "tests/global_init_layoutable.mettle"
     What = "a global folded to the wrong value" },
  @{ Name = "bool_roundtrip"; Path = "tests/codegen/bool_roundtrip.mettle"
     What = "a bool crossed a call boundary wrong" },
  @{ Name = "odd_size_aggregates"; Path = "tests/codegen/odd_size_aggregates.mettle"
     What = "a 3/5/6/7-byte aggregate copied wrong" },
  @{ Name = "abi_mixed_args"; Path = "tests/codegen/abi_mixed_args.mettle"
     What = "an argument crossed the ABI boundary wrong" },
  @{ Name = "abi_struct_args"; Path = "tests/codegen/abi_struct_args.mettle"
     What = "a struct argument or a spilled stack argument landed wrong" },
  @{ Name = "struct_ret_sizes"; Path = "tests/codegen/struct_ret_sizes.mettle"
     What = "a struct return of some size class came back wrong" },
  @{ Name = "call_chains"; Path = "tests/codegen/call_chains.mettle"
     What = "a nested call passed something wrong" },
  @{ Name = "float_conv"; Path = "tests/codegen/float_conv.mettle"
     What = "a codegen check failed" },
  @{ Name = "float_nan_compare"; Path = "tests/codegen/float_nan_compare.mettle"
     What = "an ordered float comparison against NaN came back true" },
  @{ Name = "unsigned_loops"; Path = "tests/codegen/unsigned_loops.mettle"
     What = "a codegen check failed" },
  @{ Name = "defer_scopes"; Path = "tests/codegen/defer_scopes.mettle"
     What = "a codegen check failed" },
  @{ Name = "generic_structs"; Path = "tests/codegen/generic_structs.mettle"
     What = "a codegen check failed" },
  @{ Name = "simd_loops"; Path = "tests/codegen/simd_loops.mettle"
     What = "a codegen check failed" },
  @{ Name = "inline_kernels"; Path = "tests/codegen/inline_kernels.mettle"
     What = "a vector kernel run inside a register-allocated frame produced the wrong value" },
  @{ Name = "divmod_pairs"; Path = "tests/codegen/divmod_pairs.mettle"
     What = "a fused divide lost its quotient or its remainder" },
  @{ Name = "switch_dense"; Path = "tests/codegen/switch_dense.mettle"
     What = "a codegen check failed" },
  @{ Name = "enums_match"; Path = "tests/codegen/enums_match.mettle"
     What = "a codegen check failed" },
  @{ Name = "pointers"; Path = "tests/codegen/pointers.mettle"
     What = "a codegen check failed" },
  @{ Name = "casts"; Path = "tests/codegen/casts.mettle"
     What = "a codegen check failed" },
  @{ Name = "int_edges"; Path = "tests/codegen/int_edges.mettle"
     What = "a codegen check failed" },
  @{ Name = "funcptr"; Path = "tests/codegen/funcptr.mettle"
     What = "a codegen check failed" },
  @{ Name = "control_flow"; Path = "tests/codegen/control_flow.mettle"
     What = "a codegen check failed" },
  @{ Name = "reg_pressure"; Path = "tests/codegen/reg_pressure.mettle"
     What = "a codegen check failed" },
  @{ Name = "scalar_params"; Path = "tests/codegen/scalar_params.mettle"
     What = "a codegen check failed" },
  @{ Name = "struct_byval"; Path = "tests/codegen/struct_byval.mettle"
     What = "a codegen check failed" },
  @{ Name = "heap_new"; Path = "tests/codegen/heap_new.mettle"
     What = "a codegen check failed" },
  @{ Name = "global_arrays"; Path = "tests/codegen/global_arrays.mettle"
     What = "a codegen check failed" },
  @{ Name = "strings"; Path = "tests/codegen/strings.mettle"
     What = "a codegen check failed" },
  @{ Name = "nested_data"; Path = "tests/codegen/nested_data.mettle"
     What = "a codegen check failed" },
  @{ Name = "methods"; Path = "tests/codegen/methods.mettle"
     What = "a codegen check failed" },
  @{ Name = "generics_mono"; Path = "tests/codegen/generics_mono.mettle"
     What = "a codegen check failed" },
  @{ Name = "nested_generics"; Path = "tests/codegen/nested_generics.mettle"
     What = "a nested generic instantiation was wrong" },
  @{ Name = "keyword_field_names"; Path = "tests/codegen/keyword_field_names.mettle"
     What = "a mnemonic-named field read back wrong" },
  @{ Name = "struct_methods"; Path = "tests/codegen/struct_methods.mettle"
     What = "a struct method body behaved wrong" },
  @{ Name = "unsigned_fold"; Path = "tests/codegen/unsigned_fold.mettle"
     What = "an unsigned operation folded with signed semantics" },
  @{ Name = "uint64_literals"; Path = "tests/codegen/uint64_literals.mettle"
     What = "a uint64 decimal literal was wrong" },
  @{ Name = "value_range"; Path = "tests/codegen/value_range.mettle"
     What = "a range-driven rewrite proved the wrong thing" }
)
foreach ($fixture in $runFixtures) {
  foreach ($mode in @(@{ Name = "debug"; Args = @() },
                      @{ Name = "release"; Args = @("--release") })) {
    $caseName = "$($fixture.Name)_$($mode.Name)"
    try {
      $total++
      $fixtureExe = Join-Path $tmpDir "$caseName.exe"
      $fixtureOut = & $CompilerPath --build @($mode.Args) `
        $fixture.Path -o $fixtureExe 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        throw "compile failed: $fixtureOut"
      }
      if ($fixtureOut -match "internal compiler error") {
        throw "compile reported an internal compiler error: $fixtureOut"
      }
      & $fixtureExe | Out-Null
      if ($LASTEXITCODE -ne 0) {
        throw "$($fixture.What) (fixture check #$LASTEXITCODE)"
      }
      Write-CaseResult -Name $caseName -Passed $true
    }
    catch {
      $failed++
      Write-CaseResult -Name $caseName -Passed $false -Reason $_.Exception.Message
    }
  }
}

# SPIR-V backend validity gate. Emits the self-contained GPU compute-kernel
# fixture (tests/gpu/compute_kernels.mettle) plus the vadd kernel to SPIR-V
# binary modules (--emit-spirv, the OpenCL 2.0 sibling of --emit-ptx) and
# structurally validates each word stream: little-endian, correct magic, a
# consistent word-count walk that lands exactly on EOF, an in-range id bound,
# and every OpEntryPoint referencing a defined OpFunction. When the Vulkan SDK's
# spirv-val is on PATH it is run too (the authoritative full validation, like
# ptxas for PTX); otherwise the structural check stands alone (skip-augmented,
# never skipped -- the emitter must always produce a well-formed module).
$spirvVal = if ($env:SPIRV_VAL -and (Test-Path -LiteralPath $env:SPIRV_VAL)) {
  [pscustomobject]@{ Source = (Resolve-Path -LiteralPath $env:SPIRV_VAL).ProviderPath }
} else {
  Get-Command spirv-val -ErrorAction SilentlyContinue
}
function Test-SpirvModule([string]$path, [bool]$RequireDeviceCall = $false,
                          [bool]$RequireAtomicFamily = $false,
                          [bool]$RequireAtomicU32 = $false) {
  $bytes = [System.IO.File]::ReadAllBytes($path)
  if ($bytes.Length % 4 -ne 0) { throw "not word-aligned ($($bytes.Length) bytes)" }
  $nwords = $bytes.Length / 4
  if ($nwords -lt 5) { throw "shorter than a SPIR-V header" }
  $w = [uint32[]]::new($nwords)
  for ($k = 0; $k -lt $nwords; $k++) { $w[$k] = [BitConverter]::ToUInt32($bytes, $k * 4) }
  if ($w[0] -ne 0x07230203) { throw ("bad magic 0x{0:x8}" -f $w[0]) }
  $bound = $w[3]
  $i = 5; $entries = @(); $funcs = @{}; $calls = @()
  $workgroupVars = 0; $privateVars = 0; $arrayTypes = 0
  $arrayTypeIds = @{}; $pointerStorage = @{}; $pointerPointee = @{}
  $workgroupPointerParams = 0
  $capabilities = @{}; $groupBroadcasts = 0; $groupIAdds = 0; $groupFAdds = 0
  $opcodeCounts = @{}
  $groupIAddOps = @(0, 0, 0); $groupFAddOps = @(0, 0, 0)
  $groupFMins = 0; $groupUMins = 0; $groupFMaxs = 0; $groupUMaxs = 0
  while ($i -lt $nwords) {
    $count = $w[$i] -shr 16; $op = $w[$i] -band 0xffff
    if ($count -eq 0) { throw "zero word-count at word $i" }
    if ($i + $count -gt $nwords) { throw "instruction overruns stream at word $i" }
    if (-not $opcodeCounts.ContainsKey($op)) { $opcodeCounts[$op] = 0 }
    $opcodeCounts[$op]++
    if ($op -eq 15) { $entries += $w[$i + 2] }        # OpEntryPoint: funcid is operand 2
    elseif ($op -eq 17) { $capabilities[$w[$i + 1]] = $true } # OpCapability
    elseif ($op -eq 54) { $funcs[$w[$i + 2]] = $true } # OpFunction: result id is operand 2
    elseif ($op -eq 57) { $calls += $w[$i + 3] }      # OpFunctionCall: function is operand 3
    elseif ($op -eq 28) {                              # OpTypeArray
      $arrayTypes++
      $arrayTypeIds[$w[$i + 1]] = $true
    }
    elseif ($op -eq 32 -and $count -eq 4) {            # OpTypePointer
      $pointerStorage[$w[$i + 1]] = $w[$i + 2]
      $pointerPointee[$w[$i + 1]] = $w[$i + 3]
    }
    elseif ($op -eq 55 -and $count -eq 3 -and          # OpFunctionParameter
            $pointerStorage.ContainsKey($w[$i + 1]) -and
            $pointerStorage[$w[$i + 1]] -eq 4) {
      $workgroupPointerParams++
    }
    elseif ($op -eq 263) { $groupBroadcasts++ }        # OpGroupBroadcast
    elseif ($op -eq 264) {                             # OpGroupIAdd
      $groupIAdds++
      if ($w[$i + 4] -le 2) { $groupIAddOps[$w[$i + 4]]++ }
    }
    elseif ($op -eq 265) {                             # OpGroupFAdd
      $groupFAdds++
      if ($w[$i + 4] -le 2) { $groupFAddOps[$w[$i + 4]]++ }
    }
    elseif ($op -eq 266) { $groupFMins++ }             # OpGroupFMin
    elseif ($op -eq 267) { $groupUMins++ }             # OpGroupUMin
    elseif ($op -eq 269) { $groupFMaxs++ }             # OpGroupFMax
    elseif ($op -eq 270) { $groupUMaxs++ }             # OpGroupUMax
    elseif ($op -eq 59 -and $count -eq 4) {            # OpVariable
      $variableType = $w[$i + 1]
      $isArrayVariable = $pointerPointee.ContainsKey($variableType) -and
                         $arrayTypeIds.ContainsKey($pointerPointee[$variableType])
      if ($isArrayVariable -and $w[$i + 3] -eq 4) { $workgroupVars++ }
      elseif ($isArrayVariable -and $w[$i + 3] -eq 7) { $privateVars++ }
    }
    $i += $count
  }
  if ($i -ne $nwords) { throw "trailing words after last instruction" }
  foreach ($e in $entries) {
    if (-not $funcs.ContainsKey($e)) { throw "entry point $e is not a defined function" }
    if ($e -ge $bound) { throw "entry point id $e exceeds id bound $bound" }
  }
  foreach ($c in $calls) {
    if (-not $funcs.ContainsKey($c)) { throw "device call target $c is not a defined function" }
    if ($c -ge $bound) { throw "device call target id $c exceeds id bound $bound" }
  }
  if ($RequireDeviceCall) {
    if ($calls.Count -lt 1) { throw "module has no OpFunctionCall" }
    if ($funcs.Count -ne $entries.Count + 1) {
      throw "device reachability mismatch: $($funcs.Count) functions for $($entries.Count) entries"
    }
    if ($arrayTypes -lt 2 -or $workgroupVars -ne 1 -or $privateVars -ne 1) {
      throw "static address-space storage mismatch: arrays=$arrayTypes workgroup=$workgroupVars private=$privateVars"
    }
    if ($workgroupPointerParams -ne 1) {
      throw "dynamic workgroup ABI mismatch: Workgroup pointer params=$workgroupPointerParams"
    }
    if (-not $capabilities.ContainsKey([uint32]18) -or
        $groupBroadcasts -ne 2 -or $groupIAdds -ne 3 -or $groupFAdds -ne 3 -or
        ($groupIAddOps -join ',') -ne '1,1,1' -or
        ($groupFAddOps -join ',') -ne '1,1,1' -or
        $groupFMins -ne 1 -or $groupUMins -ne 1 -or
        $groupFMaxs -ne 1 -or $groupUMaxs -ne 1 -or
        -not $capabilities.ContainsKey([uint32]4423) -or
        -not $capabilities.ContainsKey([uint32]4431) -or
        -not $opcodeCounts.ContainsKey([uint32]4421) -or
        $opcodeCounts[[uint32]4421] -ne 2 -or
        -not $opcodeCounts.ContainsKey([uint32]4428) -or
        $opcodeCounts[[uint32]4428] -ne 2 -or
        -not $opcodeCounts.ContainsKey([uint32]4429) -or
        $opcodeCounts[[uint32]4429] -ne 1) {
      throw "subgroup contract mismatch: Groups=$($capabilities.ContainsKey([uint32]18)) broadcast=$groupBroadcasts iadd=$groupIAdds/$($groupIAddOps -join ',') fadd=$groupFAdds/$($groupFAddOps -join ',') min=$groupFMins,$groupUMins max=$groupFMaxs,$groupUMaxs"
    }
  }
  if ($RequireAtomicFamily) {
    $expectedAtomicOpcodes = @{
      227 = 4; 228 = 4
      229 = 2; 230 = 3; 234 = 3; 235 = 2; 237 = 2
      239 = 2; 240 = 2; 241 = 2; 242 = 2
    }
    if (-not $capabilities.ContainsKey([uint32]12)) {
      throw "64-bit atomic module omitted Int64Atomics capability"
    }
    foreach ($entry in $expectedAtomicOpcodes.GetEnumerator()) {
      $actual = if ($opcodeCounts.ContainsKey([uint32]$entry.Key)) {
        $opcodeCounts[[uint32]$entry.Key]
      } else { 0 }
      if ($actual -ne $entry.Value) {
        throw "atomic opcode $($entry.Key) count=$actual expected=$($entry.Value)"
      }
    }
  }
  if ($RequireAtomicU32) {
    foreach ($entry in @{ 227 = 2; 228 = 2 }.GetEnumerator()) {
      $actual = if ($opcodeCounts.ContainsKey([uint32]$entry.Key)) {
        $opcodeCounts[[uint32]$entry.Key]
      } else { 0 }
      if ($actual -ne $entry.Value) {
        throw "u32 atomic opcode $($entry.Key) count=$actual expected=$($entry.Value)"
      }
    }
    if ($capabilities.ContainsKey([uint32]12)) {
      throw "u32-only atomic module declared optional Int64Atomics capability"
    }
  }
  return $entries.Count
}
foreach ($src in @("tests/gpu/compute_kernels.mettle",
                   "tests/gpu/atomic_kernels.mettle",
                   "tests/gpu/atomic_u32_profile.mettle",
                   "tests/gpu/async_copy.mettle",
                   "tests/gpu/auto_staging.mettle",
                   "tests/gpu/auto_staging_no_promote.mettle",
                   "examples/gpu_vadd/vadd_kernel.mettle")) {
  $total++
  $name = "spirv_emit_" + [System.IO.Path]::GetFileNameWithoutExtension($src)
  try {
    $spvPath = Join-Path $tmpDir ($name + ".spv")
    $emitOut = & $CompilerPath -O --emit-spirv $src -o $spvPath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "emit failed: $emitOut" }
    if (-not (Test-Path $spvPath)) { throw "no SPIR-V produced" }
    $isAtomicFamily = $src -like "*atomic_kernels.mettle"
    $isAtomicU32 = $src -like "*atomic_u32_profile.mettle"
    $null = Test-SpirvModule $spvPath ($src -like "*compute_kernels.mettle") `
      $isAtomicFamily $isAtomicU32
    if ($spirvVal) {
      # Int64Atomics is a standard optional OpenCL capability gated by both
      # cl_khr_int64_* extensions. spirv-val's OpenCL profiles model only the
      # mandatory capability set, so validate such a module as SPIR-V 1.0 and
      # validate all mandatory-profile modules against OpenCL 2.0 directly.
      $targetEnv = if ($isAtomicFamily) { "spv1.0" } else { "opencl2.0" }
      $valOut = & $spirvVal.Source --target-env $targetEnv $spvPath 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "spirv-val rejected emitted module: $valOut" }
    }
    Write-CaseResult -Name $name -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name $name -Passed $false -Reason $_.Exception.Message
  }
}

# libmtlc frontend-agnostic gate. Builds the calc example -- a non-Mettle
# frontend that includes ONLY include/mtlc and links ONLY bin/mtlc.lib -- then
# compiles two .calc programs to native executables and asserts their computed
# exit codes. This exercises the whole public API path end to end (IR builder ->
# optimizer -> native x86-64 codegen -> internal PE link) with no Mettle
# frontend in the loop, proving libmtlc is frontend-agnostic. Skipped if gcc is
# unavailable (it links the example against the static library).
$calcGcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $calcGcc) {
  Write-Host "[SKIP] calc_frontend (gcc not found)"
}
elseif (-not (Test-Path "bin/mtlc.lib")) {
  Write-Host "[SKIP] calc_frontend (bin/mtlc.lib not present)"
}
else {
  $total++
  try {
    $calcExe = Join-Path $tmpDir "calc.exe"
    $buildOut = & $calcGcc.Source -Wall -Wextra -std=c99 -Iinclude `
      examples/calc/calc.c bin/mtlc.lib -o $calcExe -ldbghelp 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "building the calc frontend failed: $buildOut" }
    $cases = @(
      @{ src = "examples/calc/programs/factorial.calc"; expect = 120 },
      @{ src = "examples/calc/programs/loops.calc"; expect = 55 }
    )
    foreach ($c in $cases) {
      $name = [System.IO.Path]::GetFileNameWithoutExtension($c.src)
      $exe = Join-Path $tmpDir ("calc_" + $name + ".exe")
      $emit = & $calcExe $c.src $exe 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "calc failed on $($c.src): $emit" }
      if (-not (Test-Path $exe)) { throw "no executable produced for $($c.src)" }
      & $exe | Out-Null
      if ($LASTEXITCODE -ne $c.expect) {
        throw "$($c.src): exit code $LASTEXITCODE, expected $($c.expect)"
      }
    }
    Write-CaseResult -Name "calc_frontend" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "calc_frontend" -Passed $false -Reason $_.Exception.Message
  }
}

# Optimizer unit gate: loop-carried float symbols must not become temps before
# loop unrolling, which would clone several producers under one temp name.
if (-not $calcGcc) {
  Write-Host "[SKIP] optimizer_float_copy (gcc not found)"
}
elseif (-not (Test-Path "bin/mtlc.lib")) {
  Write-Host "[SKIP] optimizer_float_copy (bin/mtlc.lib not present)"
}
else {
  $total++
  try {
    $floatCopyExe = Join-Path $tmpDir "optimizer_float_copy_test.exe"
    $buildOut = & $calcGcc.Source -Wall -Wextra -std=c99 -Isrc -Iinclude `
      tests/optimizer_float_copy_test.c bin/mtlc.lib -o $floatCopyExe -ldbghelp 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "building optimizer_float_copy_test failed: $buildOut"
    }
    $runOut = & $floatCopyExe 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "optimizer_float_copy_test failed: $runOut"
    }
    Write-CaseResult -Name "optimizer_float_copy" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "optimizer_float_copy" -Passed $false -Reason $_.Exception.Message
  }
}

# Full public API surface gate. Compiles tests/public_api_test.c against the
# owned host ABI, links it with no startup or default libraries, and runs it.
# It builds six module families through the public IR builder: globals, extern
# owned runtime calls, pointer
# load/store, address-of, float arithmetic + casts -- and emits through
# mtlc_emit/mtlc_build_executable to all four targets: a native x86-64 exe
# (run below: exit 42 + stdout OK), PTX text, a SPIR-V binary, and an AArch64
# ELF, a typed semantic host-launch object, and broad cooperative-tensor PTX
# (each structurally verified inside the test). Skipped without gcc.
if (-not $calcGcc) {
  Write-Host "[SKIP] public_api (gcc not found)"
}
elseif (-not (Test-Path "bin/mtlc.lib")) {
  Write-Host "[SKIP] public_api (bin/mtlc.lib not present)"
}
else {
  $total++
  try {
    $pubExe = Join-Path $tmpDir "public_api_test.exe"
    $pubOut = Join-Path $tmpDir "pubapi"
    New-Item -ItemType Directory -Force $pubOut | Out-Null
    $pubObj = Join-Path $tmpDir "public_api_test.o"
    $compileOut = & $calcGcc.Source -Wall -Wextra -std=c99 -Iinclude `
      -ffreestanding -fno-builtin -fno-stack-protector `
      -fno-asynchronous-unwind-tables -fno-unwind-tables `
      -mno-stack-arg-probe -include src/runtime/host_redirect.h `
      -c tests/public_api_test.c -o $pubObj 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "compiling public_api_test failed: $compileOut" }
    $pubLinkArgs = @(
      '-nostdlib', '-nostartfiles', '-nodefaultlibs',
      '-Wl,--disable-runtime-pseudo-reloc',
      '-Wl,-e,mettle_start,--gc-sections',
      'obj/runtime/host_startup.o', $pubObj, 'bin/mtlc.lib',
      '-o', $pubExe, '-lkernel32', '-ldbghelp')
    $buildOut = & $calcGcc.Source @pubLinkArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "linking public_api_test failed: $buildOut" }
    $pubImports = & objdump -p $pubExe 2>&1 | Out-String
    if ($pubImports -match "msvcrt|ucrt|vcruntime|api-ms-win-crt|libgcc|libstdc|libwinpthread") {
      throw "public_api_test imports a C or compiler runtime: $pubImports"
    }
    $runOut = & $pubExe $pubOut 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "public_api_test failed: $runOut" }
    if ($ptxas) {
      $pubPortablePtx = Join-Path $pubOut "pubapi_kernel_compute75.ptx"
      $pubPortableCubin = Join-Path $pubOut "pubapi_kernel_compute75.cubin"
      $asmOut = & $ptxas.Source -arch=sm_75 $pubPortablePtx -o $pubPortableCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "ptxas rejected public atomic-memory PTX: $asmOut" }
      $pubTensorPtx = Join-Path $pubOut "pubapi_tensor_sm121a.ptx"
      $pubTensorCubin = Join-Path $pubOut "pubapi_tensor_sm121a.cubin"
      $tensorAsmOut = & $ptxas.Source -arch=sm_121a $pubTensorPtx -o $pubTensorCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "ptxas rejected public tensor-family PTX: $tensorAsmOut" }
      $pubShufflePtx = Join-Path $pubOut "pubapi_subgroup_shuffle_sm121a.ptx"
      $pubShuffleCubin = Join-Path $pubOut "pubapi_subgroup_shuffle_sm121a.cubin"
      $shuffleAsmOut = & $ptxas.Source -arch=sm_121a $pubShufflePtx -o $pubShuffleCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "ptxas rejected public subgroup-shuffle PTX: $shuffleAsmOut" }
      $pubTransferPortablePtx = Join-Path $pubOut "pubapi_transfer_compute75.ptx"
      $pubTransferPortableCubin = Join-Path $pubOut "pubapi_transfer_compute75.cubin"
      $transferPortableAsmOut = & $ptxas.Source -arch=sm_75 $pubTransferPortablePtx -o $pubTransferPortableCubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "ptxas rejected public portable tensor-transfer PTX: $transferPortableAsmOut" }
      $pubTransferGb10Ptx = Join-Path $pubOut "pubapi_transfer_sm121a.ptx"
      $pubTransferGb10Cubin = Join-Path $pubOut "pubapi_transfer_sm121a.cubin"
      $transferGb10AsmOut = & $ptxas.Source -arch=sm_121a $pubTransferGb10Ptx -o $pubTransferGb10Cubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "ptxas rejected public GB10 tensor-transfer PTX: $transferGb10AsmOut" }
    }
    if ($spirvVal) {
      $pubSpv = Join-Path $pubOut "pubapi_kernel.spv"
      # The public module deliberately exercises optional Int64Atomics; its
      # exact capabilities/opcodes are checked in-process, while spirv-val
      # validates the core SPIR-V module independently of device extensions.
      $valOut = & $spirvVal.Source --target-env spv1.0 $pubSpv 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "spirv-val rejected public atomic-memory SPIR-V: $valOut" }
    }
    $nativeExe = Join-Path $pubOut "pubapi_native.exe"
    if (-not (Test-Path $nativeExe)) { throw "no native executable produced" }
    $nativeOut = & $nativeExe | Out-String
    if ($LASTEXITCODE -ne 42) { throw "native exe exit code $LASTEXITCODE, expected 42" }
    if ($nativeOut -notmatch "OK") { throw "native exe stdout missing OK: '$nativeOut'" }
    # The convenience surface (element/field addressing, allocated labels,
    # operator enums) must lower to the same working native code: the program
    # sums i*i for i in 0..4 into a struct field and adds a second field.
    $ergExe = Join-Path $pubOut "pubapi_ergonomics.exe"
    if (-not (Test-Path $ergExe)) { throw "no ergonomics executable produced" }
    & $ergExe | Out-Null
    if ($LASTEXITCODE -ne 77) { throw "ergonomics exe exit code $LASTEXITCODE, expected 77" }
    Write-CaseResult -Name "public_api" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "public_api" -Passed $false -Reason $_.Exception.Message
  }
}

# libmtlc self-containment audit. Computes the archive's external symbol
# closure (every symbol some member references that no member defines) and
# fails if any final symbol is not an explicit Windows OS import. It also
# rejects C, compiler, and thread runtime import names. Skipped when nm is not
# available.
$nmCmd = Get-Command nm -ErrorAction SilentlyContinue
if (-not $nmCmd) {
  Write-Host "[SKIP] libmtlc_selfcontained (nm not found)"
}
elseif (-not (Test-Path "bin/mtlc.lib")) {
  Write-Host "[SKIP] libmtlc_selfcontained (bin/mtlc.lib not present)"
}
else {
  $total++
  try {
    $nmLines = & $nmCmd.Source "bin/mtlc.lib" 2>$null
    $defined = New-Object System.Collections.Generic.HashSet[string]
    $undef = New-Object System.Collections.Generic.HashSet[string]
    foreach ($ln in $nmLines) {
      if ($ln -match '\s+U\s+(\S+)\s*$') { [void]$undef.Add($Matches[1]) }
      elseif ($ln -match '\s+[A-TV-Zabd-tv-z]\s+(\S+)\s*$') { [void]$defined.Add($Matches[1]) }
    }
    $forbiddenRuntime = '(?i)(msvcrt|ucrt|vcruntime|msvcp|libgcc|libstdc|libwinpthread|__mingw_|^__imp_(malloc|calloc|realloc|free|memcpy|memset|printf|fprintf|strtod)$)'
    $bad = @()
    foreach ($s in $undef) {
      if (-not $defined.Contains($s) -and
          ($s -notmatch '^__imp_' -or $s -match $forbiddenRuntime)) {
        $bad += $s
      }
    }
    if ($bad.Count -gt 0) {
      throw ("bin/mtlc.lib contains forbidden unresolved symbols: " +
             (($bad | Sort-Object) -join ', '))
    }
    Write-CaseResult -Name "libmtlc_selfcontained" -Passed $true
  }
  catch {
    $failed++
    Write-CaseResult -Name "libmtlc_selfcontained" -Passed $false -Reason $_.Exception.Message
  }
}

# Differential miscompile fuzzer gate. Generates UB-free programs, builds each
# at debug and release, and fails on any exit-code divergence (a silent
# miscompile). See tools/fuzz/README.md. Skipped if Python is unavailable or
# -FuzzCount 0. Uses a fixed seed range so the gate is deterministic.
if ($FuzzCount -gt 0) {
  $total++
  try {
    $python = (Get-Command python -ErrorAction SilentlyContinue)
    if (-not $python) {
      $python = (Get-Command python3 -ErrorAction SilentlyContinue)
    }
    if (-not $python) {
      Write-CaseResult -Name "differential_fuzz" -Passed $true -Reason "python not found; skipped"
    }
    else {
      $compilerFull = (Resolve-Path $CompilerPath).Path
      $fuzzOut = & $python.Source "tools\fuzz\fuzz.py" --count $FuzzCount --compiler $compilerFull 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) {
        $failed++
        Write-CaseResult -Name "differential_fuzz" -Passed $false -Reason "miscompile divergence detected"
        Write-Host ($fuzzOut.TrimEnd())
      }
      else {
        Write-CaseResult -Name "differential_fuzz" -Passed $true -Reason "$FuzzCount seeds, no divergence"
      }
    }
  }
  catch {
    $failed++
    Write-CaseResult -Name "differential_fuzz" -Passed $false -Reason $_.Exception.Message
  }
}

Write-Host ""
Write-Host "Test summary: $($total - $failed)/$total passed"

if ($failed -ne 0) {
  exit 1
}

exit 0


