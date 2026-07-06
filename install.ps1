<#
.SYNOPSIS
  Mettle installer for Windows.

.DESCRIPTION
  Downloads the latest Mettle release, installs it under %LOCALAPPDATA%\Mettle,
  and adds the compiler to your user PATH. No administrator rights required.

  One-liner:
    irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.ps1 | iex

  Pin a version or change the location:
    & ([scriptblock]::Create((irm https://raw.githubusercontent.com/The-Mettle-Project/Mettle/main/install.ps1))) -Version v0.13.0

.PARAMETER Version
  Release tag to install (default: latest).

.PARAMETER InstallDir
  Install location (default: %LOCALAPPDATA%\Mettle).

.PARAMETER NoModifyPath
  Install but do not modify the user PATH.
#>
[CmdletBinding()]
param(
  [string]$Version = $env:METTLE_VERSION,
  [string]$InstallDir = $(if ($env:METTLE_INSTALL_DIR) { $env:METTLE_INSTALL_DIR } else { Join-Path $env:LOCALAPPDATA 'Mettle' }),
  [switch]$NoModifyPath,
  [string]$BaseUrl = $env:METTLE_BASE_URL  # internal/testing: override the download host
)

$ErrorActionPreference = 'Stop'
$Repo = 'The-Mettle-Project/Mettle'

function Say  { param($m) Write-Host $m -ForegroundColor Blue }
function Ok   { param($m) Write-Host "[ok] $m" -ForegroundColor Green }
function Warn { param($m) Write-Host "[!] $m" -ForegroundColor Yellow }
function Die  { param($m) Write-Host "error: $m" -ForegroundColor Red; exit 1 }

# TLS 1.2 for older PowerShell hosts talking to GitHub.
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

# --- platform check ---------------------------------------------------------
$arch = $env:PROCESSOR_ARCHITECTURE
if ($arch -ne 'AMD64' -and $arch -ne 'x86_64') {
  Die "unsupported architecture '$arch'. Mettle currently targets x86-64."
}
$target = 'windows-x64'

# --- resolve version --------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($Version)) {
  Say 'Finding the latest Mettle release...'
  try {
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" `
      -Headers @{ 'User-Agent' = 'mettle-installer' }
    $Version = $rel.tag_name
  } catch {
    Die "could not determine the latest release. Pass -Version vX.Y.Z, or see https://github.com/$Repo/releases."
  }
  if ([string]::IsNullOrWhiteSpace($Version)) {
    Die "the latest release has no tag. Pass -Version vX.Y.Z explicitly."
  }
}

$bundle = "mettle-$Version-$target"
if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
  $BaseUrl = "https://github.com/$Repo/releases/download/$Version"
}
$url = "$BaseUrl/$bundle.zip"

Write-Host "Installing Mettle " -NoNewline
Write-Host $Version -ForegroundColor Green -NoNewline
Write-Host " for " -NoNewline
Write-Host $target -ForegroundColor Green

# --- download + extract -----------------------------------------------------
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("mettle-install-" + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
  $zip = Join-Path $tmp 'mettle.zip'
  Say "Downloading $url"
  try {
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing -Headers @{ 'User-Agent' = 'mettle-installer' }
  } catch {
    Die "download failed. Does $Version ship a $target build? See https://github.com/$Repo/releases."
  }
  if (-not (Test-Path $zip) -or (Get-Item $zip).Length -eq 0) {
    Die "downloaded archive is empty."
  }

  Say 'Unpacking...'
  $unzip = Join-Path $tmp 'unzipped'
  Expand-Archive -Path $zip -DestinationPath $unzip -Force

  # The zip's contents may sit at the root or under a <bundle>\ folder.
  $src = $unzip
  if (Test-Path (Join-Path $unzip $bundle)) { $src = Join-Path $unzip $bundle }
  $exe = Join-Path $src 'mettle.exe'
  if (-not (Test-Path $exe)) { $exe = Join-Path $src 'bin\mettle.exe' }
  if (-not (Test-Path $exe)) { Die "archive did not contain mettle.exe (unexpected layout)." }

  # --- install (replace any prior install) ----------------------------------
  Say "Installing to $InstallDir"
  if (Test-Path $InstallDir) {
    $backup = "$InstallDir.old"
    if (Test-Path $backup) { Remove-Item -Recurse -Force $backup }
    Rename-Item -Path $InstallDir -NewName $backup
  }
  New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
  Copy-Item -Path (Join-Path $src '*') -Destination $InstallDir -Recurse -Force
  if (Test-Path "$InstallDir.old") { Remove-Item -Recurse -Force "$InstallDir.old" }

  # The compiler lives at <InstallDir>\bin\mettle.exe; normalize if the zip put
  # it at the root.
  $binDir = Join-Path $InstallDir 'bin'
  if (-not (Test-Path (Join-Path $binDir 'mettle.exe'))) {
    if (Test-Path (Join-Path $InstallDir 'mettle.exe')) {
      New-Item -ItemType Directory -Force -Path $binDir | Out-Null
      Move-Item -Force (Join-Path $InstallDir 'mettle.exe') (Join-Path $binDir 'mettle.exe')
    } else {
      Die "installed tree is missing bin\mettle.exe."
    }
  }
  Ok "Installed mettle to $binDir\mettle.exe"

  # --- user PATH ------------------------------------------------------------
  if ($NoModifyPath) {
    Warn "Skipping PATH update (-NoModifyPath). Add this directory yourself:"
    Write-Host "    $binDir"
  } else {
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if ([string]::IsNullOrEmpty($userPath)) { $userPath = '' }
    $parts = $userPath.Split(';') | Where-Object { $_ -ne '' }
    if ($parts -notcontains $binDir) {
      $newPath = (@($parts) + $binDir) -join ';'
      [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
      $env:Path = "$env:Path;$binDir"  # also update this session
      Ok "Added $binDir to your user PATH"
      $script:PathChanged = $true
    } else {
      Ok "$binDir is already on your PATH"
    }
  }

  # --- toolchain check (detect + guide) -------------------------------------
  Write-Host ''
  Write-Host 'Checking the C toolchain Mettle links with...' -ForegroundColor DarkGray
  if (Get-Command gcc -ErrorAction SilentlyContinue) {
    Ok 'Found gcc'
  } else {
    Warn 'gcc was not found on PATH.'
    Write-Host '  Mettle uses the internal PE linker by default, but gcc is useful as'
    Write-Host '  an external linker fallback. Install one of:'
    if (Get-Command winget -ErrorAction SilentlyContinue) {
      Write-Host '    winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT' -ForegroundColor White
    }
    if (Get-Command choco -ErrorAction SilentlyContinue) {
      Write-Host '    choco install mingw' -ForegroundColor White
    }
    if (-not (Get-Command winget -ErrorAction SilentlyContinue) -and
        -not (Get-Command choco -ErrorAction SilentlyContinue)) {
      Write-Host '    install MinGW-w64 (gcc) and add it to PATH'
    }
  }

  # --- done -----------------------------------------------------------------
  Write-Host ''
  Write-Host "Mettle $Version is installed." -ForegroundColor Green
  if ($script:PathChanged) {
    Write-Host 'Open a new terminal, then:'
  } else {
    Write-Host 'Get started:'
  }
  Write-Host '    mettle --version' -ForegroundColor White
  Write-Host "    'fn main() -> int32 { return 0; }' | Out-File hello.mettle -Encoding ascii" -ForegroundColor White
  Write-Host '    mettle --build hello.mettle -o hello.exe; .\hello.exe' -ForegroundColor White
  Write-Host ''
  Write-Host "Docs: https://github.com/$Repo" -ForegroundColor Blue
}
finally {
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
