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
        "-I$shaderRoot",
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

# Every ordinary source must get fresh SPIR-V. SMAA's preset shaders are
# compiled separately below; a hand-maintained subset previously missed sky.vert.
$presetShaders = @(
    'smaa_edge.frag',
    'smaa_blend.frag',
    'smaa_neighborhood.frag',
    'smaa_neighborhood_history.frag',
    'motion_blur.frag'
)
$regularShaders = Get-ChildItem -LiteralPath $shaderRoot -File |
    Where-Object { $_.Extension -in @('.vert', '.tesc', '.tese', '.frag', '.comp') -and $_.Name -notin $presetShaders } |
    Sort-Object Name
foreach ($shader in $regularShaders) {
    $extraArguments = @()
    if ($shader.Name -eq 'workload.comp') {
        # Preserve the benchmark's existing inlined/SSA SPIR-V; rebuilding it
        # without size optimization introduced unrelated function/local traffic.
        $extraArguments = @('-Os')
    }
    Compile-Shader -Source $shader.Name -Output ($shader.Name + '.spv') -ExtraArguments $extraArguments
}
$subsurfaceGeometryShaders = @(
    'cube.frag',
    'cube_aa.frag',
    'scene.frag',
    'scene_aa.frag',
    'editor_model.frag',
    'editor_model_aa.frag',
    'sky.frag',
    'sky_aa.frag'
)
foreach ($shader in $subsurfaceGeometryShaders) {
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($shader)
    Compile-Shader `
        -Source $shader `
        -Output ($stem + '_sss.frag.spv') `
        -ExtraArguments @('-DVCR_SSS_OUTPUT=1')
}
Compile-Shader `
    -Source 'subsurface_blur.frag' `
    -Output 'subsurface_blur_vertical.frag.spv' `
    -ExtraArguments @('-DVCR_SSS_VERTICAL=1')
Compile-Shader `
    -Source 'motion_blur.frag' `
    -Output 'motion_blur_scene.frag.spv' `
    -ExtraArguments @('-DVCR_MOTION_BLUR_RECONSTRUCTED=0')
Compile-Shader `
    -Source 'motion_blur.frag' `
    -Output 'motion_blur_reconstructed.frag.spv' `
    -ExtraArguments @('-DVCR_MOTION_BLUR_RECONSTRUCTED=1')
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
        -Source "smaa_edge.frag" `
        -Output "smaa_edge_blurred_$quality.frag.spv" `
        -ExtraArguments ($sharedArguments + '-DVCR_SMAA_BLURRED_SOURCE=1')
    Compile-Shader `
        -Source "smaa_blend.frag" `
        -Output "smaa_blend_$quality.frag.spv" `
        -ExtraArguments $sharedArguments
}
Compile-Shader `
    -Source "smaa_neighborhood.frag" `
    -Output "smaa_neighborhood.frag.spv" `
    -ExtraArguments @("-I$smaaInclude")
Compile-Shader `
    -Source "smaa_neighborhood.frag" `
    -Output "smaa_neighborhood_blurred.frag.spv" `
    -ExtraArguments @("-I$smaaInclude", '-DVCR_SMAA_BLURRED_SOURCE=1')
Compile-Shader `
    -Source "smaa_neighborhood_history.frag" `
    -Output "smaa_neighborhood_history.frag.spv" `
    -ExtraArguments @("-I$smaaInclude")

if (-not $spirvVal) {
    Write-Host "Standalone spirv-val was not found; glslang's integrated SPIR-V Tools validator was used."
}
Write-Host "All selected shaders compiled and validated for Vulkan 1.3 / SPIR-V 1.5."
