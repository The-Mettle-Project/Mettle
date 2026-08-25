# Which C optimization is the gap made of?
#
# Rebuilds each benchmark's C arm under a series of flag sets and times them
# against one fixed Mettle binary, all interleaved on the same pinned core.
# A flag set that costs clang most of its lead names the optimization Mettle
# is missing; one that costs it nothing rules that optimization out.

param(
    [string]$Compiler = "bin\mettle.exe",
    [string[]]$Benchmark = @(),
    [int[]]$Suite = @(3),
    [int]$Runs = 5,
    [int]$AffinityMask = 0x10
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root

$CompilerPath = [System.IO.Path]::GetFullPath((Join-Path $Root $Compiler))
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

$mettleFlags = @($cfg.defaults.mettle_flags)
$scratch = Join-Path $env:TEMP "mettle-cflags"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$cc = "C:\Program Files\LLVM\bin\clang.exe"
$base = @("--target=x86_64-w64-windows-gnu", "-O3", "-lkernel32")

$variants = [ordered]@{
    "O3"           = @()
    "no-inline"    = @("-fno-inline")
    "no-vec"       = @("-fno-vectorize", "-fno-slp-vectorize")
    "no-unroll"    = @("-fno-unroll-loops")
    "no-tbaa"      = @("-fno-strict-aliasing")
    "frame-ptr"    = @("-fno-omit-frame-pointer")
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
    return $null
}

$rows = @()
foreach ($b in $all) {
    $src = Join-Path $Root ($b.mettle_source -replace "/", "\")
    $csrc = Join-Path $Root ($b.c_source -replace "/", "\")
    $mExe = Join-Path $scratch "$($b.name)_m.exe"
    if (Test-Path $mExe) { Remove-Item -Force $mExe }
    $a = @($mettleFlags) + @($src, "-o", $mExe)
    & $CompilerPath @a 2>&1 | Out-Null
    if (-not (Test-Path $mExe)) { Write-Warning "$($b.name): mettle build failed"; continue }

    $exes = [ordered]@{}
    foreach ($v in $variants.Keys) {
        $e = Join-Path $scratch "$($b.name)_$v.exe"
        if (Test-Path $e) { Remove-Item -Force $e }
        $ca = @($base) + @($variants[$v]) + @("-o", $e, $csrc)
        & $cc @ca 2>&1 | Out-Null
        if (Test-Path $e) { $exes[$v] = $e }
    }

    [void](Measure-Once $mExe)
    $best = @{}
    foreach ($k in $exes.Keys) { [void](Measure-Once $exes[$k]); $best[$k] = [double]::MaxValue }
    $mBest = [double]::MaxValue
    for ($i = 0; $i -lt $Runs; $i++) {
        $t = Measure-Once $mExe
        if ($null -ne $t -and $t -lt $mBest) { $mBest = $t }
        foreach ($k in $exes.Keys) {
            $t = Measure-Once $exes[$k]
            if ($null -ne $t -and $t -lt $best[$k]) { $best[$k] = $t }
        }
    }
    $row = [ordered]@{ name = $b.name; mettle = [long]$mBest }
    foreach ($k in $exes.Keys) { $row[$k] = [math]::Round($mBest / $best[$k], 3) }
    $rows += [pscustomobject]$row
    ($row.Keys | ForEach-Object { "$_=$($row[$_])" }) -join "  " | Write-Host
}
""
$rows | Format-Table -AutoSize
"ratio = mettle / that-C-variant.  A variant whose ratio drops toward 1.0 names the optimization Mettle is missing."
