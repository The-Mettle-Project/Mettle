# Generate a self-contained HTML benchmark report from harness JSON output.
#
# Usage:
#   .\tools\benchmark\generate-report.ps1
#   .\tools\benchmark\generate-report.ps1 -InputPath docs/benchmarks/latest.json
#   .\tools\benchmark\generate-report.ps1 -InputPath docs/benchmarks/latest.json -OutputPath docs/benchmarks/latest.html

param(
    [string]$InputPath = "docs/benchmarks/latest.json",
    [string]$OutputPath = "",
    [switch]$OpenReport
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root

$InvariantCulture = [System.Globalization.CultureInfo]::InvariantCulture
$AssetsDir = Join-Path $PSScriptRoot "assets"

function Normalize-Path {
    param([string]$PathValue)
    return ($PathValue -replace "/", "\")
}

function Escape-Html {
    param([string]$Text)
    if ([string]::IsNullOrEmpty($Text)) { return "" }
    return [System.Net.WebUtility]::HtmlEncode($Text)
}

function Format-Number {
    param(
        [object]$Value,
        [int]$Digits = 2
    )
    if ($null -eq $Value) { return "n/a" }
    $format = "F{0}" -f $Digits
    return ([double]$Value).ToString($format, $InvariantCulture)
}

function Format-RatioBadge {
    param(
        [object]$Ratio,
        [string]$LowerIsBetter = "mettle"
    )

    if ($null -eq $Ratio) {
        return '<span class="badge badge-neutral">n/a</span>'
    }

    $value = [double]$Ratio
    $text = ([double]$value).ToString("N2", $InvariantCulture) + "x"

    if ($value -lt 0.95) {
        return "<span class=`"badge badge-win`" title=`"Mettle faster`">$text</span>"
    }
    if ($value -gt 1.05) {
        return "<span class=`"badge badge-loss`" title=`"C faster`">$text</span>"
    }
    return "<span class=`"badge badge-parity`" title=`"Within ~5%`">$text</span>"
}

function Format-Bytes {
    param([object]$Bytes)
    if ($null -eq $Bytes) { return "n/a" }
    $b = [long]$Bytes
    if ($b -ge 1048576) {
        return (([double]$b / 1048576.0).ToString("N2", $InvariantCulture) + " MB")
    }
    if ($b -ge 1024) {
        return (([double]$b / 1024.0).ToString("N1", $InvariantCulture) + " KB")
    }
    return ($b.ToString("N0", $InvariantCulture) + " B")
}

function Format-TimeMs {
    param([object]$Ms)
    if ($null -eq $Ms) { return "n/a" }
    $value = [double]$Ms
    if ($value -ge 1000.0) {
        return (([double]$value / 1000.0).ToString("N2", $InvariantCulture) + " s")
    }
    if ($value -ge 100.0) {
        return ($value.ToString("N2", $InvariantCulture) + " ms")
    }
    if ($value -ge 1.0) {
        return ($value.ToString("N3", $InvariantCulture) + " ms")
    }
    return (($value * 1000.0).ToString("N0", $InvariantCulture) + " us")
}

function Get-InlineSvg {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return "" }
    $content = Get-Content -Raw -Path $Path
    $content = $content -replace '<\?xml[^>]*\?>\s*', ''
    return $content.Trim()
}

function Get-InlineImageDataUri {
    param(
        [byte[]]$Bytes,
        [string]$MimeType = "png"
    )

    $base64 = [Convert]::ToBase64String($Bytes)
    return "data:image/$MimeType;base64,$base64"
}

function Get-ImageMimeTypeFromBytes {
    param([byte[]]$Bytes)

    if ($Bytes.Length -ge 3 -and $Bytes[0] -eq 0xFF -and $Bytes[1] -eq 0xD8 -and $Bytes[2] -eq 0xFF) {
        return "jpeg"
    }
    if ($Bytes.Length -ge 4 -and $Bytes[0] -eq 0x89 -and $Bytes[1] -eq 0x50 -and $Bytes[2] -eq 0x4E -and $Bytes[3] -eq 0x47) {
        return "png"
    }
    if ($Bytes.Length -ge 12 -and $Bytes[8] -eq 0x57 -and $Bytes[9] -eq 0x45 -and $Bytes[10] -eq 0x42 -and $Bytes[11] -eq 0x50) {
        return "webp"
    }

    return "png"
}

function Get-InlineImageDataUriFromFile {
    param([string]$Path)

    if (-not (Test-Path $Path)) { return "" }

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $mimeType = Get-ImageMimeTypeFromBytes -Bytes $bytes
    return Get-InlineImageDataUri -Bytes $bytes -MimeType $mimeType
}

function Get-CLogoMarkup {
    param([string]$AssetsDirectory)

    foreach ($fileName in @("c-logo.png", "c-logo.webp", "c-logo.jpg", "c-logo.jpeg")) {
        $imagePath = Join-Path $AssetsDirectory $fileName
        if (Test-Path $imagePath) {
            $dataUri = Get-InlineImageDataUriFromFile $imagePath
            return "<img src=`"$dataUri`" alt=`"C`" />"
        }
    }

    $svgPath = Join-Path $AssetsDirectory "c-logo.svg"
    if (Test-Path $svgPath) {
        return Get-InlineSvg $svgPath
    }

    return ""
}

function Get-BarWidth {
    param(
        [double]$Value,
        [double]$MaxValue
    )
    if ($MaxValue -le 0 -or $Value -le 0) { return 0 }
    return [math]::Min(100.0, [math]::Round(($Value / $MaxValue) * 100.0, 1))
}

function New-BarLine {
    param(
        [string]$Kind,
        [double]$WidthPercent,
        [string]$Display
    )

    $tag = if ($Kind -eq "mettle") { "M" } else { "C" }
    return @"
<div class="bar-line">
  <span class="bar-tag $Kind">$tag</span>
  <div class="bar-shell"><div class="bar-fill bar-$Kind" style="width:${WidthPercent}%"></div></div>
  <span class="bar-label">$(Escape-Html $Display)</span>
</div>
"@
}

function New-ComparisonBars {
    param(
        [string]$Label,
        [double]$MettleValue,
        [double]$CValue,
        [string]$MettleDisplay,
        [string]$CDisplay
    )

    $max = [math]::Max($MettleValue, $CValue)
    if ($max -le 0) { $max = 1.0 }
    $mettleWidth = Get-BarWidth -Value $MettleValue -MaxValue $max
    $cWidth = Get-BarWidth -Value $CValue -MaxValue $max

    return @"
<div class="metric-row">
  <div class="metric-name">$(Escape-Html $Label)</div>
  <div class="metric-bars">
    $(New-BarLine -Kind "mettle" -WidthPercent $mettleWidth -Display $MettleDisplay)
    $(New-BarLine -Kind "c" -WidthPercent $cWidth -Display $CDisplay)
  </div>
</div>
"@
}

function New-MetricChartSection {
    param(
        [string]$Title,
        [string]$Description,
        [string]$RowsHtml
    )

    return @"
<div class="metric-section">
  <div class="metric-head">
    <h3>$Title</h3>
    <p>$Description</p>
  </div>
  <div class="metric-chart">
    $RowsHtml
  </div>
</div>
"@
}

$inputFullPath = Join-Path $Root (Normalize-Path $InputPath)
if (-not (Test-Path $inputFullPath)) {
    Write-Error "Input JSON not found: $inputFullPath"
    exit 1
}

$data = Get-Content -Raw -Path $inputFullPath | ConvertFrom-Json

$outputFullPath = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    [System.IO.Path]::ChangeExtension($inputFullPath, ".html")
} else {
    Join-Path $Root (Normalize-Path $OutputPath)
}

