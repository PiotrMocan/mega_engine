param(
    [Parameter(Mandatory = $true)]
    [string]$ExePath,
    [switch]$Validation,
    [ValidateSet('off', 'smaa', 'fsr3', 'msaa2x', 'msaa4x')][string]$AntiAliasing = 'off',
    [string]$ScreenshotPath = "",
    [string]$ExtraArguments = "",
    [float]$CameraDeltaX = 0,
    [float]$CameraDeltaY = 0,
    [ValidateSet('','cpu','gpu','direct')][string]$ExpectedBackend='',
    [ValidateSet('','off','forward','clustered')][string]$ExpectedLocalLighting='',
    [switch]$ExerciseDisplayControls,
    [switch]$ExerciseMsaa,
    [switch]$ExerciseSmaaT2x
)

$ErrorActionPreference = "Stop"
$expectedMsaaSamples = 0
if ($ExerciseMsaa -and $ExerciseSmaaT2x) {
    throw 'ExerciseMsaa and ExerciseSmaaT2x select different final AA modes'
}

$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $ExePath
$validationArgument = if ($Validation) {
    "--validation"
} else {
    "--no-validation"
}
$startInfo.Arguments =
    "--mcp-stdio $validationArgument --aa $AntiAliasing --width 640 --height 360 $ExtraArguments"
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

    $serialized = $Message | ConvertTo-Json -Compress -Depth 20
    $process.StandardInput.WriteLine($serialized)
    $process.StandardInput.Flush()
    if ($null -eq $ExpectedId) {
        return $null
    }

    $line = $process.StandardOutput.ReadLine()
    if ([string]::IsNullOrWhiteSpace($line)) {
        throw "MCP stdout closed before response $ExpectedId"
    }
    $response = $line | ConvertFrom-Json
    if ($response.id -ne $ExpectedId) {
        throw "Expected MCP response $ExpectedId, got $($response.id)"
    }
    if ($null -ne $response.error) {
        throw "MCP error $($response.error.code): $($response.error.message)"
    }
    return $response.result
}

