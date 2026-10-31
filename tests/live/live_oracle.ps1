param(
    [string]$Pattern = "tests\test_*.mettle",
    [int]$Limit = 0,
    [int]$TimeoutSeconds = 20,
    [switch]$Loops,
    [switch]$Optimize
)

$ErrorActionPreference = "Continue"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
Set-Location $root
$mettle = Join-Path $root "bin\mettle.exe"
$scratch = Join-Path $root ".tmp\live\oracle"
New-Item -ItemType Directory -Force $scratch | Out-Null

function Invoke-Program([string]$exe) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $exe
    $info.WorkingDirectory = $root
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.RedirectStandardInput = $true
    $process = [System.Diagnostics.Process]::Start($info)
    $process.StandardInput.Close()
    $out = $process.StandardOutput.ReadToEndAsync()
    $err = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        return $null
    }
    $process.WaitForExit()
    return [pscustomobject]@{
        Code = $process.ExitCode
        Out = $out.Result
        Err = $err.Result
    }
}

$files = Get-ChildItem $Pattern | Sort-Object Name
if ($Limit -gt 0) { $files = $files | Select-Object -First $Limit }
$pass = 0; $fail = 0; $skip = 0; $kept = 0
$keptNames = @()
$opt = @()
if ($Optimize) { $opt = @("-O") }
$failures = @()

foreach ($file in $files) {
    $source = Get-Content -Raw $file.FullName
    $match = [regex]::Match($source, "fn\s+main\s*\([^)]*\)[^{]*\{")
    if (-not $match.Success) { $skip++; continue }

    $patched = $source.Substring(0, $match.Index + $match.Length) + " quiesce;" + $source.Substring($match.Index + $match.Length)
    if ($Loops) {
        $bodyStart = $match.Index + $match.Length
        $bodyEnd = $source.IndexOf("`n}", $bodyStart)
        if ($bodyEnd -lt 0) { $bodyEnd = $source.Length }
        $body = $source.Substring($bodyStart, $bodyEnd - $bodyStart)
        $looped = [regex]::Replace($body, "(?m)^([ \t]*)(while|for)\b[^{;\r\n]*\{", { param($m) $m.Value + " quiesce;" })
        if ($looped -eq $body) { $skip++; continue }
        $patched = $source.Substring(0, $bodyStart) + $looped + $source.Substring($bodyEnd)
    }

    $sourceDir = Join-Path $scratch "src"
    New-Item -ItemType Directory -Force $sourceDir | Out-Null
    $liveSource = Join-Path $sourceDir $file.Name
    $include = "-I" + $file.DirectoryName
    [System.IO.File]::WriteAllText($liveSource, $source)
    $plainExe = Join-Path $scratch "plain.exe"
    Remove-Item -Force $plainExe -ErrorAction SilentlyContinue
    & $mettle --build @opt $include $liveSource -o $plainExe *> $null
    if ($LASTEXITCODE -ne 0) { $skip++; Remove-Item -Force $liveSource; continue }
    $plain = Invoke-Program $plainExe
    if ($null -eq $plain) { $skip++; Remove-Item -Force $liveSource; continue }

    [System.IO.File]::WriteAllText($liveSource, $patched)
    $liveExe = Join-Path $scratch "live.exe"
    try {
        $diagnostics = Join-Path $scratch "diagnostics.txt"
        & $mettle --build --live @opt $include $liveSource -o $liveExe 2> $diagnostics | Out-Null
        if ($LASTEXITCODE -ne 0) {
            $fail++; $failures += "$($file.Name): live build failed: $(Get-Content -Raw $diagnostics)"; continue
        }
        $same = & $mettle --live-patch $liveExe @opt $include $liveSource 2> $diagnostics | Out-String
        if ($same -notmatch "No function changed") {
            $fail++; $failures += "$($file.Name): identical source produced a patch: $same $(Get-Content -Raw $diagnostics)"; continue
        }
        $all = & $mettle --live-patch $liveExe --live-patch-all @opt $include $liveSource 2> $diagnostics | Out-String
        if ($LASTEXITCODE -ne 0 -or $all -notmatch "(?m)^Patch 1:") {
            $fail++; $failures += "$($file.Name): replace-all patch refused: $all $(Get-Content -Raw $diagnostics)"; continue
        }
        $live = Invoke-Program $liveExe
        if ($null -eq $live) {
            $fail++; $failures += "$($file.Name): patched program timed out"; continue
        }
        if ($live.Err -notmatch "\[live\] patch 1:") {
            if ($Loops) { $skip++; continue }
            $fail++; $failures += "$($file.Name): patch was not applied: $($live.Err)"; continue
        }
        if ($live.Out -ne $plain.Out -or $live.Code -ne $plain.Code) {
            $fail++; $failures += "$($file.Name): output differs (exit $($plain.Code) vs $($live.Code))"; continue
        }
        if ($live.Err -notmatch "\[live\] main moves to its new code") { $kept++; $keptNames += $file.Name }
        $pass++
    } finally {
        Remove-Item -Force $liveSource -ErrorAction SilentlyContinue
    }
}

foreach ($line in $failures) { "FAIL $line" }
foreach ($name in $keptNames) { "KEPT $name" }
"live oracle: $pass passed ($kept kept main's old code), $fail failed, $skip skipped"
if ($fail -gt 0) { exit 1 }
