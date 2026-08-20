# Incremental, parallel C compile driver for build.bat.
#
# build.bat records every compile unit it would have run into a plan file
# (one "source;object;flags" line each) and hands the whole plan here in one
# call. This script decides which units are actually out of date, compiles
# those in parallel, and reports whether anything changed.
#
# Staleness is decided from three inputs:
#   - the object is missing
#   - the recorded flag string differs from the one the object was built with
#   - the object is older than its source or any header gcc recorded in the
#     -MMD depfile beside it
#
# Exit codes: 0 = up to date or built cleanly, 1 = a unit failed to compile.

param(
  [Parameter(Mandatory = $true)][string]$Plan,
  [string]$CC = "gcc",
  [int]$Jobs = 0,
  [switch]$Prune
)

$ErrorActionPreference = "Stop"

if ($Jobs -le 0) {
  $Jobs = [int]$env:NUMBER_OF_PROCESSORS
  if ($Jobs -le 0) { $Jobs = 4 }
}

if (-not (Test-Path -LiteralPath $Plan)) {
  Write-Host "ccbuild: plan file '$Plan' not found."
  exit 1
}

$units = @()
foreach ($line in Get-Content -LiteralPath $Plan) {
  if ([string]::IsNullOrWhiteSpace($line)) { continue }
  $parts = $line.Split(';', 3)
  if ($parts.Count -lt 3) { continue }
  $units += [pscustomobject]@{
    Source = $parts[0].Trim()
    Object = $parts[1].Trim()
    Flags  = $parts[2].Trim()
  }
}

if ($units.Count -eq 0) {
  Write-Host "ccbuild: empty plan."
  exit 1
}

# One stat per distinct file: headers are shared by dozens of units, and the
# depfiles name them over and over.
$mtimeCache = @{}
function Get-Mtime {
  param([string]$Path)
  $key = $Path.ToLowerInvariant()
  if ($mtimeCache.ContainsKey($key)) { return $mtimeCache[$key] }
  $value = $null
  try {
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    $value = $item.LastWriteTimeUtc
  }
  catch {
    $value = $null
  }
  $mtimeCache[$key] = $value
  return $value
}

