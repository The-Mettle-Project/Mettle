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

$sectorSize = 512
$kernelSectors = 128
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
if ($kernel.Length -gt ($kernelSectors * $sectorSize)) {
  throw ("the kernel is $($kernel.Length) bytes; the boot sector reads only " +
         "$($kernelSectors * $sectorSize). Raise the sector count in boot.mettle.")
}

$floppy = New-Object byte[] $floppySize
[System.Array]::Copy($boot, 0, $floppy, 0, $boot.Length)
[System.Array]::Copy($kernel, 0, $floppy, $sectorSize, $kernel.Length)
[System.IO.File]::WriteAllBytes($Image, $floppy)

$used = [math]::Ceiling($kernel.Length / $sectorSize)
Write-Host "boot sector  $($boot.Length) bytes"
Write-Host "kernel       $($kernel.Length) bytes over $used of $kernelSectors sectors"
Write-Host "image        $Image"
