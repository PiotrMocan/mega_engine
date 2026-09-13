param(
    [Parameter(Mandatory = $true)]
    [string]$ExePath,
    [switch]$Validation,
    [ValidateRange(320, 16384)]
    [int]$Width = 800,
    [ValidateRange(240, 16384)]
    [int]$Height = 600,
    [ValidateSet('forward','clustered')]
    [string]$LocalLighting = 'clustered',
    [string]$ScreenshotPath,
    [string]$MaterialPath
)

$ErrorActionPreference = "Stop"

$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $ExePath
$validationArgument = if ($Validation) { "--validation" } else { "--no-validation" }
$startInfo.Arguments = "--mcp-stdio $validationArgument --width $Width --height $Height --local-lighting $LocalLighting"
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true

$process = New-Object System.Diagnostics.Process
$process.StartInfo = $startInfo
if (-not $process.Start()) {
    throw "Unable to start $ExePath"
}
$stderrTask = $process.StandardError.ReadToEndAsync()

function Send-McpMessage {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$Message,
        [Nullable[int]]$ExpectedId = $null
    )
    $process.StandardInput.WriteLine(
        ($Message | ConvertTo-Json -Compress -Depth 20))
    $process.StandardInput.Flush()
    if ($null -eq $ExpectedId) {
        return $null
    }
    $line = $process.StandardOutput.ReadLine()
    if ([string]::IsNullOrWhiteSpace($line)) {
        throw "MCP stdout closed before response $ExpectedId"
    }
    $response = $line | ConvertFrom-Json
    if ($response.id -ne $ExpectedId -or $null -ne $response.error) {
        throw "Unexpected MCP response: $line"
    }
    return $response.result
}