# A gcc depfile is a make rule: "target: prereq prereq \<newline> prereq".
# Only the prerequisites matter here.
function Get-DepPrereqs {
  param([string]$DepPath)
  $text = Get-Content -LiteralPath $DepPath -Raw -ErrorAction Stop
  $colon = $text.IndexOf(':')
  # A drive letter is not the rule separator.
  if ($colon -eq 1) { $colon = $text.IndexOf(':', 2) }
  if ($colon -lt 0) { return @() }
  $text = $text.Substring($colon + 1)
  $text = $text -replace '\\\r?\n', ' '
  return ($text -split '\s+' | Where-Object { $_ -ne '' -and $_ -ne '\' })
}

function Test-Stale {
  param($Unit)

  $objMtime = Get-Mtime $Unit.Object
  if ($null -eq $objMtime) { return $true }

  $cmdPath = [System.IO.Path]::ChangeExtension($Unit.Object, ".cmd")
  if (-not (Test-Path -LiteralPath $cmdPath)) { return $true }
  $recorded = (Get-Content -LiteralPath $cmdPath -Raw -ErrorAction SilentlyContinue)
  if ($null -eq $recorded) { return $true }
  if ($recorded.Trim() -ne ("$CC " + $Unit.Flags).Trim()) { return $true }

  $srcMtime = Get-Mtime $Unit.Source
  if ($null -eq $srcMtime) { return $true }
  if ($srcMtime -gt $objMtime) { return $true }

  $depPath = [System.IO.Path]::ChangeExtension($Unit.Object, ".d")
  if (-not (Test-Path -LiteralPath $depPath)) { return $true }
  foreach ($prereq in (Get-DepPrereqs $depPath)) {
    $depMtime = Get-Mtime $prereq
    # A prerequisite that has since been deleted invalidates the object: the
    # include it satisfied now resolves somewhere else, or not at all.
    if ($null -eq $depMtime) { return $true }
    if ($depMtime -gt $objMtime) { return $true }
  }

  return $false
}

$stale = @($units | Where-Object { Test-Stale $_ })

# Objects left behind by a source file that was renamed or deleted would still
# be swept into the archive by its wildcard, so drop anything under obj\ that
# this plan does not claim. Only a full build knows the whole object set.
if ($Prune) {
  $wanted = @{}
  foreach ($u in $units) {
    $wanted[[System.IO.Path]::GetFullPath($u.Object).ToLowerInvariant()] = $true
  }
  foreach ($orphan in (Get-ChildItem -Path "obj" -Filter "*.o" -Recurse -File -ErrorAction SilentlyContinue)) {
    if ($wanted.ContainsKey($orphan.FullName.ToLowerInvariant())) { continue }
    # Intermediates the build writes itself, not compile-unit output.
    if ($orphan.Name -eq "libmtlc-closure.o") { continue }
    Write-Host "  [prune] $($orphan.FullName.Substring((Get-Location).Path.Length + 1))"
    Remove-Item -LiteralPath $orphan.FullName -Force -ErrorAction SilentlyContinue
    foreach ($ext in @(".d", ".cmd")) {
      $side = [System.IO.Path]::ChangeExtension($orphan.FullName, $ext)
      Remove-Item -LiteralPath $side -Force -ErrorAction SilentlyContinue
    }
  }
}

if ($stale.Count -eq 0) {
  Write-Host "Up to date: $($units.Count) objects, nothing to compile."
  exit 0
}

Write-Host "Compiling $($stale.Count) of $($units.Count) objects with $Jobs jobs..."

$running = New-Object System.Collections.ArrayList
$failures = New-Object System.Collections.Generic.List[string]
$index = 0

function Complete-One {
  param($Job)
  $out = $Job.Out.Result
  $err = $Job.Err.Result
  $Job.Proc.WaitForExit()
  $code = $Job.Proc.ExitCode
  $Job.Proc.Dispose()
  $text = ($out + $err).Trim()
  if ($code -ne 0) {
    Write-Host "  [FAIL] $($Job.Unit.Source)"
    if ($text) { Write-Host $text }
    $failures.Add($Job.Unit.Source)
    Remove-Item -LiteralPath $Job.Unit.Object -Force -ErrorAction SilentlyContinue
  }
  else {
    Write-Host "  $($Job.Unit.Source)"
    if ($text) { Write-Host $text }
    Set-Content -LiteralPath ([System.IO.Path]::ChangeExtension($Job.Unit.Object, ".cmd")) `
                -Value ("$CC " + $Job.Unit.Flags) -Encoding ASCII
  }
}

while ($index -lt $stale.Count -or $running.Count -gt 0) {
  while ($index -lt $stale.Count -and $running.Count -lt $Jobs -and $failures.Count -eq 0) {
    $unit = $stale[$index]
    $index++

    $dir = Split-Path -Parent $unit.Object
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
      New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $depPath = [System.IO.Path]::ChangeExtension($unit.Object, ".d")

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $CC
    $psi.Arguments = $unit.Flags + " -MMD -MF `"$depPath`" -c `"$($unit.Source)`" -o `"$($unit.Object)`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    $proc = [System.Diagnostics.Process]::Start($psi)
    # Both pipes must be drained while the process runs, or a compiler that
    # fills one of them blocks forever waiting for a reader.
    [void]$running.Add([pscustomobject]@{
      Unit = $unit
      Proc = $proc
      Out  = $proc.StandardOutput.ReadToEndAsync()
      Err  = $proc.StandardError.ReadToEndAsync()
    })
  }

  if ($running.Count -eq 0) { break }

  $done = $null
  while ($null -eq $done) {
    foreach ($job in $running) {
      if ($job.Proc.HasExited) { $done = $job; break }
    }
    if ($null -eq $done) { Start-Sleep -Milliseconds 15 }
  }
  $running.Remove($done)
  Complete-One $done
}

if ($failures.Count -gt 0) {
  Write-Host ""
  Write-Host "Build failed: $($failures.Count) file(s) did not compile."
  foreach ($f in $failures) { Write-Host "  $f" }
  exit 1
}

exit 2
