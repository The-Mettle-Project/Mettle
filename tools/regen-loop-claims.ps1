# Regenerate tests/loop_claims.baseline from tests/loop_rot_corpus.mettle.
#
# The baseline records, per corpus kernel, the loop's dataflow fingerprint and
# whether a recognizer claimed it. The gate in run_tests.ps1 fails when a claim
# regresses from taken to untaken; the fingerprint is what distinguishes a
# recognizer that stopped matching (fingerprint unchanged) from one whose input
# changed upstream (fingerprint moved).
#
# Run this after deliberately changing the corpus or after a change that
# legitimately alters which loops are claimed, and READ THE DIFF before
# committing it. A regenerated baseline that quietly turns 1 into 0 is the
# exact regression the gate exists to report.

param([string]$CompilerPath = "")

# Continue, not Stop: the fingerprint lines arrive on stderr, and Windows
# PowerShell wraps each native stderr line in an ErrorRecord, which Stop would
# treat as a failure of the compiler itself.
$ErrorActionPreference = "Continue"
$onWindows = if ($null -eq $IsWindows) { $true } else { [bool]$IsWindows }
if (-not $CompilerPath) {
  $CompilerPath = if ($onWindows) { ".\bin\mettle.exe" } else { "./bin/mettle" }
}

$corpus = "tests/loop_rot_corpus.mettle"
$baseline = "tests/loop_claims.baseline"
$tmpExe = Join-Path ([System.IO.Path]::GetTempPath()) "loop_claims_regen.exe"

$prev = $env:METTLE_LOOP_FINGERPRINT
$env:METTLE_LOOP_FINGERPRINT = "1"
$output = & $CompilerPath --release --build $corpus -o $tmpExe 2>&1 | Out-String
if ($null -eq $prev) { Remove-Item Env:\METTLE_LOOP_FINGERPRINT -ErrorAction SilentlyContinue }
else { $env:METTLE_LOOP_FINGERPRINT = $prev }

$rows = @()
foreach ($line in ($output -split "`r?`n")) {
  if ($line -match '^\[loop-fp\] function=(\S+) loop=\S+ fp=(\S+) claimed=(\d)') {
    if ($Matches[1] -like 'rc_*') {
      $rows += [pscustomobject]@{ Fn = $Matches[1]; Fp = $Matches[2]; Claimed = $Matches[3] }
    }
  }
}

if ($rows.Count -eq 0) {
  throw "No corpus loops reported. Is METTLE_LOOP_FINGERPRINT wired up?"
}

$lines = @(
  "# Recognizer-claim baseline. Regenerate with tools/regen-loop-claims.ps1.",
  "# Columns: kernel  fingerprint  claimed(1=a recognizer took the loop)",
  "# A claim regressing 1 -> 0 fails the build; read every diff here."
)
foreach ($r in ($rows | Sort-Object Fn)) {
  $lines += ("{0} {1} {2}" -f $r.Fn, $r.Fp, $r.Claimed)
}
Set-Content -Encoding ascii -Path $baseline -Value $lines

$taken = ($rows | Where-Object { $_.Claimed -eq '1' }).Count
Write-Host ("Wrote {0}: {1} kernels, {2} claimed, {3} not" -f $baseline, $rows.Count, $taken, ($rows.Count - $taken))
