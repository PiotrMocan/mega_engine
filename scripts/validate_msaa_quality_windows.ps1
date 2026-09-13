param(
    [string]$ExePath = "",
    [string]$OutputDirectory = "",
    [switch]$SkipCapture
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $root 'build\windows-debug\bin\vulkan_cube.exe'
}
$exe = (Resolve-Path -LiteralPath $ExePath).Path
$debugTree = Join-Path $root 'build\windows-debug'
if (-not $exe.StartsWith($debugTree, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'MSAA image validation must use build\windows-debug.'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $debugTree 'msaa-quality'
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$common = '--render-fixture aa-wires --lighting dynamic --gi off --day-cycle off --time-of-day 14 --exposure fixed --exposure-ev 0 --subsurface off --depth-prepass full'
$captures = [ordered]@{
    off = Join-Path $OutputDirectory 'off.png'
    smaa = Join-Path $OutputDirectory 'smaa.png'
    smaa_t2x = Join-Path $OutputDirectory 'smaa-t2x.png'
    fsr3_native = Join-Path $OutputDirectory 'fsr3-native.png'
    msaa2x = Join-Path $OutputDirectory 'msaa2x.png'
    msaa4x = Join-Path $OutputDirectory 'msaa4x.png'
    reference = Join-Path $OutputDirectory 'off-4x-reference.png'
}
if (-not $SkipCapture) {
    foreach ($method in @('off', 'smaa', 'msaa2x', 'msaa4x')) {
        & (Join-Path $PSScriptRoot 'mcp_smoke.ps1') -ExePath $exe `
            -AntiAliasing $method -ExtraArguments $common `
            -ScreenshotPath $captures[$method] | Out-Null
    }
    & (Join-Path $PSScriptRoot 'mcp_smoke.ps1') -ExePath $exe `
        -AntiAliasing smaa -ExtraArguments "$common --smaa-mode t2x" `
        -ScreenshotPath $captures.smaa_t2x | Out-Null
    & (Join-Path $PSScriptRoot 'mcp_smoke.ps1') -ExePath $exe `
        -AntiAliasing fsr3 -ExtraArguments "$common --fsr-quality native-aa" `
        -ScreenshotPath $captures.fsr3_native | Out-Null
    & (Join-Path $PSScriptRoot 'mcp_smoke.ps1') -ExePath $exe `
        -AntiAliasing off -ExtraArguments "$common --width 2560 --height 1440" `
        -ScreenshotPath $captures.reference | Out-Null
}
foreach ($path in $captures.Values) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing MSAA quality capture: $path" }
}

Add-Type -AssemblyName System.Drawing
function Read-Bgra([string]$Path) {
    $bitmap = [Drawing.Bitmap]::FromFile($Path)
    try {
        $rectangle = [Drawing.Rectangle]::new(0, 0, $bitmap.Width, $bitmap.Height)
        $locked = $bitmap.LockBits(
            $rectangle, [Drawing.Imaging.ImageLockMode]::ReadOnly,
            [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $rowBytes = $bitmap.Width * 4
            $pixels = [byte[]]::new($rowBytes * $bitmap.Height)
            for ($y = 0; $y -lt $bitmap.Height; ++$y) {
                [Runtime.InteropServices.Marshal]::Copy(
                    [IntPtr]::Add($locked.Scan0, $y * $locked.Stride),
                    $pixels, $y * $rowBytes, $rowBytes)
            }
            return [pscustomobject]@{ Width=$bitmap.Width; Height=$bitmap.Height; Pixels=$pixels }
        } finally { $bitmap.UnlockBits($locked) }
    } finally { $bitmap.Dispose() }
}

$low = [ordered]@{
    off = Read-Bgra $captures.off
    smaa = Read-Bgra $captures.smaa
    smaa_t2x = Read-Bgra $captures.smaa_t2x
    fsr3_native = Read-Bgra $captures.fsr3_native
    msaa2x = Read-Bgra $captures.msaa2x
    msaa4x = Read-Bgra $captures.msaa4x
}
$reference = Read-Bgra $captures.reference
foreach ($image in $low.Values) {
    if ($image.Width -ne 640 -or $image.Height -ne 360) {
        throw 'Low-resolution MSAA captures must be 640x360.'
    }
}
if ($reference.Width -ne 2560 -or $reference.Height -ne 1440) {
    throw 'Supersampled reference must be 2560x1440.'
}

$linear = [double[]]::new(256)
for ($value = 0; $value -lt 256; ++$value) {
    $encoded = $value / 255.0
    $linear[$value] = if ($encoded -le 0.04045) {
        $encoded / 12.92
    } else {
        [Math]::Pow(($encoded + 0.055) / 1.055, 2.4)
    }
}
$edgeAbsolute = @{ off=0.0; smaa=0.0; smaa_t2x=0.0; fsr3_native=0.0; msaa2x=0.0; msaa4x=0.0 }
$interiorAbsolute = @{ off=0.0; smaa=0.0; smaa_t2x=0.0; fsr3_native=0.0; msaa2x=0.0; msaa4x=0.0 }
$edgeSamples = 0L
$interiorSamples = 0L
# The fixed screen-space fixture panel occupies this rectangle. Starting below
# y=72 excludes the HUD both at native size and from the 4x reference.
for ($y = 72; $y -lt 293; ++$y) {
    for ($x = 132; $x -lt 509; ++$x) {
        $sum = @(0.0, 0.0, 0.0)
        $sumSquares = @(0.0, 0.0, 0.0)
        for ($dy = 0; $dy -lt 4; ++$dy) {
            for ($dx = 0; $dx -lt 4; ++$dx) {
                $index = ((4 * $y + $dy) * $reference.Width + 4 * $x + $dx) * 4
                for ($channel = 0; $channel -lt 3; ++$channel) {
                    $sample = $linear[$reference.Pixels[$index + $channel]]
                    $sum[$channel] += $sample
                    $sumSquares[$channel] += $sample * $sample
                }
            }
        }
        $mean = @(
            ([double]$sum[0]) / 16.0
            ([double]$sum[1]) / 16.0
            ([double]$sum[2]) / 16.0
        )
        $variance = 0.0
        for ($channel = 0; $channel -lt 3; ++$channel) {
            $variance += $sumSquares[$channel] / 16.0 - $mean[$channel] * $mean[$channel]
        }
        $isEdge = $variance -gt 0.00001
        foreach ($method in $low.Keys) {
            $index = ($y * $low[$method].Width + $x) * 4
            $pixelError = 0.0
            for ($channel = 0; $channel -lt 3; ++$channel) {
                $pixelError += [Math]::Abs(
                    $linear[$low[$method].Pixels[$index + $channel]] - $mean[$channel])
            }
            if ($isEdge) { $edgeAbsolute[$method] += $pixelError / 3.0 }
            else { $interiorAbsolute[$method] += $pixelError / 3.0 }
        }
        if ($isEdge) { ++$edgeSamples } else { ++$interiorSamples }
    }
}
if ($edgeSamples -lt 100 -or $interiorSamples -lt 1000) {
    throw "Fixture reference did not expose enough edge/interior samples: $edgeSamples/$interiorSamples"
}
$result = [ordered]@{
    reference = '4x-linear-resolution Off, 4x4 box average in linear light'
    region = @{x=132; y=72; width=377; height=221}
    edge_samples = $edgeSamples
    interior_samples = $interiorSamples
    edge_mae = [ordered]@{}
    interior_mae = [ordered]@{}
}
foreach ($method in $low.Keys) {
    $result.edge_mae[$method] = $edgeAbsolute[$method] / $edgeSamples
    $result.interior_mae[$method] = $interiorAbsolute[$method] / $interiorSamples
}
if ($result.edge_mae.smaa_t2x -ge $result.edge_mae.off -or
    $result.edge_mae.msaa2x -ge $result.edge_mae.off -or
    $result.edge_mae.msaa4x -ge $result.edge_mae.off -or
    $result.edge_mae.msaa4x -gt $result.edge_mae.msaa2x * 1.02) {
    throw "MSAA did not improve edge agreement with the supersampled reference: $($result | ConvertTo-Json -Depth 5 -Compress)"
}
if ($result.interior_mae.msaa2x -gt $result.interior_mae.off + 0.0001 -or
    $result.interior_mae.msaa4x -gt $result.interior_mae.off + 0.0001) {
    throw "MSAA changed fully covered fixture interiors: $($result | ConvertTo-Json -Depth 5 -Compress)"
}
$json = $result | ConvertTo-Json -Depth 5
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'summary.json'), $json + "`n")
$json
