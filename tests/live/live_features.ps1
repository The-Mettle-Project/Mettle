param(
    [string]$Compiler = "",
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = "Continue"
$root = Resolve-Path (Join-Path $PSScriptRoot "../..")
Set-Location $root
$mettle = Join-Path $root "bin/mettle.exe"
if ($Compiler) { $mettle = (Resolve-Path $Compiler).Path }
$scratch = Join-Path $root ".tmp/live/features"
$suffix = ".exe"
if ($IsLinux) {
    $scratch = Join-Path $root ".tmp/live/features-linux"
    $suffix = ""
}
New-Item -ItemType Directory -Force $scratch | Out-Null
$failures = @()

$prelude = @'
import "std/io"
import "std/thread"

'@

function Write-Source([string]$path, [string]$text) {
    [System.IO.File]::WriteAllText($path, ($prelude + $text).Replace("`r`n", "`n"))
}

function Invoke-Mettle([string[]]$arguments) {
    $output = & $mettle @arguments 2>&1 | Out-String
    return [pscustomobject]@{ Code = $LASTEXITCODE; Text = $output }
}

function Start-Live([string]$exe, [string]$name) {
    $out = Join-Path $scratch "$name.out"
    $err = Join-Path $scratch "$name.err"
    Remove-Item -Force $out, $err -ErrorAction SilentlyContinue
    return Start-Process -FilePath $exe -WorkingDirectory $scratch -PassThru -NoNewWindow `
        -RedirectStandardOutput $out -RedirectStandardError $err
}

function Read-Text([string]$path) {
    if (-not (Test-Path $path)) { return "" }
    $stream = [System.IO.File]::Open($path, "Open", "Read", "ReadWrite")
    $reader = New-Object System.IO.StreamReader($stream)
    $text = $reader.ReadToEnd()
    $reader.Close()
    return $text
}

function Wait-For([string]$path, [string]$pattern) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ((Read-Text $path) -match $pattern) { return $true }
        Start-Sleep -Milliseconds 50
    }
    return $false
}

function Stop-Live($process) {
    if (-not $process.HasExited) { $process.Kill() }
    $process.WaitForExit()
}

function Get-Consecutive([string]$text, [string]$field) {
    $values = @([regex]::Matches($text, "$field=(-?\d+)") | ForEach-Object { [int64]$_.Groups[1].Value })
    for ($i = 1; $i -lt $values.Count; $i++) {
        if ($values[$i] -ne $values[$i - 1] + 1) { return "$field went from $($values[$i - 1]) to $($values[$i])" }
    }
    if ($values.Count -lt 6) { return "only $($values.Count) values of $field" }
    return ""
}

function Fail([string]$scenario, [string]$reason) {
    $script:failures += "${scenario}: $reason"
}

function Clear-Program([string]$name) {
    Get-ChildItem $scratch -Filter "$name*" -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}

$globalsA = @'
struct Ship {
    x: int32
    y: int32
    fuel: float64
    name: string
}

var fleet: Ship[3];
var flagship: Ship;

fn main() -> int32 {
    fleet[1].x = 0;
    flagship.name = "flag";
    var t: int64 = 0;
    while (t < 2000) {
        quiesce;
        fleet[1].x = fleet[1].x + 1;
        println("old t={t} x={fleet[1].x} name={flagship.name}");
        t = t + 1;
        thread_sleep_ms((uint32)15);
    }
    return 0;
}
'@
$globalsB = $globalsA.Replace(@'
struct Ship {
    x: int32
    y: int32
    fuel: float64
    name: string
}
'@, @'
struct Ship {
    name: string
    heading: int32
    fuel: float32
    y: int64
    x: int64
}
'@).Replace('println("old t=', 'println("new t=')

$name = "globals"
Clear-Program $name
$source = Join-Path $scratch "$name.mettle"
$exe = Join-Path $scratch "$name$suffix"
Write-Source $source $globalsA
$built = Invoke-Mettle @("--build", "--live", $source, "-o", $exe)
if ($built.Code -ne 0) {
    Fail $name "build failed: $($built.Text)"
} else {
    $before = (Get-Content "$exe.live" | Where-Object { $_ -match " fleet$" }) -join ""
    $process = Start-Live $exe $name
    $out = Join-Path $scratch "$name.out"
    $err = Join-Path $scratch "$name.err"
    if (-not (Wait-For $out "old t=3 ")) { Fail $name "the program did not start" }
    Write-Source $source $globalsB
    $patch = Invoke-Mettle @("--live-patch", $exe, $source)
    if ($patch.Text -notmatch "2 globals converted to a new layout") { Fail $name "patch said: $($patch.Text)" }
    if (-not (Wait-For $out "new t=\d+ ")) { Fail $name "main never moved to the new code" }
    $after = (Get-Content "$exe.live" | Where-Object { $_ -match " fleet$" }) -join ""
    if ($before -ne $after) { Fail $name "fleet moved from '$before' to '$after'" }
    $undo = Invoke-Mettle @("--live-undo", $exe)
    if ($undo.Text -notmatch "undoes patch 1") { Fail $name "undo said: $($undo.Text)" }
    if (-not (Wait-For $out "new t=\d+ [^\n]*\n(.*\n)*old t=")) { Fail $name "undo never returned to the old code" }
    Start-Sleep -Milliseconds 200
    Stop-Live $process
    $text = Read-Text $out
    $gap = Get-Consecutive $text "x"
    if ($gap) { Fail $name $gap }
    if ($text -match "name=(?!flag)") { Fail $name "a name lost its value" }
}

$heapA = @'
struct Ship {
    x: int32
    y: int32
    fuel: float64
    name: string
}

fn main() -> int32 {
    var fleet: Ship[] = new Ship[3];
    var flagship: Ship* = new Ship;
    flagship->name = "flag";
    var t: int64 = 0;
    while (t < 2000) {
        quiesce;
        fleet[2].x = fleet[2].x + 1;
        flagship->y = flagship->y + 1;
        println("old t={t} x={fleet[2].x} y={flagship->y} name={flagship->name}");
        t = t + 1;
        thread_sleep_ms((uint32)15);
    }
    return 0;
}
'@
$heapB = $heapA.Replace(@'
struct Ship {
    x: int32
    y: int32
    fuel: float64
    name: string
}
'@, @'
struct Ship {
    name: string
    heading: int32
    fuel: float32
    y: int64
    x: int64
}
'@).Replace('println("old t=', 'println("new t=')

$name = "heap"
Clear-Program $name
$source = Join-Path $scratch "$name.mettle"
$exe = Join-Path $scratch "$name$suffix"
Write-Source $source $heapA
$built = Invoke-Mettle @("--build", "--live", $source, "-o", $exe)
if ($built.Code -ne 0) {
    Fail $name "build failed: $($built.Text)"
} else {
    $process = Start-Live $exe $name
    $out = Join-Path $scratch "$name.out"
    $err = Join-Path $scratch "$name.err"
    if (-not (Wait-For $out "old t=3 ")) { Fail $name "the program did not start" }
    Write-Source $source $heapB
    $patch = Invoke-Mettle @("--live-patch", $exe, $source)
    if ($patch.Text -notmatch "1 heap type converted to a new layout") { Fail $name "patch said: $($patch.Text)" }
    if (-not (Wait-For $err "2 heap objects converted")) { Fail $name "the objects were not converted" }
    if (-not (Wait-For $out "new t=\d+ ")) { Fail $name "main never moved to the new code" }
    $undo = Invoke-Mettle @("--live-undo", $exe)
    if (-not (Wait-For $out "new t=\d+ [^\n]*\n(.*\n)*old t=")) { Fail $name "undo never returned to the old code" }
    Start-Sleep -Milliseconds 200
    Stop-Live $process
    $text = Read-Text $out
    foreach ($field in @("x", "y")) {
        $gap = Get-Consecutive $text $field
        if ($gap) { Fail $name $gap }
    }
    if ($text -match "name=(?!flag)") { Fail $name "a name lost its value" }
}

$crashA = @'
fn step(n: int64) -> int64 {
    return n + 1;
}

fn main() -> int32 {
    var score: int64 = 0;
    var t: int64 = 0;
    while (t < 2000) {
        quiesce;
        score = step(score);
        println("t={t} score={score}");
        t = t + 1;
        thread_sleep_ms((uint32)15);
    }
    return 0;
}
'@
$crashB = $crashA.Replace(@'
fn step(n: int64) -> int64 {
    return n + 1;
}
'@, @'
fn step(n: int64) -> int64 {
    var p: int64* = (int64*)0;
    if (n > 3) {
        *p = n;
    }
    return n + 1;
}
'@)
$crashC = $crashA.Replace("return n + 1;", "return n + 1000;")

$name = "crash"
Clear-Program $name
$source = Join-Path $scratch "$name.mettle"
$exe = Join-Path $scratch "$name$suffix"
Write-Source $source $crashA
$built = Invoke-Mettle @("--build", "--live", $source, "-o", $exe)
if ($built.Code -ne 0) {
    Fail $name "build failed: $($built.Text)"
} else {
    [System.IO.File]::WriteAllText("$exe.live.watched", "1`n")
    $process = Start-Live $exe $name
    $out = Join-Path $scratch "$name.out"
    $err = Join-Path $scratch "$name.err"
    if (-not (Wait-For $out "t=3 ")) { Fail $name "the program did not start" }
    Write-Source $source $crashB
    Invoke-Mettle @("--live-patch", $exe, $source) | Out-Null
    if (-not (Wait-For $err "stopped at the fault")) { Fail $name "the fault did not wait for a fix" }
    Write-Source $source $crashC
    Invoke-Mettle @("--live-patch", $exe, $source) | Out-Null
    if (-not (Wait-For $err "continues from its last quiesce point")) { Fail $name "the program did not continue" }
    if (-not (Wait-For $out "score=\d{4,}\r?\n(.*\n){3}")) { Fail $name "the fixed code did not run" }
    Stop-Live $process
    Remove-Item -Force "$exe.live.watched" -ErrorAction SilentlyContinue
}

$tickA = @'
import "std/live"

fn step(n: int64) -> int64 {
    return n + 1;
}

fn main() -> int32 {
    var v: int64 = 0;
    var t: int64 = 0;
    while (t < 4000) {
        live_tick(t);
        quiesce;
        v = step(v);
        println("t={t} v={v}");
        t = t + 1;
        thread_sleep_ms((uint32)15);
    }
    return 0;
}
'@
$tickB = $tickA.Replace("return n + 1;", "return n + 1000;")

function Get-Tick([string]$text) {
    $found = [regex]::Matches($text, "t=(\d+) ")
    if ($found.Count -eq 0) { return -1 }
    return [int64]$found[$found.Count - 1].Groups[1].Value
}

function Get-Switch([string]$text) {
    $previous = 0
    foreach ($match in [regex]::Matches($text, "t=(\d+) v=(\d+)")) {
        $v = [int64]$match.Groups[2].Value
        if ($v - $previous -ge 1000) { return [int64]$match.Groups[1].Value }
        $previous = $v
    }
    return -1
}

$name = "tick"
Clear-Program $name
$source = Join-Path $scratch "$name.mettle"
$exe = Join-Path $scratch "$name$suffix"
Write-Source $source $tickA
$built = Invoke-Mettle @("--build", "--live", $source, "-o", $exe)
if ($built.Code -ne 0) {
    Fail $name "build failed: $($built.Text)"
} else {
    $first = Start-Live $exe "$name-a"
    if (-not (Wait-For (Join-Path $scratch "$name-a.out") "t=20 ")) { Fail $name "the first instance did not start" }
    $second = Start-Live $exe "$name-b"
    $outA = Join-Path $scratch "$name-a.out"
    $outB = Join-Path $scratch "$name-b.out"
    if (-not (Wait-For $outB "t=3 ")) { Fail $name "the second instance did not start" }
    $at = [Math]::Max((Get-Tick (Read-Text $outA)), (Get-Tick (Read-Text $outB))) + 400
    Write-Source $source $tickB
    $patch = Invoke-Mettle @("--live-patch", $exe, "--live-at-tick", "$at", $source)
    if ($patch.Text -notmatch "at tick $at\)") { Fail $name "patch said: $($patch.Text)" }
    if (-not (Wait-For $outB "t=$($at + 3) ")) { Fail $name "the second instance never reached tick $at" }
    Stop-Live $first
    Stop-Live $second
    foreach ($instance in @($outA, $outB)) {
        $switch = Get-Switch (Read-Text $instance)
        if ($switch -ne $at) { Fail $name "$(Split-Path -Leaf $instance) switched at tick $switch, wanted $at" }
    }
}

foreach ($line in $failures) { "FAIL $line" }
"live features: $(4 - @($failures | ForEach-Object { ($_ -split ':')[0] } | Sort-Object -Unique).Count) of 4 scenarios passed"
exit $failures.Count
