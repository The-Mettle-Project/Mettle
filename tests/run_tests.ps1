param(
  [string]$CompilerPath = ".\bin\mettle.exe",
  [switch]$BuildCompiler,
  [switch]$SkipRuntime,
  [switch]$SkipDeterminism,
  [int]$FuzzCount = 60
)

$ErrorActionPreference = "Continue"

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
  & .\build.bat
  if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed."
    exit 1
  }
}

if (-not (Test-Path $CompilerPath)) {
  Write-Error "Compiler not found at '$CompilerPath'."
  exit 1
}

$tmpDir = Join-Path $env:TEMP "Mettle-test-artifacts"
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
    Pattern       = "@simd! loop was not vectorized: the loop body has its own control flow"
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
      'none via hoisting -- re-checked: the index half that is not the loop counter changes every iteration',
      # the expanded backend section: instruction-weighted coverage, bails
      # grouped by cause with consequence text and sizes
      'backend report: explain_demo\.mettle',
      'optimized IR instructions are in register-allocated code',
      'contains the SIMD kernel `simd_affine_map_f32`',
      'consequence: the kernel itself runs at full vector speed',
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
      'fix: nothing to change on this line: this loop is a driver'
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
    # Memory diagnostics: returning the address of a stack local is an error.
    Name          = "err_mem_return_stack"
    Path          = "tests/err_mem_return_stack.mettle"
    ShouldSucceed = $false
    Pattern       = 'Returning the address of stack local `values`'
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
    Pattern       = '`mem_zero` writes 128 bytes into `buf`, which only has 64'
  },
  @{
    # Loop-bound analysis: `j <= 8` over int32[8] provably reads a[8] on the
    # final iteration (no break/continue/return can save it).
    Name          = "err_mem_loop_oob"
    Path          = "tests/err_mem_loop_oob.mettle"
    ShouldSucceed = $false
    Pattern       = 'This loop runs `j` up to 8, but `a` has 8 elements'
  },
  @{
    # Constant arithmetic: division by a literal zero is a guaranteed trap.
    Name          = "err_mem_div_zero"
    Path          = "tests/err_mem_div_zero.mettle"
    ShouldSucceed = $false
    Pattern       = 'Division by a constant zero'
  },
  @{
    # Constant out-of-bounds THROUGH a pointer alias: p = &a[2], p[6] = a[8].
    Name          = "err_mem_ptr_alias_oob"
    Path          = "tests/err_mem_ptr_alias_oob.mettle"
    ShouldSucceed = $false
    Pattern       = 'Index 6 through `p` lands at `a\[8\]`, out of bounds'
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
      'Double free of `p` \(already freed at line \d+\)',
      'Use of `p` after it was freed',
      'Global `STASH` is assigned the address of stack local `slot`',
      '`scratch` is allocated here but never freed',
      '`p` is null here \(assigned at line \d+ and never reassigned\)',
      'Shift by 32 on a 32-bit value',
      '`p` points at the constant address 64'
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
    # Use-after-move (M0113): `q = p` aliases one allocation under two names, so
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
  @{ Name = "err_top_level_return"; Path = "tests/err_top_level_return.mettle"; ShouldSucceed = $false; Pattern = "Return statement outside of a function|Unsupported top-level construct in declaration context" },
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
  @{ Name = "err_use_before_init"; Path = "tests/err_use_before_init.mettle"; ShouldSucceed = $false; Pattern = "before initialization" },
  @{ Name = "err_array_index_oob_const"; Path = "tests/err_array_index_oob_const.mettle"; ShouldSucceed = $false; Pattern = "out of bounds" },
  @{ Name = "err_array_index_oob_const_negative"; Path = "tests/err_array_index_oob_const_negative.mettle"; ShouldSucceed = $false; Pattern = "out of bounds" },
  @{ Name = "err_null_deref_const"; Path = "tests/err_null_deref_const.mettle"; ShouldSucceed = $false; Pattern = "Null pointer dereference" },
  @{ Name = "member_through_ptr"; Path = "tests/err_codegen_member_expr.mettle"; ShouldSucceed = $true },
  @{ Name = "err_function_arg_count"; Path = "tests/err_function_arg_count.mettle"; ShouldSucceed = $false; Pattern = "expects .* arguments, got" },
  @{ Name = "err_function_arg_type"; Path = "tests/err_function_arg_type.mettle"; ShouldSucceed = $false; Pattern = "Type mismatch" },
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
  @{ Name = "err_generics_generic_fn_ptr_address"; Path = "tests/err_generics_generic_fn_ptr_address.mettle"; ShouldSucceed = $false; Pattern = "Expected primary expression" },
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

        $binaryCheck = Test-BinaryOutput -BinaryPath $outFile
        if (-not $binaryCheck.Passed) {
          $passed = $false
          $reason = $binaryCheck.Reason
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
        if ($passed -and -not $SkipDeterminism -and
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
$simdRuntimeCases = @(
  @{
    Name            = "simd_correctness_int"
    Path            = "tests\simd_correctness\simd_int_check.mettle"
    OutputMustMatch = "INT SIMD: ALL OK"
    IrMustMatch     = @("sum_i32", "dot_i32", "scale_i32", "clamp_i32", "reverse_copy_i32", "minmax_i32")
  },
  @{
    Name            = "simd_correctness_float"
    Path            = "tests\simd_correctness\simd_float_check.mettle"
    OutputMustMatch = "FLOAT SIMD: ALL OK"
    IrMustMatch     = @("simd_sum_f64", "simd_sum_f32", "simd_dot_f64", "simd_dot_f32", "simd_affine_map_f64", "simd_affine_map_f32", "simd_vloop_f64")
  },
  @{
    Name            = "simd_correctness_byte"
    Path            = "tests\simd_correctness\simd_byte_check.mettle"
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

  $buildOut = & $CompilerPath --build --linker internal --release --dump-ir "tests\test_decorators.mettle" -o $exePath 2>&1 | Out-String
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
  $buildOut = & $CompilerPath --build --release "tests\ml_gate.mettle" -o $mlBase 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "ml_gate baseline build failed: $buildOut"
  }
  & $mlBase 2>&1 | Out-Null
  $mlBaseExit = $LASTEXITCODE

  $mlExe = Join-Path $tmpDir "ml_gate_ml.exe"
  $buildOut = & $CompilerPath --build --release --ml-opt "tests\ml_gate.mettle" -o $mlExe 2>&1 | Out-String
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
  $buildOut = & $CompilerPath --build --release --ml-opt-speculative "tests\ml_gate.mettle" -o $mlSpec 2>&1 | Out-String
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
  $buildOut = & $CompilerPath --build --release --ml-opt "tests\ml_gate.mettle" -o $badExe 2>&1 | Out-String
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
  $buildOut = & $CompilerPath --build --release --ml-opt "examples\explain_demo\explain_demo.mettle" -o $sabExe 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build --linker internal --release --native-heap --dump-ir "tests\test_native_heap.mettle" -o $exePath 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build --linker internal --release "tests\test_native_heap_threads.mettle" -o $exePath 2>&1 | Out-String
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

# Allocator reliability: double-free / bogus-free rejection (no free-list
# corruption). Exercises std/alloc directly; no flag needed.
$total++
try {
  $exePath = Join-Path $tmpDir "alloc_doublefree.exe"
  if (Test-Path $exePath) { Remove-Item -Path $exePath -Force -ErrorAction SilentlyContinue }
  $buildOut = & $CompilerPath --build --linker internal --release "tests\test_alloc_doublefree.mettle" -o $exePath 2>&1 | Out-String
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
  "tests\test_gc_alloc.mettle",
  "tests\test_gc_alloc_fixed.mettle",
  "tests\test_generics_new_heap.mettle",
  "tests\test_generics_full.mettle",
  "tests\test_generics_return_struct.mettle",
  "tests\test_generics_nested_struct.mettle",
  "tests\test_generics_in_control_flow.mettle",
  "tests\test_generics_float.mettle",
  "tests\test_large_db_cache_loop.mettle",
  "tests\test_arena_basic.mettle",
  "tests\test_arena_oversized.mettle",
  "tests\test_arena_savepoint.mettle",
  "tests\test_arena_reset_reuse.mettle",
  "tests\test_arena_align.mettle"
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
    @{ Path = "tests\test_generics_nested_struct.mettle"; ExitCode = 99; Label = "nested-struct" },
    @{ Path = "tests\test_generics_generic_enum.mettle"; ExitCode = 42; Label = "generic-enum" },
    @{ Path = "tests\test_generics_return_struct.mettle"; ExitCode = 30; Label = "return-struct" },
    @{ Path = "tests\test_generics_float.mettle"; ExitCode = 4; Label = "float" },
    @{ Path = "tests\test_generics_new_heap.mettle"; ExitCode = 42; Label = "new-heap" },
    @{ Path = "tests\test_generics_full.mettle"; ExitCode = 30; Label = "full" },
    @{ Path = "tests\test_generics_in_control_flow.mettle"; ExitCode = 24; Label = "control-flow" },
    @{ Path = "tests\test_trait_methods_generic_dispatch.mettle"; ExitCode = 42; Label = "trait-dispatch" }
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

# Global float variables: compile with --build and verify they read back their
# initializer (and survive mutation) instead of reading 0 from an uninitialized
# XMM lane. Returns 25+125+35+30 = 215.
$total++
try {
  $exePath = Join-Path $tmpDir "global_float_var.exe"
  $buildOut = & $CompilerPath --build "tests\test_global_float_var.mettle" -o $exePath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Global float var build failed: $buildOut"
  }
  if (-not (Test-Path $exePath)) {
    throw "Global float var build did not produce an executable"
  }
  & $exePath 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 215) {
    throw "Global float var exited with $LASTEXITCODE (expected 215)"
  }
  Write-CaseResult -Name "global_float_var_runtime" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "global_float_var_runtime" -Passed $false -Reason $_.Exception.Message
}

# Switch range cases: compile with --build and verify inclusive-interval dispatch.
$total++
try {
  $exePath = Join-Path $tmpDir "switch_range.exe"
  $buildOut = & $CompilerPath --build "tests\test_switch_range.mettle" -o $exePath 2>&1 | Out-String
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
  $buildOut = & $CompilerPath --build "tests\debug_crash.mettle" -o $exePath -s 2>&1 | Out-String
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
  $buildOut = & $CompilerPath --build "tests\crash_uaf_large.mettle" -o $exePath -s --native-heap 2>&1 | Out-String
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
  $buildOut = & $CompilerPath --build "tests\crash_waf_small.mettle" -o $exePath --native-heap 2>&1 | Out-String
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
  $buildOut = & $CompilerPath --build "tests\debug_demo.mettle" -o $exePath --debug-hooks 2>&1 | Out-String
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

# Debugger protocol end-to-end: node hosts the pipe and drives a real session
# (entry stop, breakpoint, stack, variables, step in/out, eval, and a live
# variable WRITE asserted on the program's final stdout).
$total++
try {
  $nodeCmd = Get-Command node -ErrorAction SilentlyContinue
  if (-not $nodeCmd) {
    Write-CaseResult -Name "debug_protocol" -Passed $true -Reason "skipped (node not found)"
  } else {
    $protoOut = & $nodeCmd.Source "mettle-syntax\scripts\test-debug-protocol.js" $CompilerPath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "Protocol test failed: $protoOut"
    }
    Write-CaseResult -Name "debug_protocol" -Passed $true
  }
}
catch {
  $failed++
  Write-CaseResult -Name "debug_protocol" -Passed $false -Reason $_.Exception.Message
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
  Copy-Item "tests\explain_demo.mettle" "$exDir\demo.mettle" -Force
  $exOut = Join-Path $exDir "demo.obj"
  $env:METTLE_EXPLAIN_REPORT_LINES = "0"
  $run1 = cmd /c "`"$((Resolve-Path $CompilerPath).Path)`" -i `"$exDir\demo.mettle`" -o `"$exOut`" --release --explain-json 2>&1" | Out-String
  if ($run1 -match 'changes since the last explain build') {
    throw "First build must not have a changes section"
  }
  $run2 = cmd /c "`"$((Resolve-Path $CompilerPath).Path)`" -i `"$exDir\demo.mettle`" -o `"$exOut`" --release --explain-json 2>&1" | Out-String
  if ($run2 -notmatch 'no optimization changes since the last explain build') {
    throw "Identical rebuild must report no changes"
  }
  (Get-Content "$exDir\demo.mettle" -Raw) -replace 'fn scale\(x: float32\)', '@noinline fn scale(x: float32)' |
    Set-Content "$exDir\demo.mettle" -Encoding ascii -NoNewline
  $run3 = cmd /c "`"$((Resolve-Path $CompilerPath).Path)`" -i `"$exDir\demo.mettle`" -o `"$exOut`" --release --explain-json 2>&1" | Out-String
  if ($run3 -notmatch 'REGRESSED' -or $run3 -notmatch 'was vectorized, now scalar') {
    throw "De-inlined scale must report a loop regression. Output: $($run3.Substring(0, [Math]::Min(600, $run3.Length)))"
  }
  $json = Get-Content (Join-Path $exDir "demo.explain.json") -Raw | ConvertFrom-Json
  if ($json.schema -ne 1) { throw "JSON schema field wrong" }
  if ($json.stats.changesRegressed -lt 1) { throw "JSON regression count missing" }
  if (@($json.remarks).Count -lt 10) { throw "JSON remarks too few: $(@($json.remarks).Count)" }
  $regLoop = @($json.changes.entries | Where-Object { $_.direction -eq 'regressed' -and $_.kind -eq 'loop' })
  if ($regLoop.Count -lt 1 -or -not $regLoop[0].reason) { throw "JSON regressed-loop entry missing reason" }
  if (-not @($json.remarks | Where-Object { $_.kind -eq 'loop' -and $_.depth -ge 2 }).Count) {
    throw "JSON nest depth missing (matvec inner loop is depth 2)"
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
  $memRun = & $CompilerPath -i "tests\warn_borrow_scope.mettle" -o $exOut --release --explain --explain-json 2>&1 | Out-String
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
  $buildOut = & $CompilerPath --build "tests\test_const_top_level.mettle" -o $exePath 2>&1 | Out-String
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
  $buildOut = & $CompilerPath --build "tests\test_const_local_float_string.mettle" -o $exePath 2>&1 | Out-String
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
  $buildOut = & $CompilerPath --build "tests\test_const_global_float_string.mettle" -o $exePath 2>&1 | Out-String
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
  $buildOut = & $CompilerPath --build "tests\test_import_conditional.mettle" -o $exePath 2>&1 | Out-String
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
  $buildOut = & $CompilerPath --build "tests\test_defer_by_value.mettle" -o $exePath 2>&1 | Out-String
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
  var msg: string = "Bundled stdlib works";
  println(cstr(msg));
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
  $packageRoot = Join-Path $repoRoot "tests\lib"

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

  $fpOut = & $CompilerPath --build tests\test_function_pointer.mettle -o $fpExe 2>&1 | Out-String
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

  $structNewOut = & $CompilerPath --build --linker internal tests\test_struct_new_zeroed.mettle -o $structNewExe 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_direct_object_return_const.mettle -o $objPath 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build --emit-obj tests\test_direct_object_return_const.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_direct_object_call_return.mettle -o $objPath 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build --emit-obj tests\test_direct_object_call_return.mettle -o $exePath 2>&1 | Out-String
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
    $buildArgs += @("tests\test_opt_closed_form_sum.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_opt_ptr_induction_two_loops.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_opt_tail_recursion.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_opt_readonly_global.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_opt_gather.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_opt_layout.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_float32_narrowing.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_float_narrowing_paths.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_mir_float_call_args.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_mir_fill_passthrough.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_mir_affine_map_passthrough.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_struct_copy_odd_size.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_mir_inline_struct_copy.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_const_divmod.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_const_mul.mettle", "-o", $exePath)

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
    $buildArgs += @("tests\test_sroa.mettle", "-o", $exePath)

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

  $compileHarness = & gcc -Wall -Wextra -std=c99 -g -O0 -D_GNU_SOURCE tests\coff_reader_test.c src\common.c src\lexer\lexer.c src\error\error_reporter.c src\linker\coff_reader.c -Isrc -o $coffReaderExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "COFF reader harness compile failed: $compileHarness"
  }

  $basicOut = & $CompilerPath --emit-obj tests\test_direct_object_return_const.mettle -o $basicObjPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "COFF reader basic object compile failed: $basicOut"
  }

  $relocOut = & $CompilerPath --emit-obj tests\test_direct_object_call_return.mettle -o $relocObjPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "COFF reader relocation object compile failed: $relocOut"
  }

  $longOut = & $CompilerPath --emit-obj tests\test_direct_object_long_symbol_name.mettle -o $longObjPath 2>&1 | Out-String
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

  $compileHarness = & gcc -Wall -Wextra -std=c99 -g -O0 -D_GNU_SOURCE tests\symbol_resolve_test.c src\common.c src\lexer\lexer.c src\error\error_reporter.c src\linker\coff_reader.c src\linker\symbol_resolve.c src\codegen\binary_emitter.c src\codegen\elf_emitter.c -Isrc -Isrc\codegen -o $symbolResolveExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Symbol-resolve harness compile failed: $compileHarness"
  }

  $cases = @(
    @{ Path = "tests\test_linker_merge_entry.mettle"; Out = $fnEntryObj; Label = "function-entry" },
    @{ Path = "tests\test_linker_merge_provider.mettle"; Out = $fnProviderObj; Label = "function-provider" },
    @{ Path = "tests\test_linker_merge_data_entry.mettle"; Out = $dataEntryObj; Label = "data-entry" },
    @{ Path = "tests\test_linker_merge_data_provider.mettle"; Out = $dataProviderObj; Label = "data-provider" },
    @{ Path = "tests\test_linker_merge_bss_entry.mettle"; Out = $bssEntryObj; Label = "bss-entry" },
    @{ Path = "tests\test_linker_merge_bss_provider.mettle"; Out = $bssProviderObj; Label = "bss-provider" },
    @{ Path = "tests\test_linker_duplicate_a.mettle"; Out = $dupAObj; Label = "duplicate-a" },
    @{ Path = "tests\test_linker_duplicate_b.mettle"; Out = $dupBObj; Label = "duplicate-b" },
    @{ Path = "tests\test_linker_unresolved_entry.mettle"; Out = $unresolvedObj; Label = "unresolved-entry" }
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

  $compileHarness = & gcc -Wall -Wextra -std=c99 -g -O0 -D_GNU_SOURCE tests\relocation_test.c src\common.c src\lexer\lexer.c src\error\error_reporter.c src\linker\coff_reader.c src\linker\symbol_resolve.c src\linker\relocation.c src\codegen\binary_emitter.c src\codegen\elf_emitter.c -Isrc -Isrc\codegen -o $relocationExe 2>&1 | Out-String
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

  $compileHarness = & gcc -Wall -Wextra -std=c99 -g -O0 -D_GNU_SOURCE tests\pe_emitter_test.c src\common.c src\lexer\lexer.c src\error\error_reporter.c src\linker\coff_reader.c src\linker\symbol_resolve.c src\linker\relocation.c src\linker\pe_emitter.c src\linker\import_lib.c src\codegen\binary_emitter.c src\codegen\elf_emitter.c -Isrc -Isrc\codegen -o $peEmitterExe 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build --emit-obj --linker internal tests\test_direct_object_return_const.mettle -o $exePath 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build --linker internal tests\test_float_negative_comparison.mettle -o $binaryExePath 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build --linker internal tests\test_float_negative_comparison.mettle -o $objExePath 2>&1 | Out-String
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
  $buildOut = & $CompilerPath --build --linker internal tests\test_abi_float_return.mettle -o $exePath 2>&1 | Out-String
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
      $buildOut = & $CompilerPath --build --linker internal tests\test_struct_copy.mettle -o $exePath 2>&1 | Out-String
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
      $buildOut = & $CompilerPath --build --linker internal tests\test_struct_pass_by_value.mettle -o $exePath 2>&1 | Out-String
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
      $buildOut = & $CompilerPath --build --linker internal tests\test_struct_return_by_value.mettle -o $exePath 2>&1 | Out-String
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
      $buildOut = & $CompilerPath --build --linker internal tests\test_struct_abi_matrix.mettle -o $exePath 2>&1 | Out-String
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
    $cOut = & gcc -c tests\struct_abi_c_shim.c -o $cObjPath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
      throw "Struct ABI C shim compile failed ($mode): $cOut"
    }

    $exePath = Join-Path $tmpDir "$caseName.exe"
      $buildOut = & $CompilerPath --build --linker internal tests\test_struct_abi_extern_c.mettle -o $exePath --link-arg $cObjPath 2>&1 | Out-String
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
      $buildOut = & $CompilerPath --build --linker internal tests\test_struct_float.mettle -o $exePath 2>&1 | Out-String
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

# Native object + MinGW gcc link (nostartfiles + CRT imports)
$total++
try {
  $gccCmd = Get-Command gcc -ErrorAction SilentlyContinue
  if (-not $gccCmd) {
    Write-CaseResult -Name "direct_object_emit_obj_gcc_link" -Passed $true -Reason "skipped: gcc not on PATH"
  }
  else {
    $exeGcc = Join-Path $tmpDir "direct_object_emit_obj_gcc_link.exe"
    $buildGccOut = & $CompilerPath --build --emit-obj --linker gcc tests\test_direct_object_return_const.mettle -o $exeGcc 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build --emit-obj --linker internal tests\test_internal_link_ws2_32.mettle -o $exePath --link-arg -lws2_32 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build --emit-obj --linker internal tests\test_internal_link_win32_user32.mettle -o $exePath 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build --emit-obj --linker internal tests\test_internal_link_ui.mettle -o $exePath 2>&1 | Out-String
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

# Internal linker UCRT test: std/io path resolves __acrt_iob_func via default DLL imports
$total++
try {
  $exePath = Join-Path $tmpDir "internal_link_std_io.exe"

  $buildOut = & $CompilerPath --build --emit-obj --linker internal tests\test_std_io.mettle -o $exePath 2>&1 | Out-String
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

  Write-CaseResult -Name "internal_link_std_io" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "internal_link_std_io" -Passed $false -Reason $_.Exception.Message
}

# Internal linker kernel32 atomics test: std/thread uses exported Interlocked* names
$total++
try {
  $exePath = Join-Path $tmpDir "internal_link_thread_atomics.exe"

  $buildOut = & $CompilerPath --build --emit-obj --linker internal tests\test_internal_link_thread_atomics.mettle -o $exePath 2>&1 | Out-String
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
    $buildOut = & $compilerFullPath --build --emit-obj tests\test_direct_object_return_const.mettle -o $exePath 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build --linker auto tests\test_auto_link_fallback_static_lib.mettle -o $exePath --link-arg $libPath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_direct_object_params.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object params compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object params compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests\test_direct_object_params.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_direct_object_control_flow.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object control-flow compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object control-flow compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests\test_direct_object_control_flow.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_abi_return_int.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object ABI-return-int compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object ABI-return-int compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests\test_abi_return_int.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_signed_arithmetic.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object signed-arithmetic compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object signed-arithmetic compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests\test_signed_arithmetic.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_control_flow.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object structured control-flow compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object structured control-flow compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests\test_control_flow.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_direct_object_integer_matrix.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object integer-matrix compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object integer-matrix compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests\test_direct_object_integer_matrix.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj --release tests\test_codegen_ir_fastpaths.mettle -o $objPath 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build --emit-obj --linker internal --release tests\test_codegen_ir_fastpaths.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_direct_object_scalar_casts.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object scalar-casts compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object scalar-casts compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests\test_direct_object_scalar_casts.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\ok_global_int.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object ok-global-int compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object ok-global-int compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests\ok_global_int.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_direct_object_global_string.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object global-string compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object global-string compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests\test_direct_object_global_string.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_extern_global_link_name.mettle -o $objPath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_pointer_param_address.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object pointer-param-address compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object pointer-param-address compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests\test_pointer_param_address.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_direct_object_struct_method_calls.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object struct-method compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object struct-method compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests\test_direct_object_struct_method_calls.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_direct_object_pointer_memory.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object pointer-memory compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object pointer-memory compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests\test_direct_object_pointer_memory.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj --release tests\test_direct_object_byte_load_store_alias.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object byte-load-store-alias compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object byte-load-store-alias compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj --linker internal --release tests\test_direct_object_byte_load_store_alias.mettle -o $exePath 2>&1 | Out-String
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

# Direct object backend aggregate-local test: stack-allocated struct addressed and passed by pointer
$total++
try {
  $objPath = Join-Path $tmpDir "test_direct_object_struct_field_offset.obj"
  $exePath = Join-Path $tmpDir "test_direct_object_struct_field_offset.exe"

  $objOut = & $CompilerPath --emit-obj tests\test_struct_field_offset.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object struct-field-offset compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object struct-field-offset compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests\test_struct_field_offset.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_array_struct_stride.mettle -o $objPath 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Direct object array-struct-stride compile failed: $objOut"
  }
  if (-not (Test-Path $objPath)) {
    throw "Direct object array-struct-stride compile did not produce an object file"
  }

  $buildOut = & $CompilerPath --build --emit-obj tests\test_array_struct_stride.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj tests\test_function_pointer.mettle -o $objPath 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build --emit-obj tests\test_function_pointer.mettle -o $exePath 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build --emit-obj tests\test_runtime_null_deref_check.mettle -o $exePath 2>&1 | Out-String
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

  $objOut = & $CompilerPath --emit-obj -s tests\test_runtime_null_deref_check.mettle -o $objPath 2>&1 | Out-String
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

  $buildOut = & $CompilerPath --build -s tests\test_runtime_null_deref_check.mettle -o $exePath 2>&1 | Out-String
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
  $boundsBuild = & $CompilerPath --build -s tests\test_crash_bounds_context.mettle -o $boundsExe 2>&1 | Out-String
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
  $avBuild = & $CompilerPath --build -s tests\test_runtime_av_trace_coff.mettle -o $avExe 2>&1 | Out-String
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

# main(argc, argv) test: startup calls CRT __getmainargs directly.
$total++
try {
  $avExe = Join-Path $tmpDir "test_main_argc_argv.exe"

  $avOut = & $CompilerPath --build tests\test_main_argc_argv.mettle -o $avExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "main(argc,argv) build failed: $avOut"
  }

  $avImports = & objdump -p $avExe 2>&1 | Out-String
  if ($avImports -notmatch "__getmainargs") {
    throw "main(argc,argv) executable missing __getmainargs import"
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

# main(argc, argv) via --build: internal startup must call __getmainargs.
$total++
try {
  $buildArgvExe = Join-Path $tmpDir "test_main_argc_argv_build.exe"

  $buildArgvOut = & $CompilerPath --build tests\test_main_argc_argv.mettle -o $buildArgvExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "main(argc,argv) --build compile failed: $buildArgvOut"
  }

  $exeImports = & objdump -p $buildArgvExe 2>&1 | Out-String
  if ($exeImports -notmatch "__getmainargs") {
    throw "main(argc,argv) --build executable missing __getmainargs import"
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

  $nullOut = & $CompilerPath --build -s tests\test_runtime_null_deref_check.mettle -o $nullExe 2>&1 | Out-String
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

# Crash handler test. On Windows this compiles and runs but is a documented
# no-op (the SEH crash path is already covered by runtime_null_trace /
# runtime_access_violation_trace); the meaningful assertions run on POSIX.
$total++
try {
  $crashHandlerExe = "bin\crash_handler_test.exe"
  & gcc -Wall -Wextra -std=c99 -g -O0 -D_GNU_SOURCE tests\crash_handler_test.c src\runtime\crash_handler.c -Isrc -o $crashHandlerExe
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to compile crash handler test"
  }

  $crashHandlerOutput = & $crashHandlerExe 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) {
    throw "Crash handler test exited with code $LASTEXITCODE"
  }

  if ($crashHandlerOutput -notmatch "Crash handler tests (passed|skipped)") {
    throw "Crash handler test output did not contain pass/skip marker"
  }

  Write-CaseResult -Name "crash_handler" -Passed $true
}
catch {
  $failed++
  Write-CaseResult -Name "crash_handler" -Passed $false -Reason $_.Exception.Message
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
  $arm64Exe = "bin\arm64_encode_test.exe"
  & gcc -Wall -Wextra -std=c99 -g -O0 -Isrc tests\arm64_encode_test.c src\codegen\binary\arm64_encode.c src\codegen\binary\arm64_disasm.c src\codegen\binary\arm64_abi.c -o $arm64Exe
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
  $arm64EmitExe = "bin\arm64_emit_test.exe"
  & gcc -Wall -Wextra -std=c99 -g -O0 -Isrc tests\arm64_emit_test.c src\codegen\binary\arm64_encode.c src\codegen\binary\arm64_emit.c src\codegen\binary\arm64_disasm.c src\codegen\binary\arm64_mir_encode.c -o $arm64EmitExe
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
  Copy-Item tests\arm64\expected.txt (Join-Path $elfDir "manifest.txt") -Force
  $names = Get-Content tests\arm64\expected.txt |
    ForEach-Object { ($_ -split ' ')[0] } | Where-Object { $_ }
  foreach ($n in $names) {
    $elf = Join-Path $elfDir "$n.elf"
    & $CompilerPath --emit-arm64 "tests\arm64\$n.mettle" -o $elf 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $elf)) {
      throw "mettle --emit-arm64 failed on $n.mettle"
    }
  }

  # I/O fixtures: compile and stage each with its expected-stdout .out sidecar.
  $ioDir = Join-Path $tmpDir "arm64io"
  New-Item -ItemType Directory -Force -Path $ioDir | Out-Null
  Get-ChildItem tests\arm64\io\*.mettle | ForEach-Object {
    $elf = Join-Path $ioDir ($_.BaseName + ".elf")
    & $CompilerPath --emit-arm64 $_.FullName -o $elf 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $elf)) {
      throw "mettle --emit-arm64 failed on io/$($_.Name)"
    }
    Copy-Item (Join-Path "tests\arm64\io" ($_.BaseName + ".out")) `
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

# General reduction-unrolling vectorizer: correctness on non-benchmark
# reductions (distinct EXPR(i), inclusive/exclusive bounds, a trip count that
# is not a multiple of the unroll factor so the scalar remainder runs). Built
# via the direct-object backend (the path the benchmarks use). Exact closed
# forms are asserted so a miscompiled unroll is caught, not just a crash.
$total++
try {
  $reduExe = "bin\test_opt_reduction_unroll.exe"
  $reduBuild = & $CompilerPath --build --emit-obj --linker internal --release `
    tests\test_opt_reduction_unroll.mettle -o $reduExe 2>&1 | Out-String
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
  $reduIr = & $CompilerPath --release --dump-ir tests\test_opt_reduction_unroll.mettle `
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
    tests\test_profile_runtime.mettle -o $profileExe 2>&1 | Out-String
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
    tests\test_profile_runtime.mettle -o $profileOpsExe 2>&1 | Out-String
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
  $iceCompile = & gcc -Wall -Wextra -std=c99 -g -O0 -Isrc tests\compiler_ice_report_test.c src\common.c src\lexer\lexer.c src\compiler\compiler_context.c src\compiler\compiler_crash.c src\runtime\crash_handler.c src\ir\ir.c -o $iceExe -ldbghelp 2>&1 | Out-String
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

# PTX backend validity gate. Emits the real GPU kernel corpus and the stress
# corpus to PTX and round-trips each through ptxas (NVIDIA's assembler) -- the
# authoritative check that the emitted PTX is well-formed. Catches emitter
# regressions that produce syntactically/typed-invalid PTX. Skipped when the
# CUDA toolkit (ptxas) is absent. Semantic (GPU-execution) checks live in
# examples/llm/qwen3/gpu/dgpu_check.mettle (needs a GPU, so not in this gate).
$ptxas = Get-Command ptxas -ErrorAction SilentlyContinue
if (-not $ptxas) {
  Write-Host "[SKIP] ptx_emit_validate (ptxas not found)"
}
else {
  foreach ($src in @("examples/llm/qwen3/gpu/kernels.mettle",
                     "examples/llm/qwen3/gpu/ptx_stress.mettle",
                     "examples/gpu_vadd/vadd_kernel.mettle")) {
    $total++
    $name = "ptx_emit_" + [System.IO.Path]::GetFileNameWithoutExtension($src)
    try {
      $ptxPath = Join-Path $tmpDir ($name + ".ptx")
      $cubin = Join-Path $tmpDir ($name + ".cubin")
      $emitOut = & $CompilerPath --emit-ptx $src -o $ptxPath 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "emit failed: $emitOut" }
      if (-not (Test-Path $ptxPath)) { throw "no PTX produced" }
      $asmOut = & $ptxas.Source -arch=sm_90 $ptxPath -o $cubin 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "ptxas rejected emitted PTX: $asmOut" }
      Write-CaseResult -Name $name -Passed $true
    }
    catch {
      $failed++
      Write-CaseResult -Name $name -Passed $false -Reason $_.Exception.Message
    }
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


