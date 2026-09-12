param(
    [Parameter(Mandatory = $true)][string]$ExePath,
    [ValidateRange(1, 100)][int]$Startups = 3,
    [ValidateRange(1, 100)][int]$Cycles = 2,
    [ValidateSet('canonical','animated','stress','streaming','lighting','shadow-resolution')][string]$Scenario='canonical',
    [ValidateSet('voxel','off','legacy')][string]$ShadowLighting='voxel'
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $ExePath).Path
$tree = Split-Path (Split-Path $exe -Parent) -Parent
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$allowedTrees = @('windows-debug', 'windows-release', 'windows-asan') |
    ForEach-Object { Join-Path $root "build\$_" }
if ($tree -notin $allowedTrees) {
    throw 'Use an executable in one of the three native Windows build trees.'
}
$outputDirectory = Join-Path $tree "render-validation/$Scenario"
if ($Scenario -eq 'shadow-resolution') { $outputDirectory += "-$ShadowLighting" }
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

for ($startup = 1; $startup -le $Startups; ++$startup) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $exe
    $info.WorkingDirectory = $outputDirectory
    $info.Arguments = '--mcp-stdio --validation --aa off --width 640 --height 360'
    if ($Scenario -eq 'shadow-resolution') {
        $info.Arguments += ' --day-cycle off --time-of-day 14'
        if ($ShadowLighting -eq 'legacy') { $info.Arguments += ' --lighting legacy' }
        else { $info.Arguments += " --gi $ShadowLighting" }
    }
    if ($Scenario -eq 'lighting') { $info.Arguments += ' --lighting-room --day-cycle off --time-of-day 12 --gi-extent 48 --camera-z 4' }
    if ($Scenario -eq 'animated') { $info.Arguments += ' --animate-scene --animate-camera' }
    if ($Scenario -eq 'stress') { $info.Arguments += ' --scene-count 100000 --animate-scene --animate-camera' }
    if ($Scenario -eq 'streaming') {
        $info.Arguments += ' --scene-count 2048 --scene-walls off --camera-z 300 --stream-asset tree_stump_01_4k/tree_stump_01_4k.vcrasset.json --animate-scene'
    }
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.EnvironmentVariables['VK_VALIDATION_VALIDATE_SYNC'] = 'true'
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $info
    [void]$process.Start()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $script:requestId = 0
    $captures = 0

    function Request([string]$Method, [hashtable]$Arguments, [bool]$AllowToolError=$false) {
        $script:requestId += 1
        $request = @{
            jsonrpc = '2.0'; id = $script:requestId
            method = $Method; params = $Arguments
        } | ConvertTo-Json -Depth 12 -Compress
        $process.StandardInput.WriteLine($request)
        $process.StandardInput.Flush()
        $read = $process.StandardOutput.ReadLineAsync()
        if (-not $read.Wait(30000)) { throw "Timed out: $Method" }
        if ([string]::IsNullOrWhiteSpace($read.Result)) { throw 'MCP stdout closed.' }
        $response = $read.Result | ConvertFrom-Json
        if ($response.id -ne $script:requestId -or $null -ne $response.error) {
            throw "Invalid MCP response for $Method"
        }
        if ($response.result.isError -and -not $AllowToolError) {
            throw "Tool failed: $($response.result.content[0].text)"
        }
        return $response.result
    }

    function Tool([string]$Name, [hashtable]$Arguments = @{}) {
        return Request 'tools/call' @{ name = $Name; arguments = $Arguments }
    }

    try {
        $null = Request 'initialize' @{
            protocolVersion = '2025-11-25'; capabilities = @{}
            clientInfo = @{ name = 'vcr-render-validation'; version = '1.0' }
        }
        $process.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized"}')
        $process.StandardInput.Flush()
        $state = Tool 'debug.get_state'
        if ($Scenario -eq 'shadow-resolution') {
            Add-Type -AssemblyName System.Drawing
            foreach ($bad in @(0,512,2049,16384,2048.5,-1,4294967296,'4096')) {
                $rejected = Request 'tools/call' @{name='render.set_shadow_resolution';arguments=@{resolution=$bad}} $true
                if (-not $rejected.isError) { throw "Invalid resolution accepted: $bad" }
            }
            if ((Tool 'debug.get_state').structuredContent.render.lighting.shadow_resolution_requested -ne 2048) {
                throw 'Invalid requests changed shadow settings.'
            }
            # Allow initial voxel work to settle before comparing static captures.
            Start-Sleep -Milliseconds 500
            function ShadowCapture([int]$Resolution, [string]$Name) {
                $null = Tool 'render.set_shadow_resolution' @{resolution=$Resolution}
                $capture = Tool 'capture.screenshot'
                $light = (Tool 'debug.get_state').structuredContent.render.lighting
                if ($light.shadow_resolution -ne $Resolution -or $light.shadow_resolution_requested -ne $Resolution) {
                    throw "Shadow resize was not applied: $Resolution"
                }
                $image = @($capture.content | Where-Object {$_.type -eq 'image'})[0]
                $path = Join-Path $outputDirectory "$startup-$Name.png"
                [IO.File]::WriteAllBytes($path,[Convert]::FromBase64String($image.data))
                return $path
            }
            function CompareShadows([string]$Before,[string]$After) {
                $a=[Drawing.Bitmap]::FromFile($Before)
                $b=[Drawing.Bitmap]::FromFile($After)
                try {
                    # Ignore HUD metrics. Static Off/SMAA receivers must survive
                    # atlas destruction/recreation, including returning to a size.
                    for($y=72;$y -lt $a.Height;$y+=4){for($x=0;$x -lt $a.Width;$x+=4){
                        if($a.GetPixel($x,$y).ToArgb() -ne $b.GetPixel($x,$y).ToArgb()) {
                            throw "Shadow round trip changed the static image: $After ($x,$y)"
                        }
                    }}
                } finally { $a.Dispose();$b.Dispose() }
            }
        }
        if ($Scenario -eq 'stress') {
            # A moving wall must not voxelize at rendering frequency. Allow two
            # batches of slack for the initial volume and request/frame boundaries.
            Start-Sleep -Milliseconds 500
            $before = (Tool 'debug.get_state').structuredContent.render.lighting.geometry_updates
            $interval = [Diagnostics.Stopwatch]::StartNew()
            Start-Sleep -Milliseconds 400
            $after = (Tool 'debug.get_state').structuredContent.render.lighting.geometry_updates
            $interval.Stop()
            $updates = $after - $before
            $limit = [Math]::Ceiling($interval.Elapsed.TotalSeconds * 30) + 2
            if ($updates -le 0 -or $updates -gt $limit) {
                throw "Motion GI scheduler: $updates batches in $($interval.Elapsed.TotalSeconds) seconds, limit $limit."
            }
            Write-Output "Motion GI scheduler: $updates batches in $($interval.Elapsed.TotalSeconds) seconds."
        }
        if ($Scenario -eq 'streaming') {
            $deadline=[DateTime]::UtcNow.AddSeconds(30)
            while ($state.structuredContent.render.assets_pending -or $state.structuredContent.render.visibility.submitted -lt 2049) {
                if ([DateTime]::UtcNow -gt $deadline) { throw 'Streamed model was not published.' }
                Start-Sleep -Milliseconds 30
                $state = Tool 'debug.get_state'
            }
            if ($state.structuredContent.render.upload_high_water_bytes -le 0) { throw 'Upload arena was not used.' }
        }
        if ($Scenario -eq 'lighting') {
            function Settled {
                $deadline=[DateTime]::UtcNow.AddSeconds(20)
                do {
                    Start-Sleep -Milliseconds 40
                    $s=(Tool 'debug.get_state').structuredContent.render.lighting
                    if ([DateTime]::UtcNow -gt $deadline) { throw 'Voxel geometry did not settle.' }
                } while ($s.pending_bricks -gt 0 -or $s.geometry_updates -eq 0)
                return $s
            }
            function CaptureLighting([string]$Name) {
                $capture=Tool 'capture.screenshot'
                $image=@($capture.content | Where-Object { $_.type -eq 'image' })[0]
                [IO.File]::WriteAllBytes((Join-Path $outputDirectory "$startup-$Name.png"),[Convert]::FromBase64String($image.data))
            }
            $first=Settled
            Start-Sleep -Milliseconds 200
            $cached=Settled
            if ($cached.geometry_updates -ne $first.geometry_updates -or $cached.radiance_updates -ne $first.radiance_updates) {
                throw 'Static lighting was recalculated without a change.'
            }
            CaptureLighting 'closed-noon'
            $null=Tool 'lighting.set' @{hour=0;cycle=$false;gi=$true}
            $night=Settled
            if ($night.geometry_updates -ne $first.geometry_updates -or $night.radiance_updates -le $first.radiance_updates) {
                throw 'Time-only edit must update radiance without rebuilding geometry.'
            }
            CaptureLighting 'closed-night'
            $null=Tool 'lighting.set' @{hour=12;cycle=$false;gi=$true}
            $null=Settled
            $null=Tool 'test.set_roof' @{enabled=$false}
            $opened=Settled
            if ($opened.geometry_updates -le $night.geometry_updates) {throw 'Roof deletion did not invalidate GI.'}
            CaptureLighting 'open-noon'
            $null=Tool 'lighting.set' @{hour=12;cycle=$false;gi=$false}
            CaptureLighting 'open-no-gi'
            $null=Tool 'lighting.set' @{hour=12;cycle=$false;gi=$true}
            $null=Tool 'test.set_roof' @{enabled=$true}
            $restored=Settled
            if ($restored.geometry_updates -le $opened.geometry_updates) {throw 'Roof restoration did not invalidate GI.'}
            CaptureLighting 'restored-noon'
            $null=Tool 'lighting.set' @{hour=6;cycle=$false;gi=$true}
            $dawn=Settled
            CaptureLighting 'closed-dawn'
            if ($dawn.geometry_updates -ne $restored.geometry_updates) {throw 'Dawn rebuilt static geometry.'}
            [pscustomobject]@{first=$first;cached=$cached;night=$night;opened=$opened;restored=$restored;dawn=$dawn} |
                ConvertTo-Json -Depth 6 | Set-Content (Join-Path $outputDirectory "$startup-lighting.json")
            if ((Get-FileHash (Join-Path $outputDirectory "$startup-closed-noon.png")).Hash -ne
                (Get-FileHash (Join-Path $outputDirectory "$startup-restored-noon.png")).Hash) {
                throw 'Roof restoration left a visual residue.'
            }
            Add-Type -AssemblyName System.Drawing
            function ReceiverLuminance([string]$Name) {
                $bitmap=[Drawing.Bitmap]::FromFile((Join-Path $outputDirectory "$startup-$Name.png"))
                try {
                    $sum=0.0;$count=0
                    for($y=180;$y -lt 210;$y+=4) {for($x=304;$x -lt 335;$x+=4) {
                        $pixel=$bitmap.GetPixel($x,$y)
                        $sum+=0.2126*$pixel.R+0.7152*$pixel.G+0.0722*$pixel.B;$count++
                    }}
                    return $sum/$count
                } finally {$bitmap.Dispose()}
            }
            $openLight=ReceiverLuminance 'open-noon'
            $closedLight=ReceiverLuminance 'closed-noon'
            $noGiLight=ReceiverLuminance 'open-no-gi'
            $nightLight=ReceiverLuminance 'closed-night'
            if($openLight -le $closedLight*1.15 -or $openLight -le $noGiLight*1.15 -or $nightLight -ge $closedLight*.2) {
                throw 'The shaded receiver did not respond to GI, the roof opening, or night.'
            }
            [pscustomobject]@{open=$openLight;closed=$closedLight;no_gi=$noGiLight;night=$nightLight;restoration_identical=$true} |
                ConvertTo-Json | Set-Content (Join-Path $outputDirectory "$startup-image-check.json")
            $null=Tool 'input.key' @{key='shift';action='down'}
            $null=Tool 'input.key' @{key='s';action='down'}
            Start-Sleep -Milliseconds 1100
            $null=Tool 'input.key' @{key='s';action='up'}
            $null=Tool 'input.key' @{key='shift';action='up'}
            $recentered=Settled
            if($recentered.geometry_updates -le $dawn.geometry_updates) {throw 'Camera movement did not rebuild the recentered volume.'}
            $null=Tool 'lighting.set' @{hour=12;cycle=$true;gi=$true}
        }
        $methods = @('off', 'smaa')
        if ($state.structuredContent.render.anti_aliasing.fsr3_available) {
            $methods += 'fsr3'
        }
        for ($cycle = 0; $cycle -lt $Cycles; ++$cycle) {
            $null = Tool 'window.set_size' @{ width = 640 + 128 * $cycle; height = 360 + 72 * $cycle }
            $null = Tool 'render.set_present_mode' @{ mode = 'fifo' }
            $null = Tool 'render.set_present_mode' @{ mode = 'immediate' }
            foreach ($aa in $methods) {
                $state = Tool 'debug.get_state'
                if ($state.structuredContent.render.anti_aliasing.method -ne $aa) {
                    throw "AA switch did not select $aa"
                }
                $null = Tool 'input.mouse_move' @{ delta_x = 3; delta_y = -1 }
                if ($Scenario -eq 'shadow-resolution') {
                    Start-Sleep -Milliseconds 200
                    $before = ShadowCapture 2048 "$cycle-$aa-before"
                    foreach($resolution in @(1024,4096,8192)) {
                        $null = ShadowCapture $resolution "$cycle-$aa-$resolution"
                    }
                    $after = ShadowCapture 2048 "$cycle-$aa-after"
                    $same = ShadowCapture 2048 "$cycle-$aa-same"
                    if ($aa -ne 'fsr3') {
                        CompareShadows $before $after
                        CompareShadows $after $same
                    }
                    $captures += 6
                }
                $capture = Tool 'capture.screenshot'
                if ($capture.structuredContent.width -le 0 -or
                    $capture.structuredContent.height -le 0) {
                    throw 'Capture did not complete after resize/AA change.'
                }
                $captures += 1
                $null = Tool 'input.key' @{ key = 'f6'; action = 'press' }
            }
            $null = Tool 'render.set_instances' @{ count = 100000 }
            $null = Tool 'render.set_mode' @{ mode = 'instancing' }
            if ($Scenario -eq 'shadow-resolution') { $null = ShadowCapture 1024 "$cycle-instancing" }
            $null = Tool 'capture.screenshot'
            $null = Tool 'render.set_gpu_workload' @{ iterations = 8 }
            $null = Tool 'render.set_mode' @{ mode = 'gpu-saturation' }
            if ($Scenario -eq 'shadow-resolution') { $null = ShadowCapture 4096 "$cycle-saturation" }
            $null = Tool 'capture.screenshot'
            $captures += 2
            $null = Tool 'render.set_gpu_workload' @{ iterations = 0 }
            $null = Tool 'render.set_mode' @{ mode = 'scene' }
            if ($Scenario -eq 'shadow-resolution') { $null = ShadowCapture 2048 "$cycle-scene"; $captures += 3 }
        }
        if ($Scenario -eq 'shadow-resolution' -and $ShadowLighting -ne 'legacy') {
            $null = Tool 'lighting.set' @{hour=14;cycle=$true;gi=($ShadowLighting -eq 'voxel')}
            $null = ShadowCapture 4096 'running-day'
            $captures += 1
            if ((Tool 'debug.get_state').structuredContent.render.lighting.hour -le 14) {
                throw 'Time stopped after changing shadow resolution.'
            }
        }
        $null = Tool 'engine.quit'
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(15000)) { throw 'Shutdown timed out.' }
        $stderr = $stderrTask.Result
        $logPath = Join-Path $outputDirectory "startup-$startup.log"
        [IO.File]::WriteAllText($logPath, $stderr)
        if ($process.ExitCode -ne 0 -or
            $stderr -match '\[Vulkan ERROR\]|Validation Error|VUID-|SYNC-HAZARD|hazard detected|FidelityFX error:|AddressSanitizer|Live GPU allocations: [1-9]') {
            throw "Renderer validation failed; see $logPath"
        }
        if ($stderr -match 'validation requested.*unavailable' -or
            $stderr -notmatch 'Live GPU allocations: 0 \(0 bytes\)') {
            throw "Validation layer or allocation check was not available; see $logPath"
        }
        [pscustomobject]@{
            startup = $startup; cycles = $Cycles; captures = $captures
            scenario = $Scenario
            aa_methods = $methods
            validation_errors = 0; live_gpu_allocations = 0; exit_code = $process.ExitCode
        } | ConvertTo-Json -Compress
    }
    finally {
        if (-not $process.HasExited) {
            $process.Kill()
            $process.WaitForExit()
        }
        if ($stderrTask.IsCompleted) {
            [IO.File]::WriteAllText(
                (Join-Path $outputDirectory "startup-$startup.log"), $stderrTask.Result)
        }
        $process.Dispose()
    }
}
