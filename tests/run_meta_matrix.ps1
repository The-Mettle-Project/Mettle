param(
    [string]$Compiler = "bin/mettle.exe",
    [string[]]$Cases = @()
)

$ErrorActionPreference = "Continue"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root
$scratch = Join-Path $root ".tmp/meta_matrix"
New-Item -ItemType Directory -Force $scratch | Out-Null

$Cases = @($Cases | ForEach-Object { $_ -split "," })
if ($Cases.Count -eq 0) {
    $Cases = (Get-ChildItem tests -Filter "test_layout_*.mettle").BaseName +
             (Get-ChildItem tests -Filter "test_meta_*.mettle").BaseName +
             (Get-ChildItem tests -Filter "test_pass_*.mettle").BaseName +
             (Get-ChildItem tests -Filter "test_regalloc_from_*.mettle").BaseName +
             (Get-ChildItem tests -Filter "test_element_*.mettle").BaseName
    $Cases = @($Cases | Where-Object { $_ })
}

$modes = @(
    @{ Name = "none"; Args = @() },
    @{ Name = "safe"; Args = @("--safe") },
    @{ Name = "verify"; Args = @("--verify") },
    @{ Name = "release_verify"; Args = @("--release", "--verify") },
    @{ Name = "check_effects"; Args = @("--check-effects") },
    @{ Name = "debug_info"; Args = @("-g") },
    @{ Name = "release_safe"; Args = @("--release", "--safe") },
    @{ Name = "check_proofs"; Args = @("--check-proofs") },
    @{ Name = "live"; Args = @("--live") }
)

$passed = 0
$failed = @()
foreach ($case in $Cases) {
    foreach ($mode in $modes) {
        $name = "$case.$($mode.Name)"
        $exe = Join-Path $scratch "$name.exe"
        $flags = $mode.Args
        if ($case -eq "test_meta_select") { $env:METTLE_IF_CONVERT = "1" }
        $built = & cmd /c "`"$Compiler`" --build $flags tests/$case.mettle -o `"$exe`" 2>&1" | Out-String
        $buildCode = $LASTEXITCODE
        if ($case -eq "test_meta_select") { Remove-Item Env:METTLE_IF_CONVERT }
        if ($buildCode -ne 0) {
            $failed += "$name build: $built"
            continue
        }
        $out = & cmd /c "`"$exe`" 2>&1" | Out-String
        if ($LASTEXITCODE -ne 0) {
            $failed += "$name run ${LASTEXITCODE}: $out"
            continue
        }
        $passed++
    }
}

"matrix: $passed passed, $($failed.Count) failed"
foreach ($line in $failed) { $line }
if ($failed.Count -gt 0) { exit 1 }