$mettleLogo = Get-InlineSvg (Join-Path $Root "mettle.svg")
$cLogo = Get-CLogoMarkup -AssetsDirectory $AssetsDir

$benchmarks = @($data.benchmarks)
$compileBenchmarks = @($data.compile_benchmarks)
$summary = $data.summary
$hostInfo = $data.host
$harness = $data.harness
$failed = @($data.failed)

if ($null -eq $summary -and $benchmarks.Count -gt 0) {
    $runtimeRatios = @($benchmarks | ForEach-Object { if ($null -ne $_.relative) { [double]$_.relative } })
    $compileRatios = @($benchmarks | ForEach-Object { if ($null -ne $_.compile_relative) { [double]$_.compile_relative } })
    $sizeRatios = @($benchmarks | ForEach-Object { if ($null -ne $_.size_relative) { [double]$_.size_relative } })

    $geomean = {
        param([double[]]$Values)
        if ($Values.Count -eq 0) { return $null }
        $sumLogs = 0.0
        foreach ($value in $Values) {
            if ($value -le 0) { return $null }
            $sumLogs += [math]::Log($value)
        }
        return [math]::Round([math]::Exp($sumLogs / $Values.Count), 3)
    }

    $wins = 0; $losses = 0; $parity = 0
    $fastestWin = $null; $slowestLoss = $null
    foreach ($bench in $benchmarks) {
        if ($null -eq $bench.relative) { continue }
        $ratio = [double]$bench.relative
        if ($ratio -lt 0.95) {
            $wins++
            if ($null -eq $fastestWin -or $ratio -lt [double]$fastestWin.relative) {
                $fastestWin = [pscustomobject]@{ name = $bench.name; relative = $ratio }
            }
        } elseif ($ratio -gt 1.05) {
            $losses++
            if ($null -eq $slowestLoss -or $ratio -gt [double]$slowestLoss.relative) {
                $slowestLoss = [pscustomobject]@{ name = $bench.name; relative = $ratio }
            }
        } else {
            $parity++
        }
    }

    $summary = [pscustomobject]@{
        runtime_wins = $wins
        runtime_losses = $losses
        runtime_parity = $parity
        runtime_geomean = & $geomean $runtimeRatios
        compile_geomean = & $geomean $compileRatios
        size_geomean = & $geomean $sizeRatios
        fastest_win = $fastestWin
        slowest_loss = $slowestLoss
    }
}