try {
    $initialize = Send-McpMessage @{
        jsonrpc = "2.0"; id = 1; method = "initialize"
        params = @{
            protocolVersion = "2025-11-25"; capabilities = @{}
            clientInfo = @{ name = "vcr-editor-smoke"; version = "1.0.0" }
        }
    } 1
    Send-McpMessage @{ jsonrpc = "2.0"; method = "notifications/initialized" }

    $toolList = Send-McpMessage @{
        jsonrpc = "2.0"; id = 2; method = "tools/list"; params = @{}
    } 2
    $toolNames = @($toolList.tools | ForEach-Object { $_.name })
    foreach ($required in @(
        "editor.get_state",
        "render.set_exposure",
        "render.set_color_grading",
        "render.set_subsurface",
        "render.set_motion_blur",
        "render.set_anti_aliasing",
        "editor.scene.list_nodes",
        "editor.scene.create_node",
        "editor.scene.duplicate_node",
        "editor.selection.set",
        "editor.camera.focus_selection",
        "editor.landscape.create_flat_ground",
        "editor.landscape.scatter_model",
        "editor.assets.list",
        "editor.assets.import",
        "editor.material.get",
        "editor.material.set_tessellation",
        "editor.history.undo",
        "capture.screenshot",
        "engine.quit"
    )) {
        if ($required -notin $toolNames) {
            throw "Required editor MCP tool is missing: $required"
        }
    }

    $before = Send-McpMessage @{
        jsonrpc = "2.0"; id = 3; method = "tools/call"
        params = @{ name = "editor.get_state"; arguments = @{} }
    } 3
    if ($before.isError) { throw "Unable to read editor state" }

    if (-not [string]::IsNullOrWhiteSpace($MaterialPath)) {
        $materialBefore = Send-McpMessage @{
            jsonrpc = "2.0"; id = 2010; method = "tools/call"
            params = @{
                name = "editor.material.get"
                arguments = @{ path = $MaterialPath }
            }
        } 2010
        if ($materialBefore.isError -or
            $null -eq $materialBefore.structuredContent.material.displacement_texture) {
            throw "Unable to inspect tessellated material"
        }
        $original = $materialBefore.structuredContent.material.tessellation
        $temporaryTarget = if ([double]$original.target_edge_pixels -lt 1024.0) {
            [double]$original.target_edge_pixels + 1.0
        } else { 1023.0 }
        $materialChanged = Send-McpMessage @{
            jsonrpc = "2.0"; id = 2011; method = "tools/call"
            params = @{
                name = "editor.material.set_tessellation"
                arguments = @{
                    path = $MaterialPath
                    enabled = [bool]$original.enabled
                    displacement_scale = [double]$original.displacement_scale
                    displacement_center = [double]$original.displacement_center
                    target_edge_pixels = $temporaryTarget
                    maximum_factor = [double]$original.maximum_factor
                    up_facing_only = [bool]$original.up_facing_only
                    normal_threshold = [double]$original.normal_threshold
                    uv_edge_fade = [double]$original.uv_edge_fade
                }
            }
        } 2011
        if ($materialChanged.isError -or
            [double]$materialChanged.structuredContent.material.tessellation.target_edge_pixels -ne
                $temporaryTarget) {
            throw "Material tessellation mutation failed"
        }
        $materialRestored = Send-McpMessage @{
            jsonrpc = "2.0"; id = 2012; method = "tools/call"
            params = @{
                name = "editor.material.set_tessellation"
                arguments = @{
                    path = $MaterialPath
                    enabled = [bool]$original.enabled
                    displacement_scale = [double]$original.displacement_scale
                    displacement_center = [double]$original.displacement_center
                    target_edge_pixels = [double]$original.target_edge_pixels
                    maximum_factor = [double]$original.maximum_factor
                    up_facing_only = [bool]$original.up_facing_only
                    normal_threshold = [double]$original.normal_threshold
                    uv_edge_fade = [double]$original.uv_edge_fade
                }
            }
        } 2012
        if ($materialRestored.isError -or
            [double]$materialRestored.structuredContent.material.tessellation.target_edge_pixels -ne
                [double]$original.target_edge_pixels) {
            throw "Material tessellation restore failed"
        }
    }

    $exposure = Send-McpMessage @{
        jsonrpc = '2.0'; id = 101; method = 'tools/call'
        params = @{ name = 'render.set_exposure'; arguments = @{
            mode = 'auto'; metering = 'center'; compensation_ev = 0
            minimum_ev = -10; maximum_ev = 10
            low_percentile = 0.02; high_percentile = 0.98
            brighten_seconds = 1.0; darken_seconds = 0.25
        } }
    } 101
    if ($exposure.isError -or $exposure.structuredContent.mode_requested -cne 'auto') {
        throw 'Editor exposure control failed'
    }
    $grading = Send-McpMessage @{
        jsonrpc = '2.0'; id = 102; method = 'tools/call'
        params = @{ name = 'render.set_color_grading'; arguments = @{
            mode = 'trilinear'; size = 32
        } }
    } 102
    if ($grading.isError -or $grading.structuredContent.mode_requested -cne 'trilinear' -or
        $grading.structuredContent.size -ne 32) {
        throw 'Editor color-grading control failed'
    }
    $subsurface = Send-McpMessage @{
        jsonrpc = '2.0'; id = 103; method = 'tools/call'
        params = @{ name = 'render.set_subsurface'; arguments = @{ mode = 'full' } }
    } 103
    if ($subsurface.isError -or $subsurface.structuredContent.mode_requested -cne 'full') {
        throw 'Editor subsurface control failed'
    }
    $motionBlur = Send-McpMessage @{
        jsonrpc = '2.0'; id = 1031; method = 'tools/call'
        params = @{ name = 'render.set_motion_blur'; arguments = @{
            mode = 'camera'; quality = 'high'; shutter = 'centered'
            angle_degrees = 180; maximum_radius_pixels = 24
            camera_fade_start = 0; camera_fade_end = 0
        } }
    } 1031
    $motionState = Send-McpMessage @{
        jsonrpc = '2.0'; id = 1032; method = 'tools/call'
        params = @{ name = 'editor.get_state'; arguments = @{} }
    } 1032
    if ($motionBlur.isError -or
        $motionBlur.structuredContent.mode_actual -cne 'camera' -or
        $motionState.structuredContent.motion_blur.mode_actual -cne 'camera' -or
        $motionState.structuredContent.motion_blur.maximum_radius_pixels -ne 24) {
        throw 'Editor motion-blur control or state round-trip failed'
    }
    $motionBlurOff = Send-McpMessage @{
        jsonrpc = '2.0'; id = 1033; method = 'tools/call'
        params = @{ name = 'render.set_motion_blur'; arguments = @{
            mode = 'off'; quality = 'high'; shutter = 'centered'
            angle_degrees = 180; maximum_radius_pixels = 32
            camera_fade_start = 0; camera_fade_end = 0
        } }
    } 1033
    if ($motionBlurOff.isError) {
        throw 'Editor motion blur could not return to default off'
    }
    $rejectedMsaa = Send-McpMessage @{
        jsonrpc = '2.0'; id = 104; method = 'tools/call'
        params = @{ name = 'render.set_anti_aliasing'; arguments = @{ method = 'msaa2x' } }
    } 104
    if (-not $rejectedMsaa.isError) {
        throw 'Editor accepted MSAA while SSS full was requested'
    }
    $subsurfaceOff = Send-McpMessage @{
        jsonrpc = '2.0'; id = 105; method = 'tools/call'
        params = @{ name = 'render.set_subsurface'; arguments = @{ mode = 'off' } }
    } 105
    $msaa2 = Send-McpMessage @{
        jsonrpc = '2.0'; id = 106; method = 'tools/call'
        params = @{ name = 'render.set_anti_aliasing'; arguments = @{ method = 'msaa2x' } }
    } 106
    if ($subsurfaceOff.isError -or $msaa2.isError -or
        $msaa2.structuredContent.sample_count -ne 2) {
        throw 'Editor MSAA 2x round-trip failed'
    }
    $invalidSmaaMode = Send-McpMessage @{
        jsonrpc = '2.0'; id = 107; method = 'tools/call'
        params = @{ name = 'render.set_anti_aliasing'; arguments = @{ method = 'off'; smaa_mode = 't2x' } }
    } 107
    if (-not $invalidSmaaMode.isError) {
        throw 'Editor accepted smaa_mode with a non-SMAA method'
    }
    $smaaT2x = Send-McpMessage @{
        jsonrpc = '2.0'; id = 109; method = 'tools/call'
        params = @{ name = 'render.set_anti_aliasing'; arguments = @{ method = 'smaa'; smaa_mode = 't2x' } }
    } 109
    if ($smaaT2x.isError -or $smaaT2x.structuredContent.smaa_mode -cne 't2x' -or
        -not $smaaT2x.structuredContent.smaa_temporal -or
        $smaaT2x.structuredContent.smaa_history_valid -or
        $smaaT2x.structuredContent.smaa_history_incremental_bytes -ne 0) {
        throw 'Editor SMAA T2x round-trip failed'
    }
    $subsurface = Send-McpMessage @{
        jsonrpc = '2.0'; id = 108; method = 'tools/call'
        params = @{ name = 'render.set_subsurface'; arguments = @{ mode = 'full' } }
    } 108
    if ($subsurface.isError) { throw 'Editor failed to restore SSS after MSAA' }

    $assets = Send-McpMessage @{
        jsonrpc = "2.0"; id = 4; method = "tools/call"
        params = @{
            name = "editor.assets.list"
            arguments = @{ directory = ""; offset = 0; limit = 20 }
        }
    } 4
    if ($assets.isError) { throw "Unable to list editor assets" }

    $folder = Send-McpMessage @{
        jsonrpc = "2.0"; id = 5; method = "tools/call"
        params = @{
            name = "editor.scene.create_node"
            arguments = @{ name = "MCP Smoke"; kind = "folder" }
        }
    } 5
    if ($folder.isError) { throw "Unable to create editor node" }
    $folderId = $folder.structuredContent.node.id

    $ground = Send-McpMessage @{
        jsonrpc = "2.0"; id = 6; method = "tools/call"
        params = @{
            name = "editor.landscape.create_flat_ground"
            arguments = @{
                name = "MCP Ground"; parent = $folderId
                center = @(0, -1, 0); width = 20; depth = 12
            }
        }
    } 6
    if ($ground.isError) { throw "Unable to create flat terrain" }
    $groundId = $ground.structuredContent.node.id

    $update = Send-McpMessage @{
        jsonrpc = "2.0"; id = 7; method = "tools/call"
        params = @{
            name = "editor.scene.update_node"
            arguments = @{
                id = $groundId
                base_color = @(0.2, 0.45, 0.1, 1.0)
                roughness = 0.9
                model = 'subsurface'
                subsurface_profile = 'medium'
                subsurface_strength = 0.5
            }
        }
    } 7
    if ($update.isError -or -not $update.structuredContent.changed -or
        $update.structuredContent.node.pbr.model -cne 'subsurface' -or
        $update.structuredContent.node.pbr.subsurface_profile -cne 'medium' -or
        $update.structuredContent.node.pbr.subsurface_strength -ne 0.5) {
        throw "Unable to update authored ground material"
    }

    $selection = Send-McpMessage @{
        jsonrpc = "2.0"; id = 8; method = "tools/call"
        params = @{
            name = "editor.selection.set"
            arguments = @{ id = $groundId }
        }
    } 8
    if ($selection.isError -or
        $selection.structuredContent.selection.index -ne $groundId.index) {
        throw "Unable to select the authored ground"
    }

    $duplicate = Send-McpMessage @{
        jsonrpc = "2.0"; id = 9; method = "tools/call"
        params = @{
            name = "editor.scene.duplicate_node"
            arguments = @{ id = $groundId }
        }
    } 9
    if ($duplicate.isError -or
        $duplicate.structuredContent.duplicated_nodes -ne 1) {
        throw "Unable to duplicate the selected authored ground"
    }

    $focus = Send-McpMessage @{
        jsonrpc = "2.0"; id = 10; method = "tools/call"
        params = @{
            name = "editor.camera.focus_selection"
            arguments = @{}
        }
    } 10
    if ($focus.isError) { throw "Unable to focus the editor selection" }

    $nodes = Send-McpMessage @{
        jsonrpc = "2.0"; id = 11; method = "tools/call"
        params = @{
            name = "editor.scene.list_nodes"
            arguments = @{ offset = 0; limit = 100 }
        }
    } 11
    if ($nodes.isError -or
        $nodes.structuredContent.total -ne $before.structuredContent.node_count + 3) {
        throw "Editor scene mutations are not visible through list_nodes"
    }

    $point = Send-McpMessage @{
        jsonrpc = "2.0"; id = 12; method = "tools/call"
        params = @{
            name = "editor.scene.create_node"
            arguments = @{
                name = "MCP Point Light"; kind = "point_light"; parent = $folderId
                translation = @(-2, 3, -4); light_color = @(1.0, 0.25, 0.1)
                intensity_candela = 180; light_range = 12
            }
        }
    } 12
    if ($point.isError -or $point.structuredContent.node.light.intensity_candela -ne 180) {
        throw "Unable to create a typed point light"
    }

    $spot = Send-McpMessage @{
        jsonrpc = "2.0"; id = 13; method = "tools/call"
        params = @{
            name = "editor.scene.create_node"
            arguments = @{
                name = "MCP Spot Light"; kind = "spot_light"; parent = $folderId
                translation = @(2, 4, -2); rotation_degrees = @(-45, 0, 0)
                light_color = @(0.1, 0.35, 1.0); intensity_candela = 240
                light_range = 18; inner_cone_degrees = 10; outer_cone_degrees = 28
            }
        }
    } 13
    if ($spot.isError -or $spot.structuredContent.node.light.outer_cone_degrees -ne 28) {
        throw "Unable to create a typed spot light"
    }
    $spotId = $spot.structuredContent.node.id

    $spotPatch = Send-McpMessage @{
        jsonrpc = "2.0"; id = 14; method = "tools/call"
        params = @{
            name = "editor.scene.update_node"
            arguments = @{ id = $spotId; intensity_candela = 320; inner_cone_degrees = 12 }
        }
    } 14
    if ($spotPatch.isError -or -not $spotPatch.structuredContent.changed -or
        $spotPatch.structuredContent.node.light.intensity_candela -ne 320 -or
        $spotPatch.structuredContent.node.light.outer_cone_degrees -ne 28) {
        throw "Partial spot-light patch did not preserve the remaining cone fields"
    }

    $rejectedSpot = Send-McpMessage @{
        jsonrpc = "2.0"; id = 15; method = "tools/call"
        params = @{
            name = "editor.scene.update_node"
            arguments = @{ id = $spotId; inner_cone_degrees = 40 }
        }
    } 15
    if (-not $rejectedSpot.isError) {
        throw "Inverted spot cone was accepted"
    }

    $lightNodes = Send-McpMessage @{
        jsonrpc = "2.0"; id = 16; method = "tools/call"
        params = @{
            name = "editor.scene.list_nodes"
            arguments = @{ offset = 0; limit = 100 }
        }
    } 16
    if ($lightNodes.isError -or
        $lightNodes.structuredContent.total -ne $before.structuredContent.node_count + 5) {
        throw "Point and spot lights are not visible through list_nodes"
    }

    $screenshot = Send-McpMessage @{
        jsonrpc = "2.0"; id = 17; method = "tools/call"
        params = @{ name = "capture.screenshot"; arguments = @{} }
    } 17
    $image = $screenshot.content |
        Where-Object { $_.type -eq "image" } |
        Select-Object -First 1
    if ($screenshot.isError -or $null -eq $image -or $image.mimeType -ne "image/png") {
        throw "Editor screenshot did not return PNG ImageContent"
    }
    if (-not [string]::IsNullOrWhiteSpace($ScreenshotPath)) {
        [System.IO.File]::WriteAllBytes(
            $ScreenshotPath,
            [Convert]::FromBase64String($image.data))
    }

    $clusterState = Send-McpMessage @{
        jsonrpc = "2.0"; id = 18; method = "tools/call"
        params = @{ name = "editor.get_state"; arguments = @{} }
    } 18
    if ($clusterState.isError -or
        $clusterState.structuredContent.local_light_backend_actual -cne $LocalLighting -or
        $clusterState.structuredContent.local_light_count -ne 2 -or
        ($LocalLighting -eq 'clustered' -and
         $clusterState.structuredContent.local_light_assignments -le 0) -or
        $clusterState.structuredContent.local_light_dropped -ne 0) {
        throw "Requested local-light backend did not produce valid statistics"
    }
    if ($clusterState.structuredContent.exposure.mode_requested -cne 'auto' -or
        $clusterState.structuredContent.color_grading.mode_requested -cne 'trilinear' -or
        $clusterState.structuredContent.color_grading.size -ne 32 -or
        $clusterState.structuredContent.subsurface.mode_requested -cne 'full' -or
         $clusterState.structuredContent.subsurface.mode_actual -cne 'full' -or
         -not $clusterState.structuredContent.subsurface.provisioned -or
         $clusterState.structuredContent.subsurface.resident_bytes -le 0 -or
         $clusterState.structuredContent.anti_aliasing.method -cne 'smaa' -or
         $clusterState.structuredContent.anti_aliasing.smaa_mode -cne 't2x' -or
         -not $clusterState.structuredContent.anti_aliasing.smaa_temporal -or
         -not $clusterState.structuredContent.anti_aliasing.smaa_history_valid -or
         $clusterState.structuredContent.anti_aliasing.smaa_history_resident_bytes -le 0 -or
         $clusterState.structuredContent.anti_aliasing.smaa_history_incremental_bytes -ne 0) {
        throw 'Editor display controls did not round-trip through editor.get_state'
    }

    $undo = Send-McpMessage @{
        jsonrpc = "2.0"; id = 19; method = "tools/call"
        params = @{ name = "editor.history.undo"; arguments = @{} }
    } 19
    if ($undo.isError -or -not $undo.structuredContent.undone) {
        throw "Editor undo failed"
    }

    $quit = Send-McpMessage @{
        jsonrpc = "2.0"; id = 20; method = "tools/call"
        params = @{ name = "engine.quit"; arguments = @{} }
    } 20
    if (-not $quit.structuredContent.shutting_down) {
        throw "engine.quit was not acknowledged"
    }
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(15000)) {
        throw "MCP editor did not exit after engine.quit"
    }
    $stderr = $stderrTask.Result
    if ($process.ExitCode -ne 0) {
        throw "MCP editor exited with $($process.ExitCode):`n$stderr"
    }
    if ($stderr -match '\[Vulkan ERROR\]|Validation Error|VUID-|SYNC-HAZARD|AddressSanitizer|Live GPU allocations: [1-9]') {
        throw "Editor validation reported an error:`n$stderr"
    }

    [pscustomobject]@{
        protocol = $initialize.protocolVersion
        tools = $toolNames.Count
        initial_nodes = $before.structuredContent.node_count
        listed_nodes = $nodes.structuredContent.total
        listed_assets = $assets.structuredContent.total
        screenshot_width = $screenshot.structuredContent.width
        screenshot_height = $screenshot.structuredContent.height
        validation = [bool]$Validation
        exposure_mode = $exposure.structuredContent.mode_actual
        color_grading = $grading.structuredContent.mode_actual
        subsurface = $clusterState.structuredContent.subsurface
        exit_code = $process.ExitCode
    } | ConvertTo-Json -Compress
}
finally {
    if (-not $process.HasExited) {
        $process.Kill()
        $process.WaitForExit()
    }
    $process.Dispose()
}
