param(
    [int[]]$Suite = @(3),
    [string[]]$Benchmark = @(),
    [switch]$NoInline,
    [string]$Baseline = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
$objdump = "C:\msys64\mingw64\bin\objdump.exe"
$cc = "C:\Program Files\LLVM\bin\clang.exe"
$scratch = Join-Path $env:TEMP "mettle-codesize"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

$cfg = ([System.IO.File]::ReadAllText((Join-Path $Root "docs\benchmarks\harness.json"))).TrimStart([char]0xFEFF) | ConvertFrom-Json
$all = @($cfg.benchmarks | Where-Object { $_.kind -eq "runtime" })
$want = @()
foreach ($b in $Benchmark) { $want += ($b -split ",") | ForEach-Object { $_.Trim() } }
$want = @($want | Where-Object { $_ })
if ($want.Count -gt 0) { $all = @($all | Where-Object { $want -contains $_.name }) }
if ($Suite.Count -gt 0) {
    $all = @($all | Where-Object { $s = if ($null -ne $_.suite) { [int]$_.suite } else { 1 }; $Suite -contains $s })
}

function Count-Functions {
    param([string]$ObjPath)
    $text = & $objdump -d --no-show-raw-insn $ObjPath
    $counts = [ordered]@{}
    $cur = $null
    foreach ($line in $text) {
        if ($line -match '^[0-9a-f]+ <(.+)>:') { $cur = $Matches[1]; if (-not $counts.Contains($cur)) { $counts[$cur] = 0 }; continue }
        if ($null -ne $cur -and $line -match '^\s+[0-9a-f]+:') { $counts[$cur] = $counts[$cur] + 1 }
    }
    return $counts
}

$hot = @{
    json_parse   = @("skip_ws", "new_node", "scan_string", "scan_number", "match_word", "parse_value", "parse_object", "parse_array", "walk")
    interp_ast   = @("intern", "span_equals", "push_token", "lex", "precedence", "new_node", "peek_kind", "peek_op", "expect_punct", "parse_primary", "parse_expr", "parse_block", "parse_stmt", "parse_program", "eval", "exec")
    word_freq    = @("hash_word", "keys_equal", "table_insert_raw", "table_grow", "table_reset", "count_words", "top_k_hash")
    huffman      = @("heap_less", "heap_push", "heap_pop", "build_tree", "assign_codes", "encode", "decode", "roundtrip")
    lz77         = @("hash3", "compress", "decompress", "roundtrip")
    astar_grid   = @("find_open", "heuristic", "heap_push", "heap_pop", "relax", "astar", "path_hash", "run_queries")
    regex_match  = @("class_set", "class_has", "inst_matches", "match_here", "scan_lines", "run_patterns")
    physics_grid = @("cell_of", "bucket", "resolve", "collide", "integrate", "simulate")
}

$rows = @()
foreach ($b in $all) {
    $src = Join-Path $Root ($b.mettle_source -replace "/", "\")
    $csrc = Join-Path $Root ($b.c_source -replace "/", "\")
    $mObj = Join-Path $scratch "$($b.name)_m.exe"
    $cObj = Join-Path $scratch "$($b.name)_c.o"
    if ($NoInline) { $env:METTLE_SKIP_PASS = "inline_small_functions,inline_self_recursion" } else { $env:METTLE_SKIP_PASS = "" }
    & (Join-Path $Root "bin\mettle.exe") --build --emit-obj --linker internal --release $src -o $mObj 2>&1 | Out-Null
    $env:METTLE_SKIP_PASS = ""
    $cArgs = @("--target=x86_64-w64-windows-gnu", "-O3", "-c", "-o", $cObj, $csrc)
    if ($NoInline) { $cArgs = @("--target=x86_64-w64-windows-gnu", "-O3", "-fno-inline", "-c", "-o", $cObj, $csrc) }
    & $cc @cArgs 2>&1 | Out-Null
    $mObjPath = [System.IO.Path]::ChangeExtension($mObj, ".obj")
    if (-not (Test-Path $mObjPath) -or -not (Test-Path $cObj)) { Write-Warning "$($b.name): build failed"; continue }
    $m = Count-Functions $mObjPath
    $c = Count-Functions $cObj
    $names = if ($hot.ContainsKey($b.name)) { $hot[$b.name] } else { @() }
    $mTot = 0; $cTot = 0
    foreach ($n in $names) {
        $mv = if ($m.Contains($n)) { $m[$n] } else { 0 }
        $cv = if ($c.Contains($n)) { $c[$n] } else { 0 }
        $mTot += $mv; $cTot += $cv
    }
    $rows += [pscustomobject]@{
        name    = $b.name
        mettle  = $mTot
        c       = $cTot
        ratio   = if ($cTot -gt 0) { [math]::Round($mTot / $cTot, 3) } else { 0 }
    }
}
$rows | Format-Table -AutoSize
$mSum = ($rows | Measure-Object -Property mettle -Sum).Sum
$cSum = ($rows | Measure-Object -Property c -Sum).Sum
"hot-function instructions: mettle {0}  c {1}  ratio {2:N3}" -f $mSum, $cSum, ($mSum / $cSum) | Write-Host
