# Dedicated Mettle benchmark harness against C.
#
# - Reads benchmark matrix from docs/benchmarks/harness.json
# - Compiles Mettle and C binaries (timed)
# - Records executable sizes
# - Executes binaries (warmup + median of N runs)
# - Parses "Time: <N> us" output (falls back to ms)
# - Optionally profiles large compile-only fixtures (parse_stress, profiler)
# - Writes canonical JSON to docs/benchmarks/latest.json
# - Mirrors JSON to web/benchmarks.json for the web server
#
# Usage:
#   .\tools\benchmark\run-benchmarks.ps1
#   .\tools\benchmark\run-benchmarks.ps1 -BuildCompiler
#   .\tools\benchmark\run-benchmarks.ps1 -Runs 7 -Warmup 2
#   .\tools\benchmark\run-benchmarks.ps1 -Benchmark fib,grep
#   .\tools\benchmark\run-benchmarks.ps1 -Suite 2          # Suite 2 only (new benchmark set)
#   .\tools\benchmark\run-benchmarks.ps1 -MlOpt        # compile Mettle with --ml-opt
#   .\tools\benchmark\run-benchmarks.ps1 -CompileOnly
#   .\tools\benchmark\run-benchmarks.ps1 -SkipCompileBenchmarks
#   .\tools\benchmark\run-benchmarks.ps1 -Quiet
#   .\tools\benchmark\run-benchmarks.ps1 -OpenReport
#   .\tools\benchmark\run-benchmarks.ps1 -NoReport

