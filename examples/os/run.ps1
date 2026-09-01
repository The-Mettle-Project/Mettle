param(
  [string]$Name = "MettleOS",
  [string]$Image = (Join-Path $PSScriptRoot "mettleos.vhd"),
  [string]$SerialLog = (Join-Path $PSScriptRoot "serial.log"),
  [int]$Memory = 256,
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

function Invoke-Manage {
  $previous = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  & $manage @args | Out-Null
  $ErrorActionPreference = $previous
}

$existing = & $manage list vms
$known = [bool]($existing -match "^`"$Name`"")

if ($known -and $Recreate) {
  Invoke-Manage controlvm $Name poweroff
  Start-Sleep -Milliseconds 500
  & $manage unregistervm $Name --delete
  $known = $false
}

if ($known) {
  $info = & $manage showvminfo $Name --machinereadable
  if ($info -match 'VMState="(running|paused|stuck)"') {
    Invoke-Manage controlvm $Name poweroff
    Start-Sleep -Milliseconds 1200
    $info = & $manage showvminfo $Name --machinereadable
  }
  if ($info -match 'VMState="(saved|aborted-saved)"') {
    & $manage discardstate $Name
    $info = & $manage showvminfo $Name --machinereadable
  }
  if ($info -match 'storagecontrollername\d+="Floppy"') {
    Invoke-Manage storageattach $Name --storagectl "Floppy" --port 0 --device 0 --medium none
    Invoke-Manage storagectl $Name --name "Floppy" --remove
  }
  $stale = Join-Path $PSScriptRoot "mettleos.img"
  if (Test-Path $stale) {
    Invoke-Manage closemedium floppy $stale
  }
}

if (-not $known) {
  & $manage createvm --name $Name --ostype Other_64 --register
}

$info = & $manage showvminfo $Name --machinereadable
if (-not ($info -match 'storagecontrollername\d+="IDE"')) {
  & $manage storagectl $Name --name "IDE" --add ide --controller PIIX4 --bootable on
}

& $manage modifyvm $Name --memory $Memory --vram 64 --acpi on --ioapic off
& $manage modifyvm $Name --graphicscontroller vmsvga --accelerate3d off
& $manage modifyvm $Name --boot1 disk --boot2 none --boot3 none --boot4 none
& $manage modifyvm $Name --nic1 none --usb off
& $manage modifyvm $Name --uart1 0x3f8 4 --uartmode1 file $SerialLog
& $manage storageattach $Name --storagectl "IDE" --port 0 --device 0 --type hdd --medium $Image

$type = if ($Headless) { "headless" } else { "gui" }
& $manage startvm $Name --type $type

Write-Host "everything the kernel prints is mirrored to $SerialLog"
