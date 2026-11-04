param(
    [string]$Compiler = "bin\mettle.exe",
    [int]$Runs = 15,
    [int]$BuildRuns = 5,
    [int]$AffinityMask = 0x10,
    [string]$GeneratedMettle = ".tmp\perf\scale\p6400.mettle",
    [string]$GeneratedC = ".tmp\perf\scale\p6400.c"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root

$CompilerPath = (Resolve-Path $Compiler).Path
$Clang = "C:\Program Files\LLVM\bin\clang.exe"
$Scratch = Join-Path $Root ".tmp\suite3_exhibit"
New-Item -ItemType Directory -Force -Path $Scratch | Out-Null

$Harness = ([System.IO.File]::ReadAllText((Join-Path $Root "docs\benchmarks\harness.json")).TrimStart([char]0xFEFF)) | ConvertFrom-Json
$Programs = @($Harness.benchmarks | Where-Object { $_.kind -eq "runtime" -and $_.suite -eq 3 })
$MettleFlags = @($Harness.defaults.mettle_flags)
$O3Flags = @("--target=x86_64-w64-windows-gnu") + @($Harness.defaults.c_flags)
$NativeFlags = $O3Flags + @("-march=native")

function Invoke-Build([string]$Tool, [string[]]$BuildArgs, [string]$Exe) {
    Remove-Item -Force -ErrorAction SilentlyContinue $Exe
    $keep = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $Tool @BuildArgs 2>&1 | Out-String
    $ErrorActionPreference = $keep
    if (-not (Test-Path $Exe)) { throw "build failed for ${Exe}: $out" }
}

function Invoke-Pinned([string]$Exe) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.WorkingDirectory = Split-Path -Parent $Exe
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $p = [System.Diagnostics.Process]::Start($psi)
    try {
        $p.ProcessorAffinity = [IntPtr]$AffinityMask
        $p.PriorityClass = "High"
    } catch { }
    $text = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    if ($text -notmatch "Time:\s*(\d+)\s*us") { throw "no timer in the output of $Exe" }
    return @{ us = [double]$Matches[1]; text = $text }
}

function Get-Answers([string]$Text) {
    $lines = $Text -split "`r?`n" | Where-Object { $_ -notmatch "^Time:|^Per pass" }
    return ($lines -join "`n").Trim()
}

$Rows = @()
foreach ($bench in $Programs) {
    $source = Join-Path $Root ($bench.mettle_source -replace "/", "\")
    $csource = Join-Path $Root ($bench.c_source -replace "/", "\")
    $m = Join-Path $Scratch "$($bench.name)_m.exe"
    $o3 = Join-Path $Scratch "$($bench.name)_o3.exe"
    $native = Join-Path $Scratch "$($bench.name)_native.exe"
    Invoke-Build $CompilerPath ($MettleFlags + @($source, "-o", $m)) $m
    Invoke-Build $Clang ($O3Flags + @("-o", $o3, $csource)) $o3
    Invoke-Build $Clang ($NativeFlags + @("-o", $native, $csource)) $native
    $order = @($m, $o3, $native)
    $best = @{}
    $answer = @{}
    foreach ($exe in $order) { [void](Invoke-Pinned $exe); $best[$exe] = [double]::MaxValue }
    for ($i = 0; $i -lt $Runs; $i++) {
        foreach ($exe in $order) {
            $run = Invoke-Pinned $exe
            if ($run.us -lt $best[$exe]) { $best[$exe] = $run.us }
            $answer[$exe] = Get-Answers $run.text
        }
        [array]::Reverse($order)
    }
    $row = [ordered]@{
        name = $bench.name
        m = [long]$best[$m]
        o3 = [long]$best[$o3]
        native = [long]$best[$native]
        same_o3 = ($answer[$m] -ceq $answer[$o3])
        same_native = ($answer[$m] -ceq $answer[$native])
    }
    $Rows += New-Object PSObject -Property $row
    "{0,-14} mettle {1,7}  o3 {2,7} {3:N3}x {4}  native {5,7} {6:N3}x {7}" -f $row.name, $row.m, $row.o3, ($row.m / $row.o3), $row.same_o3, $row.native, ($row.m / $row.native), $row.same_native
}

$Json = @($Rows | ForEach-Object {
    '  {"name": "' + $_.name + '", "m": ' + $_.m + ', "o3": ' + $_.o3 + ', "native": ' + $_.native +
    ', "same_o3": ' + $_.same_o3.ToString().ToLower() + ', "same_native": ' + $_.same_native.ToString().ToLower() + '}'
}) -join ",`n"
[System.IO.File]::WriteAllText((Join-Path $Root "website\exhibits\suite3.json"), "[`n$Json`n]`n")

if (-not (Test-Path $GeneratedMettle) -or -not (Test-Path $GeneratedC)) {
    "generated build skipped: no $GeneratedMettle or $GeneratedC"
    exit 0
}
$MettleBuild = [double]::MaxValue
$ClangBuild = [double]::MaxValue
$BuildM = Join-Path $Scratch "generated_m.exe"
$BuildC = Join-Path $Scratch "generated_c.exe"
for ($i = 0; $i -lt $BuildRuns; $i++) {
    $t = (Measure-Command { Invoke-Build $CompilerPath ($MettleFlags + @((Resolve-Path $GeneratedMettle).Path, "-o", $BuildM)) $BuildM }).TotalSeconds
    if ($t -lt $MettleBuild) { $MettleBuild = $t }
    $t = (Measure-Command { Invoke-Build $Clang ($O3Flags + @("-o", $BuildC, (Resolve-Path $GeneratedC).Path)) $BuildC }).TotalSeconds
    if ($t -lt $ClangBuild) { $ClangBuild = $t }
}
$Lines = @(Get-Content $GeneratedMettle).Count
$BuildJson = '{"lines": ' + $Lines + ', "mettle_s": ' + $MettleBuild.ToString("F2", [Globalization.CultureInfo]::InvariantCulture) +
    ', "clang_s": ' + $ClangBuild.ToString("F2", [Globalization.CultureInfo]::InvariantCulture) + '}'
[System.IO.File]::WriteAllText((Join-Path $Root "website\exhibits\build_speed.json"), "$BuildJson`n")
"generated build: $BuildJson"
