# Fetch the libmtlc backend that Mettle compiles against (Windows).
#
#   .\get-libmtlc.ps1
#
# Mettle is a frontend: it lexes, parses, type-checks and lowers .mettle source
# into libmtlc's IR. Everything after that -- optimization, code generation,
# linking -- is libmtlc (https://github.com/The-Mettle-Project/libmtlc). This
# script downloads the libmtlc source at the revision pinned in libmtlc.version,
# unpacks it into .\libmtlc, and builds the static archive the driver links.
#
# The download is a source checkout, not a release bundle: the driver uses the
# backend's own headers, so headers and archive must come from one revision. A
# prebuilt release ships only the public mtlc\ API, which is the surface for a
# foreign frontend rather than for Mettle's own driver.
#
# Overrides:
#   -Version <rev>   a tag, branch or commit SHA instead of libmtlc.version
#                    (or set $env:LIBMTLC_VERSION)
#   -Dir <path>      where to unpack (default .\libmtlc, or $env:LIBMTLC_DIR)
#   -SkipBuild       download only; do not build the archive
#   -Force           re-download even if the pinned revision is already present
[CmdletBinding()]
param(
  [string]$Version = $env:LIBMTLC_VERSION,
  [string]$Dir     = $(if ($env:LIBMTLC_DIR) { $env:LIBMTLC_DIR } else { ".\libmtlc" }),
  [switch]$SkipBuild,
  [switch]$Force
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$repo = "The-Mettle-Project/libmtlc"
$root = $PSScriptRoot

function Say($m)  { Write-Host $m -ForegroundColor Blue }
function Ok($m)   { Write-Host "ok $m" -ForegroundColor Green }
function Warn($m) { Write-Host "warning: $m" -ForegroundColor Yellow }

# --------------------------------------------------------------------------
# Resolve the pinned revision.
# --------------------------------------------------------------------------
if (-not $Version) {
  $pin = Join-Path $root "libmtlc.version"
  if (Test-Path $pin) {
    $Version = (Get-Content $pin | Where-Object { $_ -notmatch '^\s*(#|$)' } | Select-Object -First 1).Trim()
  }
}
if (-not $Version) { $Version = "main" }

if (-not [IO.Path]::IsPathRooted($Dir)) { $Dir = Join-Path $root $Dir }
$stamp = Join-Path $Dir ".libmtlc-revision"

Write-Host "libmtlc $Version" -ForegroundColor White

# --------------------------------------------------------------------------
# Download, unless this revision is already unpacked or the user pointed us at
# their own checkout. A local checkout is left completely alone -- that is the
# whole point of LIBMTLC_DIR when you are working on both halves at once.
# --------------------------------------------------------------------------
$isLocalCheckout = (Test-Path (Join-Path $Dir ".git")) -and -not (Test-Path $stamp)
if ($isLocalCheckout) {
  Say "$Dir is a git checkout; leaving it untouched"
} elseif ((Test-Path $stamp) -and -not $Force -and
          (Get-Content $stamp -Raw).Trim() -eq $Version) {
  Say "$Dir is already at $Version (use -Force to re-download)"
} else {
  $url = "https://codeload.github.com/$repo/zip/$Version"
  Say "Downloading $url"
  $tmp = Join-Path ([IO.Path]::GetTempPath()) ("libmtlc-" + [Guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory -Force $tmp | Out-Null
  try {
    $zip = Join-Path $tmp "libmtlc.zip"
    try {
      Invoke-WebRequest $url -OutFile $zip -UseBasicParsing `
        -Headers @{ "User-Agent" = "get-libmtlc" }
    } catch {
      throw "download failed. Does $repo have a revision '$Version'? See https://github.com/$repo"
    }

    Say "Unpacking to $Dir"
    Expand-Archive -Path $zip -DestinationPath $tmp -Force
    $unpacked = Get-ChildItem -Directory $tmp | Select-Object -First 1
    if (-not $unpacked) { throw "the archive was empty." }
    foreach ($probe in @("include\mtlc\mtlc.h", "src\ir\ir.h", "Makefile")) {
      if (-not (Test-Path (Join-Path $unpacked.FullName $probe))) {
        throw "the archive is missing $probe (unexpected layout)."
      }
    }

    if (Test-Path $Dir) { Remove-Item -Recurse -Force $Dir }
    $parent = Split-Path -Parent $Dir
    if ($parent -and -not (Test-Path $parent)) { New-Item -ItemType Directory -Force $parent | Out-Null }
    Move-Item $unpacked.FullName $Dir
    Set-Content -Path $stamp -Value $Version -Encoding utf8
    Ok "unpacked libmtlc $Version into $Dir"
  } finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
  }
}

# --------------------------------------------------------------------------
# Build the archive. libmtlc's own build script owns the object list, so the
# backend is described in exactly one place: --backend-only stops after
# archiving bin\mtlc.lib, without building libmtlc's copy of the driver.
# --------------------------------------------------------------------------
if ($SkipBuild) {
  Say "skipping the build (-SkipBuild)"
  exit 0
}

$prebuilt = @("lib\mtlc.lib", "lib\libmtlc.a", "bin\mtlc.lib", "bin\libmtlc.a") |
  ForEach-Object { Join-Path $Dir $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($prebuilt -and -not $Force) {
  Ok "using the archive already present at $prebuilt"
  exit 0
}

$buildBat = Join-Path $Dir "build.bat"
if (-not (Test-Path $buildBat)) { throw "$buildBat not found; cannot build the backend." }

Say "Building the backend archive (this takes a few minutes)"
# cmd does not resolve a bare batch name against the working directory, so pass
# the full path; Push-Location still sets the cwd the script builds in.
function Invoke-LibmtlcBuild([string]$batArgs) {
  Push-Location $Dir
  try {
    & cmd /c "`"$buildBat`" $batArgs"
    return $LASTEXITCODE
  } finally { Pop-Location }
}

# Build the archive with the same compiler the driver will use: mixing gcc and
# clang objects in one link works often enough to be a trap.
$cc = if ($env:CC) { $env:CC } else { "" }

$code = Invoke-LibmtlcBuild "$cc --backend-only".Trim()
if ($code -ne 0) {
  # An older libmtlc has no --backend-only mode; a full build still produces
  # bin\mtlc.lib on its way to the driver, so fall back to that.
  Warn "--backend-only failed (exit $code); trying a full libmtlc build"
  $code = Invoke-LibmtlcBuild "$cc --skip-tests".Trim()
  if ($code -ne 0) { throw "building libmtlc failed (exit $code)." }
}

$lib = Join-Path $Dir "bin\mtlc.lib"
if (-not (Test-Path $lib)) { throw "the build finished but $lib is missing." }
Ok "built $lib"
Write-Host ""
Write-Host "Now build Mettle:"
Write-Host "  .\build.bat" -ForegroundColor White
