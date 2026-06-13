# Compare Rust vs C on the Mettle example benchmarks.
#
# - Reads benchmark matrix from docs/benchmarks/harness.json
# - Compiles C (gcc -O2) and Rust (rustc -O) with timed builds
# - Records executable sizes
# - Runs warmup + median of N runs, parses "Time: <N> us"
# - Writes docs/benchmarks/rust-vs-c.json
#
# Usage:
#   .\tools\benchmark\compare-rust.ps1
#   .\tools\benchmark\compare-rust.ps1 -Runs 7 -Warmup 2
#   .\tools\benchmark\compare-rust.ps1 -Benchmark grep,fib
#   .\tools\benchmark\compare-rust.ps1 -Quiet

param(
    [switch]$Quiet,
    [string]$ConfigPath = "docs/benchmarks/harness.json",
    [string]$OutputPath = "docs/benchmarks/rust-vs-c.json",
    [string[]]$Benchmark = @(),
    [int]$Runs = 5,
    [int]$Warmup = 1
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root

$InvariantCulture = [System.Globalization.CultureInfo]::InvariantCulture

function Write-Log {
    param([string]$Message)
    if (-not $Quiet) { Write-Host $Message }
}

function Normalize-Path {
    param([string]$PathValue)
    return ($PathValue -replace "/", "\")
}

function Get-FullPath {
    param([string]$PathValue)

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return ""
    }

    $normalized = Normalize-Path $PathValue
    if ([System.IO.Path]::IsPathRooted($normalized)) {
        return [System.IO.Path]::GetFullPath($normalized)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $Root $normalized))
}

function Get-BenchmarkRustExe {
    param([object]$Bench)

    if ($null -ne $Bench.rust_exe -and -not [string]::IsNullOrWhiteSpace([string]$Bench.rust_exe)) {
        return Normalize-Path ([string]$Bench.rust_exe)
    }

    if ($null -eq $Bench.c_exe -or [string]::IsNullOrWhiteSpace([string]$Bench.c_exe)) {
        throw "Benchmark '$($Bench.name)' does not define rust_exe or c_exe."
    }

    $cExe = Normalize-Path ([string]$Bench.c_exe)
    if ($cExe -match "_c\.exe$") {
        return ($cExe -replace "_c\.exe$", "_rs.exe")
    }

    return [System.IO.Path]::ChangeExtension($cExe, "_rs.exe")
}

function Get-BenchmarkRustSource {
    param([object]$Bench)

    if ($null -ne $Bench.rust_source -and -not [string]::IsNullOrWhiteSpace([string]$Bench.rust_source)) {
        return Normalize-Path ([string]$Bench.rust_source)
    }

    return [System.IO.Path]::ChangeExtension((Get-BenchmarkRustExe -Bench $Bench), ".rs")
}

function Get-BenchmarkCSource {
    param([object]$Bench)

    if ($null -ne $Bench.c_source -and -not [string]::IsNullOrWhiteSpace([string]$Bench.c_source)) {
        return Normalize-Path ([string]$Bench.c_source)
    }

    if ($null -eq $Bench.c_exe -or [string]::IsNullOrWhiteSpace([string]$Bench.c_exe)) {
        throw "Benchmark '$($Bench.name)' does not define c_source or c_exe."
    }

    $cExe = Normalize-Path ([string]$Bench.c_exe)
    if ($cExe -match "_c\.exe$") {
        return ($cExe -replace "_c\.exe$", ".c")
    }

    return [System.IO.Path]::ChangeExtension($cExe, ".c")
}

function Parse-TimeUs {
    param([string]$Output)

    if ([string]::IsNullOrWhiteSpace($Output)) { return $null }

    if ($Output -match "(?im)^\s*Time:\s*([0-9]+)\s*us\b") {
        return [double]$Matches[1]
    }

    $msPatterns = @(
        "(?im)^\s*Time:\s*([0-9]+(?:\.[0-9]+)?)\s*ms\b",
        "(?im)^\s*Elapsed(?:\s+time)?\s*:\s*([0-9]+(?:\.[0-9]+)?)\s*ms\b"
    )

    foreach ($pattern in $msPatterns) {
        if ($Output -match $pattern) {
            return [double]$Matches[1] * 1000.0
        }
    }

    return $null
}