param(
    [switch]$BuildCompiler,
    [switch]$CompileOnly,
    [switch]$SkipCompileBenchmarks,
    [switch]$Quiet,
    [switch]$NoReport,
    [switch]$OpenReport,
    [switch]$Gcc,
    [switch]$Clang,
    [switch]$MlOpt,
    [string]$ConfigPath = "docs/benchmarks/harness.json",
    [string]$CompilerPath = "",
    [string[]]$Benchmark = @(),
    [string[]]$CFlags = @(),
    [int]$Runs = 0,
    [int]$Warmup = -1,
    [int[]]$Suite = @()
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

function Test-BenchmarkSelected {
    param([string]$Name)

    if ($null -eq $Benchmark -or $Benchmark.Count -eq 0) {
        return $true
    }

    foreach ($item in $Benchmark) {
        if ($item -eq $Name) {
            return $true
        }
    }

    return $false
}

function Get-BenchmarkSuite {
    param([object]$Bench)

    if ($null -ne $Bench.suite) {
        return [int]$Bench.suite
    }

    return 1
}

function Test-SuiteSelected {
    param([int]$SuiteNumber)

    if ($null -eq $Suite -or $Suite.Count -eq 0) {
        return $true
    }

    foreach ($item in $Suite) {
        if ($item -eq $SuiteNumber) {
            return $true
        }
    }

    return $false
}

function Get-BenchmarkMettleExe {
    param([object]$Bench)

    if ($null -ne $Bench.mettle_exe -and -not [string]::IsNullOrWhiteSpace([string]$Bench.mettle_exe)) {
        return Normalize-Path ([string]$Bench.mettle_exe)
    }

    throw "Benchmark '$($Bench.name)' does not define mettle_exe."
}

function Get-BenchmarkMettleSource {
    param([object]$Bench)

    if ($null -ne $Bench.mettle_source -and -not [string]::IsNullOrWhiteSpace([string]$Bench.mettle_source)) {
        return Normalize-Path ([string]$Bench.mettle_source)
    }

    return [System.IO.Path]::ChangeExtension((Get-BenchmarkMettleExe -Bench $Bench), ".mettle")
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

function Parse-ProfileTotalMs {
    param([string]$Output)

    if ([string]::IsNullOrWhiteSpace($Output)) { return $null }

    if ($Output -match "(?im)^\s*total\s+([0-9]+(?:\.[0-9]+)?)\s*ms\b") {
        return [double]$Matches[1]
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

function Get-StdDev {
    param([double[]]$Values)

    if ($null -eq $Values -or $Values.Count -lt 2) { return $null }

    $mean = ($Values | Measure-Object -Average).Average
    $sumSquares = 0.0
    foreach ($value in $Values) {
        $delta = [double]$value - $mean
        $sumSquares += $delta * $delta
    }

    return [math]::Sqrt($sumSquares / ($Values.Count - 1))
}

function Get-TimingStats {
    param([double[]]$Values)

    if ($null -eq $Values -or $Values.Count -eq 0) { return $null }

    $sorted = @($Values | Sort-Object)
    return [ordered]@{
        median_us = [uint64][math]::Round((Get-Median -Values $Values), 0)
        min_us = [uint64][math]::Round([double]$sorted[0], 0)
        max_us = [uint64][math]::Round([double]$sorted[$sorted.Count - 1], 0)
        stddev_us = if ($Values.Count -ge 2) { [math]::Round((Get-StdDev -Values $Values), 1) } else { $null }
        samples = $Values.Count
    }
}

function Get-GeometricMean {
    param([double[]]$Values)

    if ($null -eq $Values -or $Values.Count -eq 0) { return $null }

    $sumLogs = 0.0
    foreach ($value in $Values) {
        if ($value -le 0) { return $null }
        $sumLogs += [math]::Log([double]$value)
    }

    return [math]::Round([math]::Exp($sumLogs / $Values.Count), 3)
}

function Get-BenchmarkHostInfo {
    param(
        [string]$CompilerFullPath,
        [string]$GccVersion,
        [string]$CCompilerName = "gcc"
    )

    $osCaption = $null
    $cpuName = $null
    $logicalProcessors = $null
    $ramGb = $null

    try {
        $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
        $osCaption = [string]$os.Caption
        if ($null -ne $os.TotalVisibleMemorySize) {
            $ramGb = [math]::Round([double]$os.TotalVisibleMemorySize / 1048576.0, 1)
        }
    } catch {
        $osCaption = [string][System.Environment]::OSVersion
    }

    try {
        $cpu = Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1
        if ($null -ne $cpu) {
            $cpuName = [string]$cpu.Name
            $logicalProcessors = [int]$cpu.NumberOfLogicalProcessors
        }
    } catch {
        $logicalProcessors = [int][System.Environment]::ProcessorCount
    }

    return [ordered]@{
        machine_name = [string]$env:COMPUTERNAME
        os = $osCaption
        cpu = $cpuName
        logical_processors = $logicalProcessors
        ram_gb = $ramGb
        c_compiler = $CCompilerName
        c_compiler_version = $GccVersion
        compiler = $CompilerFullPath
    }
}

function Get-BenchmarkSummary {
    param([array]$RuntimeResults)

    $runtimeRatios = @()
    $compileRatios = @()
    $sizeRatios = @()
    $wins = 0
    $losses = 0
    $parity = 0
    $fastestWin = $null
    $slowestLoss = $null

    foreach ($row in $RuntimeResults) {
        if ($null -ne $row.relative) {
            $ratio = [double]$row.relative
            $runtimeRatios += $ratio

            if ($ratio -lt 0.95) {
                $wins++
                if ($null -eq $fastestWin -or $ratio -lt [double]$fastestWin.relative) {
                    $fastestWin = [ordered]@{ name = $row.name; relative = $ratio }
                }
            } elseif ($ratio -gt 1.05) {
                $losses++
                if ($null -eq $slowestLoss -or $ratio -gt [double]$slowestLoss.relative) {
                    $slowestLoss = [ordered]@{ name = $row.name; relative = $ratio }
                }
            } else {
                $parity++
            }
        }

        if ($null -ne $row.compile_relative) {
            $compileRatios += [double]$row.compile_relative
        }

        if ($null -ne $row.size_relative) {
            $sizeRatios += [double]$row.size_relative
        }
    }

    return [ordered]@{
        runtime_wins = $wins
        runtime_losses = $losses
        runtime_parity = $parity
        runtime_geomean = Get-GeometricMean -Values $runtimeRatios
        compile_geomean = Get-GeometricMean -Values $compileRatios
        size_geomean = Get-GeometricMean -Values $sizeRatios
        fastest_win = $fastestWin
        slowest_loss = $slowestLoss
    }
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

function Get-MettleBuildArgs {
    param(
        [object]$Config,
        [object]$Bench
    )

    $args = @()
    if ($null -ne $Config.defaults -and $null -ne $Config.defaults.mettle_flags) {
        $args += @($Config.defaults.mettle_flags)
    } else {
        $args += @("--build", "--emit-obj", "--linker", "internal", "--release")
    }

    if ($null -ne $Bench.mettle_flags) {
        $args = @($Bench.mettle_flags)
    }

    if ($script:MlOpt -and ($args -notcontains "--ml-opt")) {
        $args += "--ml-opt"
    }

    return $args
}

function Get-CCompileArgs {
    param(
        [object]$Config,
        [string]$SourcePath,
        [string]$ExePath
    )

    $flags = @()
    if ($CFlags.Count -gt 0) {
        $flags = @($CFlags)
    } elseif ($null -ne $Config.defaults -and $null -ne $Config.defaults.c_flags) {
        $flags = @($Config.defaults.c_flags)
    } else {
        $flags = @("-O3", "-lkernel32")
    }

    return @($flags) + @("-o", $ExePath, $SourcePath)
}

function Compile-MettleBenchmark {
    param(
        [string]$CompilerPath,
        [string]$SourcePath,
        [string]$ExePath,
        [string]$BenchmarkName,
        [object]$Config,
        [object]$Bench
    )

    $exeDir = Split-Path -Parent $ExePath
    if (-not (Test-Path $exeDir)) {
        New-Item -ItemType Directory -Path $exeDir -Force | Out-Null
    }

    if (Test-Path $ExePath) {
        Remove-Item -LiteralPath $ExePath -Force
    }

    $buildArgs = Get-MettleBuildArgs -Config $Config -Bench $Bench
    $args = @($buildArgs) + @($SourcePath, "-o", $ExePath)
    $compile = Invoke-CapturedProcess -FilePath $CompilerPath -Arguments $args -WorkingDirectory $Root
    if ($compile.ExitCode -ne 0) {
        throw "Mettle compile failed for ${BenchmarkName}:`n$($compile.Output)"
    }

    if (-not (Test-Path $ExePath)) {
        throw "Mettle compile did not produce $ExePath"
    }

    return [double]$compile.ElapsedMs
}

function Export-MettleAsmSnapshot {
    param(
        [string]$CompilerPath,
        [string]$SourcePath,
        [string]$BenchmarkName,
        [object]$Config,
        [object]$Bench
    )

    $asmDir = Join-Path $Root ".tmp\bench-asm"
    if (-not (Test-Path $asmDir)) {
        New-Item -ItemType Directory -Path $asmDir -Force | Out-Null
    }

    $objPath = Join-Path $asmDir "${BenchmarkName}_mettle.obj"
    $asmPath = Join-Path $asmDir "${BenchmarkName}_mettle.asm.txt"

    if (Test-Path $objPath) {
        Remove-Item -LiteralPath $objPath -Force
    }

    $buildArgs = Get-MettleBuildArgs -Config $Config -Bench $Bench
    $args = @($buildArgs) + @($SourcePath, "-o", $objPath)
    $compile = Invoke-CapturedProcess -FilePath $CompilerPath -Arguments $args -WorkingDirectory $Root
    if ($compile.ExitCode -ne 0) {
        Write-Log "  asm snapshot skipped for ${BenchmarkName}: $($compile.Output)"
        return
    }

    $objdump = Get-Command objdump -CommandType Application -ErrorAction SilentlyContinue
    if (-not $objdump) {
        Write-Log "  asm snapshot skipped for ${BenchmarkName}: objdump not on PATH"
        return
    }

    $disasm = Invoke-CapturedProcess -FilePath "objdump" -Arguments @("-d", "-M", "intel", $objPath) -WorkingDirectory $Root
    if ($disasm.ExitCode -eq 0) {
        $disasm.Output | Set-Content -Path $asmPath -Encoding UTF8
        Write-Log "  asm snapshot: $asmPath"
    }
}

function Compile-CBenchmarkWithFlags {
    param(
        [string]$SourcePath,
        [string]$ExePath,
        [string]$BenchmarkName,
        [string[]]$Flags,
        [string]$CCompiler = "gcc"
    )

    $exeDir = Split-Path -Parent $ExePath
    if (-not (Test-Path $exeDir)) {
        New-Item -ItemType Directory -Path $exeDir -Force | Out-Null
    }

    if (Test-Path $ExePath) {
        Remove-Item -LiteralPath $ExePath -Force
    }

    $args = @($Flags) + @("-o", $ExePath, $SourcePath)
    $compile = Invoke-CapturedProcess -FilePath $CCompiler -Arguments $args -WorkingDirectory $Root
    if ($compile.ExitCode -ne 0) {
        throw "C compile failed for ${BenchmarkName}:`n$($compile.Output)"
    }

    return [double]$compile.ElapsedMs
}

function Profile-MettleCompile {
    param(
        [string]$CompilerPath,
        [string]$SourcePath,
        [string]$BenchmarkName
    )

    $args = @("--profile", $SourcePath)
    $compile = Invoke-CapturedProcess -FilePath $CompilerPath -Arguments $args -WorkingDirectory $Root
    if ($compile.ExitCode -ne 0) {
        throw "Mettle profile compile failed for ${BenchmarkName}:`n$($compile.Output)"
    }

    $profileTotalMs = Parse-ProfileTotalMs $compile.Output
    if ($null -eq $profileTotalMs) {
        throw "Could not parse profile total for ${BenchmarkName}:`n$($compile.Output)"
    }

    return [double]$profileTotalMs
}

function Compile-CBenchmark {
    param(
        [string]$SourcePath,
        [string]$ExePath,
        [string]$BenchmarkName,
        [object]$Config,
        [string]$CCompiler = "gcc"
    )

    $exeDir = Split-Path -Parent $ExePath
    if (-not (Test-Path $exeDir)) {
        New-Item -ItemType Directory -Path $exeDir -Force | Out-Null
    }

    if (Test-Path $ExePath) {
        Remove-Item -LiteralPath $ExePath -Force
    }

    $args = Get-CCompileArgs -Config $Config -SourcePath $SourcePath -ExePath $ExePath
    $compile = Invoke-CapturedProcess -FilePath $CCompiler -Arguments $args -WorkingDirectory $Root
    if ($compile.ExitCode -ne 0) {
        throw "C compile failed for ${BenchmarkName}:`n$($compile.Output)"
    }

    if (-not (Test-Path $ExePath)) {
        throw "C compile did not produce $ExePath"
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

    $median = Get-Median -Values $values
    return [pscustomobject][ordered]@{
        median_us = [double]$median
        runs_us = @($values | ForEach-Object { [uint64][math]::Round([double]$_, 0) })
        stats = Get-TimingStats -Values $values
    }
}

function Write-SummaryTable {
    param(
        [array]$RuntimeResults,
        [array]$CompileResults
    )

    if ($Quiet) { return }

    Write-Log ""
    Write-Log "=== Runtime summary (Mettle vs C, median) ==="
    $suiteGroups = $RuntimeResults | Group-Object -Property { if ($null -ne $_.suite) { [int]$_.suite } else { 1 } } | Sort-Object Name
    foreach ($group in $suiteGroups) {
        Write-Log ""
        Write-Log "--- Suite $($group.Name) ---"
        Write-Log ("{0,-16} {1,12} {2,12} {3,8} {4,8} {5,8}" -f "benchmark", "mettle", "c", "runtime", "compile", "size")
        foreach ($row in $group.Group) {
            $runtimeRatio = if ($null -ne $row.relative) { "{0}x" -f ([double]$row.relative).ToString("F2", $InvariantCulture) } else { "FAIL" }
            $compileRatio = if ($null -ne $row.compile_relative) { "{0}x" -f ([double]$row.compile_relative).ToString("F2", $InvariantCulture) } else { "FAIL" }
            $sizeRatio = if ($null -ne $row.size_relative) { "{0}x" -f ([double]$row.size_relative).ToString("F2", $InvariantCulture) } else { "FAIL" }
            Write-Log ("{0,-16} {1,12} {2,12} {3,8} {4,8} {5,8}" -f `
                $row.name, `
                $(if ($null -ne $row.mettle_ms) { "{0} ms" -f ([double]$row.mettle_ms).ToString("F3", $InvariantCulture) } else { "FAIL" }), `
                $(if ($null -ne $row.c_ms) { "{0} ms" -f ([double]$row.c_ms).ToString("F3", $InvariantCulture) } else { "FAIL" }), `
                $runtimeRatio, $compileRatio, $sizeRatio)
        }
    }

    if ($CompileResults.Count -gt 0) {
        Write-Log ""
        Write-Log "=== Compile-only summary (Mettle --profile total) ==="
        Write-Log ("{0,-14} {1,12}" -f "benchmark", "compile_ms")
        foreach ($row in $CompileResults) {
            Write-Log ("{0,-14} {1,12}" -f $row.name, $(Format-Number $row.mettle_compile_ms 0))
        }
    }
}

$configFullPath = Join-Path $Root (Normalize-Path $ConfigPath)
if (-not (Test-Path $configFullPath)) {
    Write-Error "Harness config not found: $configFullPath"
    exit 1
}

$config = Get-Content -Raw -Path $configFullPath | ConvertFrom-Json

$effectiveRuns = if ($Runs -gt 0) { $Runs } elseif ($null -ne $config.defaults -and $null -ne $config.defaults.runs) { [int]$config.defaults.runs } else { 5 }
$effectiveWarmup = if ($Warmup -ge 0) { $Warmup } elseif ($null -ne $config.defaults -and $null -ne $config.defaults.warmup) { [int]$config.defaults.warmup } else { 1 }

if ($effectiveRuns -lt 1) {
    Write-Error "-Runs must be at least 1."
    exit 1
}

if ($effectiveWarmup -lt 0) {
    Write-Error "-Warmup must be at least 0."
    exit 1
}

if ($BuildCompiler) {
    Write-Log "Building compiler..."
    & .\build.bat
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

$compilerFullPath = if ([string]::IsNullOrWhiteSpace($CompilerPath)) {
    Get-FullPath "bin\mettle.exe"
} else {
    Get-FullPath $CompilerPath
}

if (-not (Test-Path $compilerFullPath)) {
    Write-Error "Compiler not found: $compilerFullPath"
    exit 1
}

if ($Clang -and $Gcc) {
    Write-Error "-Clang and -Gcc are mutually exclusive."
    exit 1
}

$cCompiler = if ($Clang) { "clang" } else { "gcc" }

$cCompilerCommand = Get-Command $cCompiler -CommandType Application -ErrorAction SilentlyContinue
if (-not $cCompilerCommand) {
    Write-Error "$cCompiler is required for the C benchmark builds but was not found on PATH."
    exit 1
}

if ($MlOpt) { Write-Log "ML optimizer enabled: compiling Mettle with --ml-opt" }

$cVersionOutput = (& $cCompiler --version 2>&1 | Out-String).Trim()
$cVersionLine = ($cVersionOutput -split "`r?`n")[0]
$hostInfo = Get-BenchmarkHostInfo -CompilerFullPath $compilerFullPath -GccVersion $cVersionLine -CCompilerName $cCompiler

$results = @()
$compileResults = @()
$failed = @()

if ($null -ne $config.benchmarks -and $config.benchmarks.Count -gt 0) {
    foreach ($bench in $config.benchmarks) {
        $name = [string]$bench.name
        $suiteNumber = Get-BenchmarkSuite -Bench $bench
        if (-not (Test-BenchmarkSelected -Name $name) -or -not (Test-SuiteSelected -SuiteNumber $suiteNumber)) {
            continue
        }

        $description = [string]$bench.description
        $mettleExe = Get-FullPath (Get-BenchmarkMettleExe -Bench $bench)
        $cExe = Get-FullPath ([string]$bench.c_exe)
        $mettleSource = Get-FullPath (Get-BenchmarkMettleSource -Bench $bench)
        $cSource = Get-FullPath (Get-BenchmarkCSource -Bench $bench)

        Write-Log "Building $name..."

        try {
            if (-not (Test-Path $mettleSource)) {
                throw "Mettle source not found: $mettleSource"
            }
            if (-not (Test-Path $cSource)) {
                throw "C source not found: $cSource"
            }

            $mettleCompileMs = Compile-MettleBenchmark -CompilerPath $compilerFullPath -SourcePath $mettleSource -ExePath $mettleExe -BenchmarkName $name -Config $config -Bench $bench
            $cCompileMs = Compile-CBenchmark -SourcePath $cSource -ExePath $cExe -BenchmarkName $name -Config $config -CCompiler $cCompiler

            if ($bench.track_asm -eq $true) {
                Export-MettleAsmSnapshot -CompilerPath $compilerFullPath -SourcePath $mettleSource -BenchmarkName $name -Config $config -Bench $bench
            }

            $mettleExeBytes = Get-ExeSizeBytes -ExePath $mettleExe
            $cExeBytes = Get-ExeSizeBytes -ExePath $cExe

            $mettleUs = $null
            $cUs = $null
            $cNoinlineUs = $null
            $mettleRunsUs = $null
            $cRunsUs = $null
            $mettleStats = $null
            $cStats = $null
            if (-not $CompileOnly) {
                $mettleMeasure = Measure-BenchmarkExe -ExePath $mettleExe -WarmupCount $effectiveWarmup -RunCount $effectiveRuns
                $cMeasure = Measure-BenchmarkExe -ExePath $cExe -WarmupCount $effectiveWarmup -RunCount $effectiveRuns
                if ($null -ne $mettleMeasure) {
                    $mettleUs = $mettleMeasure.median_us
                    $mettleRunsUs = $mettleMeasure.runs_us
                    $mettleStats = $mettleMeasure.stats
                }
                if ($null -ne $cMeasure) {
                    $cUs = $cMeasure.median_us
                    $cRunsUs = $cMeasure.runs_us
                    $cStats = $cMeasure.stats
                }
                if ($name -eq "matrix_mul" -and $CFlags.Count -eq 0) {
                    $cNoinlineExe = Get-FullPath ($cExe -replace "_c\.exe$", "_c_noinline.exe")
                    try {
                        Compile-CBenchmarkWithFlags -SourcePath $cSource -ExePath $cNoinlineExe -BenchmarkName "${name}_noinline" -Flags @("-O3", "-fno-inline", "-lkernel32") -CCompiler $cCompiler | Out-Null
                        $cNoinlineMeasure = Measure-BenchmarkExe -ExePath $cNoinlineExe -WarmupCount $effectiveWarmup -RunCount $effectiveRuns
                        if ($null -ne $cNoinlineMeasure) {
                            $cNoinlineUs = $cNoinlineMeasure.median_us
                        }
                    } catch {
                        Write-Log "  optional C -fno-inline baseline skipped: $($_.Exception.Message)"
                    }
                }
            }

            $mettleMs = if ($null -ne $mettleUs) { [math]::Round($mettleUs / 1000.0, 3) } else { $null }
            $cMs = if ($null -ne $cUs) { [math]::Round($cUs / 1000.0, 3) } else { $null }

            $runtimeRatio = Format-Ratio -Numerator $mettleUs -Denominator $cUs
            $compileRatio = Format-Ratio -Numerator $mettleCompileMs -Denominator $cCompileMs
            $sizeRatio = Format-Ratio -Numerator $mettleExeBytes -Denominator $cExeBytes

            $results += [ordered]@{
                name = $name
                kind = if ($null -ne $bench.kind) { [string]$bench.kind } else { "runtime" }
                suite = $suiteNumber
                description = $description
                mettle_us = if ($null -ne $mettleUs) { [uint64][math]::Round($mettleUs, 0) } else { $null }
                c_us = if ($null -ne $cUs) { [uint64][math]::Round($cUs, 0) } else { $null }
                c_noinline_us = if ($null -ne $cNoinlineUs) { [uint64][math]::Round($cNoinlineUs, 0) } else { $null }
                mettle_runs_us = $mettleRunsUs
                c_runs_us = $cRunsUs
                mettle_stats = $mettleStats
                c_stats = $cStats
                mettle_ms = $mettleMs
                c_ms = $cMs
                relative = $runtimeRatio
                mettle_compile_ms = [math]::Round($mettleCompileMs, 0)
                c_compile_ms = [math]::Round($cCompileMs, 0)
                compile_relative = $compileRatio
                mettle_exe_bytes = $mettleExeBytes
                c_exe_bytes = $cExeBytes
                size_relative = $sizeRatio
            }

            if (-not $CompileOnly) {
                Write-Log ("  runtime  Mettle: {0} | C: {1}{2} | median of {3} run(s), {4} warmup" -f `
                    $(if ($null -ne $mettleUs) { Format-BenchTime $mettleUs } else { "FAIL" }), `
                    $(if ($null -ne $cUs) { Format-BenchTime $cUs } else { "FAIL" }), `
                    $(if ($null -ne $runtimeRatio) { " | $runtimeRatio`x vs C" } else { "" }), `
                    $effectiveRuns, $effectiveWarmup)
            }
            Write-Log ("  compile  Mettle: {0} ms | C: {1} ms{2}" -f `
                (Format-Number $mettleCompileMs 0), `
                (Format-Number $cCompileMs 0), `
                $(if ($null -ne $compileRatio) { " | $compileRatio`x vs C" } else { "" }))
            if (-not $CompileOnly) {
                Write-Log ("  size     Mettle: {0} | C: {1}{2}" -f `
                    (Format-FileSize $mettleExeBytes), `
                    (Format-FileSize $cExeBytes), `
                    $(if ($null -ne $sizeRatio) { " | $sizeRatio`x vs C" } else { "" }))
            }
        } catch {
            Write-Log "  FAILED: $($_.Exception.Message)"
            $failed += $name
        }
    }
} else {
    Write-Error "Harness config has no runtime benchmarks: $configFullPath"
    exit 1
}

if (-not $SkipCompileBenchmarks -and $null -ne $config.compile_benchmarks -and $config.compile_benchmarks.Count -gt 0) {
    foreach ($bench in $config.compile_benchmarks) {
        $name = [string]$bench.name
        if (-not (Test-BenchmarkSelected -Name $name)) {
            continue
        }

        $description = [string]$bench.description
        $mettleSource = Get-FullPath (Normalize-Path ([string]$bench.mettle_source))

        Write-Log "Profiling compile: $name..."

        try {
            if (-not (Test-Path $mettleSource)) {
                throw "Mettle source not found: $mettleSource"
            }

            $mettleCompileMs = Profile-MettleCompile -CompilerPath $compilerFullPath -SourcePath $mettleSource -BenchmarkName $name

            $compileResults += [ordered]@{
                name = $name
                description = $description
                mettle_source = [string]$bench.mettle_source
                mettle_compile_ms = [math]::Round($mettleCompileMs, 0)
            }

            Write-Log ("  compile  Mettle profile total: {0} ms" -f (Format-Number $mettleCompileMs 0))
        } catch {
            Write-Log "  FAILED: $($_.Exception.Message)"
            $failed += $name
        }
    }
}

Write-SummaryTable -RuntimeResults $results -CompileResults $compileResults

$mettleFlags = if ($null -ne $config.defaults -and $null -ne $config.defaults.mettle_flags) {
    @($config.defaults.mettle_flags)
} else {
    @("--build", "--emit-obj", "--linker", "internal", "--release")
}
if ($MlOpt -and ($mettleFlags -notcontains "--ml-opt")) {
    $mettleFlags += "--ml-opt"
}

$summary = Get-BenchmarkSummary -RuntimeResults $results

$payload = [ordered]@{
    generated = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    host = $hostInfo
    summary = $summary
    harness = [ordered]@{
        config = $ConfigPath
        mode = if ($null -ne $config.mode) { [string]$config.mode } else { "mettle-vs-c" }
        runs = $effectiveRuns
        warmup = $effectiveWarmup
        compile_only = [bool]$CompileOnly
        compiler = $compilerFullPath
        c_compiler = $cCompiler
        mettle_flags = @($mettleFlags)
        c_flags = if ($CFlags.Count -gt 0) { @($CFlags) } elseif ($null -ne $config.defaults.c_flags) { @($config.defaults.c_flags) } else { @("-O3", "-lkernel32") }
    }
    benchmarks = $results
    compile_benchmarks = $compileResults
    failed = $failed
}

$json = $payload | ConvertTo-Json -Depth 10

$primaryOutput = Normalize-Path ([string]$config.outputs.primary)
$primaryOutputPath = Join-Path $Root $primaryOutput
$primaryDir = Split-Path -Parent $primaryOutputPath
if (-not (Test-Path $primaryDir)) { New-Item -ItemType Directory -Path $primaryDir -Force | Out-Null }
$json | Set-Content -Path $primaryOutputPath -Encoding UTF8

$mirrorOutput = Normalize-Path ([string]$config.outputs.mirror_web)
if (-not [string]::IsNullOrWhiteSpace($mirrorOutput)) {
    $mirrorPath = Join-Path $Root $mirrorOutput
    $mirrorDir = Split-Path -Parent $mirrorPath
    if (-not (Test-Path $mirrorDir)) { New-Item -ItemType Directory -Path $mirrorDir -Force | Out-Null }
    $json | Set-Content -Path $mirrorPath -Encoding UTF8
}

Write-Log ""
Write-Log "Wrote $primaryOutputPath"
if (-not [string]::IsNullOrWhiteSpace($mirrorOutput)) {
    Write-Log "Mirrored to $(Join-Path $Root $mirrorOutput)"
}

if (-not $NoReport) {
    $reportOutput = if ($null -ne $config.outputs.report_html -and -not [string]::IsNullOrWhiteSpace([string]$config.outputs.report_html)) {
        [string]$config.outputs.report_html
    } else {
        [System.IO.Path]::ChangeExtension($primaryOutput, ".html")
    }

    $reportScript = Join-Path $PSScriptRoot "generate-report.ps1"
    if (Test-Path $reportScript) {
        $reportArgs = @{
            InputPath = $primaryOutput
            OutputPath = $reportOutput
        }
        if ($OpenReport) {
            $reportArgs.OpenReport = $true
        }
        & $reportScript @reportArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Log "Report generation failed."
        }
    } else {
        Write-Log "Report generator not found: $reportScript"
    }
}

if (-not $Quiet -and $null -ne $summary.runtime_geomean) {
    Write-Log ""
    Write-Log ("Summary: {0} faster, {1} parity, {2} slower | runtime geomean {3}x | compile geomean {4}x | size geomean {5}x" -f `
        $summary.runtime_wins, `
        $summary.runtime_parity, `
        $summary.runtime_losses, `
        (Format-Number $summary.runtime_geomean 2), `
        (Format-Number $summary.compile_geomean 2), `
        (Format-Number $summary.size_geomean 2))
}

if ($failed.Count -gt 0) {
    Write-Log ""
    Write-Log ("Failed benchmarks: {0}" -f ($failed -join ", "))
    exit 1
}
