# Stage ONLY the libmtlc backend for embedding in another project.
#
# After .\build.bat has produced bin\mtlc.lib, this copies the two things a
# frontend developer needs (the public headers and the static library) into
# dist\libmtlc\, a self-contained folder to copy elsewhere or zip up. It does
# not touch the Mettle driver, stdlib, or runtime.
#
#   .\tools\dist-libmtlc.ps1                 # -> dist\libmtlc
#   .\tools\dist-libmtlc.ps1 -OutDir C:\pkg  # -> C:\pkg\libmtlc
[CmdletBinding()]
param(
  [string]$OutDir = "dist"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$lib = Join-Path $root "bin\mtlc.lib"
$include = Join-Path $root "include\mtlc"

if (-not (Test-Path $lib)) {
  Write-Error "bin\mtlc.lib not found. Run .\build.bat first."
}
if (-not (Test-Path $include)) {
  Write-Error "include\mtlc not found (run from the repository)."
}

$dest = Join-Path $OutDir "libmtlc"
$destInc = Join-Path $dest "include\mtlc"
$destLib = Join-Path $dest "lib"
Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $destInc | Out-Null
New-Item -ItemType Directory -Force $destLib | Out-Null

Copy-Item (Join-Path $include "*.h") $destInc
Copy-Item $lib $destLib

Write-Host "staged $dest"
Write-Host "  headers: $destInc"
Write-Host "  library: $(Join-Path $destLib 'mtlc.lib')"
Write-Host ""
Write-Host "link a frontend with:"
Write-Host "  gcc -I$dest\include app.c $dest\lib\mtlc.lib -o app.exe -ldbghelp"
