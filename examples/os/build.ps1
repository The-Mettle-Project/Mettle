param(
  [string]$Compiler = (Join-Path $PSScriptRoot "../../bin/mettle.exe"),
  [string]$Image = (Join-Path $PSScriptRoot "mettleos.img")
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$bootSource = Join-Path $here "boot.mettle"
$kernelSource = Join-Path $here "kernel.mettle"
$bootImage = Join-Path $here "boot.bin"
$kernelImage = Join-Path $here "kernel.bin"
$archiveImage = Join-Path $here "files.img"
$fileRoot = Join-Path $here "files"

$sectorSize = 512
$kernelRoom = 65536
$archiveRoom = 65536
$floppySize = 1474560

if (-not (Test-Path $Compiler)) {
  throw "no compiler at $Compiler"
}

& $Compiler $bootSource --target i8086-none --image-base 0x7c00 --emit-flat $bootImage
if ($LASTEXITCODE -ne 0) { throw "the boot sector did not compile" }

& $Compiler $kernelSource --target x86_64-none --image-base 0x20000 --emit-flat $kernelImage
if ($LASTEXITCODE -ne 0) { throw "the kernel did not compile" }

$boot = [System.IO.File]::ReadAllBytes($bootImage)
$kernel = [System.IO.File]::ReadAllBytes($kernelImage)

if ($boot.Length -ne $sectorSize) {
  throw "the boot sector is $($boot.Length) bytes, not $sectorSize"
}
if ($boot[510] -ne 0x55 -or $boot[511] -ne 0xAA) {
  throw "the boot sector carries no signature"
}
if ($kernel.Length -gt $kernelRoom) {
  throw "the kernel is $($kernel.Length) bytes and the image reserves $kernelRoom for it"
}

$files = @()
if (Test-Path $fileRoot) {
  $files = @(Get-ChildItem -Path $fileRoot -File | Sort-Object Name)
}

$entrySize = 48
$dataStart = 16 + ($files.Count * $entrySize)
$archiveSize = $dataStart
foreach ($file in $files) { $archiveSize += [int]$file.Length }

if ($archiveSize -gt $archiveRoom) {
  throw "the files add up to $archiveSize bytes and the image reserves $archiveRoom for them"
}

$archive = New-Object byte[] ([math]::Max($archiveSize, 16))
$magic = [System.Text.Encoding]::ASCII.GetBytes("METTLEFS")
[System.Array]::Copy($magic, 0, $archive, 0, 8)
[System.Array]::Copy([System.BitConverter]::GetBytes([uint32]$files.Count), 0, $archive, 8, 4)

$cursor = $dataStart
for ($i = 0; $i -lt $files.Count; $i++) {
  $file = $files[$i]
  $entry = 16 + ($i * $entrySize)
  $name = [System.Text.Encoding]::ASCII.GetBytes($file.Name)
  if ($name.Length -gt 31) {
    throw "the name $($file.Name) is longer than 31 bytes"
  }
  [System.Array]::Copy($name, 0, $archive, $entry, $name.Length)
  [System.Array]::Copy([System.BitConverter]::GetBytes([uint64]$cursor), 0, $archive, $entry + 32, 8)
  [System.Array]::Copy([System.BitConverter]::GetBytes([uint64]$file.Length), 0, $archive, $entry + 40, 8)
  $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
  [System.Array]::Copy($bytes, 0, $archive, $cursor, $bytes.Length)
  $cursor += $bytes.Length
}
[System.IO.File]::WriteAllBytes($archiveImage, $archive)

$floppy = New-Object byte[] $floppySize
[System.Array]::Copy($boot, 0, $floppy, 0, $boot.Length)
[System.Array]::Copy($kernel, 0, $floppy, $sectorSize, $kernel.Length)
[System.Array]::Copy($archive, 0, $floppy, $sectorSize + $kernelRoom, $archive.Length)
[System.IO.File]::WriteAllBytes($Image, $floppy)

Write-Host "boot sector  $($boot.Length) bytes"
Write-Host "kernel       $($kernel.Length) bytes of $kernelRoom reserved"
Write-Host "files        $($files.Count) in $($archive.Length) bytes of $archiveRoom reserved"
Write-Host "image        $Image"
