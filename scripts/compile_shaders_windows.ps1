$ErrorActionPreference = "Stop"

$rendererRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$shaderRoot = Join-Path $rendererRoot "shaders"
$outputRoot = Join-Path $shaderRoot "spv"
$smaaInclude = Join-Path $shaderRoot "third_party\smaa"

function Resolve-VulkanTool {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [switch]$AllowMissing
    )

    if ($env:VULKAN_SDK) {
        $sdkCandidate = Join-Path $env:VULKAN_SDK "Bin\$Name"
        if (Test-Path -LiteralPath $sdkCandidate) {
            return $sdkCandidate
        }
    }
    $localCandidate = Join-Path $rendererRoot ".tools\glslang-main\bin\$Name"
    if (Test-Path -LiteralPath $localCandidate) {
        return $localCandidate
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    if ($AllowMissing) {
        return $null
    }
    throw "$Name was not found. Install the Vulkan SDK or add its Bin directory to PATH."
}

$glslang = Resolve-VulkanTool "glslangValidator.exe"
$spirvVal = Resolve-VulkanTool "spirv-val.exe" -AllowMissing
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

function Compile-Shader {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Output,
        [string[]]$ExtraArguments = @()
    )

    $sourcePath = Join-Path $shaderRoot $Source
    $outputPath = Join-Path $outputRoot $Output
    $arguments = @(
        "-V",
        "--target-env", "vulkan1.3",
        "--target-env", "spirv1.5",
        "--spirv-val",
        "-o", $outputPath
    ) + $ExtraArguments + @($sourcePath)
    & $glslang @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "glslangValidator failed for $Source"
    }
    if ($spirvVal) {
        & $spirvVal --target-env vulkan1.3 $outputPath
        if ($LASTEXITCODE -ne 0) {
            throw "spirv-val failed for $Output"
        }
    }
}

$regularShaders = @(
    @("cube_aa.vert", "cube_aa.vert.spv"),
    @("cube_aa.frag", "cube_aa.frag.spv"),
    @("scene_aa.vert", "scene_aa.vert.spv"),
    @("scene_aa.frag", "scene_aa.frag.spv"),
    @("sky_aa.frag", "sky_aa.frag.spv"),
    @("fullscreen.vert", "fullscreen.vert.spv"),
    @("composite.frag", "composite.frag.spv"),
    @("hud.frag", "hud.frag.spv")
)
foreach ($shader in $regularShaders) {
    Compile-Shader -Source $shader[0] -Output $shader[1]
}

$smaaPresets = @(
    @("low", "SMAA_PRESET_LOW"),
    @("medium", "SMAA_PRESET_MEDIUM"),
    @("high", "SMAA_PRESET_HIGH"),
    @("ultra", "SMAA_PRESET_ULTRA")
)
foreach ($preset in $smaaPresets) {
    $quality = $preset[0]
    $define = "-D$($preset[1])=1"
    $sharedArguments = @("-I$smaaInclude", $define)
    Compile-Shader `
        -Source "smaa_edge.frag" `
        -Output "smaa_edge_$quality.frag.spv" `
        -ExtraArguments $sharedArguments
    Compile-Shader `
        -Source "smaa_blend.frag" `
        -Output "smaa_blend_$quality.frag.spv" `
        -ExtraArguments $sharedArguments
}
Compile-Shader `
    -Source "smaa_neighborhood.frag" `
    -Output "smaa_neighborhood.frag.spv" `
    -ExtraArguments @("-I$smaaInclude")

if (-not $spirvVal) {
    Write-Host "Standalone spirv-val was not found; glslang's integrated SPIR-V Tools validator was used."
}
Write-Host "All anti-aliasing shaders compiled and validated for Vulkan 1.3 / SPIR-V 1.5."
