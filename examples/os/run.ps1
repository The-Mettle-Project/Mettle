param(
  [string]$Name = "MettleOS",
  [string]$Image = (Join-Path $PSScriptRoot "mettleos.img"),
  [switch]$Headless,
  [switch]$Recreate
)

$ErrorActionPreference = "Stop"

$manage = "VBoxManage"
if (-not (Get-Command $manage -ErrorAction SilentlyContinue)) {
  $candidate = Join-Path $env:ProgramFiles "Oracle/VirtualBox/VBoxManage.exe"
  if (-not (Test-Path $candidate)) {
    throw "VBoxManage is not on PATH and is not at $candidate"
  }
  $manage = $candidate
}

if (-not (Test-Path $Image)) {
  throw "no disk image at $Image; run build.ps1 first"
}

$existing = & $manage list vms
$known = $existing -match "^`"$Name`""

if ($known -and $Recreate) {
  & $manage controlvm $Name poweroff 2>$null | Out-Null
  Start-Sleep -Milliseconds 500
  & $manage unregistervm $Name --delete
  $known = $false
}

if (-not $known) {
  & $manage createvm --name $Name --ostype Other_64 --register
  & $manage modifyvm $Name --memory 64 --vram 16 --acpi on --ioapic off
  & $manage modifyvm $Name --boot1 floppy --boot2 none --boot3 none --boot4 none
  & $manage modifyvm $Name --nic1 none --usb off
  & $manage storagectl $Name --name "Floppy" --add floppy
}

& $manage storageattach $Name --storagectl "Floppy" --port 0 --device 0 --type fdd --medium $Image

$type = if ($Headless) { "headless" } else { "gui" }
& $manage startvm $Name --type $type
