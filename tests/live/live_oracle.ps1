param(
    [string]$Pattern = "tests\test_*.mettle",
    [int]$Limit = 0,
    [int]$TimeoutSeconds = 20
)

$ErrorActionPreference = "Continue"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
Set-Location $root
$mettle = Join-Path $root "bin\mettle.exe"
$scratch = Join-Path $root ".tmp\live\oracle"
New-Item -ItemType Directory -Force $scratch | Out-Null

function Invoke-Program([string]$exe) {
    $out = Join-Path $scratch "stdout.txt"
    $err = Join-Path $scratch "stderr.txt"
    $process = Start-Process -FilePath $exe -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError $err -WorkingDirectory $root
    $null = $process.Handle
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        return $null
    }
    $process.WaitForExit()
    return [pscustomobject]@{
        Code = $process.ExitCode
        Out = (Get-Content -Raw $out)
        Err = (Get-Content -Raw $err)
    }
}

$files = Get-ChildItem $Pattern | Sort-Object Name
if ($Limit -gt 0) { $files = $files | Select-Object -First $Limit }
$pass = 0; $fail = 0; $skip = 0
$failures = @()

foreach ($file in $files) {
    $source = Get-Content -Raw $file.FullName
    $match = [regex]::Match($source, "fn\s+main\s*\([^)]*\)[^{]*\{")
    if (-not $match.Success) { $skip++; continue }

    $sourceDir = Join-Path $scratch "src"
    New-Item -ItemType Directory -Force $sourceDir | Out-Null
    $liveSource = Join-Path $sourceDir $file.Name
    $include = "-I" + $file.DirectoryName
    [System.IO.File]::WriteAllText($liveSource, $source)
    $plainExe = Join-Path $scratch "plain.exe"
    Remove-Item -Force $plainExe -ErrorAction SilentlyContinue
    & $mettle --build $include $liveSource -o $plainExe *> $null
    if ($LASTEXITCODE -ne 0) { $skip++; Remove-Item -Force $liveSource; continue }
    $plain = Invoke-Program $plainExe
    if ($null -eq $plain) { $skip++; Remove-Item -Force $liveSource; continue }

    $patched = $source.Substring(0, $match.Index + $match.Length) + " quiesce;" + $source.Substring($match.Index + $match.Length)
    [System.IO.File]::WriteAllText($liveSource, $patched)
    $liveExe = Join-Path $scratch "live.exe"
    try {
        $diagnostics = Join-Path $scratch "diagnostics.txt"
        & $mettle --build --live $include $liveSource -o $liveExe 2> $diagnostics | Out-Null
        if ($LASTEXITCODE -ne 0) {
            $fail++; $failures += "$($file.Name): live build failed: $(Get-Content -Raw $diagnostics)"; continue
        }
        $same = & $mettle --live-patch $liveExe $include $liveSource 2> $diagnostics | Out-String
        if ($same -notmatch "No function changed") {
            $fail++; $failures += "$($file.Name): identical source produced a patch: $same $(Get-Content -Raw $diagnostics)"; continue
        }
        $all = & $mettle --live-patch $liveExe --live-patch-all $include $liveSource 2> $diagnostics | Out-String
        if ($LASTEXITCODE -ne 0 -or $all -notmatch "(?m)^Patch 1:") {
            $fail++; $failures += "$($file.Name): replace-all patch refused: $all $(Get-Content -Raw $diagnostics)"; continue
        }
        $live = Invoke-Program $liveExe
        if ($null -eq $live) {
            $fail++; $failures += "$($file.Name): patched program timed out"; continue
        }
        if ($live.Err -notmatch "\[live\] patch 1:") {
            $fail++; $failures += "$($file.Name): patch was not applied: $($live.Err)"; continue
        }
        if ($live.Out -ne $plain.Out -or $live.Code -ne $plain.Code) {
            $fail++; $failures += "$($file.Name): output differs (exit $($plain.Code) vs $($live.Code))"; continue
        }
        $pass++
    } finally {
        Remove-Item -Force $liveSource -ErrorAction SilentlyContinue
    }
}

foreach ($line in $failures) { "FAIL $line" }
"live oracle: $pass passed, $fail failed, $skip skipped"
if ($fail -gt 0) { exit 1 }
