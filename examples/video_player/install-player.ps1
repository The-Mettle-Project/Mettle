#Requires -Version 5.1
<#
Registers examples/video_player/player.exe with Windows so it can be chosen
as the handler for .mp4, .mov, .m4v and .avi.

Windows 10 and 11 do not let a program make itself the default. The choice
lives in a UserChoice key that Windows protects with a hash and rewrites if
anything tampers with it. Registration is the part an application can do; the
final pick is one click in Settings, or Open with -> Always.

  .\install-player.ps1              register
  .\install-player.ps1 -Unregister  remove every key this script wrote

Everything is written under HKCU. No administrator rights, nothing outside
the current user.
#>
[CmdletBinding()]
param(
    [switch]$Unregister,
    [string]$ExePath
)

$ErrorActionPreference = 'Stop'

$ProgId      = 'Mettle.VideoPlayer'
$AppName     = 'Mettle Video Player'
$AppKeyPath  = 'HKCU:\Software\Mettle Video Player'
$Extensions  = @('.mp4', '.mov', '.m4v', '.avi')

if (-not $ExePath) {
    $ExePath = Join-Path $PSScriptRoot 'player.exe'
}

function Remove-KeyIfPresent([string]$path) {
    if (Test-Path $path) { Remove-Item -Path $path -Recurse -Force }
}

if ($Unregister) {
    Remove-KeyIfPresent "HKCU:\Software\Classes\$ProgId"
    Remove-KeyIfPresent 'HKCU:\Software\Classes\Applications\player.exe'
    Remove-KeyIfPresent $AppKeyPath

    foreach ($ext in $Extensions) {
        $p = "HKCU:\Software\Classes\$ext\OpenWithProgids"
        if (Test-Path $p) {
            Remove-ItemProperty -Path $p -Name $ProgId -ErrorAction SilentlyContinue
        }
    }

    $reg = 'HKCU:\Software\RegisteredApplications'
    if (Test-Path $reg) {
        Remove-ItemProperty -Path $reg -Name $AppName -ErrorAction SilentlyContinue
    }

    Write-Output "Unregistered $AppName."
    return
}

if (-not (Test-Path $ExePath)) {
    throw "player.exe not found at $ExePath. Build it first with examples\video_player\build.bat"
}
$ExePath = (Resolve-Path $ExePath).Path
$command = '"{0}" "%1"' -f $ExePath

# The document type: what the file is, how to open it, what icon to draw.
New-Item -Path "HKCU:\Software\Classes\$ProgId\shell\open\command" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\$ProgId" -Name '(default)' -Value 'Video file'
Set-ItemProperty -Path "HKCU:\Software\Classes\$ProgId" -Name 'FriendlyTypeName' -Value 'Video file'
New-Item -Path "HKCU:\Software\Classes\$ProgId\DefaultIcon" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\$ProgId\DefaultIcon" -Name '(default)' -Value "$ExePath,0"
Set-ItemProperty -Path "HKCU:\Software\Classes\$ProgId\shell\open\command" -Name '(default)' -Value $command

# The application itself, so it appears in the Open with list.
New-Item -Path 'HKCU:\Software\Classes\Applications\player.exe\shell\open\command' -Force | Out-Null
Set-ItemProperty -Path 'HKCU:\Software\Classes\Applications\player.exe' -Name 'FriendlyAppName' -Value $AppName
Set-ItemProperty -Path 'HKCU:\Software\Classes\Applications\player.exe\shell\open\command' -Name '(default)' -Value $command
New-Item -Path 'HKCU:\Software\Classes\Applications\player.exe\SupportedTypes' -Force | Out-Null
foreach ($ext in $Extensions) {
    Set-ItemProperty -Path 'HKCU:\Software\Classes\Applications\player.exe\SupportedTypes' -Name $ext -Value ''
}

# Offer the type on each extension without disturbing whatever is default now.
foreach ($ext in $Extensions) {
    New-Item -Path "HKCU:\Software\Classes\$ext\OpenWithProgids" -Force | Out-Null
    Set-ItemProperty -Path "HKCU:\Software\Classes\$ext\OpenWithProgids" -Name $ProgId -Value ([byte[]]@()) -Type None
}

# Capabilities, which is what puts the app in Settings, Default apps.
New-Item -Path "$AppKeyPath\Capabilities\FileAssociations" -Force | Out-Null
Set-ItemProperty -Path "$AppKeyPath\Capabilities" -Name 'ApplicationName' -Value $AppName
Set-ItemProperty -Path "$AppKeyPath\Capabilities" -Name 'ApplicationDescription' -Value 'Plays H.264 and Motion JPEG video with AAC or PCM audio.'
foreach ($ext in $Extensions) {
    Set-ItemProperty -Path "$AppKeyPath\Capabilities\FileAssociations" -Name $ext -Value $ProgId
}

New-Item -Path 'HKCU:\Software\RegisteredApplications' -Force | Out-Null
Set-ItemProperty -Path 'HKCU:\Software\RegisteredApplications' -Name $AppName -Value 'Software\Mettle Video Player\Capabilities'

Write-Output "Registered $AppName for $($Extensions -join ', ')"
Write-Output "  $ExePath"
Write-Output ''
Write-Output 'Windows will not let a program make itself the default, so pick it once:'
Write-Output '  right click a video -> Open with -> Choose another app -> Mettle Video Player -> Always'
Write-Output 'or Settings -> Apps -> Default apps -> Mettle Video Player.'
