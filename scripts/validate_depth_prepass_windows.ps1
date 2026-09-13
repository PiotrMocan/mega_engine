param(
    [Parameter(Mandatory=$true)][string]$ExePath,
    [ValidateSet('canonical','cubes','stress','overdraw-1','overdraw-4','overdraw-16','material-spheres')][string]$Fixture='canonical',
    [ValidateSet('cpu','gpu')][string]$VisibilityBackend='cpu',
    [ValidateSet('dynamic','legacy')][string]$Lighting='dynamic',
    [ValidateSet('off','coarse')][string]$FrontToBack='off',
    [ValidateSet('linear','hierarchy')][string]$CpuCulling='linear',
    [ValidatePattern('^[a-zA-Z0-9_-]*$')][string]$RunTag='',
    [ValidateRange(1,100)][int]$Startups=1,
    [ValidateRange(1,100)][int]$Cycles=1
)

$ErrorActionPreference = 'Stop'
$Fixture = $Fixture.ToLowerInvariant()
$VisibilityBackend = $VisibilityBackend.ToLowerInvariant()
$Lighting = $Lighting.ToLowerInvariant()
$FrontToBack = $FrontToBack.ToLowerInvariant()
$overdrawFixture = $Fixture -in @('overdraw-1','overdraw-4','overdraw-16')
$materialFixture = $Fixture -eq 'material-spheres'
if ($materialFixture -and $VisibilityBackend -ne 'cpu') { throw 'Material chart uses static CPU PBR batches.' }
if ($overdrawFixture -and $VisibilityBackend -ne 'cpu') {
    throw 'Controlled overdraw fixtures require CPU visibility with frustum culling only.'
}
$exe = (Resolve-Path -LiteralPath $ExePath).Path
$tree = Split-Path (Split-Path $exe -Parent) -Parent
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$allowedTrees = @('windows-debug','windows-release','windows-asan') |
    ForEach-Object { Join-Path $root "build\$_" }
if ($tree -notin $allowedTrees) {
    throw 'Use an executable in one of the three native Windows build trees.'
}
$outputDirectory = Join-Path $tree "render-validation/depth-prepass-$Fixture/$VisibilityBackend-$Lighting"
if ($FrontToBack -eq 'coarse') { $outputDirectory += '-coarse' }
if ($CpuCulling -eq 'hierarchy') { $outputDirectory += '-hierarchy' }
if ($RunTag) { $outputDirectory += "-$RunTag" }
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
Add-Type -AssemblyName System.Drawing