$generated = if ($null -ne $data.generated) { [string]$data.generated } else { (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ") }
$generatedLocal = try {
    $utc = [datetime]::Parse($generated, $null, [System.Globalization.DateTimeStyles]::AdjustToUniversal)
    $utc.ToLocalTime().ToString("MMMM d, yyyy \at h:mm tt")
} catch {
    $generated
}

$runtimeWins = if ($null -ne $summary.runtime_wins) { [int]$summary.runtime_wins } else { 0 }
$runtimeLosses = if ($null -ne $summary.runtime_losses) { [int]$summary.runtime_losses } else { 0 }
$runtimeParity = if ($null -ne $summary.runtime_parity) { [int]$summary.runtime_parity } else { 0 }
$runtimeGeomean = if ($null -ne $summary.runtime_geomean) { [double]$summary.runtime_geomean } else { $null }
$compileGeomean = if ($null -ne $summary.compile_geomean) { [double]$summary.compile_geomean } else { $null }
$sizeGeomean = if ($null -ne $summary.size_geomean) { [double]$summary.size_geomean } else { $null }
$fastestWin = if ($null -ne $summary.fastest_win) { [string]$summary.fastest_win.name } else { "n/a" }
$fastestWinRatio = if ($null -ne $summary.fastest_win) { [double]$summary.fastest_win.relative } else { $null }
$slowestLoss = if ($null -ne $summary.slowest_loss) { [string]$summary.slowest_loss.name } else { "n/a" }
$slowestLossRatio = if ($null -ne $summary.slowest_loss) { [double]$summary.slowest_loss.relative } else { $null }

$sortedBenchmarks = @($benchmarks | Sort-Object name)

$runtimeChartRows = ""
foreach ($bench in $sortedBenchmarks) {
    $mettleMs = if ($null -ne $bench.mettle_ms) { [double]$bench.mettle_ms } else { 0 }
    $cMs = if ($null -ne $bench.c_ms) { [double]$bench.c_ms } else { 0 }
    $runtimeChartRows += (New-ComparisonBars -Label $bench.name `
        -MettleValue $mettleMs -CValue $cMs `
        -MettleDisplay (Format-TimeMs $mettleMs) -CDisplay (Format-TimeMs $cMs))
}

$compileChartRows = ""
foreach ($bench in $sortedBenchmarks) {
    $mettleMs = if ($null -ne $bench.mettle_compile_ms) { [double]$bench.mettle_compile_ms } else { 0 }
    $cMs = if ($null -ne $bench.c_compile_ms) { [double]$bench.c_compile_ms } else { 0 }
    $compileChartRows += (New-ComparisonBars -Label $bench.name `
        -MettleValue $mettleMs -CValue $cMs `
        -MettleDisplay ((Format-Number $mettleMs 0) + " ms") -CDisplay ((Format-Number $cMs 0) + " ms"))
}

$sizeChartRows = ""
foreach ($bench in $sortedBenchmarks) {
    $mettleBytes = if ($null -ne $bench.mettle_exe_bytes) { [double]$bench.mettle_exe_bytes } else { 0 }
    $cBytes = if ($null -ne $bench.c_exe_bytes) { [double]$bench.c_exe_bytes } else { 0 }
    $sizeChartRows += (New-ComparisonBars -Label $bench.name `
        -MettleValue $mettleBytes -CValue $cBytes `
        -MettleDisplay (Format-Bytes $bench.mettle_exe_bytes) -CDisplay (Format-Bytes $bench.c_exe_bytes))
}

$runtimeChartSection = New-MetricChartSection -Title "Runtime (median)" `
    -Description "Median execution time per benchmark. Bars are scaled within each row." `
    -RowsHtml $runtimeChartRows
$compileChartSection = New-MetricChartSection -Title "Compile time" `
    -Description "Wall-clock compile for each matched source pair." `
    -RowsHtml $compileChartRows
$sizeChartSection = New-MetricChartSection -Title "Executable size" `
    -Description "Linked output size on disk." `
    -RowsHtml $sizeChartRows

$detailRows = ""
foreach ($bench in $benchmarks) {
    $statsM = $bench.mettle_stats
    $statsC = $bench.c_stats
    $spreadM = if ($null -ne $statsM -and $null -ne $statsM.min_us -and $null -ne $statsM.max_us) {
        "{0}-{1} us" -f (Format-Number ($statsM.min_us) 0), (Format-Number ($statsM.max_us) 0)
    } else { "n/a" }
    $spreadC = if ($null -ne $statsC -and $null -ne $statsC.min_us -and $null -ne $statsC.max_us) {
        "{0}-{1} us" -f (Format-Number ($statsC.min_us) 0), (Format-Number ($statsC.max_us) 0)
    } else { "n/a" }

    $noinlineCell = "n/a"
    if ($null -ne $bench.c_noinline_us) {
        $noinlineMs = [math]::Round([double]$bench.c_noinline_us / 1000.0, 3)
        $noinlineCell = Format-TimeMs $noinlineMs
    }

    $detailRows += @"
<tr>
  <td class="name-cell">
    <div class="bench-name">$(Escape-Html $bench.name)</div>
    <div class="bench-desc">$(Escape-Html $bench.description)</div>
  </td>
  <td class="num">$(Format-TimeMs $bench.mettle_ms)<div class="sub">$spreadM</div></td>
  <td class="num">$(Format-TimeMs $bench.c_ms)<div class="sub">$spreadC</div></td>
  <td class="num">$noinlineCell</td>
  <td class="num">$(Format-RatioBadge $bench.relative)</td>
  <td class="num">$(Format-Number $bench.mettle_compile_ms 0) ms</td>
  <td class="num">$(Format-Number $bench.c_compile_ms 0) ms</td>
  <td class="num">$(Format-RatioBadge $bench.compile_relative)</td>
  <td class="num">$(Format-Bytes $bench.mettle_exe_bytes)</td>
  <td class="num">$(Format-Bytes $bench.c_exe_bytes)</td>
  <td class="num">$(Format-RatioBadge $bench.size_relative)</td>
</tr>
"@
}

$compileOnlyRows = ""
foreach ($bench in $compileBenchmarks) {
    $compileOnlyRows += @"
<tr>
  <td class="name-cell">
    <div class="bench-name">$(Escape-Html $bench.name)</div>
    <div class="bench-desc">$(Escape-Html $bench.description)</div>
  </td>
  <td class="num mono">$(Escape-Html $bench.mettle_source)</td>
  <td class="num">$(Format-Number $bench.mettle_compile_ms 0) ms</td>
</tr>
"@
}

$failedBanner = ""
if ($failed.Count -gt 0) {
    $failedBanner = @"
<div class="alert alert-error">
  <strong>Failed benchmarks:</strong> $(Escape-Html ($failed -join ", "))
</div>
"@
}

$cFlagsText = if ($null -ne $harness.c_flags) { ($harness.c_flags -join " ") } else { "-O3" }
$mettleFlagsText = if ($null -ne $harness.mettle_flags) { ($harness.mettle_flags -join " ") } else { "--build --emit-obj --linker internal --release" }

$hostRows = ""
if ($null -ne $hostInfo) {
    $hostFields = @(
        @{ label = "Machine"; value = $hostInfo.machine_name },
        @{ label = "OS"; value = $hostInfo.os },
        @{ label = "CPU"; value = $hostInfo.cpu },
        @{ label = "Logical cores"; value = $hostInfo.logical_processors },
        @{ label = "RAM"; value = if ($null -ne $hostInfo.ram_gb) { (Format-Number $hostInfo.ram_gb 1) + " GB" } else { $null } },
        @{ label = "GCC"; value = $hostInfo.gcc_version },
        @{ label = "Mettle compiler"; value = $hostInfo.compiler }
    )
    foreach ($field in $hostFields) {
        if ($null -eq $field.value -or [string]::IsNullOrWhiteSpace([string]$field.value)) { continue }
        $hostRows += "<tr><th>$(Escape-Html $field.label)</th><td>$(Escape-Html ([string]$field.value))</td></tr>"
    }
}

$geomeanRuntimeClass = if ($null -ne $runtimeGeomean -and $runtimeGeomean -lt 1.0) { "positive" } elseif ($null -ne $runtimeGeomean -and $runtimeGeomean -gt 1.0) { "negative" } else { "neutral" }
$geomeanCompileClass = if ($null -ne $compileGeomean -and $compileGeomean -lt 1.0) { "positive" } else { "neutral" }
$geomeanSizeClass = if ($null -ne $sizeGeomean -and $sizeGeomean -lt 1.0) { "positive" } else { "neutral" }

$html = @"
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Mettle vs C - Benchmark Report</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=DM+Sans:ital,opsz,wght@0,9..40,400;0,9..40,500;0,9..40,600;0,9..40,700;1,9..40,400&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet">
  <style>
    :root {
      --bg: #0c0d10;
      --bg-elevated: #14161c;
      --bg-card: #181b23;
      --border: rgba(255,255,255,0.08);
      --text: #eef0f4;
      --text-muted: #9aa3b2;
      --mettle: #f4c542;
      --mettle-dim: rgba(244,197,66,0.15);
      --c-lang: #3b9fd9;
      --c-dim: rgba(59,159,217,0.15);
      --win: #3ecf8e;
      --loss: #ff6b6b;
      --parity: #9aa3b2;
      --accent: #8b7cf6;
      --shadow: 0 24px 80px rgba(0,0,0,0.45);
      --radius: 18px;
    }

    * { box-sizing: border-box; }
    html { scroll-behavior: smooth; }
    body {
      margin: 0;
      font-family: "DM Sans", system-ui, sans-serif;
      background:
        radial-gradient(1200px 600px at 10% -10%, rgba(139,124,246,0.12), transparent 60%),
        radial-gradient(900px 500px at 90% 0%, rgba(59,159,217,0.10), transparent 55%),
        radial-gradient(700px 400px at 50% 100%, rgba(244,197,66,0.06), transparent 50%),
        var(--bg);
      color: var(--text);
      line-height: 1.5;
      min-height: 100vh;
    }

    .container { max-width: 1200px; margin: 0 auto; padding: 2rem 1.25rem 4rem; }

    header.hero {
      display: grid;
      gap: 1.5rem;
      padding: 2rem;
      border: 1px solid var(--border);
      border-radius: calc(var(--radius) + 4px);
      background: linear-gradient(135deg, rgba(24,27,35,0.95), rgba(12,13,16,0.85));
      box-shadow: var(--shadow);
      margin-bottom: 2rem;
    }

    .hero-top {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      justify-content: space-between;
      gap: 1.5rem;
    }

    .versus {
      display: flex;
      align-items: center;
      gap: 1.25rem;
    }

    .logo {
      width: 72px;
      height: 72px;
      display: grid;
      place-items: center;
      border-radius: 20px;
      background: var(--bg-card);
      border: 1px solid var(--border);
      padding: 12px;
    }
    .logo svg { width: 100%; height: 100%; display: block; }
    .logo.mettle { background: var(--mettle-dim); border-color: rgba(244,197,66,0.25); }
    .logo.mettle svg polygon,
    .logo.mettle svg path { fill: var(--mettle); }
    .logo.c-lang {
      background: var(--c-dim);
      border: 1px solid rgba(59,159,217,0.25);
      padding: 8px;
    }
    .logo.c-lang img,
    .logo.c-lang svg {
      width: 100%;
      height: 100%;
      display: block;
      object-fit: contain;
    }

    .vs-badge {
      font-size: 0.75rem;
      font-weight: 700;
      letter-spacing: 0.12em;
      text-transform: uppercase;
      color: var(--text-muted);
      padding: 0.35rem 0.65rem;
      border: 1px solid var(--border);
      border-radius: 999px;
      background: rgba(255,255,255,0.03);
    }

    .hero-copy h1 {
      margin: 0 0 0.35rem;
      font-size: clamp(1.75rem, 4vw, 2.5rem);
      font-weight: 700;
      letter-spacing: -0.03em;
    }
    .hero-copy p {
      margin: 0;
      color: var(--text-muted);
      max-width: 52ch;
    }

    .meta-pills {
      display: flex;
      flex-wrap: wrap;
      gap: 0.5rem;
    }
    .pill {
      font-size: 0.8125rem;
      color: var(--text-muted);
      background: rgba(255,255,255,0.04);
      border: 1px solid var(--border);
      border-radius: 999px;
      padding: 0.35rem 0.75rem;
    }
    .pill strong { color: var(--text); font-weight: 600; }

    .summary-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
      gap: 1rem;
      margin-bottom: 2rem;
    }

    .summary-card {
      background: var(--bg-card);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      padding: 1.25rem 1.35rem;
      position: relative;
      overflow: hidden;
    }
    .summary-card::before {
      content: "";
      position: absolute;
      inset: 0 auto auto 0;
      width: 100%;
      height: 3px;
      background: linear-gradient(90deg, var(--mettle), var(--c-lang));
      opacity: 0.7;
    }
    .summary-card .label {
      font-size: 0.75rem;
      text-transform: uppercase;
      letter-spacing: 0.08em;
      color: var(--text-muted);
      margin-bottom: 0.35rem;
    }
    .summary-card .value {
      font-size: 1.75rem;
      font-weight: 700;
      letter-spacing: -0.03em;
      line-height: 1.1;
    }
    .summary-card .value.positive { color: var(--win); }
    .summary-card .value.negative { color: var(--loss); }
    .summary-card .value.neutral { color: var(--text); }
    .summary-card .detail {
      margin-top: 0.5rem;
      font-size: 0.875rem;
      color: var(--text-muted);
    }

    section.panel {
      background: var(--bg-card);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      padding: 1.5rem;
      margin-bottom: 1.5rem;
    }
    section.panel h2 {
      margin: 0 0 0.35rem;
      font-size: 1.25rem;
      letter-spacing: -0.02em;
    }
    section.panel .section-lead {
      margin: 0 0 1.25rem;
      color: var(--text-muted);
      font-size: 0.9375rem;
    }

    .legend {
      display: flex;
      gap: 1rem;
      flex-wrap: wrap;
      margin-bottom: 1rem;
      font-size: 0.8125rem;
      color: var(--text-muted);
    }
    .legend-item { display: flex; align-items: center; gap: 0.4rem; }
    .legend-swatch {
      width: 12px; height: 12px; border-radius: 3px;
    }
    .legend-swatch.mettle { background: var(--mettle); }
    .legend-swatch.c { background: var(--c-lang); }

    .metric-sections {
      display: flex;
      flex-direction: column;
      gap: 2rem;
    }

    .metric-section {
      border: 1px solid var(--border);
      border-radius: 14px;
      overflow: hidden;
      background: rgba(255,255,255,0.015);
    }

    .metric-head {
      padding: 1rem 1.15rem;
      border-bottom: 1px solid var(--border);
      background: rgba(255,255,255,0.02);
    }
    .metric-head h3 {
      margin: 0;
      font-size: 1rem;
      letter-spacing: -0.01em;
    }
    .metric-head p {
      margin: 0.25rem 0 0;
      font-size: 0.8125rem;
      color: var(--text-muted);
    }

    .metric-chart {
      padding: 0.35rem 1rem 0.75rem;
    }

    .metric-row {
      display: grid;
      grid-template-columns: 118px minmax(0, 1fr);
      gap: 1rem;
      align-items: center;
      padding: 0.7rem 0;
      border-bottom: 1px solid rgba(255,255,255,0.05);
    }
    .metric-row:last-child { border-bottom: none; }

    .metric-name {
      font-family: "JetBrains Mono", monospace;
      font-size: 0.8125rem;
      color: var(--text);
      font-weight: 500;
    }

    .metric-bars {
      display: grid;
      gap: 0.45rem;
    }

    .bar-line {
      display: grid;
      grid-template-columns: 24px minmax(0, 1fr) 6.75rem;
      gap: 0.55rem;
      align-items: center;
    }

    .bar-tag {
      width: 24px;
      height: 24px;
      border-radius: 7px;
      display: grid;
      place-items: center;
      font-size: 0.625rem;
      font-weight: 700;
      letter-spacing: 0.04em;
    }
    .bar-tag.mettle {
      background: var(--mettle-dim);
      color: var(--mettle);
      border: 1px solid rgba(244,197,66,0.22);
    }
    .bar-tag.c {
      background: var(--c-dim);
      color: var(--c-lang);
      border: 1px solid rgba(59,159,217,0.22);
    }

    .bar-shell {
      height: 10px;
      background: rgba(255,255,255,0.06);
      border-radius: 999px;
      overflow: hidden;
    }

    .bar-fill {
      height: 100%;
      border-radius: 999px;
      min-width: 3px;
    }
    .bar-fill.bar-mettle { background: var(--mettle); }
    .bar-fill.bar-c { background: var(--c-lang); }

    .bar-label {
      font-family: "JetBrains Mono", monospace;
      font-size: 0.75rem;
      color: var(--text-muted);
      text-align: right;
      white-space: nowrap;
      font-variant-numeric: tabular-nums;
    }

    .table-wrap { overflow-x: auto; border-radius: 12px; border: 1px solid var(--border); }
    table.data {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.875rem;
    }
    table.data th, table.data td {
      padding: 0.85rem 1rem;
      border-bottom: 1px solid var(--border);
      vertical-align: top;
    }
    table.data th {
      text-align: left;
      font-size: 0.6875rem;
      text-transform: uppercase;
      letter-spacing: 0.07em;
      color: var(--text-muted);
      background: rgba(255,255,255,0.02);
      white-space: nowrap;
    }
    table.data tr:last-child td { border-bottom: none; }
    table.data tr:hover td { background: rgba(255,255,255,0.02); }
    table.data td.num { text-align: right; white-space: nowrap; font-variant-numeric: tabular-nums; }
    table.data td.mono { font-family: "JetBrains Mono", monospace; font-size: 0.75rem; text-align: left; }

    .name-cell .bench-name {
      font-weight: 600;
      font-family: "JetBrains Mono", monospace;
      font-size: 0.8125rem;
    }
    .name-cell .bench-desc {
      margin-top: 0.2rem;
      color: var(--text-muted);
      font-size: 0.8125rem;
      max-width: 28rem;
    }
    .sub { font-size: 0.6875rem; color: var(--text-muted); margin-top: 0.15rem; }

    .badge {
      display: inline-block;
      font-family: "JetBrains Mono", monospace;
      font-size: 0.75rem;
      font-weight: 500;
      padding: 0.2rem 0.45rem;
      border-radius: 6px;
    }
    .badge-win { background: rgba(62,207,142,0.15); color: var(--win); }
    .badge-loss { background: rgba(255,107,107,0.15); color: var(--loss); }
    .badge-parity { background: rgba(154,163,178,0.12); color: var(--parity); }
    .badge-neutral { background: rgba(255,255,255,0.05); color: var(--text-muted); }

    .alert {
      padding: 0.85rem 1rem;
      border-radius: 12px;
      margin-bottom: 1.5rem;
      font-size: 0.875rem;
    }
    .alert-error {
      background: rgba(255,107,107,0.12);
      border: 1px solid rgba(255,107,107,0.25);
      color: #ffb4b4;
    }

    .methodology {
      display: grid;
      gap: 0.75rem;
      font-size: 0.9375rem;
      color: var(--text-muted);
    }
    .methodology code {
      font-family: "JetBrains Mono", monospace;
      font-size: 0.8125rem;
      background: rgba(255,255,255,0.05);
      padding: 0.1rem 0.35rem;
      border-radius: 4px;
      color: var(--text);
    }

    footer {
      margin-top: 2rem;
      padding-top: 1.5rem;
      border-top: 1px solid var(--border);
      color: var(--text-muted);
      font-size: 0.8125rem;
    }
    footer table { width: 100%; max-width: 640px; }
    footer th {
      text-align: left;
      padding: 0.25rem 1rem 0.25rem 0;
      color: var(--text-muted);
      font-weight: 500;
      width: 140px;
    }
    footer td { color: var(--text); word-break: break-word; }

    @media (max-width: 720px) {
      .metric-row { grid-template-columns: 1fr; gap: 0.45rem; }
      .bar-line { grid-template-columns: 24px minmax(0, 1fr) 5.5rem; }
      .hero-top { flex-direction: column; align-items: flex-start; }
    }
  </style>
</head>
<body>
  <div class="container">
    <header class="hero">
      <div class="hero-top">
        <div class="versus">
          <div class="logo mettle" title="Mettle">$mettleLogo</div>
          <span class="vs-badge">vs</span>
          <div class="logo c-lang" title="GCC C">$cLogo</div>
        </div>
        <div class="hero-copy">
          <h1>Benchmark Report</h1>
          <p>Head-to-head runtime, compile-time, and binary-size comparison across the Mettle example suite against GCC-optimized C.</p>
        </div>
      </div>
      <div class="meta-pills">
        <span class="pill"><strong>Generated</strong> $(Escape-Html $generatedLocal)</span>
        <span class="pill"><strong>Runs</strong> $($harness.runs) (+ $($harness.warmup) warmup)</span>
        <span class="pill"><strong>Benchmarks</strong> $($benchmarks.Count) runtime$(if ($compileBenchmarks.Count -gt 0) { ", $($compileBenchmarks.Count) compile-only" })</span>
      </div>
    </header>

    $failedBanner

    <div class="summary-grid">
      <div class="summary-card">
        <div class="label">Runtime record</div>
        <div class="value neutral">$runtimeWins / $($runtimeWins + $runtimeLosses + $runtimeParity)</div>
        <div class="detail">$runtimeWins faster · $runtimeParity parity · $runtimeLosses slower <span style="opacity:0.6">(±5%)</span></div>
      </div>
      <div class="summary-card">
        <div class="label">Geometric mean runtime</div>
        <div class="value $geomeanRuntimeClass">$(if ($null -ne $runtimeGeomean) { (Format-Number $runtimeGeomean 2) + "x" } else { "n/a" })</div>
        <div class="detail">&lt;1x means Mettle wins overall on median runtime</div>
      </div>
      <div class="summary-card">
        <div class="label">Compile speed (geomean)</div>
        <div class="value $geomeanCompileClass">$(if ($null -ne $compileGeomean) { (Format-Number $compileGeomean 2) + "x" } else { "n/a" })</div>
        <div class="detail">Mettle compile time relative to GCC</div>
      </div>
      <div class="summary-card">
        <div class="label">Binary size (geomean)</div>
        <div class="value $geomeanSizeClass">$(if ($null -ne $sizeGeomean) { (Format-Number $sizeGeomean 2) + "x" } else { "n/a" })</div>
        <div class="detail">Mettle executables vs GCC-linked C</div>
      </div>
      <div class="summary-card">
        <div class="label">Biggest win</div>
        <div class="value positive">$(Escape-Html $fastestWin)</div>
        <div class="detail">$(if ($null -ne $fastestWinRatio) { (Format-Number $fastestWinRatio 2) + "x faster than C" } else { "n/a" })</div>
      </div>
      <div class="summary-card">
        <div class="label">Biggest gap</div>
        <div class="value negative">$(Escape-Html $slowestLoss)</div>
        <div class="detail">$(if ($null -ne $slowestLossRatio) { (Format-Number $slowestLossRatio 2) + "x vs C" } else { "n/a" })</div>
      </div>
    </div>

    <section class="panel">
      <h2>Visual comparison</h2>
      <p class="section-lead">Each row compares Mettle and GCC on the same benchmark. Values sit beside the bars, not on top of them.</p>
      <div class="legend">
        <span class="legend-item"><span class="legend-swatch mettle"></span> Mettle</span>
        <span class="legend-item"><span class="legend-swatch c"></span> GCC C ($cFlagsText)</span>
      </div>
      <div class="metric-sections">
        $runtimeChartSection
        $compileChartSection
        $sizeChartSection
      </div>
    </section>

    <section class="panel">
      <h2>Detailed results</h2>
      <p class="section-lead">Median of $($harness.runs) measured runs after $($harness.warmup) warmup iteration(s). Spread shows min-max across runs.</p>
      <div class="table-wrap">
        <table class="data">
          <thead>
            <tr>
              <th>Benchmark</th>
              <th>Mettle runtime</th>
              <th>C runtime</th>
              <th>C -fno-inline</th>
              <th>Runtime ratio</th>
              <th>Mettle compile</th>
              <th>C compile</th>
              <th>Compile ratio</th>
              <th>Mettle size</th>
              <th>C size</th>
              <th>Size ratio</th>
            </tr>
          </thead>
          <tbody>
            $detailRows
          </tbody>
        </table>
      </div>
    </section>

$(if ($compileBenchmarks.Count -gt 0) {
@"
    <section class="panel">
      <h2>Compile-only stress tests</h2>
      <p class="section-lead">Large fixtures timed with <code>mettle --profile</code> (full compiler pipeline, no linked executable).</p>
      <div class="table-wrap">
        <table class="data">
          <thead>
            <tr>
              <th>Benchmark</th>
              <th>Source</th>
              <th>Profile total</th>
            </tr>
          </thead>
          <tbody>
            $compileOnlyRows
          </tbody>
        </table>
      </div>
    </section>
"@
} else { "" })

    <section class="panel">
      <h2>Methodology</h2>
      <div class="methodology">
        <p>Each benchmark is a matched pair under <code>examples/</code> - the same workload implemented in Mettle and C, printing <code>Time: &lt;N&gt; us</code> via QueryPerformanceCounter.</p>
        <p>Mettle builds use <code>$mettleFlagsText</code>. C builds use GCC with <code>$cFlagsText</code>. Runtime ratios are Mettle÷C on median microseconds; values below 1.0 mean Mettle is faster.</p>
        <p>Parity is defined as within ±5% of C. Geometric means aggregate ratios without letting a single outlier dominate the headline number.</p>
      </div>
    </section>

    <footer>
      <table>
        $hostRows
      </table>
      <p style="margin-top:1rem;opacity:0.7">Report generated by <code>tools/benchmark/run-benchmarks.ps1</code> · Source: <code>$(Escape-Html $InputPath)</code></p>
    </footer>
  </div>
</body>
</html>
"@

$outputDir = Split-Path -Parent $outputFullPath
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$html | Set-Content -Path $outputFullPath -Encoding UTF8
Write-Host "Wrote $outputFullPath"

if ($OpenReport) {
    Start-Process $outputFullPath
}
