param(
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$Baseline = "bin\mettle.exe",
    [string]$Candidate = "bin\mettle.exe",
    [string]$CandidateEnv = "",
    [int]$Runs = 7,
    [int]$AffinityMask = 0x10,
    [int[]]$Pads = @(0, 16, 32, 48, 64, 80, 96, 112)
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Base = [System.IO.Path]::GetFileNameWithoutExtension($Source)
$Scratch = Join-Path $Root ".tmp\placement_sweep\$Base"
New-Item -ItemType Directory -Force -Path "$Scratch\baseline", "$Scratch\candidate" | Out-Null
$Flags = @("--build", "--emit-obj", "--linker", "internal", "--release")
$SourcePath = (Resolve-Path $Source).Path
$SourceDir = Split-Path -Parent $SourcePath
$Text = [System.IO.File]::ReadAllText($SourcePath)

function Write-PaddedSource([int]$Pad) {
    $nops = ""
    for ($i = 0; $i -lt $Pad; $i++) { $nops += "    nop`n" }
    $fn = "@noinline fn placement_pad() -> void {`n  asm {`n$nops    nop`n  }`n}`n`n"
    $at = [regex]::Match($Text, "(?m)^(@\w+\s+)*fn ").Index
    $padded = $Text.Substring(0, $at) + $fn + $Text.Substring($at)
    $padded = [regex]::Replace($padded, "fn main\(\) -> int32 \{", "fn main() -> int32 {`n  placement_pad();")
    $path = Join-Path $SourceDir "__placement_$Pad.mettle"
    [System.IO.File]::WriteAllText($path, $padded)
    return $path
}

function Build-One([string]$Compiler, [string]$PaddedSource, [string]$Exe, [string]$EnvPair) {
    Remove-Item -Force -ErrorAction SilentlyContinue $Exe
    $kv = if ($EnvPair) { $EnvPair -split "=", 2 } else { $null }
    if ($kv) { Set-Item -Path "env:$($kv[0])" -Value $kv[1] }
    & $Compiler @Flags $PaddedSource -o $Exe 2>&1 | Out-Null
    if ($kv) { Remove-Item -Path "env:$($kv[0])" -ErrorAction SilentlyContinue }
    if (-not (Test-Path $Exe)) { throw "build failed: $Compiler $PaddedSource" }
}

function Measure-Once([string]$Exe) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = Split-Path -Parent $Exe
    $p = [System.Diagnostics.Process]::Start($psi)
    try { $p.ProcessorAffinity = [IntPtr]$AffinityMask; $p.PriorityClass = "High" } catch { }
    $out = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    if ($out -match "Time:\s*(\d+)\s*us") { return [double]$Matches[1] }
    throw "no timer in the output of $Exe"
}

$BaselinePath = (Resolve-Path $Baseline).Path
$CandidatePath = (Resolve-Path $Candidate).Path
$Pairs = @()
foreach ($pad in $Pads) {
    $padded = Write-PaddedSource $pad
    try {
        $a = "$Scratch\baseline\p$pad.exe"
        $b = "$Scratch\candidate\p$pad.exe"
        Build-One $BaselinePath $padded $a ""
        Build-One $CandidatePath $padded $b $CandidateEnv
        $Pairs += @{ pad = $pad; a = $a; b = $b; ta = @(); tb = @() }
    }
    finally {
        Remove-Item -Force -ErrorAction SilentlyContinue $padded
    }
}
for ($i = 0; $i -lt $Runs; $i++) {
    foreach ($q in $Pairs) {
        if ($i % 2 -eq 0) {
            $q.ta += Measure-Once $q.a; $q.tb += Measure-Once $q.b
        } else {
            $q.tb += Measure-Once $q.b; $q.ta += Measure-Once $q.a
        }
    }
}
$BestA = @(); $BestB = @()
foreach ($q in $Pairs) {
    $BestA += ($q.ta | Measure-Object -Minimum).Minimum
    $BestB += ($q.tb | Measure-Object -Minimum).Minimum
}
$SortedA = $BestA | Sort-Object
$SortedB = $BestB | Sort-Object
$MedianA = $SortedA[[int]($SortedA.Count / 2)]
$MedianB = $SortedB[[int]($SortedB.Count / 2)]
$Label = if ($CandidateEnv) { "candidate with $CandidateEnv" } else { "candidate" }
"{0}: baseline {1} us, {2} {3} us. Below 1.0 the candidate is faster: {4:N4}x" -f $Base, $MedianA, $Label, $MedianB, ($MedianB / $MedianA)
"spread across placements: baseline {0:N3}, candidate {1:N3}" -f ($SortedA[-1] / $SortedA[0]), ($SortedB[-1] / $SortedB[0])