# Runtime asset lookup finds the project assets from this output directory.
# Keep evidence that canonical still exercises imported opaque model batches.
$canonicalEvidence = $null
if ($Fixture -eq 'canonical') {
    $scenePath = Join-Path $root 'assets/scenes/editor_scene.json'
    $scene = Get-Content -LiteralPath $scenePath -Raw | ConvertFrom-Json
    $models = @($scene.nodes | Where-Object {$_.kind -eq 'model' -and $_.visible -ne $false})
    if ($models.Count -eq 0) { throw 'Canonical depth-prepass validation requires imported model nodes.' }
    $canonicalEvidence = [pscustomobject]@{
        path=$scenePath; sha256=(Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
        authored_visible_models=$models.Count
        note='Authored model coverage; runtime assets_pending must settle before comparisons.'
    }
}

for ($startup=1; $startup -le $Startups; ++$startup) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $exe
    $info.WorkingDirectory = $outputDirectory
    $info.Arguments = "--mcp-stdio --validation --aa off --width 640 --height 360 --present immediate --lighting $Lighting --gi off --day-cycle off --time-of-day 14 --depth-prepass off --visibility-backend $VisibilityBackend"
    $info.Arguments += " --front-to-back $FrontToBack"
    $info.Arguments += " --cpu-culling $CpuCulling"
    if ($Fixture -eq 'cubes') { $info.Arguments += ' --scene-count 128 --scene-walls off --camera-z 12' }
    if ($Fixture -eq 'stress') { $info.Arguments += ' --scene-count 100000 --scene-walls off --camera-z 12' }
    if ($overdrawFixture) { $info.Arguments += " --render-fixture $Fixture --camera-z 12 --culling frustum --lod off" }
    if ($materialFixture) { $info.Arguments += ' --render-fixture material-spheres --camera-z 12 --culling frustum --lod off' }
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.EnvironmentVariables['VK_VALIDATION_VALIDATE_SYNC'] = 'true'
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    $started = $false
    $stderrTask = $null
    $script:requestId = 0
    $script:captures = 0
    $script:width = 640; $script:height = 360
    $logPath = Join-Path $outputDirectory "startup-$startup.log"

    function Request([string]$Method, [hashtable]$Arguments, [bool]$AllowToolError=$false) {
        $script:requestId++
        $request = @{jsonrpc='2.0'; id=$script:requestId; method=$Method; params=$Arguments} |
            ConvertTo-Json -Depth 12 -Compress
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
    function Tool([string]$Name, [hashtable]$Arguments=@{}) {
        return Request 'tools/call' @{name=$Name; arguments=$Arguments}
    }
    function AssertState([string]$Prepass, [string]$Aa, [string]$Mode) {
        $snapshot = (Tool 'debug.get_state').structuredContent
        $render = $snapshot.render
        if ($render.depth_prepass -cne $Prepass -or $render.mode -cne $Mode -or
            $render.anti_aliasing.method -cne $Aa -or $render.depth_convention -cne 'reverse-z' -or
            $render.lighting.dynamic -ne ($Lighting -eq 'dynamic') -or
            $render.lighting.voxel_gi -ne $false -or $render.lighting.cycle -ne $false -or
            $render.lighting.hour -ne 14 -or $render.assets_pending -ne 0 -or
            $snapshot.virtual_keys_held -ne 0) {
            throw "Unexpected depth prepass state: prepass=$Prepass, AA=$Aa, mode=$Mode, lighting=$Lighting."
        }
        $expectedBackend = if ($Mode -eq 'instancing') { 'direct' } else { $VisibilityBackend }
        if ($render.visibility_backend -cne $expectedBackend) {
            throw "Visibility backend fallback: expected=$expectedBackend, actual=$($render.visibility_backend)."
        }
        $expectedSorting = if ($FrontToBack -eq 'coarse' -and $expectedBackend -eq 'cpu' -and -not $materialFixture) { 'coarse' } else { 'off' }
        if ($render.front_to_back_requested -cne $FrontToBack -or
            $render.front_to_back_actual -cne $expectedSorting) {
            throw "Front-to-back settings differ: requested=$FrontToBack, expected actual=$expectedSorting."
        }
        $expectedCulling = if ($expectedBackend -eq 'cpu' -and -not $materialFixture) { $CpuCulling } else { 'off' }
        if ($render.cpu_culling_requested -cne $CpuCulling -or $render.cpu_culling_actual -cne $expectedCulling) {
            throw "CPU culling differs: requested=$CpuCulling, expected actual=$expectedCulling."
        }
        $expectedFixture = if ($overdrawFixture -or $materialFixture) { $Fixture } else { 'none' }
        if ($render.render_fixture -cne $expectedFixture) { throw 'The requested render fixture was not applied.' }
        if ($Mode -eq 'scene' -and -not $materialFixture -and $render.visibility.visible -le 0) {
            throw 'The scene has no visible instances.'
        }
        if ($Mode -eq 'scene' -and $materialFixture -and ($render.instances -ne 28 -or $render.triangles -ne 61825)) {
            throw 'The material chart must draw all 28 sphere instances with their real geometry.'
        }
        return $snapshot
    }
    function RejectInvalidPrepasses([string]$Prepass, [string]$Aa, [string]$Mode) {
        $before = AssertState $Prepass $Aa $Mode
        foreach ($bad in @(
            @{}, @{mode=''}, @{mode='on'}, @{mode='Partial'}, @{mode='full '},
            @{mode=$null}, @{mode=$true}, @{mode=1}, @{mode=@('full')},
            @{mode=@{value='full'}}, @{prepass='full'}, @{mode='full';extra=1}
        )) {
            $rejected = Request 'tools/call' @{name='render.set_depth_prepass';arguments=$bad} $true
            if (-not $rejected.isError) { throw 'Invalid depth prepass payload was accepted.' }
            $after = AssertState $Prepass $Aa $Mode
            if (($before.camera | ConvertTo-Json -Compress) -cne ($after.camera | ConvertTo-Json -Compress)) {
                throw 'Invalid prepass request changed the camera.'
            }
        }
    }
    function SettledMetrics([string]$Prepass) {
        $null = Tool 'debug.reset_metrics'
        $timer = [Diagnostics.Stopwatch]::StartNew()
        do {
            $metrics = (Tool 'debug.get_metrics').structuredContent
            $value = $metrics.latest.gpu_depth_prepass_ms
            # Query results lag the current frame. More than two frame contexts
            # must retire after reset before an old mode can satisfy this test.
            if ($metrics.frames_rendered -ge 6 -and $null -ne $value -and
                $metrics.latest.gpu_frame_ms -gt 0 -and
                (($Prepass -eq 'off' -and $value -eq 0) -or ($Prepass -ne 'off' -and $value -gt 0))) {
                return $metrics
            }
            Start-Sleep -Milliseconds 20
        } while ($timer.Elapsed.TotalSeconds -lt 5)
        $metrics | ConvertTo-Json -Depth 8 |
            Set-Content (Join-Path $outputDirectory "$startup-metric-failure.json")
        throw "Prepass timing did not settle to zero when off or positive when enabled: $Prepass."
    }
    function SaveCapture($Capture, [string]$Name) {
        $images = @($Capture.content | Where-Object {$_.type -eq 'image'})
        if ($images.Count -ne 1 -or $images[0].mimeType -cne 'image/png' -or
            [string]::IsNullOrWhiteSpace($images[0].data) -or
            $Capture.structuredContent.width -ne $script:width -or
            $Capture.structuredContent.height -ne $script:height) {
            throw "Missing PNG or unexpected capture dimensions: $Name"
        }
        $path = Join-Path $outputDirectory "$startup-$Name.png"
        [IO.File]::WriteAllBytes($path, [Convert]::FromBase64String($images[0].data))
        $bitmap = [Drawing.Bitmap]::FromFile($path)
        try {
            if ($bitmap.Width -ne $script:width -or $bitmap.Height -ne $script:height -or
                $bitmap.RawFormat.Guid -ne [Drawing.Imaging.ImageFormat]::Png.Guid) {
                throw "Decoded PNG format or dimensions differ: $Name"
            }
            # A constant image cannot pass due only to text in the top AA HUD.
            # This is a nonempty-image guard, not a general geometry detector.
            $minimum=765; $maximum=0
            for ($y=72; $y -lt $bitmap.Height; $y+=16) {
                for ($x=0; $x -lt $bitmap.Width; $x+=16) {
                    $pixel=$bitmap.GetPixel($x,$y)
                    $sum=[int]$pixel.R+[int]$pixel.G+[int]$pixel.B
                    $minimum=[Math]::Min($minimum,$sum); $maximum=[Math]::Max($maximum,$sum)
                }
            }
            if ($maximum -le $minimum) { throw "Capture has no varying scene pixels: $Name" }
            $rectangle=New-Object Drawing.Rectangle(0,0,$bitmap.Width,$bitmap.Height)
            $locked=$bitmap.LockBits($rectangle,[Drawing.Imaging.ImageLockMode]::ReadOnly,
                [Drawing.Imaging.PixelFormat]::Format32bppArgb)
            try {
                $rowBytes=$bitmap.Width*4
                $pixels=New-Object byte[] ($rowBytes*$bitmap.Height)
                for ($y=0; $y -lt $bitmap.Height; ++$y) {
                    [Runtime.InteropServices.Marshal]::Copy(
                        [IntPtr]::Add($locked.Scan0,$y*$locked.Stride),$pixels,$y*$rowBytes,$rowBytes)
                }
            } finally { $bitmap.UnlockBits($locked) }
            $sha=[Security.Cryptography.SHA256]::Create()
            try { $hash=[BitConverter]::ToString($sha.ComputeHash($pixels)).Replace('-','') }
            finally { $sha.Dispose() }
            return [pscustomobject]@{path=$path; width=$bitmap.Width; height=$bitmap.Height; pixel_sha256=$hash}
        } finally { $bitmap.Dispose() }
    }
    function PrepassCapture([string]$Prepass, [string]$Aa, [string]$Mode, [string]$Name, [bool]$CheckInvalid=$false) {
        $null = Tool 'render.set_depth_prepass' @{mode=$Prepass}
        $null = AssertState $Prepass $Aa $Mode
        if ($CheckInvalid) { RejectInvalidPrepasses $Prepass $Aa $Mode }
        $metrics = SettledMetrics $Prepass
        $before = AssertState $Prepass $Aa $Mode
        $capture = Tool 'capture.screenshot'
        $after = AssertState $Prepass $Aa $Mode
        if (($before.camera | ConvertTo-Json -Compress) -cne ($after.camera | ConvertTo-Json -Compress)) {
            throw 'Camera changed during a prepass capture.'
        }
        $image = SaveCapture $capture $Name
        $script:captures++
        $saved = [pscustomobject]@{image=$image; state=$after; metrics=$metrics}
        $saved | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $outputDirectory "$startup-$Name.json")
        return $saved
    }
    function ReadCapturePixels($Image) {
        $bitmap = [Drawing.Bitmap]::FromFile($Image.path)
        try {
            if ($bitmap.Width -ne $Image.width -or $bitmap.Height -ne $Image.height) {
                throw 'Comparison PNG dimensions differ from the saved capture.'
            }
            $rectangle = New-Object Drawing.Rectangle(0,0,$bitmap.Width,$bitmap.Height)
            $locked = $bitmap.LockBits($rectangle,[Drawing.Imaging.ImageLockMode]::ReadOnly,
                [Drawing.Imaging.PixelFormat]::Format32bppArgb)
            try {
                $rowBytes = $bitmap.Width*4
                $pixels = New-Object byte[] ($rowBytes*$bitmap.Height)
                for ($y=0; $y -lt $bitmap.Height; ++$y) {
                    [Runtime.InteropServices.Marshal]::Copy(
                        [IntPtr]::Add($locked.Scan0,$y*$locked.Stride),$pixels,$y*$rowBytes,$rowBytes)
                }
                return ,$pixels
            } finally { $bitmap.UnlockBits($locked) }
        } finally { $bitmap.Dispose() }
    }
    function CompareCaptures($Before, $After, [string]$Name) {
        if ($Before.image.width -ne $After.image.width -or $Before.image.height -ne $After.image.height -or
            $Before.state.render.anti_aliasing.method -cne $After.state.render.anti_aliasing.method -or
            $Before.state.render.mode -cne $After.state.render.mode -or
            ($Before.state.camera | ConvertTo-Json -Compress) -cne ($After.state.camera | ConvertTo-Json -Compress)) {
            throw "Prepass comparison changed camera, render mode, dimensions or AA: $Name"
        }
        $temporal = $Before.state.render.anti_aliasing.method -eq 'fsr3'
        $numericAllowed = $Fixture -eq 'canonical' -and -not $temporal -and
            $Before.state.render.depth_prepass -cne $After.state.render.depth_prepass
        $exactRequired = -not $temporal -and -not $numericAllowed
        $identical = $Before.image.pixel_sha256 -ceq $After.image.pixel_sha256
        $pixelCount = [long]$Before.image.width*$Before.image.height
        $rgbPixels = 0L; $alphaPixels = 0L; $maximumError = 0
        if (-not $identical) {
            $beforePixels = ReadCapturePixels $Before.image
            $afterPixels = ReadCapturePixels $After.image
            for ($i=0; $i -lt $beforePixels.Length; $i+=4) {
                $pixelDiffers = $false
                for ($channel=0; $channel -lt 3; ++$channel) {
                    $difference = [Math]::Abs([int]$beforePixels[$i+$channel]-[int]$afterPixels[$i+$channel])
                    if ($difference -gt 0) { $pixelDiffers = $true }
                    if ($difference -gt $maximumError) { $maximumError = $difference }
                }
                if ($pixelDiffers) { $rgbPixels++ }
                if ($beforePixels[$i+3] -ne $afterPixels[$i+3]) { $alphaPixels++ }
            }
        }
        $maxChannelError = if ($numericAllowed) { 1 } else { 0 }
        $maxPixelFraction = if ($numericAllowed) { 0.0001 } else { 0.0 }
        # An integer inequality enforces exactly 0.01%, without rounding the
        # pixel allowance upwards for small render targets.
        $withinFraction = if ($numericAllowed) { $rgbPixels*10000 -le $pixelCount } else { $rgbPixels -eq 0 }
        $numericEquivalent = $alphaPixels -eq 0 -and $maximumError -le $maxChannelError -and $withinFraction
        $accepted = $alphaPixels -eq 0 -and ($temporal -or $numericEquivalent)
        [pscustomobject]@{
            identical=$identical; exact_identical=$identical; exact_required=$exactRequired
            numeric_equivalent=$numericEquivalent; comparison_passed=$accepted
            numeric_allowance_eligible=$numericAllowed
            numeric_allowance_used=($numericAllowed -and -not $identical -and $numericEquivalent)
            pixel_count=$pixelCount; rgb_differing_pixels=$rgbPixels
            rgb_differing_fraction=$rgbPixels/[double]$pixelCount
            alpha_differing_pixels=$alphaPixels; max_rgb_channel_error=$maximumError
            threshold=@{max_rgb_channel_error=$maxChannelError; max_rgb_differing_fraction=$maxPixelFraction; alpha_must_match=$true}
            temporal_history_note=$(if ($temporal) { 'FSR jitter/history can change RGB pixels; exact equality is not required, alpha must match.' }
                elseif ($numericAllowed) { 'Canonical cross-prepass comparison permits an engineering budget of 1 LSB on at most 0.01% of pixels; the cause of observed differences is not established.' }
                else { 'Every decoded pixel must match at the same pose; restoration and procedural fixtures remain exact.' })
            before=$Before.image; after=$After.image
        } | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $outputDirectory "$startup-$Name-check.json")
        if (-not $accepted) { throw "Depth prepass exceeded the image comparison contract: $Name" }
    }
    function RoundTrip([string]$Aa, [string]$Mode, [string]$Name, [bool]$CheckInvalid=$false) {
        $before = PrepassCapture 'off' $Aa $Mode "$Name-off-before" $CheckInvalid
        $partial = PrepassCapture 'partial' $Aa $Mode "$Name-partial" $CheckInvalid
        $full = PrepassCapture 'full' $Aa $Mode "$Name-full" $CheckInvalid
        $after = PrepassCapture 'off' $Aa $Mode "$Name-off-after"
        CompareCaptures $before $partial "$Name-partial"
        CompareCaptures $before $full "$Name-full"
        CompareCaptures $before $after "$Name-restoration"
        return $after
    }

    try {
        $started = $process.Start()
        if (-not $started) { throw 'Renderer process did not start.' }
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $null = Request 'initialize' @{
            protocolVersion='2025-11-25'; capabilities=@{}
            clientInfo=@{name='vcr-depth-prepass-validation';version='1.0'}
        }
        $process.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized"}')
        $process.StandardInput.Flush()
        $capabilities = (Tool 'debug.get_capabilities').structuredContent
        if (-not $capabilities.validation_enabled -or -not $capabilities.gpu_timestamps -or
            -not $capabilities.screenshot_supported) {
            throw 'Validation, GPU timestamps and screenshot support are required.'
        }
        $settle = [Diagnostics.Stopwatch]::StartNew()
        do {
            $initial = (Tool 'debug.get_state').structuredContent
            if ($initial.render.assets_pending -eq 0) { break }
            Start-Sleep -Milliseconds 50
        } while ($settle.Elapsed.TotalSeconds -lt 10)
        $null = Tool 'input.release_all'
        $initial = AssertState 'off' 'off' 'scene'
        [pscustomobject]@{
            executable=$exe; executable_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
            arguments=$info.Arguments; fixture=$Fixture; visibility_backend=$VisibilityBackend; lighting=$Lighting
            front_to_back_requested=$FrontToBack; front_to_back_actual=$initial.render.front_to_back_actual
            cpu_culling_requested=$CpuCulling; cpu_culling_actual=$initial.render.cpu_culling_actual
            canonical=$canonicalEvidence; capabilities=$capabilities; initial_state=$initial
        } | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $outputDirectory "$startup-environment.json")
        $methods = @('off','smaa')
        if ($initial.render.anti_aliasing.fsr3_available) { $methods += 'fsr3' }
        for ($cycle=0; $cycle -lt $Cycles; ++$cycle) {
            $baseline = $null
            foreach ($aa in $methods) {
                $sceneCapture = RoundTrip $aa 'scene' "$cycle-$aa-scene" ($cycle -eq 0 -and $aa -eq 'off')
                if ($aa -eq 'off') { $baseline = $sceneCapture }
                $null = Tool 'input.key' @{key='f6';action='press'}
            }
            $null = Tool 'window.set_size' @{width=1280;height=720}
            $script:width=1280; $script:height=720
            $null = RoundTrip 'off' 'scene' "$cycle-resized"
            $null = Tool 'window.set_size' @{width=640;height=360}
            $script:width=640; $script:height=360
            $restored = RoundTrip 'off' 'scene' "$cycle-resize-restored"
            CompareCaptures $baseline $restored "$cycle-resize-baseline"
            $null = Tool 'render.set_instances' @{count=128}
            $null = Tool 'render.set_mode' @{mode='instancing'}
            $null = RoundTrip 'off' 'instancing' "$cycle-instancing"
            $null = Tool 'render.set_mode' @{mode='gpu-saturation'}
            $null = RoundTrip 'off' 'gpu-saturation' "$cycle-saturation"
            # frame_serial seeds the real compute workload, so active workload
            # colors intentionally differ. Compare stable mode output above,
            # then exercise compute + prepass without claiming pixel equality.
            $null = Tool 'render.set_gpu_workload' @{iterations=8}
            $active = PrepassCapture 'full' 'off' 'gpu-saturation' "$cycle-saturation-active-full"
            $inactive = PrepassCapture 'off' 'off' 'gpu-saturation' "$cycle-saturation-active-off"
            if ($active.state.render.gpu_workload_iterations -ne 8 -or
                $inactive.state.render.gpu_workload_iterations -ne 8) {
                throw 'Active GPU workload was not applied.'
            }
            [pscustomobject]@{
                iterations=8; exact_required=$false
                note='Compute uses frame_serial as frame_seed and changes color; stable iterations=0 mode equality was checked separately.'
                full=$active.image; off=$inactive.image
            } | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $outputDirectory "$startup-$cycle-active-workload-coverage.json")
            $null = Tool 'render.set_gpu_workload' @{iterations=0}
            $null = Tool 'render.set_mode' @{mode='scene'}
            $restored = RoundTrip 'off' 'scene' "$cycle-scene-restored"
            CompareCaptures $baseline $restored "$cycle-mode-baseline"
        }
        $null = Tool 'engine.quit'
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(15000)) { throw 'Shutdown timed out.' }
        $stderr = $stderrTask.Result
        [IO.File]::WriteAllText($logPath,$stderr)
        if ($process.ExitCode -ne 0 -or
            $stderr -match '\[Vulkan ERROR\]|Validation Error|VUID-|SYNC-HAZARD|hazard detected|FidelityFX error:|AddressSanitizer|Live GPU allocations: [1-9]') {
            throw "Renderer validation failed; see $logPath"
        }
        if ($stderr -match 'validation requested.*unavailable' -or
            $stderr -notmatch 'Live GPU allocations: 0 \(0 bytes\)') {
            throw "Validation layer or allocation gate was unavailable; see $logPath"
        }
        $summary = [pscustomobject]@{
            startup=$startup; cycles=$Cycles; captures=$script:captures; fixture=$Fixture
            visibility_backend=$VisibilityBackend; lighting=$Lighting; aa_methods=$methods
            front_to_back_requested=$FrontToBack; front_to_back_actual=$restored.state.render.front_to_back_actual
            cpu_culling_requested=$CpuCulling; cpu_culling_actual=$restored.state.render.cpu_culling_actual
            validation_errors=0; live_gpu_allocations=0; exit_code=$process.ExitCode
        }
        $summary | ConvertTo-Json | Set-Content (Join-Path $outputDirectory "$startup-summary.json")
        $summary | ConvertTo-Json -Compress
    } finally {
        if ($started -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
        if ($null -ne $stderrTask -and $stderrTask.IsCompleted) {
            [IO.File]::WriteAllText($logPath,$stderrTask.Result)
        }
        $process.Dispose()
    }
}
