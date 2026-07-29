# Interleaved A/B runtime comparison of two Mettle compilers.
#
# Compiles each selected benchmark with a BASE compiler and a NEW compiler,
# then times both arms *inside the same iteration* (alternating which goes
# first) so scheduler drift and thermal ramp hit both equally. Reports the
# minimum over -Runs iterations, which is the statistic that actually settles:
# a median still tracks the noise floor drifting under it.
#
# Runs are pinned to one P-core (CPU 4 by default -- CPU 0 takes interrupt and
# DPC work) at High priority. Unpinned, this box's hybrid scheduler moves a run
# between P- and E-cores and swings the number ~1.5x, which swamps anything
# worth measuring.
#
# Binaries are hashed: where the two arms are byte-identical the measured
# delta is pure noise, which calibrates the floor for free.
#
# Usage:
#   .\tools\benchmark\ab.ps1
#   .\tools\benchmark\ab.ps1 -Benchmark crc32,rec_fib -Runs 11
#   .\tools\benchmark\ab.ps1 -Base .\.perfref\bin\mettle.exe -New .\bin\mettle.exe

param(
    [string]$Base = ".perfref\bin\mettle.exe",
    [string]$New  = "bin\mettle.exe",
    [string[]]$Benchmark = @(),
    [int]$Runs = 9,
    [int]$AffinityMask = 0x10,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root

$BasePath = [System.IO.Path]::GetFullPath((Join-Path $Root $Base))
$NewPath  = [System.IO.Path]::GetFullPath((Join-Path $Root $New))
foreach ($p in @($BasePath, $NewPath)) {
    if (-not (Test-Path $p)) { throw "compiler not found: $p" }
}

# Benchmark matrix comes from the same config the headline harness uses, so the
# two never drift apart on which source file a name refers to.
$cfgRaw = [System.IO.File]::ReadAllText((Join-Path $Root "docs\benchmarks\harness.json"))
$cfg = $cfgRaw.TrimStart([char]0xFEFF) | ConvertFrom-Json
$all = @($cfg.benchmarks | Where-Object { $_.kind -eq "runtime" })

# -Benchmark arrives as one comma-joined string when the script is invoked
# through `powershell -File`; split it back apart so both call styles work.
$want = @()
foreach ($b in $Benchmark) { $want += ($b -split ",") | ForEach-Object { $_.Trim() } }
$want = @($want | Where-Object { $_ })
if ($want.Count -gt 0) {
    $all = @($all | Where-Object { $want -contains $_.name })
    foreach ($w in $want) {
        if (-not ($all | Where-Object { $_.name -eq $w })) { Write-Warning "no such benchmark: $w" }
    }
}
if ($all.Count -eq 0) { throw "no benchmarks selected" }

$mettleFlags = @($cfg.defaults.mettle_flags)
$scratch = Join-Path $env:TEMP "mettle-ab"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

function Build-Arm {
    param([string]$Compiler, [string]$Source, [string]$OutExe)

    if (Test-Path $OutExe) { Remove-Item -Force $OutExe }
    $args = @($mettleFlags) + @($Source, "-o", $OutExe)
    $out = & $Compiler @args 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $OutExe)) {
        return @{ ok = $false; log = ($out -join "`n") }
    }
    return @{ ok = $true; log = "" }
}

