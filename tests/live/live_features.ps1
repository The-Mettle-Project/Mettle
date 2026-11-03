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

$layoutsA = @'
import "std/layout"

struct Ship {
    x: int32
    fuel: float64
    kind: uint8
    next: Ship*
}

var fleet: Ship[3];
var flagship: Ship;

fn main() -> int32 {
    fleet[1].x = 0;
    fleet[2].fuel = 2.5;
    fleet[0].kind = 5;
    flagship.next = &flagship;
    flagship.kind = 3;
    var t: int64 = 0;
    while (t < 2000) {
        quiesce;
        fleet[1].x = fleet[1].x + 1;
        println("old t={t} x={fleet[1].x} fuel={fleet[2].fuel} kind={fleet[0].kind} self={flagship.next == &flagship} tag={flagship.kind}");
        t = t + 1;
        thread_sleep_ms((uint32)15);
    }
    return 0;
}
'@
$layoutsB = $layoutsA.Replace('struct Ship {', '@tagged(next, kind, 3) struct Tagged {').Replace('println("old t=', 'println("new t=')
$layoutsB = $layoutsB.Replace('var fleet: Ship[3];', @'
@soa struct Column {
    x: int32
    fuel: float64
    kind: uint8
}

var fleet: Column[3];
'@).Replace('var flagship: Ship;', 'var flagship: Tagged;').Replace('next: Ship*', 'next: Tagged*')

$name = "layouts"
Clear-Program $name
$source = Join-Path $scratch "$name.mettle"
$exe = Join-Path $scratch "$name$suffix"
Write-Source $source $layoutsA
$built = Invoke-Mettle @("--build", "--live", $source, "-o", $exe)
if ($built.Code -ne 0) {
    Fail $name "build failed: $($built.Text)"
} else {
    $process = Start-Live $exe $name
    $out = Join-Path $scratch "$name.out"
    if (-not (Wait-For $out "old t=3 ")) { Fail $name "the program did not start" }
    Write-Source $source $layoutsB
    $patch = Invoke-Mettle @("--live-patch", $exe, $source)
    if ($patch.Text -notmatch "2 globals converted to a new layout") { Fail $name "patch said: $($patch.Text)" }
    if (-not (Wait-For $out "new t=\d+ ")) { Fail $name "main never moved to the new code" }
    $undo = Invoke-Mettle @("--live-undo", $exe)
    if ($undo.Text -notmatch "undoes patch 1") { Fail $name "undo said: $($undo.Text)" }
    if (-not (Wait-For $out "new t=\d+ [^\n]*\n(.*\n)*old t=")) { Fail $name "undo never returned to the old code" }
    Start-Sleep -Milliseconds 200
    Stop-Live $process
    $text = Read-Text $out
    $gap = Get-Consecutive $text "x"
    if ($gap) { Fail $name $gap }
    if ($text -match "fuel=(?!2\.5)") { Fail $name "a column lost its value: $text" }
    if ($text -match "kind=(?!5)") { Fail $name "a narrow column lost its value" }
    if ($text -match "self=false") { Fail $name "a packed pointer lost its value" }
    if ($text -match "tag=(?!3)") { Fail $name "a packed tag lost its value" }
}

function Test-HeapLayout([string]$name, [string]$before, [string]$after, [string]$objects, [string[]]$fields, [string]$keep) {
    Clear-Program $name
    $source = Join-Path $scratch "$name.mettle"
    $exe = Join-Path $scratch "$name$suffix"
    Write-Source $source $before
    $built = Invoke-Mettle @("--build", "--live", $source, "-o", $exe)
    if ($built.Code -ne 0) {
        Fail $name "build failed: $($built.Text)"
        return
    }
    $process = Start-Live $exe $name
    $out = Join-Path $scratch "$name.out"
    $err = Join-Path $scratch "$name.err"
    if (-not (Wait-For $out "old t=3 ")) { Fail $name "the program did not start: $(Read-Text $err)" }
    Write-Source $source $after
    $patch = Invoke-Mettle @("--live-patch", $exe, $source)
    if ($patch.Text -notmatch "heap types? converted to a new layout") { Fail $name "patch said: $($patch.Text)" }
    if (-not (Wait-For $err "$objects heap objects? converted")) { Fail $name "the objects were not converted: $(Read-Text $err)" }
    if (-not (Wait-For $out "new t=\d+ ")) { Fail $name "main never moved to the new code" }
    Start-Sleep -Milliseconds 300
    $undo = Invoke-Mettle @("--live-undo", $exe)
    if (-not (Wait-For $out "new t=\d+ [^\n]*\n(.*\n)*old t=")) { Fail $name "undo never returned to the old code" }
    Start-Sleep -Milliseconds 300
    Stop-Live $process
    $text = Read-Text $out
    foreach ($field in $fields) {
        $gap = Get-Consecutive $text $field
        if ($gap) { Fail $name $gap }
    }
    if ($keep -and ($text -split "`n" | Where-Object { $_ -match " t=\d+ " -and $_ -notmatch $keep })) {
        Fail $name "a line lost the value '$keep'"
    }
}

$columnsHeapA = @'
import "std/layout"

@soa struct Ship {
    x: int32
    fuel: float64
    id: int64
}

fn main() -> int32 {
    var fleet: Ship[] = new Ship[4];
    for i in 0..4 {
        fleet[i].id = i * 10 + 7;
        fleet[i].fuel = 1.5;
    }
    var t: int64 = 0;
    while (t < 2000) {
        quiesce;
        fleet[1].x = fleet[1].x + 1;
        println("old t={t} x={fleet[1].x} id={fleet[3].id} fuel={(int64)(fleet[2].fuel * 2.0)}");
        t = t + 1;
        thread_sleep_ms((uint32)15);
    }
    return 0;
}
'@
$columnsHeapB = $columnsHeapA.Replace('fuel: float64', "extra: int32`n    fuel: float64").Replace('println("old t=', 'println("new t=')
Test-HeapLayout "columns_heap" $columnsHeapA $columnsHeapB "1" @("x") "id=37 fuel=3"

$structsToColumnsA = @'
import "std/layout"

struct Ship {
    x: int32
    fuel: float64
    id: int64
    name: string
}

fn main() -> int32 {
    var fleet: Ship[] = new Ship[5];
    var big: Ship[] = new Ship[4000];
    for i in 0..5 {
        fleet[i].id = i * 10 + 7;
        fleet[i].fuel = 1.5;
        fleet[i].name = "ship";
    }
    big[3999].id = 77;
    var t: int64 = 0;
    while (t < 2000) {
        quiesce;
        fleet[1].x = fleet[1].x + 1;
        big[3999].x = big[3999].x + 1;
        println("old t={t} x={fleet[1].x} y={big[3999].x} id={fleet[4].id} big={big[3999].id} name={fleet[2].name}");
        t = t + 1;
        thread_sleep_ms((uint32)15);
    }
    return 0;
}
'@
$structsToColumnsB = $structsToColumnsA.Replace('struct Ship {', '@soa struct Ship {').Replace('println("old t=', 'println("new t=')
Test-HeapLayout "structs_to_columns" $structsToColumnsA $structsToColumnsB "2" @("x", "y") "id=47 big=77 name=ship"

$columnsToStructsB = $structsToColumnsA.Replace('println("old t=', 'println("new t=')
$columnsToStructsA = $structsToColumnsA.Replace('struct Ship {', '@soa struct Ship {')
Test-HeapLayout "columns_to_structs" $columnsToStructsA $columnsToStructsB "2" @("x", "y") "id=47 big=77 name=ship"

$soaToHotColdA = @'
import "std/layout"

@soa struct Ship {
    x: int32
    fuel: float64
    id: int64
}

fn main() -> int32 {
    var fleet: Ship[] = new Ship[6];
    for i in 0..6 {
        fleet[i].id = i + 100;
    }
    var t: int64 = 0;
    while (t < 2000) {
        quiesce;
        fleet[5].x = fleet[5].x + 1;
        println("old t={t} x={fleet[5].x} id={fleet[5].id}");
        t = t + 1;
        thread_sleep_ms((uint32)15);
    }
    return 0;
}
'@
$soaToHotColdB = $soaToHotColdA.Replace('@soa struct', '@hot_cold(x, id) struct').Replace('println("old t=', 'println("new t=')
Test-HeapLayout "soa_to_hot_cold" $soaToHotColdA $soaToHotColdB "1" @("x") "id=105"

$linkedA = @'
import "std/layout"

struct Node {
    key: int64
    next: Node*
    note: float64
}

fn push(head: Node*, key: int64) -> Node* {
    var n: Node* = new Node;
    n.key = key;
    n.next = head;
    n.note = 0.5;
    return n;
}

fn total(head: Node*) -> int64 {
    var sum: int64 = 0;
    var walk: Node* = head;
    while (walk != 0) {
        sum = sum + walk.key;
        walk = walk.next;
    }
    return sum;
}

fn main() -> int32 {
    var pool: Node[] = new Node[4];
    var head: Node* = 0;
    head = push(head, 1);
    head = push(head, 2);
    pool[0].key = 100;
    pool[1].key = 200;
    pool[1].next = &pool[0];
    pool[2].key = 300;
    pool[2].next = &pool[1];
    pool[0].next = head;
    var t: int64 = 0;
    var entry: Node* = &pool[2];
    while (t < 2000) {
        quiesce;
        entry.note = entry.note + 1.0;
        println("old t={t} x={(int64)entry.note} sum={total(entry)}");
        t = t + 1;
        thread_sleep_ms((uint32)15);
    }
    return 0;
}
'@
$linkedB = $linkedA.Replace('struct Node {', '@hot_cold(key, next) struct Node {').Replace('println("old t=', 'println("new t=')
Test-HeapLayout "nodes_to_references" $linkedA $linkedB "3" @("x") "sum=603"
Test-HeapLayout "references_to_nodes" $linkedB.Replace('println("new t=', 'println("old t=') $linkedA.Replace('println("old t=', 'println("new t=') "3" @("x") "sum=603"

$ownAllocatorA = @'
import "std/layout"

fn take(bytes: int64, align: int64) -> rawptr {
    return malloc(bytes);
}

fn give(p: rawptr) {
    free(p);
}

@soa @allocate_with(take, give) struct Ship {
    x: int32
    fuel: float64
}

fn main() -> int32 {
    var fleet: Ship[] = new Ship[4];
    var t: int64 = 0;
    while (t < 2000) {
        quiesce;
        fleet[1].x = fleet[1].x + 1;
        println("old t={t}");
        t = t + 1;
        thread_sleep_ms((uint32)15);
    }
    return 0;
}
'@
$ownAllocatorB = $ownAllocatorA.Replace('fuel: float64', "fuel: float64`n    extra: int32")

$name = "columns_own_allocator"
Clear-Program $name
$source = Join-Path $scratch "$name.mettle"
$exe = Join-Path $scratch "$name$suffix"
Write-Source $source $ownAllocatorA
$built = Invoke-Mettle @("--build", "--live", $source, "-o", $exe)
if ($built.Code -ne 0) {
    Fail $name "build failed: $($built.Text)"
} else {
    $process = Start-Live $exe $name
    $out = Join-Path $scratch "$name.out"
    if (-not (Wait-For $out "old t=3")) { Fail $name "the program did not start" }
    Write-Source $source $ownAllocatorB
    $patch = Invoke-Mettle @("--live-patch", $exe, $source)
    if ($patch.Code -eq 0 -or $patch.Text -notmatch "its own allocator") { Fail $name "the patch was not refused: $($patch.Text)" }
    Stop-Live $process
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
    tag: string
    speed: float64
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
    if (-not (Wait-For $err "main continues from its last quiesce point")) { Fail $name "the program did not continue" }
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

$heldA = $tickA.Replace(@'
fn step(n: int64) -> int64 {
    return n + 1;
}
'@, @'
fn step(n: int64) -> int64 {
    var p: int64* = (int64*)0;
    if (n == 60) {
        *p = n;
    }
    return n + 1;
}
'@)
$heldB = $heldA.Replace("return n + 1;", "return n + 2;")
$heldC = $tickA.Replace("return n + 1;", "return n + 1000;")

$name = "held"
Clear-Program $name
$source = Join-Path $scratch "$name.mettle"
$exe = Join-Path $scratch "$name$suffix"
Write-Source $source $heldA
$built = Invoke-Mettle @("--build", "--live", $source, "-o", $exe)
if ($built.Code -ne 0) {
    Fail $name "build failed: $($built.Text)"
} else {
    [System.IO.File]::WriteAllText("$exe.live.watched", "1`n")
    $process = Start-Live $exe $name
    $out = Join-Path $scratch "$name.out"
    $err = Join-Path $scratch "$name.err"
    if (-not (Wait-For $out "t=3 ")) { Fail $name "the program did not start" }
    Write-Source $source $heldB
    $patch = Invoke-Mettle @("--live-patch", $exe, "--live-at-tick", "1000000", $source)
    if ($patch.Text -notmatch "at tick 1000000\)") { Fail $name "patch said: $($patch.Text)" }
    if (-not (Wait-For $err "stopped at the fault")) { Fail $name "the old code did not fault" }
    Start-Sleep -Milliseconds 500
    if ((Read-Text $err) -match "patch 1:") { Fail $name "the held patch applied before a fix arrived" }
    Write-Source $source $heldC
    Invoke-Mettle @("--live-patch", $exe, $source) | Out-Null
    if (-not (Wait-For $err "main continues from its last quiesce point")) { Fail $name "the fix stayed queued behind the held patch" }
    if (-not (Wait-For $out "v=\d{4,}\r?\n(.*\n){3}")) { Fail $name "the fixed code did not run" }
    Stop-Live $process
    Remove-Item -Force "$exe.live.watched" -ErrorAction SilentlyContinue
}

foreach ($line in $failures) { "FAIL $line" }
$scenarios = @(Select-String -Path $PSCommandPath -Pattern '^(\$name = "|Test-HeapLayout ")').Count
"live features: $($scenarios - @($failures | ForEach-Object { ($_ -split ':')[0] } | Sort-Object -Unique).Count) of $scenarios scenarios passed"
exit $failures.Count
