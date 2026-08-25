# Sweep the inline budgets and report all three axes at once: runtime against
# the C arm, Mettle compile time, and the produced binary's size. Reads the
# budgets through the MTL_INL_* env overrides, so one compiler build covers
# the whole sweep.

param(
    [string[]]$Benchmark = @(),
    [int[]]$Suite = @(3),
    [int]$Runs = 5,
    [int]$AffinityMask = 0x10,
    [string[]]$Config = @("128/2/512", "256/6/512", "384/8/1024", "512/12/1536")
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
$CompilerPath = Join-Path $Root "bin\mettle.exe"

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
$scratch = Join-Path $env:TEMP "mettle-sweep"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$cc = "C:\Program Files\LLVM\bin\clang.exe"
$cFlags = @("--target=x86_64-w64-windows-gnu", "-O3", "-lkernel32")

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

# C arms are built once; they do not depend on the sweep.
$cExes = @{}
foreach ($b in $all) {
    $csrc = Join-Path $Root ($b.c_source -replace "/", "\")
    $e = Join-Path $scratch "$($b.name)_c.exe"
    if (-not (Test-Path $e)) {
        $ca = @($cFlags) + @("-o", $e, $csrc)
        & $cc @ca 2>&1 | Out-Null
    }
    if (Test-Path $e) { $cExes[$b.name] = $e }
}

$report = @()
foreach ($conf in $Config) {
    $parts = $conf -split "/"
    $env:MTL_INL_BODY = $parts[0]
    $env:MTL_INL_CALLS = $parts[1]
    $env:MTL_INL_CALLER = $parts[2]

    foreach ($b in $all) {
        $src = Join-Path $Root ($b.mettle_source -replace "/", "\")
        $mExe = Join-Path $scratch "$($b.name)_m.exe"
        if (Test-Path $mExe) { Remove-Item -Force $mExe }
        $a = @($mettleFlags) + @($src, "-o", $mExe)
        # Compile three times, keep the fastest: compile time is the second axis.
        $best = [double]::MaxValue
        for ($k = 0; $k -lt 3; $k++) {
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            & $CompilerPath @a 2>&1 | Out-Null
            $sw.Stop()
            if ($sw.Elapsed.TotalMilliseconds -lt $best) { $best = $sw.Elapsed.TotalMilliseconds }
        }
        if (-not (Test-Path $mExe)) { Write-Warning "$($b.name) @ $conf : build failed"; continue }
        $bytes = (Get-Item $mExe).Length
        $cExe = $cExes[$b.name]

        [void](Measure-Once $mExe); [void](Measure-Once $cExe)
        $mBest = [double]::MaxValue; $cBest = [double]::MaxValue
        for ($i = 0; $i -lt $Runs; $i++) {
            $t = Measure-Once $mExe; if ($null -ne $t -and $t -lt $mBest) { $mBest = $t }
            $t = Measure-Once $cExe; if ($null -ne $t -and $t -lt $cBest) { $cBest = $t }
        }
        $report += [pscustomobject]@{
            config = $conf
            name   = $b.name
            ratio  = [math]::Round($mBest / $cBest, 3)
            us     = [long]$mBest
            cc_ms  = [math]::Round($best, 1)
            bytes  = $bytes
        }
    }
    $slice = @($report | Where-Object { $_.config -eq $conf })
    $logSum = 0.0
    foreach ($r in $slice) { $logSum += [math]::Log($r.ratio) }
    $geo = [math]::Exp($logSum / $slice.Count)
    $ccSum = ($slice | Measure-Object -Property cc_ms -Sum).Sum
    $byteSum = ($slice | Measure-Object -Property bytes -Sum).Sum
    "{0,-14} geomean {1:N4}   compile {2,7:N1} ms total   size {3,8} bytes total" -f $conf, $geo, $ccSum, $byteSum | Write-Host
}
Remove-Item Env:\MTL_INL_BODY, Env:\MTL_INL_CALLS, Env:\MTL_INL_CALLER -ErrorAction SilentlyContinue
""
$report | Format-Table -AutoSize
