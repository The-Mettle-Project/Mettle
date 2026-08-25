# Interleaved Mettle-vs-C runtime comparison.
#
# Same measurement discipline as ab.ps1 (pinned P-core, High priority,
# alternating arm order, minimum over -Runs iterations) but the two arms are
# the Mettle binary and the C binary for the same benchmark, so the ratio it
# reports is the one the goal is stated in.
#
# Usage:
#   .\tools\benchmark\mc.ps1 -Suite 3
#   .\tools\benchmark\mc.ps1 -Benchmark json_parse -Runs 11 -CC clang

param(
    [string]$Compiler = "bin\mettle.exe",
    [string[]]$Benchmark = @(),
    [int[]]$Suite = @(),
    [int]$Runs = 9,
    [int]$AffinityMask = 0x10,
    [string]$CC = "clang",
    [switch]$SkipBuild,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root

$CompilerPath = [System.IO.Path]::GetFullPath((Join-Path $Root $Compiler))
if (-not (Test-Path $CompilerPath)) { throw "compiler not found: $CompilerPath" }

$cfgRaw = [System.IO.File]::ReadAllText((Join-Path $Root "docs\benchmarks\harness.json"))
$cfg = $cfgRaw.TrimStart([char]0xFEFF) | ConvertFrom-Json
$all = @($cfg.benchmarks | Where-Object { $_.kind -eq "runtime" })

$want = @()
foreach ($b in $Benchmark) { $want += ($b -split ",") | ForEach-Object { $_.Trim() } }
$want = @($want | Where-Object { $_ })
if ($want.Count -gt 0) { $all = @($all | Where-Object { $want -contains $_.name }) }
if ($Suite.Count -gt 0) {
    $all = @($all | Where-Object { $s = if ($null -ne $_.suite) { [int]$_.suite } else { 1 }; $Suite -contains $s })
}
if ($all.Count -eq 0) { throw "no benchmarks selected" }

$mettleFlags = @($cfg.defaults.mettle_flags)
$cFlags = @($cfg.defaults.c_flags)
$scratch = Join-Path $env:TEMP "mettle-mc"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

$ccPath = $CC
if ($CC -eq "clang") {
    $ccPath = "C:\Program Files\LLVM\bin\clang.exe"
    if (-not (Test-Path $ccPath)) { $ccPath = "clang" }
    $cFlags = @("--target=x86_64-w64-windows-gnu") + $cFlags
}

function Build-Mettle {
    param([string]$Source, [string]$OutExe)
    if (Test-Path $OutExe) { Remove-Item -Force $OutExe }
    $a = @($mettleFlags) + @($Source, "-o", $OutExe)
    $out = & $CompilerPath @a 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $OutExe)) { return @{ ok = $false; log = ($out -join "`n") } }
    return @{ ok = $true; log = "" }
}

function Build-C {
    param([string]$Source, [string]$OutExe)
    if (Test-Path $OutExe) { Remove-Item -Force $OutExe }
    $a = @($cFlags) + @("-o", $OutExe, $Source)
    $out = & $ccPath @a 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $OutExe)) { return @{ ok = $false; log = ($out -join "`n") } }
    return @{ ok = $true; log = "" }
}

function Measure-Once {
    param([string]$Exe)
    $outFile = Join-Path $scratch "run.out"
    $p = Start-Process -FilePath $Exe -PassThru -NoNewWindow `
        -RedirectStandardOutput $outFile -WorkingDirectory (Split-Path -Parent $Exe)
    try {
        $p.ProcessorAffinity = [System.IntPtr]$AffinityMask
        $p.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::High
    } catch { }
    $p.WaitForExit()
    $text = Get-Content $outFile -Raw
    if ($text -match "Time:\s*(\d+)\s*us") { return [double]$Matches[1] }
    if ($text -match "Time:\s*([\d.]+)\s*ms") { return [double]$Matches[1] * 1000.0 }
    return $null
}

$results = @()
foreach ($b in $all) {
    $src = Join-Path $Root ($b.mettle_source -replace "/", "\")
    $csrc = Join-Path $Root ($b.c_source -replace "/", "\")
    if (-not (Test-Path $src)) { Write-Warning "$($b.name): missing mettle source"; continue }
    if (-not (Test-Path $csrc)) { Write-Warning "$($b.name): missing c source"; continue }

    $mExe = Join-Path $scratch "$($b.name)_m.exe"
    $cExe = Join-Path $scratch "$($b.name)_c.exe"

    if (-not $SkipBuild -or -not (Test-Path $mExe)) {
        $mb = Build-Mettle $src $mExe
        if (-not $mb.ok) { Write-Warning "$($b.name): mettle build failed`n$($mb.log)"; continue }
    }
    if (-not $SkipBuild -or -not (Test-Path $cExe)) {
        $cb = Build-C $csrc $cExe
        if (-not $cb.ok) { Write-Warning "$($b.name): c build failed`n$($cb.log)"; continue }
    }

    [void](Measure-Once $mExe)
    [void](Measure-Once $cExe)

    $mTimes = @(); $cTimes = @()
    $failed = $false
    for ($i = 0; $i -lt $Runs; $i++) {
        if ($i % 2 -eq 0) {
            $t1 = Measure-Once $cExe; $t2 = Measure-Once $mExe
        } else {
            $t2 = Measure-Once $mExe;  $t1 = Measure-Once $cExe
        }
        if ($null -eq $t1 -or $null -eq $t2) { $failed = $true; break }
        $cTimes += $t1; $mTimes += $t2
    }
    if ($failed -or $mTimes.Count -eq 0) { Write-Warning "$($b.name): run failed"; continue }

    $cmin = ($cTimes | Measure-Object -Minimum).Minimum
    $mmin = ($mTimes | Measure-Object -Minimum).Minimum
    $ratio = if ($cmin -gt 0) { $mmin / $cmin } else { 0 }

    $results += [pscustomobject]@{
        name     = $b.name
        c_us     = [long]$cmin
        mettle_us = [long]$mmin
        ratio    = [math]::Round($ratio, 4)
        bytes    = (Get-Item $mExe).Length
    }
    if (-not $Quiet) {
        "{0,-16} c {1,9} us   mettle {2,9} us   {3,6:N3}x" -f $b.name, [long]$cmin, [long]$mmin, $ratio | Write-Host
    }
}

if ($results.Count -eq 0) { throw "no results" }
""
$results | Sort-Object ratio | Format-Table -AutoSize
$logSum = 0.0
foreach ($r in $results) { if ($r.ratio -gt 0) { $logSum += [math]::Log($r.ratio) } }
$geo = [math]::Exp($logSum / $results.Count)
"geomean mettle/c = {0:N4}" -f $geo | Write-Host
