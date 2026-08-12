# libmtlc docs fetcher (Windows): download the docs/libmtlc reference set.
#
#   irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/get-libmtlc-docs.ps1 | iex
#
# Downloads the API reference, IR model, type system, pipeline, and internals
# docs (docs/libmtlc/*.md) via the GitHub Contents API and writes them into
# .\libmtlc\docs (or .\libmtlc-docs if no .\libmtlc folder exists yet, e.g. you
# haven't run get-libmtlc.ps1). One API call, no git required.
#
# Overrides (set before piping to iex, or use the script form with params):
#   $env:LIBMTLC_VERSION   a specific tag/branch/commit (default: latest release tag, else main)
#   $env:LIBMTLC_DOCS_DIR  where to write the docs (default: .\libmtlc\docs or .\libmtlc-docs)
[CmdletBinding()]
param(
  [string]$Version = $env:LIBMTLC_VERSION,
  [string]$Dir     = $env:LIBMTLC_DOCS_DIR
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$repo = "The-Mettle-Project/Mettle"

function Say($m)  { Write-Host $m -ForegroundColor Blue }
function Ok($m)   { Write-Host "ok $m" -ForegroundColor Green }

if (-not $Version) {
  Say "Finding the latest release..."
  try {
    $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" `
      -Headers @{ "User-Agent" = "get-libmtlc-docs" }
    $Version = $rel.tag_name
  } catch {}
  if (-not $Version) {
    Write-Host "no published release found; using main" -ForegroundColor Yellow
    $Version = "main"
  }
}

if (-not $Dir) {
  $Dir = if (Test-Path ".\libmtlc") { ".\libmtlc\docs" } else { ".\libmtlc-docs" }
}

Write-Host "Fetching libmtlc docs ($Version)" -ForegroundColor White
$uri = "https://api.github.com/repos/$repo/contents/docs/libmtlc?ref=$Version"
Say "Listing $uri"

try {
  $items = Invoke-RestMethod $uri -Headers @{ "User-Agent" = "get-libmtlc-docs" }
} catch {
  throw "could not list docs/libmtlc at ref '$Version'. Does that tag/branch exist? See https://github.com/$repo."
}

New-Item -ItemType Directory -Force $Dir | Out-Null

$count = 0
foreach ($item in $items | Where-Object { $_.type -eq "file" -and $_.name -like "*.md" }) {
  $bytes = [Convert]::FromBase64String($item.content -replace "`n", "")
  $out = Join-Path $Dir $item.name
  [IO.File]::WriteAllBytes($out, $bytes)
  Write-Host "  $($item.name)"
  $count++
}

Ok "Wrote $count docs to $Dir"
Write-Host ""
Write-Host "Start here: $Dir\README.md" -ForegroundColor Blue