function Invoke-CapturedProcess {
    param(
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = ""
    )

    $previousLocation = Get-Location
    $previousErrorActionPreference = $ErrorActionPreference
    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        Set-Location $WorkingDirectory
    }

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $ErrorActionPreference = "Continue"
        $output = & $FilePath @Arguments 2>&1 | Out-String
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
        $stopwatch.Stop()
        Set-Location $previousLocation
    }

    return [pscustomobject][ordered]@{
        ExitCode = $exitCode
        Output = $output
        ElapsedMs = [double]$stopwatch.Elapsed.TotalMilliseconds
    }
}

function Get-Median {
    param([double[]]$Values)

    if ($null -eq $Values -or $Values.Count -eq 0) { return $null }

    $sorted = @($Values | Sort-Object)
    $middle = [int]($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) {
        return [double]$sorted[$middle]
    }

    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Format-Number {
    param(
        [object]$Value,
        [int]$Digits = 2
    )

    if ($null -eq $Value) { return "" }
    return ([double]$Value).ToString("F$Digits", $InvariantCulture)
}

function Format-BenchTime {
    param([double]$Microseconds)

    if ($null -eq $Microseconds) { return "FAIL" }

    $us = [uint64][math]::Round([double]$Microseconds, 0)
    $ms = [double]$Microseconds / 1000.0
    $msDigits = if ($ms -ge 100.0) { 2 } elseif ($ms -ge 1.0) { 3 } else { 3 }
    $msText = $ms.ToString("F$msDigits", $InvariantCulture)
    $usText = $us.ToString("N0", $InvariantCulture)
    return "$msText ms ($usText us)"
}

function Format-FileSize {
    param([long]$Bytes)

    if ($null -eq $Bytes -or $Bytes -lt 0) { return "FAIL" }

    if ($Bytes -ge 1048576) {
        return ("{0} MB" -f (([double]$Bytes / 1048576.0).ToString("F2", $InvariantCulture)))
    }

    if ($Bytes -ge 1024) {
        return ("{0} KB" -f (([double]$Bytes / 1024.0).ToString("F1", $InvariantCulture)))
    }

    return ("{0} B" -f $Bytes)
}

function Format-Ratio {
    param(
        [double]$Numerator,
        [double]$Denominator
    )

    if ($null -eq $Numerator -or $null -eq $Denominator -or $Denominator -le 0) {
        return $null
    }

    return [math]::Round($Numerator / $Denominator, 2)
}

function Compile-CBenchmark {
    param(
        [string]$SourcePath,
        [string]$ExePath,
        [string]$BenchmarkName
    )

    $exeDir = Split-Path -Parent $ExePath
    if (-not (Test-Path $exeDir)) {
        New-Item -ItemType Directory -Path $exeDir -Force | Out-Null
    }

    if (Test-Path $ExePath) {
        Remove-Item -LiteralPath $ExePath -Force
    }

    $args = @("-O2", "-o", $ExePath, $SourcePath, "-lkernel32")
    $compile = Invoke-CapturedProcess -FilePath "gcc" -Arguments $args -WorkingDirectory $Root
    if ($compile.ExitCode -ne 0) {
        throw "C compile failed for ${BenchmarkName}:`n$($compile.Output)"
    }

    if (-not (Test-Path $ExePath)) {
        throw "C compile did not produce $ExePath"
    }

    return [double]$compile.ElapsedMs
}

function Compile-RustBenchmark {
    param(
        [string]$SourcePath,
        [string]$ExePath,
        [string]$BenchmarkName
    )

    $exeDir = Split-Path -Parent $ExePath
    if (-not (Test-Path $exeDir)) {
        New-Item -ItemType Directory -Path $exeDir -Force | Out-Null
    }

    if (Test-Path $ExePath) {
        Remove-Item -LiteralPath $ExePath -Force
    }

    $args = @("-O", "--edition", "2021", "-o", $ExePath, $SourcePath)
    $compile = Invoke-CapturedProcess -FilePath "rustc" -Arguments $args -WorkingDirectory $Root
    if ($compile.ExitCode -ne 0) {
        throw "Rust compile failed for ${BenchmarkName}:`n$($compile.Output)"
    }

    if (-not (Test-Path $ExePath)) {
        throw "Rust compile did not produce $ExePath"
    }

    return [double]$compile.ElapsedMs
}

function Get-ExeSizeBytes {
    param([string]$ExePath)

    if (-not (Test-Path $ExePath)) {
        return $null
    }

    return (Get-Item -LiteralPath $ExePath).Length
}

function Measure-BenchmarkExe {
    param(
        [string]$ExePath,
        [int]$WarmupCount,
        [int]$RunCount
    )

    if (-not (Test-Path $ExePath)) {
        return $null
    }

    for ($i = 0; $i -lt $WarmupCount; $i++) {
        $warmup = Invoke-CapturedProcess -FilePath $ExePath
        if ($warmup.ExitCode -ne 0) {
            return $null
        }
    }

    $values = @()
    for ($i = 0; $i -lt $RunCount; $i++) {
        $run = Invoke-CapturedProcess -FilePath $ExePath
        if ($run.ExitCode -ne 0) {
            return $null
        }

        $reportedUs = Parse-TimeUs $run.Output
        if ($null -ne $reportedUs) {
            $values += [double]$reportedUs
            continue
        }

        if ($null -ne $run.ElapsedMs) {
            $values += [double]$run.ElapsedMs * 1000.0
        }
    }

    if ($values.Count -eq 0) {
        return $null
    }

    return Get-Median -Values $values
}

if ($Runs -lt 1) {
    Write-Error "-Runs must be at least 1."
    exit 1
}

if ($Warmup -lt 0) {
    Write-Error "-Warmup must be at least 0."
    exit 1
}

$configFullPath = Join-Path $Root (Normalize-Path $ConfigPath)
if (-not (Test-Path $configFullPath)) {
    Write-Error "Harness config not found: $configFullPath"
    exit 1
}

$config = Get-Content -Raw -Path $configFullPath | ConvertFrom-Json
if ($null -eq $config -or $null -eq $config.benchmarks -or $config.benchmarks.Count -eq 0) {
    Write-Error "Harness config has no benchmarks: $configFullPath"
    exit 1
}

$gccCommand = Get-Command gcc -CommandType Application -ErrorAction SilentlyContinue
if (-not $gccCommand) {
    Write-Error "gcc is required but was not found on PATH."
    exit 1
}

$rustcCommand = Get-Command rustc -CommandType Application -ErrorAction SilentlyContinue
if (-not $rustcCommand) {
    Write-Error "rustc is required but was not found on PATH."
    exit 1
}

$benchmarkFilter = @()
if ($Benchmark.Count -gt 0) {
    foreach ($entry in $Benchmark) {
        foreach ($part in ($entry -split ",")) {
            $trimmed = $part.Trim()
            if (-not [string]::IsNullOrWhiteSpace($trimmed)) {
                $benchmarkFilter += $trimmed
            }
        }
    }
}

$selectedBenchmarks = @()
foreach ($bench in $config.benchmarks) {
    $name = [string]$bench.name
    if ($benchmarkFilter.Count -gt 0 -and -not ($benchmarkFilter -contains $name)) {
        continue
    }
    $selectedBenchmarks += $bench
}

if ($selectedBenchmarks.Count -eq 0) {
    Write-Error "No benchmarks selected."
    exit 1
}

$results = @()
$failed = @()

foreach ($bench in $selectedBenchmarks) {
    $name = [string]$bench.name
    $description = [string]$bench.description
    $cExe = Get-FullPath ([string]$bench.c_exe)
    $rustExe = Get-FullPath (Get-BenchmarkRustExe -Bench $bench)
    $cSource = Get-FullPath (Get-BenchmarkCSource -Bench $bench)
    $rustSource = Get-FullPath (Get-BenchmarkRustSource -Bench $bench)

    Write-Log "Building $name..."

    try {
        if (-not (Test-Path $cSource)) {
            throw "C source not found: $cSource"
        }
        if (-not (Test-Path $rustSource)) {
            throw "Rust source not found: $rustSource"
        }

        $cCompileMs = Compile-CBenchmark -SourcePath $cSource -ExePath $cExe -BenchmarkName $name
        $rustCompileMs = Compile-RustBenchmark -SourcePath $rustSource -ExePath $rustExe -BenchmarkName $name

        $cExeBytes = Get-ExeSizeBytes -ExePath $cExe
        $rustExeBytes = Get-ExeSizeBytes -ExePath $rustExe

        $cUs = Measure-BenchmarkExe -ExePath $cExe -WarmupCount $Warmup -RunCount $Runs
        $rustUs = Measure-BenchmarkExe -ExePath $rustExe -WarmupCount $Warmup -RunCount $Runs

        $cMs = if ($null -ne $cUs) { [math]::Round($cUs / 1000.0, 3) } else { $null }
        $rustMs = if ($null -ne $rustUs) { [math]::Round($rustUs / 1000.0, 3) } else { $null }

        $runtimeRatio = Format-Ratio -Numerator $rustUs -Denominator $cUs
        $compileRatio = Format-Ratio -Numerator $rustCompileMs -Denominator $cCompileMs
        $sizeRatio = Format-Ratio -Numerator $rustExeBytes -Denominator $cExeBytes

        $results += [ordered]@{
            name = $name
            description = $description
            rust_us = if ($null -ne $rustUs) { [uint64][math]::Round($rustUs, 0) } else { $null }
            c_us = if ($null -ne $cUs) { [uint64][math]::Round($cUs, 0) } else { $null }
            rust_ms = $rustMs
            c_ms = $cMs
            relative = $runtimeRatio
            rust_compile_ms = [math]::Round($rustCompileMs, 0)
            c_compile_ms = [math]::Round($cCompileMs, 0)
            compile_relative = $compileRatio
            rust_exe_bytes = $rustExeBytes
            c_exe_bytes = $cExeBytes
            size_relative = $sizeRatio
        }

        Write-Log ("  runtime  Rust: {0} | C: {1}{2} | median of {3} run(s), {4} warmup" -f `
            $(if ($null -ne $rustUs) { Format-BenchTime $rustUs } else { "FAIL" }), `
            $(if ($null -ne $cUs) { Format-BenchTime $cUs } else { "FAIL" }), `
            $(if ($null -ne $runtimeRatio) { " | $runtimeRatio`x vs C" } else { "" }), `
            $Runs, $Warmup)
        Write-Log ("  compile  Rust: {0} ms | C: {1} ms{2}" -f `
            (Format-Number $rustCompileMs 0), `
            (Format-Number $cCompileMs 0), `
            $(if ($null -ne $compileRatio) { " | $compileRatio`x vs C" } else { "" }))
        Write-Log ("  size     Rust: {0} | C: {1}{2}" -f `
            (Format-FileSize $rustExeBytes), `
            (Format-FileSize $cExeBytes), `
            $(if ($null -ne $sizeRatio) { " | $sizeRatio`x vs C" } else { "" }))
    } catch {
        Write-Log "  FAILED: $($_.Exception.Message)"
        $failed += $name
    }
}

$payload = [ordered]@{
    generated = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    harness = [ordered]@{
        config = $ConfigPath
        mode = "rust-vs-c"
        runs = $Runs
        warmup = $Warmup
        rustc = $rustcCommand.Source
        gcc = $gccCommand.Source
    }
    benchmarks = $results
    failed = $failed
}

$json = $payload | ConvertTo-Json -Depth 10

$outputFullPath = Get-FullPath $OutputPath
$outputDir = Split-Path -Parent $outputFullPath
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}
$json | Set-Content -Path $outputFullPath -Encoding UTF8

Write-Log ""
Write-Log "Wrote $outputFullPath"
