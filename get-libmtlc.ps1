# libmtlc fetcher (Windows): get ONLY the backend (headers + static library).
#
#   irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/get-libmtlc.ps1 | iex
#
# Downloads the prebuilt libmtlc release for Windows x64 and unpacks it into
# .\libmtlc (include\ + lib\). That folder is everything a frontend links
# against. No compiler driver, no stdlib, no runtime.
#
# Overrides (set before piping to iex, or use the script form with params):
#   $env:LIBMTLC_VERSION   a specific tag (e.g. v0.13.0) instead of latest
#   $env:LIBMTLC_DIR       where to unpack (default: .\libmtlc)
#   $env:LIBMTLC_BASE_URL  a mirror or local test server standing in for GitHub
[CmdletBinding()]
param(
  [string]$Version = $env:LIBMTLC_VERSION,
  [string]$Dir     = $(if ($env:LIBMTLC_DIR) { $env:LIBMTLC_DIR } else { ".\libmtlc" })
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$repo = "The-Mettle-Project/Mettle"
$target = "windows-x64"

function Say($m)  { Write-Host $m -ForegroundColor Blue }
function Ok($m)   { Write-Host "ok $m" -ForegroundColor Green }

if (-not $Version) {
  Say "Finding the latest release..."
  try {
    $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" `
      -Headers @{ "User-Agent" = "get-libmtlc" }
    $Version = $rel.tag_name
  } catch {}
  if (-not $Version) { throw "could not determine the latest release. Set `$env:LIBMTLC_VERSION." }
}

$bundle  = "libmtlc-$Version-$target"
$baseUrl = if ($env:LIBMTLC_BASE_URL) { $env:LIBMTLC_BASE_URL }
           else { "https://github.com/$repo/releases/download/$Version" }
$url = "$baseUrl/$bundle.zip"

Write-Host "Fetching libmtlc $Version for $target" -ForegroundColor White
Say "Downloading $url"

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("libmtlc-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
  $zip = Join-Path $tmp "libmtlc.zip"
  try {
    Invoke-WebRequest $url -OutFile $zip -UseBasicParsing -Headers @{ "User-Agent" = "get-libmtlc" }
  } catch {
    throw "download failed. Does $Version ship a $target libmtlc asset? See https://github.com/$repo/releases."
  }

  Say "Unpacking to $Dir"
  Expand-Archive -Path $zip -DestinationPath $tmp -Force
  $src = Join-Path $tmp "libmtlc"
  if (-not (Test-Path $src)) { $src = $tmp }
  if (-not (Test-Path (Join-Path $src "lib\mtlc.lib"))) {
    throw "archive did not contain lib\mtlc.lib (unexpected layout)."
  }

  if (Test-Path $Dir) { Remove-Item -Recurse -Force $Dir }
  $parent = Split-Path -Parent $Dir
  if ($parent -and -not (Test-Path $parent)) { New-Item -ItemType Directory -Force $parent | Out-Null }
  Copy-Item -Recurse $src $Dir
  Ok "Installed libmtlc to $Dir"
}
finally {
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Link a frontend with:"
Write-Host "  gcc -I$Dir\include app.c $Dir\lib\mtlc.lib -o app.exe -ldbghelp" -ForegroundColor White
Write-Host ""
Write-Host "API reference: https://github.com/$repo/blob/main/docs/libmtlc/api.md" -ForegroundColor Blue
