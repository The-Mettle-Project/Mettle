# Compare benchmark executables produced by two Mettle compiler versions.
#
# Default mode builds:
# - old compiler: clean archive of BaselineRef, usually HEAD
# - new compiler: current working tree
#
# Outputs are written under bin/benchmark-compare by default, which is ignored by
# git and keeps the checked-in Mettle-vs-C benchmark data untouched.
#
# Usage:
#   .\tools\benchmark\compare-mettle-versions.ps1
#   .\tools\benchmark\compare-mettle-versions.ps1 -Runs 7 -Benchmark collatz,sum_squares
#   .\tools\benchmark\compare-mettle-versions.ps1 -OldCompilerPath C:\old\mettle.exe -NewCompilerPath .\bin\mettle.exe -SkipBuildCurrent

param(
    [string]$BaselineRef = "HEAD",
    [string]$OldCompilerPath = "",
    [string]$NewCompilerPath = "",
    [string]$ConfigPath = "docs/benchmarks/harness.json",
    [string]$OutputRoot = "bin/benchmark-compare",
    [string[]]$Benchmark = @(),
    [int]$Runs = 5,
    [switch]$SkipBuildOld,
    [switch]$SkipBuildCurrent,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root

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

function Assert-PathInside {
    param(
        [string]$ParentPath,
        [string]$ChildPath
    )

    $parent = [System.IO.Path]::GetFullPath($ParentPath).TrimEnd("\", "/") + "\"
    $child = [System.IO.Path]::GetFullPath($ChildPath).TrimEnd("\", "/") + "\"

    if (-not $child.StartsWith($parent, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove path outside benchmark output root: $ChildPath"
    }
}

function New-CleanDirectory {
    param(
        [string]$PathValue,
        [string]$GuardRoot
    )

    if (Test-Path $PathValue) {
        Assert-PathInside -ParentPath $GuardRoot -ChildPath $PathValue
        Remove-Item -LiteralPath $PathValue -Recurse -Force
    }

    New-Item -ItemType Directory -Path $PathValue -Force | Out-Null
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
        ElapsedMs = [math]::Round($stopwatch.Elapsed.TotalMilliseconds, 0)
    }
}

function Parse-TimeMs {
    param([string]$Output)

    if ([string]::IsNullOrWhiteSpace($Output)) { return $null }

    $patterns = @(
        "(?im)^\s*Time:\s*([0-9]+(?:\.[0-9]+)?)\s*ms\b",
        "(?im)^\s*Elapsed(?:\s+time)?\s*:\s*([0-9]+(?:\.[0-9]+)?)\s*ms\b",
        "(?im)\b([0-9]+(?:\.[0-9]+)?)\s*ms\b"
    )

    foreach ($pattern in $patterns) {
        if ($Output -match $pattern) {
            return [double]$Matches[1]
        }
    }

    return $null
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

function Get-Mean {
    param([double[]]$Values)

    if ($null -eq $Values -or $Values.Count -eq 0) { return $null }

    $sum = 0.0
    foreach ($value in $Values) {
        $sum += [double]$value
    }

    return $sum / [double]$Values.Count
}

function Get-BenchmarkSource {
    param([object]$Bench)

    if ($null -ne $Bench.source -and -not [string]::IsNullOrWhiteSpace([string]$Bench.source)) {
        return Normalize-Path ([string]$Bench.source)
    }

    if ($null -eq $Bench.mettle_exe -or [string]::IsNullOrWhiteSpace([string]$Bench.mettle_exe)) {
        throw "Benchmark '$($Bench.name)' does not define source or mettle_exe."
    }

    return [System.IO.Path]::ChangeExtension((Normalize-Path ([string]$Bench.mettle_exe)), ".mettle")
}

function Export-BaselineSource {
    param(
        [string]$RefName,
        [string]$Destination,
        [string]$GuardRoot
    )

    New-CleanDirectory -PathValue $Destination -GuardRoot $GuardRoot
    $archivePath = Join-Path $GuardRoot "baseline-source.zip"
    if (Test-Path $archivePath) {
        Remove-Item -LiteralPath $archivePath -Force
    }

    Write-Log "Exporting baseline source from $RefName..."
    $archive = Invoke-CapturedProcess -FilePath "git" -Arguments @("archive", "--format=zip", "-o", $archivePath, $RefName) -WorkingDirectory $Root
    if ($archive.ExitCode -ne 0) {
        throw "git archive failed for '$RefName':`n$($archive.Output)"
    }

    Expand-Archive -LiteralPath $archivePath -DestinationPath $Destination -Force
    Remove-Item -LiteralPath $archivePath -Force
}

function Build-Compiler {
    param(
        [string]$SourceRoot,
        [string]$Label
    )

    Write-Log "Building $Label compiler..."
    # The baseline is an exported source tree, so it has no libmtlc of its own:
    # the backend is a gitignored dependency and git archive does not carry it.
    # Point the build at this checkout's copy, as an absolute path, so both the
    # baseline and the current build link the same backend -- which is what
    # makes the comparison a frontend comparison. A no-op for a build.bat that
    # builds the backend itself and ignores the variable.
    $previousLibmtlc = $env:LIBMTLC_DIR
    if (-not $env:LIBMTLC_DIR) {
        $localLibmtlc = Join-Path $Root "libmtlc"
        if (Test-Path (Join-Path $localLibmtlc "include\mtlc\mtlc.h")) {
            $env:LIBMTLC_DIR = [System.IO.Path]::GetFullPath($localLibmtlc)
        }
    }
    try {
        $build = Invoke-CapturedProcess -FilePath ".\build.bat" -WorkingDirectory $SourceRoot
    }
    finally {
        $env:LIBMTLC_DIR = $previousLibmtlc
    }
    if ($build.ExitCode -ne 0) {
        throw "$Label compiler build failed:`n$($build.Output)"
    }

    $compiler = Join-Path $SourceRoot "bin\mettle.exe"
    if (-not (Test-Path $compiler)) {
        throw "$Label compiler was not produced at $compiler"
    }

    return [pscustomobject][ordered]@{
        Path = [System.IO.Path]::GetFullPath($compiler)
        BuildMs = $build.ElapsedMs
    }
}

function Compile-Benchmark {
    param(
        [string]$CompilerPath,
        [string]$SourcePath,
        [string]$ExePath,
        [string]$Label,
        [string]$BenchmarkName
    )

    $exeDir = Split-Path -Parent $ExePath
    if (-not (Test-Path $exeDir)) {
        New-Item -ItemType Directory -Path $exeDir -Force | Out-Null
    }

    $args = @("--build", "--emit-obj", "--linker", "internal", "--release", $SourcePath, "-o", $ExePath)
    $compile = Invoke-CapturedProcess -FilePath $CompilerPath -Arguments $args -WorkingDirectory $Root
    if ($compile.ExitCode -ne 0) {
        throw "$Label compile failed for ${BenchmarkName}:`n$($compile.Output)"
    }

    if (-not (Test-Path $ExePath)) {
        throw "$Label compile did not produce $ExePath"
    }

    return [double]$compile.ElapsedMs
}

function Run-BenchmarkExe {
    param(
        [string]$ExePath,
        [string]$Label,
        [string]$BenchmarkName,
        [int]$RunIndex
    )

    $run = Invoke-CapturedProcess -FilePath $ExePath -WorkingDirectory $Root
    if ($run.ExitCode -ne 0) {
        throw "$Label run $RunIndex failed for $BenchmarkName with exit code $($run.ExitCode):`n$($run.Output)"
    }

    $reportedMs = Parse-TimeMs $run.Output
    $usedMs = $reportedMs
    if ($null -eq $usedMs) {
        $usedMs = [double]$run.ElapsedMs
    }

    return [pscustomobject][ordered]@{
        benchmark = $BenchmarkName
        compiler = $Label
        run = $RunIndex
        reported_ms = $reportedMs
        wall_ms = [double]$run.ElapsedMs
        used_ms = [double]$usedMs
    }
}

function Format-Number {
    param(
        [object]$Value,
        [int]$Digits = 2
    )

    if ($null -eq $Value) { return "" }
    return ([double]$Value).ToString("F$Digits", [System.Globalization.CultureInfo]::InvariantCulture)
}

function Write-MarkdownReport {
    param(
        [string]$PathValue,
        [object[]]$SummaryRows,
        [object]$Payload
    )

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("# Mettle Version Benchmark Compare")
    $lines.Add("")
    $lines.Add("- Generated: $($Payload.generated)")
    $lines.Add("- Baseline ref: $($Payload.baseline_ref)")
    $lines.Add("- Old compiler: ``$($Payload.compilers.old.path)``")
    $lines.Add("- New compiler: ``$($Payload.compilers.new.path)``")
    $lines.Add("- Runs per benchmark: $($Payload.runs_per_benchmark)")
    if ($null -ne $Payload.geomean_speedup) {
        $lines.Add("- Geomean runtime speedup: $((Format-Number $Payload.geomean_speedup 3))x")
    }
    $lines.Add("")
    $lines.Add("| Benchmark | Old median ms | New median ms | Speedup | Old compile ms | New compile ms |")
    $lines.Add("| --- | ---: | ---: | ---: | ---: | ---: |")

    foreach ($row in $SummaryRows) {
        $lines.Add("| $($row.benchmark) | $(Format-Number $row.old_median_ms 2) | $(Format-Number $row.new_median_ms 2) | $(Format-Number $row.speedup 3)x | $(Format-Number $row.old_compile_ms 0) | $(Format-Number $row.new_compile_ms 0) |")
    }

    $reportDir = Split-Path -Parent $PathValue
    if (-not (Test-Path $reportDir)) {
        New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
    }
    $lines | Set-Content -Path $PathValue -Encoding UTF8
}

if ($Runs -lt 1) {
    throw "-Runs must be at least 1."
}

$outputFullPath = Get-FullPath $OutputRoot
if (-not (Test-Path $outputFullPath)) {
    New-Item -ItemType Directory -Path $outputFullPath -Force | Out-Null
}

$configFullPath = Get-FullPath $ConfigPath
if (-not (Test-Path $configFullPath)) {
    throw "Harness config not found: $configFullPath"
}

$config = Get-Content -Raw -Path $configFullPath | ConvertFrom-Json
if ($null -eq $config -or $null -eq $config.benchmarks -or $config.benchmarks.Count -eq 0) {
    throw "Harness config has no benchmarks: $configFullPath"
}

$benchmarkFilter = @()
foreach ($entry in $Benchmark) {
    foreach ($namePart in ([string]$entry).Split(",")) {
        $trimmed = $namePart.Trim()
        if (-not [string]::IsNullOrWhiteSpace($trimmed)) {
            $benchmarkFilter += $trimmed
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
    throw "No benchmarks selected."
}

$oldBuildMs = $null
if (-not [string]::IsNullOrWhiteSpace($OldCompilerPath)) {
    $oldCompilerFullPath = Get-FullPath $OldCompilerPath
    if (-not (Test-Path $oldCompilerFullPath)) {
        throw "Old compiler not found: $oldCompilerFullPath"
    }
} else {
    $baselineSource = Join-Path $outputFullPath "baseline-source"
    if (-not $SkipBuildOld) {
        Export-BaselineSource -RefName $BaselineRef -Destination $baselineSource -GuardRoot $outputFullPath
        $oldBuild = Build-Compiler -SourceRoot $baselineSource -Label "old"
        $oldCompilerFullPath = $oldBuild.Path
        $oldBuildMs = $oldBuild.BuildMs
    } else {
        $oldCompilerFullPath = Join-Path $baselineSource "bin\mettle.exe"
        if (-not (Test-Path $oldCompilerFullPath)) {
            throw "Old compiler not found at $oldCompilerFullPath. Remove -SkipBuildOld or pass -OldCompilerPath."
        }
        $oldCompilerFullPath = [System.IO.Path]::GetFullPath($oldCompilerFullPath)
    }
}

$newBuildMs = $null
if (-not [string]::IsNullOrWhiteSpace($NewCompilerPath)) {
    $newCompilerFullPath = Get-FullPath $NewCompilerPath
    if (-not (Test-Path $newCompilerFullPath)) {
        throw "New compiler not found: $newCompilerFullPath"
    }
} else {
    if (-not $SkipBuildCurrent) {
        $newBuild = Build-Compiler -SourceRoot $Root -Label "new"
        $newCompilerFullPath = $newBuild.Path
        $newBuildMs = $newBuild.BuildMs
    } else {
        $newCompilerFullPath = Get-FullPath "bin\mettle.exe"
        if (-not (Test-Path $newCompilerFullPath)) {
            throw "New compiler not found at $newCompilerFullPath. Remove -SkipBuildCurrent or pass -NewCompilerPath."
        }
    }
}

$summaryRows = @()
$runRows = @()
$failures = @()

$oldExeDir = Join-Path $outputFullPath "exe\old"
$newExeDir = Join-Path $outputFullPath "exe\new"
New-CleanDirectory -PathValue $oldExeDir -GuardRoot $outputFullPath
New-CleanDirectory -PathValue $newExeDir -GuardRoot $outputFullPath

foreach ($bench in $selectedBenchmarks) {
    $name = [string]$bench.name
    $description = [string]$bench.description
    $sourceRelative = Get-BenchmarkSource -Bench $bench
    $sourcePath = Get-FullPath $sourceRelative
    if (-not (Test-Path $sourcePath)) {
        throw "Benchmark source not found for ${name}: $sourcePath"
    }

    Write-Log ""
    Write-Log "Benchmark $name..."

    $oldExe = Join-Path $oldExeDir "$name.exe"
    $newExe = Join-Path $newExeDir "$name.exe"

    try {
        $oldCompileMs = Compile-Benchmark -CompilerPath $oldCompilerFullPath -SourcePath $sourcePath -ExePath $oldExe -Label "old" -BenchmarkName $name
        $newCompileMs = Compile-Benchmark -CompilerPath $newCompilerFullPath -SourcePath $sourcePath -ExePath $newExe -Label "new" -BenchmarkName $name

        $oldValues = @()
        $newValues = @()
        for ($i = 1; $i -le $Runs; $i++) {
            if (($i % 2) -eq 1) {
                $oldRun = Run-BenchmarkExe -ExePath $oldExe -Label "old" -BenchmarkName $name -RunIndex $i
                $newRun = Run-BenchmarkExe -ExePath $newExe -Label "new" -BenchmarkName $name -RunIndex $i
            } else {
                $newRun = Run-BenchmarkExe -ExePath $newExe -Label "new" -BenchmarkName $name -RunIndex $i
                $oldRun = Run-BenchmarkExe -ExePath $oldExe -Label "old" -BenchmarkName $name -RunIndex $i
            }

            $runRows += $oldRun
            $runRows += $newRun
            $oldValues += [double]$oldRun.used_ms
            $newValues += [double]$newRun.used_ms
        }

        $oldMedian = Get-Median -Values $oldValues
        $newMedian = Get-Median -Values $newValues
        $oldMean = Get-Mean -Values $oldValues
        $newMean = Get-Mean -Values $newValues
        $speedup = $null
        $deltaPercent = $null
        if ($null -ne $oldMedian -and $null -ne $newMedian -and $newMedian -gt 0) {
            $speedup = [double]$oldMedian / [double]$newMedian
            $deltaPercent = (([double]$newMedian - [double]$oldMedian) / [double]$oldMedian) * 100.0
        }

        $summaryRow = [pscustomobject][ordered]@{
            benchmark = $name
            description = $description
            source = $sourceRelative
            old_median_ms = $oldMedian
            new_median_ms = $newMedian
            old_mean_ms = $oldMean
            new_mean_ms = $newMean
            speedup = $speedup
            delta_percent = $deltaPercent
            old_compile_ms = $oldCompileMs
            new_compile_ms = $newCompileMs
        }
        $summaryRows += $summaryRow

        Write-Log ("  old median {0} ms | new median {1} ms | speedup {2}x" -f (Format-Number $oldMedian 2), (Format-Number $newMedian 2), (Format-Number $speedup 3))
    } catch {
        $message = $_.Exception.Message
        $failures += [pscustomobject][ordered]@{
            benchmark = $name
            error = $message
        }
        Write-Log "  FAILED: $message"
    }
}

$geomeanSpeedup = $null
$validSpeedups = @()
foreach ($row in $summaryRows) {
    if ($null -ne $row.speedup -and [double]$row.speedup -gt 0) {
        $validSpeedups += [double]$row.speedup
    }
}
if ($validSpeedups.Count -gt 0) {
    $logSum = 0.0
    foreach ($speed in $validSpeedups) {
        $logSum += [math]::Log($speed)
    }
    $geomeanSpeedup = [math]::Exp($logSum / [double]$validSpeedups.Count)
}

$summaryCsvPath = Join-Path $outputFullPath "mettle-version-summary.csv"
$runsCsvPath = Join-Path $outputFullPath "mettle-version-runs.csv"
$jsonPath = Join-Path $outputFullPath "mettle-version-compare.json"
$markdownPath = Join-Path $outputFullPath "mettle-version-summary.md"

$summaryRows | Export-Csv -Path $summaryCsvPath -NoTypeInformation -Encoding UTF8
$runRows | Export-Csv -Path $runsCsvPath -NoTypeInformation -Encoding UTF8

$payload = [ordered]@{
    generated = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    baseline_ref = $BaselineRef
    config = $ConfigPath
    output_root = $OutputRoot
    runs_per_benchmark = $Runs
    compilers = [ordered]@{
        old = [ordered]@{
            path = $oldCompilerFullPath
            build_ms = $oldBuildMs
        }
        new = [ordered]@{
            path = $newCompilerFullPath
            build_ms = $newBuildMs
        }
    }
    geomean_speedup = $geomeanSpeedup
    benchmarks = $summaryRows
    runs = $runRows
    failures = $failures
}

($payload | ConvertTo-Json -Depth 12) | Set-Content -Path $jsonPath -Encoding UTF8
Write-MarkdownReport -PathValue $markdownPath -SummaryRows $summaryRows -Payload ([pscustomobject]$payload)

Write-Log ""
Write-Log "Wrote $summaryCsvPath"
Write-Log "Wrote $runsCsvPath"
Write-Log "Wrote $jsonPath"
Write-Log "Wrote $markdownPath"
if ($null -ne $geomeanSpeedup) {
    Write-Log ("Geomean runtime speedup: {0}x" -f (Format-Number $geomeanSpeedup 3))
}

if ($failures.Count -gt 0) {
    exit 1
}