try {
    $initialize = Send-McpMessage @{
        jsonrpc = "2.0"
        id = 1
        method = "initialize"
        params = @{
            protocolVersion = "2025-11-25"
            capabilities = @{}
            clientInfo = @{
                name = "vcr-windows-smoke"
                version = "1.0.0"
            }
        }
    } 1
    if ($initialize.protocolVersion -ne "2025-11-25") {
        throw "Unexpected MCP protocol version"
    }
    Send-McpMessage @{
        jsonrpc = "2.0"
        method = "notifications/initialized"
    }

    $toolList = Send-McpMessage @{
        jsonrpc = "2.0"
        id = 2
        method = "tools/list"
        params = @{}
    } 2
    $toolNames = @($toolList.tools | ForEach-Object { $_.name })
    foreach ($required in @(
        "debug.get_state",
        "debug.get_capabilities",
        "test.run_self_check",
        "capture.screenshot",
        "render.set_exposure",
        "render.set_color_grading",
        "render.set_subsurface",
        "render.set_motion_blur",
        "render.set_anti_aliasing",
        "input.key",
        "engine.quit"
    )) {
        if ($required -notin $toolNames) {
            throw "Required MCP tool is missing: $required"
        }
    }

    $selfCheck = Send-McpMessage @{
        jsonrpc = "2.0"
        id = 3
        method = "tools/call"
        params = @{
            name = "test.run_self_check"
            arguments = @{}
        }
    } 3
    if ($selfCheck.isError -or
        -not $selfCheck.structuredContent.passed) {
        throw "MCP runtime self-check failed"
    }

    if ($ExerciseDisplayControls) {
        $exposure = Send-McpMessage @{
            jsonrpc = '2.0'; id = 32; method = 'tools/call'
            params = @{ name = 'render.set_exposure'; arguments = @{
                mode = 'auto'; metering = 'center'; compensation_ev = 0
                minimum_ev = -10; maximum_ev = 10
                low_percentile = 0.02; high_percentile = 0.98
                brighten_seconds = 1.0; darken_seconds = 0.25
            } }
        } 32
        if ($exposure.isError -or $exposure.structuredContent.mode_requested -cne 'auto') {
            throw 'Typed exposure control failed'
        }
        $grading = Send-McpMessage @{
            jsonrpc = '2.0'; id = 33; method = 'tools/call'
            params = @{ name = 'render.set_color_grading'; arguments = @{
                mode = 'trilinear'; size = 32
            } }
        } 33
        if ($grading.isError -or $grading.structuredContent.mode_requested -cne 'trilinear' -or
            $grading.structuredContent.size -ne 32) {
            throw 'Typed color-grading control failed'
        }
        $subsurface = Send-McpMessage @{
            jsonrpc = '2.0'; id = 38; method = 'tools/call'
            params = @{ name = 'render.set_subsurface'; arguments = @{ mode = 'full' } }
        } 38
        if ($subsurface.isError -or $subsurface.structuredContent.mode_requested -cne 'full') {
            throw 'Typed subsurface control failed'
        }
        $motionBlur = Send-McpMessage @{
            jsonrpc = '2.0'; id = 39; method = 'tools/call'
            params = @{ name = 'render.set_motion_blur'; arguments = @{
                mode = 'all'; quality = 'low'; shutter = 'trailing'
                angle_degrees = 180; maximum_radius_pixels = 16
                camera_fade_start = 0; camera_fade_end = 0
            } }
        } 39
        $motionState = Send-McpMessage @{
            jsonrpc = '2.0'; id = 391; method = 'tools/call'
            params = @{ name = 'debug.get_state'; arguments = @{} }
        } 391
        if ($motionBlur.isError -or
            $motionBlur.structuredContent.mode_actual -cne 'all' -or
            $motionState.structuredContent.render.motion_blur.mode_actual -cne 'all' -or
            $motionState.structuredContent.render.motion_blur.quality -cne 'low') {
            throw 'Typed motion-blur control or state round-trip failed'
        }
        $motionBlurOff = Send-McpMessage @{
            jsonrpc = '2.0'; id = 392; method = 'tools/call'
            params = @{ name = 'render.set_motion_blur'; arguments = @{
                mode = 'off'; quality = 'high'; shutter = 'centered'
                angle_degrees = 180; maximum_radius_pixels = 32
                camera_fade_start = 0; camera_fade_end = 0
            } }
        } 392
        if ($motionBlurOff.isError) {
            throw 'Motion blur could not return to the default-off state'
        }
        $fixed = Send-McpMessage @{
            jsonrpc = '2.0'; id = 34; method = 'tools/call'
            params = @{ name = 'render.set_exposure'; arguments = @{
                mode = 'fixed'; metering = 'uniform'; compensation_ev = 1
                minimum_ev = -10; maximum_ev = 10
                low_percentile = 0.02; high_percentile = 0.98
                brighten_seconds = 1.0; darken_seconds = 0.25
            } }
        } 34
        if ($fixed.isError -or $fixed.structuredContent.mode_requested -cne 'fixed') {
            throw 'Fixed exposure switch failed'
        }
        $exposure = Send-McpMessage @{
            jsonrpc = '2.0'; id = 35; method = 'tools/call'
            params = @{ name = 'render.set_exposure'; arguments = @{
                mode = 'auto'; metering = 'center'; compensation_ev = 0
                minimum_ev = -10; maximum_ev = 10
                low_percentile = 0.02; high_percentile = 0.98
                brighten_seconds = 1.0; darken_seconds = 0.25
            } }
        } 35
        $grading = Send-McpMessage @{
            jsonrpc = '2.0'; id = 36; method = 'tools/call'
            params = @{ name = 'render.set_color_grading'; arguments = @{
                mode = 'tetrahedral'; size = 64
            } }
        } 36
        $resize = Send-McpMessage @{
            jsonrpc = '2.0'; id = 37; method = 'tools/call'
            params = @{ name = 'window.set_size'; arguments = @{ width = 704; height = 396 } }
        } 37
        if ($grading.isError -or $grading.structuredContent.mode_requested -cne 'tetrahedral' -or
            $grading.structuredContent.size -ne 64 -or $resize.isError) {
            throw 'Display mode-switch or resize control failed'
        }
    }

    if ($ExerciseMsaa) {
        $msaaCapabilities = Send-McpMessage @{
            jsonrpc='2.0'; id=40; method='tools/call'
            params=@{name='debug.get_capabilities'; arguments=@{}}
        } 40
        if ($msaaCapabilities.isError -or
            -not $msaaCapabilities.structuredContent.anti_aliasing.msaa_2x_available) {
            throw 'MSAA 2x is unavailable on the validation device'
        }
        $msaa2 = Send-McpMessage @{
            jsonrpc='2.0'; id=41; method='tools/call'
            params=@{name='render.set_anti_aliasing'; arguments=@{method='msaa2x'}}
        } 41
        if ($msaa2.isError -or $msaa2.structuredContent.method -cne 'msaa2x' -or
            $msaa2.structuredContent.sample_count -ne 2 -or
            $msaa2.structuredContent.color_resolve -cne 'average' -or
            $msaa2.structuredContent.depth_resolve -cne 'none') {
            throw 'Typed MSAA 2x control failed'
        }
        $resizeMsaa = Send-McpMessage @{
            jsonrpc='2.0'; id=49; method='tools/call'
            params=@{name='window.set_size'; arguments=@{width=704;height=396}}
        } 49
        if ($resizeMsaa.isError) { throw 'MSAA resize request failed' }
        $rejectSss = Send-McpMessage @{
            jsonrpc='2.0'; id=42; method='tools/call'
            params=@{name='render.set_subsurface'; arguments=@{mode='full'}}
        } 42
        if (-not $rejectSss.isError) {
            throw 'MSAA accepted the incompatible SSS full mode'
        }
        $off = Send-McpMessage @{
            jsonrpc='2.0'; id=43; method='tools/call'
            params=@{name='render.set_anti_aliasing'; arguments=@{method='off'}}
        } 43
        $sss = Send-McpMessage @{
            jsonrpc='2.0'; id=44; method='tools/call'
            params=@{name='render.set_subsurface'; arguments=@{mode='full'}}
        } 44
        $rejectMsaa = Send-McpMessage @{
            jsonrpc='2.0'; id=45; method='tools/call'
            params=@{name='render.set_anti_aliasing'; arguments=@{method='msaa2x'}}
        } 45
        if ($off.isError -or $sss.isError -or -not $rejectMsaa.isError) {
            throw 'SSS/MSAA exclusion did not preserve the active mode'
        }
        $null = Send-McpMessage @{
            jsonrpc='2.0'; id=46; method='tools/call'
            params=@{name='render.set_subsurface'; arguments=@{mode='off'}}
        } 46
        if ($msaaCapabilities.structuredContent.anti_aliasing.msaa_4x_available) {
            $msaa4 = Send-McpMessage @{
                jsonrpc='2.0'; id=47; method='tools/call'
                params=@{name='render.set_anti_aliasing'; arguments=@{method='msaa4x'}}
            } 47
            if ($msaa4.isError -or $msaa4.structuredContent.sample_count -ne 4) {
                throw 'Typed MSAA 4x control failed'
            }
        }
        $msaa2 = Send-McpMessage @{
            jsonrpc='2.0'; id=48; method='tools/call'
            params=@{name='render.set_anti_aliasing'; arguments=@{method='msaa2x'}}
        } 48
        if ($msaa2.isError) { throw 'Failed to restore MSAA 2x for capture' }
        $expectedMsaaSamples = 2
    }

    if ($ExerciseSmaaT2x) {
        $smaaCapabilities = Send-McpMessage @{
            jsonrpc='2.0'; id=50; method='tools/call'
            params=@{name='debug.get_capabilities'; arguments=@{}}
        } 50
        if ($smaaCapabilities.isError -or
            't2x' -notin $smaaCapabilities.structuredContent.anti_aliasing.smaa_modes -or
            $smaaCapabilities.structuredContent.anti_aliasing.smaa_edge_detection -cne 'color') {
            throw 'SMAA T2x capability contract is missing'
        }
        $rejectModeWithoutSmaa = Send-McpMessage @{
            jsonrpc='2.0'; id=51; method='tools/call'
            params=@{name='render.set_anti_aliasing'; arguments=@{method='off';smaa_mode='t2x'}}
        } 51
        $rejectInvalidMode = Send-McpMessage @{
            jsonrpc='2.0'; id=52; method='tools/call'
            params=@{name='render.set_anti_aliasing'; arguments=@{method='smaa';smaa_mode='2x'}}
        } 52
        if (-not $rejectModeWithoutSmaa.isError -or -not $rejectInvalidMode.isError) {
            throw 'Invalid SMAA mode combinations were accepted'
        }
        $t2x = Send-McpMessage @{
            jsonrpc='2.0'; id=53; method='tools/call'
            params=@{name='render.set_anti_aliasing'; arguments=@{method='smaa';smaa_mode='t2x'}}
        } 53
        if ($t2x.isError -or $t2x.structuredContent.method -cne 'smaa' -or
            $t2x.structuredContent.smaa_mode -cne 't2x' -or
            -not $t2x.structuredContent.smaa_temporal -or
            $t2x.structuredContent.smaa_history_valid -or
            $t2x.structuredContent.smaa_history_incremental_bytes -ne 0 -or
            -not $t2x.structuredContent.smaa_history_reuses_fsr_output) {
            throw 'Typed SMAA T2x control failed'
        }
        $resizeT2x = Send-McpMessage @{
            jsonrpc='2.0'; id=54; method='tools/call'
            params=@{name='window.set_size'; arguments=@{width=704;height=396}}
        } 54
        if ($resizeT2x.isError) { throw 'SMAA T2x resize request failed' }
    }

    if ($CameraDeltaX -ne 0 -or $CameraDeltaY -ne 0) {
        $null = Send-McpMessage @{
            jsonrpc = '2.0'; id = 30; method = 'tools/call'
            params = @{ name = 'input.mouse_move'; arguments = @{ delta_x = $CameraDeltaX; delta_y = $CameraDeltaY } }
        } 30
    }
    $screenshot = Send-McpMessage @{
        jsonrpc = "2.0"
        id = 4
        method = "tools/call"
        params = @{
            name = "capture.screenshot"
            arguments = @{}
        }
    } 4
    if ($screenshot.isError) {
        throw "MCP screenshot failed: $($screenshot.content[0].text)"
    }
    $image = $screenshot.content |
        Where-Object { $_.type -eq "image" } |
        Select-Object -First 1
    if ($null -eq $image -or $image.mimeType -ne "image/png") {
        throw "MCP screenshot did not return PNG ImageContent"
    }
    $png = [Convert]::FromBase64String($image.data)
    $pngSignature = [byte[]](
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)
    for ($index = 0; $index -lt $pngSignature.Length; ++$index) {
        if ($png[$index] -ne $pngSignature[$index]) {
            throw "MCP screenshot has an invalid PNG signature"
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($ScreenshotPath)) {
        [System.IO.File]::WriteAllBytes($ScreenshotPath, $png)
    }

    $state = Send-McpMessage @{
        jsonrpc='2.0'; id=31; method='tools/call'
        params=@{name='debug.get_state'; arguments=@{}}
    } 31
    if ($state.isError) { throw 'Unable to read renderer state after capture' }
    if ($ExerciseMsaa -and
        ($state.structuredContent.render.anti_aliasing.method -cne 'msaa2x' -or
         $state.structuredContent.render.anti_aliasing.sample_count -ne $expectedMsaaSamples -or
         $state.structuredContent.render.anti_aliasing.color_resolve -cne 'average' -or
         $state.structuredContent.render.anti_aliasing.depth_resolve -cne 'none' -or
         $state.structuredContent.render.anti_aliasing.resident_bytes -le 0 -or
         $state.structuredContent.render.anti_aliasing.target_width -ne 704 -or
         $state.structuredContent.render.anti_aliasing.target_height -ne 396)) {
        throw 'MSAA resources and resolve contract did not converge before capture'
    }
    if ($ExerciseSmaaT2x -and
        ($state.structuredContent.render.anti_aliasing.method -cne 'smaa' -or
         $state.structuredContent.render.anti_aliasing.smaa_mode -cne 't2x' -or
         -not $state.structuredContent.render.anti_aliasing.smaa_temporal -or
         -not $state.structuredContent.render.anti_aliasing.smaa_history_valid -or
         $state.structuredContent.render.anti_aliasing.smaa_history_resident_bytes -le 0 -or
         $state.structuredContent.render.anti_aliasing.smaa_history_incremental_bytes -ne 0 -or
         -not $state.structuredContent.render.anti_aliasing.smaa_history_reuses_fsr_output -or
         $state.structuredContent.render.anti_aliasing.smaa_history_width -ne 704 -or
         $state.structuredContent.render.anti_aliasing.smaa_history_height -ne 396)) {
        throw "SMAA T2x history and resize contract did not converge before capture: $($state.structuredContent.render.anti_aliasing | ConvertTo-Json -Compress)"
    }
    if ($ExerciseDisplayControls -and
        ($state.structuredContent.render.exposure.mode_requested -cne 'auto' -or
         $state.structuredContent.render.color_grading.mode_requested -cne 'tetrahedral' -or
         $state.structuredContent.render.color_grading.size -ne 64 -or
         $state.structuredContent.render.subsurface.mode_requested -cne 'full' -or
         $state.structuredContent.render.subsurface.mode_actual -cne 'full' -or
         -not $state.structuredContent.render.subsurface.provisioned -or
         $state.structuredContent.render.subsurface.resident_bytes -le 0)) {
        throw 'Display controls did not round-trip through debug.get_state'
    }
    if ($ExpectedBackend -and $state.structuredContent.render.visibility_backend -ne $ExpectedBackend) {
        throw "Expected $ExpectedBackend visibility backend, got $($state.structuredContent.render.visibility_backend)"
    }
    $localLighting = $state.structuredContent.render.lighting
    if ($ExpectedLocalLighting -and $localLighting.local_light_backend_actual -cne $ExpectedLocalLighting) {
        throw "Expected $ExpectedLocalLighting local-light backend, got $($localLighting.local_light_backend_actual)"
    }

    $quit = Send-McpMessage @{
        jsonrpc = "2.0"
        id = 5
        method = "tools/call"
        params = @{
            name = "engine.quit"
            arguments = @{}
        }
    } 5
    if (-not $quit.structuredContent.shutting_down) {
        throw "engine.quit was not acknowledged"
    }
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(15000)) {
        throw "MCP renderer did not exit after engine.quit"
    }
    $stderr = $stderrTask.Result
    if ($process.ExitCode -ne 0) {
        throw "MCP renderer exited with $($process.ExitCode):`n$stderr"
    }
    if ($stderr -match '\[Vulkan ERROR\]|Validation Error|VUID-|SYNC-HAZARD|hazard detected|FidelityFX error:|AddressSanitizer|Live GPU allocations: [1-9]') {
        throw "Vulkan validation reported an error:`n$stderr"
    }

    [pscustomobject]@{
        protocol = $initialize.protocolVersion
        tools = $toolNames.Count
        self_check = $true
        screenshot_width = $screenshot.structuredContent.width
        screenshot_height = $screenshot.structuredContent.height
        screenshot_bytes = $png.Length
        validation = [bool]$Validation
        exposure = $state.structuredContent.render.exposure
        color_grading = $state.structuredContent.render.color_grading
        subsurface = $state.structuredContent.render.subsurface
        anti_aliasing = $state.structuredContent.render.anti_aliasing
        visibility_backend = $state.structuredContent.render.visibility_backend
        visibility = $state.structuredContent.render.visibility
        local_light_backend_requested = $localLighting.local_light_backend_requested
        local_light_backend_actual = $localLighting.local_light_backend_actual
        local_light_count = $localLighting.local_light_count
        local_light_assignments = $localLighting.local_light_assignments
        local_light_maximum_list = $localLighting.local_light_maximum_list
        local_light_overflow_clusters = $localLighting.local_light_overflow_clusters
        local_light_dropped = $localLighting.local_light_dropped
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