# One timed run: start pinned + High priority, scrape the benchmark's own
# self-reported "Time: <N> us" line rather than timing the process, so process
# startup and I/O stay out of the number.
function Measure-Once {
    param([string]$Exe)

    $outFile = Join-Path $scratch "run.out"
    $p = Start-Process -FilePath $Exe -PassThru -NoNewWindow `
        -RedirectStandardOutput $outFile -WorkingDirectory (Split-Path -Parent $Exe)
    try {
        $p.ProcessorAffinity = [System.IntPtr]$AffinityMask
        $p.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::High
    } catch { }
    # Not $p.ExitCode: a -PassThru process object does not populate it unless
    # the caller opted into raising events. A missing "Time:" line below is the
    # failure signal instead.
    $p.WaitForExit()

    $text = Get-Content $outFile -Raw
    if ($text -match "Time:\s*(\d+)\s*us") { return [double]$Matches[1] }
    if ($text -match "Time:\s*([\d.]+)\s*ms") { return [double]$Matches[1] * 1000.0 }
    return $null
}

function Hash-Of { param([string]$Path) (Get-FileHash $Path -Algorithm SHA256).Hash }

$results = @()
foreach ($b in $all) {
    $src = Join-Path $Root ($b.mettle_source -replace "/", "\")
    if (-not (Test-Path $src)) { Write-Warning "$($b.name): missing source"; continue }

    $baseExe = Join-Path $scratch "$($b.name)_base.exe"
    $newExe  = Join-Path $scratch "$($b.name)_new.exe"

    $bb = Build-Arm $BasePath $src $baseExe
    if (-not $bb.ok) { Write-Warning "$($b.name): base build failed`n$($bb.log)"; continue }
    $nb = Build-Arm $NewPath  $src $newExe
    if (-not $nb.ok) { Write-Warning "$($b.name): new build failed`n$($nb.log)"; continue }

    $identical = (Hash-Of $baseExe) -eq (Hash-Of $newExe)

    # Warm the caches and the branch predictors once per arm before measuring.
    [void](Measure-Once $baseExe)
    [void](Measure-Once $newExe)

    $baseTimes = @(); $newTimes = @()
    $failed = $false
    for ($i = 0; $i -lt $Runs; $i++) {
        # Alternate arm order so a monotonic drift (thermal, background load)
        # cannot systematically favour whichever arm always ran first.
        if ($i % 2 -eq 0) {
            $t1 = Measure-Once $baseExe; $t2 = Measure-Once $newExe
        } else {
            $t2 = Measure-Once $newExe;  $t1 = Measure-Once $baseExe
        }
        if ($null -eq $t1 -or $null -eq $t2) { $failed = $true; break }
        $baseTimes += $t1; $newTimes += $t2
    }
    if ($failed -or $baseTimes.Count -eq 0) { Write-Warning "$($b.name): run failed"; continue }

    $bmin = ($baseTimes | Measure-Object -Minimum).Minimum
    $nmin = ($newTimes  | Measure-Object -Minimum).Minimum
    $ratio = if ($bmin -gt 0) { $nmin / $bmin } else { 0 }

    $results += [pscustomobject]@{
        name      = $b.name
        base_us   = [long]$bmin
        new_us    = [long]$nmin
        ratio     = [math]::Round($ratio, 4)
        delta_pct = [math]::Round(($ratio - 1.0) * 100.0, 2)
        identical = $identical
    }
    if (-not $Quiet) {
        $tag = if ($identical) { "  (identical binaries)" } else { "" }
        $sign = if ($ratio -lt 1) { "faster" } else { "slower" }
        "{0,-18} base {1,10} us   new {2,10} us   {3,6:P2} {4}{5}" -f `
            $b.name, [long]$bmin, [long]$nmin, [math]::Abs($ratio - 1.0), $sign, $tag | Write-Host
    }
}

if ($results.Count -eq 0) { throw "no results" }

""
$results | Sort-Object ratio | Format-Table -AutoSize

# Geometric mean of the per-benchmark ratios: the summary statistic that does
# not let one long-running benchmark dominate the verdict.
$logSum = 0.0
foreach ($r in $results) { if ($r.ratio -gt 0) { $logSum += [math]::Log($r.ratio) } }
$geo = [math]::Exp($logSum / $results.Count)
"geomean new/base = {0:N4}  ({1:N2}% {2})" -f $geo, ([math]::Abs($geo - 1) * 100), $(if ($geo -lt 1) { "faster" } else { "slower" }) | Write-Host

$identCount = @($results | Where-Object { $_.identical }).Count
if ($identCount -gt 0) {
    $noise = @($results | Where-Object { $_.identical } | ForEach-Object { [math]::Abs($_.delta_pct) })
    "noise floor from {0} byte-identical pair(s): max |delta| {1:N2}%" -f $identCount, (($noise | Measure-Object -Maximum).Maximum) | Write-Host
}
