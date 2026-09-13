param(
    [Parameter(Mandatory = $true)][string]$ExePath,
    [ValidateRange(1, 100)][int]$Startups = 3,
    [ValidateRange(1, 100)][int]$Cycles = 2,
    [ValidateSet('canonical','animated','stress','streaming','lighting','shadow-resolution','rendering-pipeline','shadow-cache','cascade-range')][string]$Scenario='canonical',
    [ValidateSet('voxel','off','legacy')][string]$ShadowLighting='voxel',
    [ValidateSet('auto','d32','d16')][string]$ShadowDepth='auto',
    [ValidateSet('','auto','cpu','gpu')][string]$VisibilityBackend='',
    [ValidatePattern('^[a-zA-Z0-9_-]*$')][string]$RunTag='',
    [string]$StreamAsset='validation/lighting-submeshes/triangles.vcrasset.json'
)

$ErrorActionPreference = 'Stop'
$ShadowDepth = $ShadowDepth.ToLowerInvariant()
$shadowCacheScenario = $Scenario -in @('shadow-cache','cascade-range')
if ([string]::IsNullOrWhiteSpace($StreamAsset) -or $StreamAsset.Contains('"')) {
    throw 'StreamAsset must be a nonempty asset-relative path without double quotes.'
}
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
if ($ShadowDepth -ne 'auto') { $outputDirectory += "-$ShadowDepth" }
if ($VisibilityBackend) { $outputDirectory += "-$VisibilityBackend" }
if ($RunTag) { $outputDirectory += "-$RunTag" }
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

for ($startup = 1; $startup -le $Startups; ++$startup) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $exe
    $info.WorkingDirectory = $outputDirectory
    $info.Arguments = '--mcp-stdio --validation --aa off --width 640 --height 360'
    $info.Arguments += " --shadow-depth $ShadowDepth"
    if ($VisibilityBackend) { $info.Arguments += " --visibility-backend $VisibilityBackend" }
    if ($Scenario -eq 'rendering-pipeline') {
        $info.Arguments += ' --lighting dynamic --day-cycle off --time-of-day 14 --gi off --scene-count 128 --scene-walls off --camera-z 12'
    }
    if ($Scenario -eq 'shadow-cache') {
        $info.Arguments += ' --lighting-room --camera-z 4 --gi-extent 48 --lighting dynamic --gi off --day-cycle off --time-of-day 14 --shadow-cache full'
    }
    if ($Scenario -eq 'cascade-range') {
        $info.Arguments += ' --render-fixture cascade-cache --camera-z 12 --lighting dynamic --gi off --day-cycle off --time-of-day 14 --shadow-cache full --shadow-resolution 2048 --tone-mapper reinhard'
    }
    if ($Scenario -eq 'shadow-resolution') {
        $info.Arguments += ' --day-cycle off --time-of-day 14'
        if ($ShadowLighting -eq 'legacy') { $info.Arguments += ' --lighting legacy' }
        else { $info.Arguments += " --gi $ShadowLighting" }
    }
    if ($Scenario -eq 'lighting') { $info.Arguments += ' --lighting-room --day-cycle off --time-of-day 12 --gi-extent 48 --camera-z 4' }
    if ($Scenario -eq 'animated') { $info.Arguments += ' --animate-scene --animate-camera' }
    if ($Scenario -eq 'stress') { $info.Arguments += ' --scene-count 100000 --animate-scene --animate-camera' }
    if ($Scenario -eq 'streaming') {
        $info.Arguments += " --scene-count 2048 --scene-walls off --camera-z 300 --stream-asset `"$StreamAsset`" --animate-scene"
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

    function AssertShadowDepth($Lighting) {
        if ($Lighting.shadow_depth_requested -cne $ShadowDepth -or
            $Lighting.shadow_depth_actual -cnotin @('d32','d16') -or
            ($ShadowDepth -ne 'auto' -and $Lighting.shadow_depth_actual -cne $ShadowDepth) -or
            $Lighting.shadow_linear_filter -isnot [bool]) {
            throw "Shadow depth profile or filter metadata was not applied: requested=$ShadowDepth, actual=$($Lighting.shadow_depth_actual)."
        }
        $script:validatedShadowDepth = $Lighting.shadow_depth_actual
        $script:validatedShadowLinearFilter = $Lighting.shadow_linear_filter
    }

    function Tool([string]$Name, [hashtable]$Arguments = @{}) {
        $result = Request 'tools/call' @{ name = $Name; arguments = $Arguments }
        if ($Name -eq 'debug.get_state') { AssertShadowDepth $result.structuredContent.render.lighting }
        return $result
    }

    try {
        $null = Request 'initialize' @{
            protocolVersion = '2025-11-25'; capabilities = @{}
            clientInfo = @{ name = 'vcr-render-validation'; version = '1.0' }
        }
        $process.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized"}')
        $process.StandardInput.Flush()
        $state = Tool 'debug.get_state'
        if ($Scenario -eq 'rendering-pipeline' -or $shadowCacheScenario) {
            Add-Type -AssemblyName System.Drawing
            function AssertPipelineState([string]$Mapper, [string]$Aa) {
                $render = (Tool 'debug.get_state').structuredContent.render
                if ($render.tone_mapper -ne $Mapper -or $render.depth_convention -ne 'reverse-z' -or
                    $render.anti_aliasing.method -ne $Aa -or $render.lighting.dynamic -ne $true -or
                    $render.lighting.cycle -ne $false -or $render.lighting.hour -ne 14 -or
                    $render.lighting.voxel_gi -ne $false) {
                    throw "Rendering pipeline state changed unexpectedly: mapper=$Mapper, AA=$Aa."
                }
            }
            function RejectInvalidMappers([string]$Mapper, [string]$Aa) {
                AssertPipelineState $Mapper $Aa
                foreach ($bad in @(
                    @{}, @{mapper=''}, @{mapper='aces'}, @{mapper='GT7'}, @{mapper='gt7 '},
                    @{mapper=$null}, @{mapper=$true}, @{mapper=1}, @{mapper=@('gt7')},
                    @{mapper=@{name='gt7'}}, @{tone_mapper='gt7'}, @{mapper='gt7';extra=1}
                )) {
                    $rejected = Request 'tools/call' @{name='render.set_tone_mapper';arguments=$bad} $true
                    if (-not $rejected.isError) {
                        throw "Invalid tone mapper request accepted: $($bad | ConvertTo-Json -Compress)"
                    }
                    AssertPipelineState $Mapper $Aa
                }
            }
            function FixturePatch($Bitmap, [double]$U, [double]$V=(2.0/3.0)) {
                $x = [int][Math]::Floor($U * $Bitmap.Width)
                $y = [int][Math]::Floor($V * $Bitmap.Height)
                $red = 0.0; $green = 0.0; $blue = 0.0
                for ($dy=-1; $dy -le 1; ++$dy) {
                    for ($dx=-1; $dx -le 1; ++$dx) {
                        $pixel = $Bitmap.GetPixel($x + $dx, $y + $dy)
                        $red += $pixel.R; $green += $pixel.G; $blue += $pixel.B
                    }
                }
                return [pscustomobject]@{x=$x; y=$y; r=$red/9; g=$green/9; b=$blue/9}
            }
            function AssertProceduralGeometry($Bitmap, [string]$Name) {
                if ([Math]::Abs($Bitmap.Width / [double]$Bitmap.Height - 16.0/9.0) -gt 0.002) {
                    throw "Procedural fixture requires a 16:9 capture: $Name"
                }
                # This contract belongs only to the 128-cube fixture: cubes
                # i=63,64,65 are at x=-4,0,4; y=-10; z=-20, viewed from (0,4,12)
                # with the default -12 degree pitch and no mouse movement.
                # Their front faces occupy these three patches; the alternating
                # patches see sky through the gaps. A sky-only frame has no
                # repeated face/gap contrast, even though its gradient is nonempty.
                foreach ($u in @(.45, .50, .55)) {
                    $front = FixturePatch $Bitmap $u
                    $gap = FixturePatch $Bitmap ($u + .025)
                    $contrast = [pscustomobject]@{
                        r=$front.r-$gap.r; g=$front.g-$gap.g; b=$front.b-$gap.b
                    }
                    if ($contrast.r -lt 8 -or $contrast.g -lt 15 -or $contrast.b -lt 24) {
                        throw "Procedural cube face missing at u=$u, v=2/3: $Name; RGB face-gap contrast=$($contrast | ConvertTo-Json -Compress)"
                    }
                    [pscustomobject]@{front=$front; gap=$gap; contrast=$contrast}
                }
            }
            function AssertCascadeRangeShadows($Bitmap) {
                if ($Bitmap.Width -lt 640 -or $Bitmap.Height -lt 360 -or
                    [Math]::Abs($Bitmap.Width / [double]$Bitmap.Height - 16.0/9.0) -gt 0.002) {
                    throw 'Cascade range shadow patches require a capture of at least 640x360 at 16:9.'
                }
                # Receiver-only patches for the cascade-cache fixture, camera
                # (0,4,12), pitch -12 degrees, hour 14, 2048 atlas, AA off and
                # Reinhard. Pixel centres are normalized from the 640x360
                # reference. Same-row lit patches use the same floor material.
                # The last two shadows cover only 1-2 rows at this resolution;
                # their lower thresholds account for 3x3 averaging, not an
                # exemption from proving that these distant shadows exist.
                $probes = @(
                    @{name='near'; x=315; lit_x=346; y=308; minimum=@(40,30,20)},
                    @{name='after-first-split'; x=351; lit_x=375; y=208; minimum=@(35,25,18)},
                    @{name='second-split-blend'; x=336; lit_x=352; y=156; minimum=@(12,9,6)},
                    @{name='third-cascade'; x=425; lit_x=440; y=136; minimum=@(9,7,5)},
                    @{name='fourth-cascade'; x=425; lit_x=439; y=133; minimum=@(6,5,4)}
                )
                foreach ($probe in $probes) {
                    $v = ($probe.y + .5) / 360.0
                    $shadow = FixturePatch $Bitmap (($probe.x + .5) / 640.0) $v
                    $lit = FixturePatch $Bitmap (($probe.lit_x + .5) / 640.0) $v
                    $contrast = [pscustomobject]@{
                        r=$lit.r-$shadow.r; g=$lit.g-$shadow.g; b=$lit.b-$shadow.b
                    }
                    if ($lit.r -lt 125 -or $lit.g -lt 128 -or $lit.b -lt 132 -or
                        $contrast.r -lt $probe.minimum[0] -or
                        $contrast.g -lt $probe.minimum[1] -or
                        $contrast.b -lt $probe.minimum[2]) {
                        throw "Cascade range floor shadow missing: $($probe.name); lit=$($lit | ConvertTo-Json -Compress); contrast=$($contrast | ConvertTo-Json -Compress)"
                    }
                    [pscustomobject]@{
                        name=$probe.name; shadow=$shadow; lit=$lit; contrast=$contrast
                        minimum_rgb_contrast=$probe.minimum
                    }
                }
            }
            function SavePipelineCapture($Capture, [string]$Name, [bool]$CheckFixtureGeometry=$false) {
                $images = @($Capture.content | Where-Object {$_.type -eq 'image'})
                if ($images.Count -ne 1 -or $images[0].mimeType -ne 'image/png' -or
                    [string]::IsNullOrWhiteSpace($images[0].data) -or
                    $Capture.structuredContent.width -le 0 -or $Capture.structuredContent.height -le 0) {
                    throw "Missing PNG capture: $Name"
                }
                $bytes = [Convert]::FromBase64String($images[0].data)
                if ($bytes.Length -lt 8) { throw "Empty PNG capture: $Name" }
                $path = Join-Path $outputDirectory "$startup-$Name.png"
                [IO.File]::WriteAllBytes($path, $bytes)
                $bitmap = [Drawing.Bitmap]::FromFile($path)
                try {
                    if ($bitmap.Width -ne $Capture.structuredContent.width -or
                        $bitmap.Height -ne $Capture.structuredContent.height -or
                        $bitmap.RawFormat.Guid -ne [Drawing.Imaging.ImageFormat]::Png.Guid) {
                        throw "PNG dimensions or format do not match the capture: $Name"
                    }
                    # Check scene samples below the fixed AA HUD, so an empty
                    # scene cannot pass merely because the overlay has text.
                    $minimum = 765; $maximum = 0; $samples = 0
                    $stepX = [Math]::Max(1, [int][Math]::Floor($bitmap.Width / 16))
                    $stepY = [Math]::Max(1, [int][Math]::Floor(($bitmap.Height - 72) / 16))
                    for ($y=72; $y -lt $bitmap.Height; $y+=$stepY) {
                        for ($x=0; $x -lt $bitmap.Width; $x+=$stepX) {
                            $pixel = $bitmap.GetPixel($x, $y)
                            $sum = [int]$pixel.R + [int]$pixel.G + [int]$pixel.B
                            $minimum = [Math]::Min($minimum, $sum)
                            $maximum = [Math]::Max($maximum, $sum)
                            $samples++
                        }
                    }
                    if ($samples -eq 0 -or $maximum -le $minimum) {
                        throw "Capture has no varying scene pixels: $Name"
                    }
                    $geometry = @()
                    if ($CheckFixtureGeometry) {
                        $geometry = @(AssertProceduralGeometry $bitmap $Name)
                    }
                    # Hash every decoded pixel, including the static HUD. Row
                    # copies normalize any bitmap padding or negative stride.
                    $rectangle = New-Object Drawing.Rectangle(0, 0, $bitmap.Width, $bitmap.Height)
                    $locked = $bitmap.LockBits($rectangle, [Drawing.Imaging.ImageLockMode]::ReadOnly,
                        [Drawing.Imaging.PixelFormat]::Format32bppArgb)
                    try {
                        $rowBytes = $bitmap.Width * 4
                        $pixels = New-Object byte[] ($rowBytes * $bitmap.Height)
                        for ($y=0; $y -lt $bitmap.Height; ++$y) {
                            [Runtime.InteropServices.Marshal]::Copy(
                                [IntPtr]::Add($locked.Scan0, $y * $locked.Stride),
                                $pixels, $y * $rowBytes, $rowBytes)
                        }
                    } finally { $bitmap.UnlockBits($locked) }
                    $sha = [Security.Cryptography.SHA256]::Create()
                    try { $pixelHash = [BitConverter]::ToString($sha.ComputeHash($pixels)).Replace('-', '') }
                    finally { $sha.Dispose() }
                    return [pscustomobject]@{path=$path; width=$bitmap.Width; height=$bitmap.Height; pixel_sha256=$pixelHash; geometry_fixture=$geometry}
                } finally { $bitmap.Dispose() }
            }
            function PipelineCapture([string]$Mapper, [string]$Aa, [string]$Name) {
                $null = Tool 'render.set_tone_mapper' @{mapper=$Mapper}
                AssertPipelineState $Mapper $Aa
                $capture = Tool 'capture.screenshot'
                AssertPipelineState $Mapper $Aa
                return SavePipelineCapture $capture $Name $true
            }
            AssertPipelineState 'reinhard' 'off'
        }
        if ($shadowCacheScenario) {
            $script:shadowExpectedHour = 14
            $script:shadowPartialObserved = $false
            $script:shadowRoofAvailable = $false
            if ($Scenario -eq 'shadow-cache') {
                $toolList = Request 'tools/list' @{}
                $script:shadowRoofAvailable = 'test.set_roof' -in @($toolList.tools.name)
                if (-not $script:shadowRoofAvailable) { throw 'Shadow fixture requires the procedural roof control.' }
                # Expose the box shadow to direct sun during partial updates; a
                # closed roof would make an equality test insensitive to this shadow.
                $null = Tool 'test.set_roof' @{enabled=$false}
            }
            function ShadowCacheState([string]$Mode) {
                $snapshot = (Tool 'debug.get_state').structuredContent
                $light = $snapshot.render.lighting
                if ($light.cascade_cache -ne ($Mode -eq 'cascade') -or
                    $light.dynamic -ne $true -or $light.voxel_gi -ne $false -or
                    $light.cycle -ne $false -or $light.hour -ne $script:shadowExpectedHour -or
                    $snapshot.render.depth_convention -ne 'reverse-z') {
                    throw "Shadow cache settings were not applied: $Mode"
                }
                if ($Scenario -eq 'cascade-range' -and $snapshot.render.render_fixture -cne 'cascade-cache') {
                    throw 'Cascade range validation requires the cascade-cache render fixture.'
                }
                foreach ($field in @('shadow_passes','shadow_tiles','last_shadow_mask')) {
                    if ($null -eq $light.PSObject.Properties[$field]) { throw "Missing shadow state counter: $field" }
                }
                if ($light.shadow_tiles -lt $light.shadow_passes -or
                    $light.shadow_tiles -gt 4 * $light.shadow_passes -or
                    $light.last_shadow_mask -lt 0 -or $light.last_shadow_mask -gt 15) {
                    throw 'Invalid shadow work counters or cascade mask.'
                }
                return $snapshot
            }
            function RejectInvalidShadowCaches([string]$Mode) {
                foreach ($bad in @(
                    @{}, @{mode=''}, @{mode='tile'}, @{mode='Cascade'}, @{mode='full '},
                    @{mode=$null}, @{mode=$true}, @{mode=1}, @{mode=@('full')},
                    @{mode=@{value='full'}}, @{cache='cascade'}, @{mode='cascade';extra=1}
                )) {
                    $rejected = Request 'tools/call' @{name='render.set_shadow_cache';arguments=$bad} $true
                    if (-not $rejected.isError) { throw 'Invalid shadow cache payload was accepted.' }
                    $null = ShadowCacheState $Mode
                }
            }
            function ShadowCacheCapture([string]$Mode, [string]$Name, [bool]$SelectMode=$true) {
                if ($SelectMode) { $null = Tool 'render.set_shadow_cache' @{mode=$Mode} }
                $null = ShadowCacheState $Mode
                $capture = Tool 'capture.screenshot'
                $snapshot = ShadowCacheState $Mode
                $saved = SavePipelineCapture $capture $Name
                $script:captures += 1
                return [pscustomobject]@{
                    image=$saved; lighting=$snapshot.render.lighting
                    camera=$snapshot.camera; aa=$snapshot.render.anti_aliasing.method
                }
            }
            function CompareShadowCaches($Cached, $Full, [string]$Name) {
                if ($Cached.image.width -ne $Full.image.width -or $Cached.image.height -ne $Full.image.height -or
                    $Cached.aa -ne $Full.aa -or
                    ($Cached.camera | ConvertTo-Json -Compress) -ne ($Full.camera | ConvertTo-Json -Compress)) {
                    throw "Shadow cache comparison changed camera, dimensions or AA: $Name"
                }
                $identical = $Cached.image.pixel_sha256 -eq $Full.image.pixel_sha256
                [pscustomobject]@{
                    identical=$identical; exact_required=($Cached.aa -ne 'fsr3')
                    temporal_history_note=$(if ($Cached.aa -eq 'fsr3') {
                        'FSR history/jitter differences are recorded; exact pixel equality is not required.'
                    } else { 'Every decoded pixel must be identical at the same pose.' })
                    cached=$Cached; full=$Full
                } | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $outputDirectory "$startup-$Name.json")
                if ($Cached.aa -ne 'fsr3' -and -not $identical) {
                    throw "Cascade cache differs from full redraw at the same pose: $Name"
                }
            }
            function ShadowCacheRoundTrip([string]$Name) {
                $first = ShadowCacheCapture 'full' "$Name-full-before"
                RejectInvalidShadowCaches 'full'
                $cached = ShadowCacheCapture 'cascade' "$Name-cascade"
                RejectInvalidShadowCaches 'cascade'
                $last = ShadowCacheCapture 'full' "$Name-full-after"
                CompareShadowCaches $cached $last "$Name-cache-check"
                CompareShadowCaches $first $last "$Name-restoration-check"
            }
            function CompareCurrentShadowCache([string]$Name) {
                $cached = ShadowCacheCapture 'cascade' "$Name-cascade" $false
                $full = ShadowCacheCapture 'full' "$Name-full"
                CompareShadowCaches $cached $full "$Name-check"
                $null = Tool 'render.set_shadow_cache' @{mode='cascade'}
                # Settle the forced invalidation before measuring the next edit.
                $null = ShadowCacheCapture 'cascade' "$Name-cascade-restored" $false
            }
            # Translation is intentionally small: near cascades should cross
            # their snapped texels before all distant cascades need a refresh.
            $previous = ShadowCacheCapture 'cascade' 'partial-start'
            if ($Scenario -eq 'shadow-cache') {
                $shadowBitmap = [Drawing.Bitmap]::FromFile($previous.image.path)
                try {
                    $shadow = FixturePatch $shadowBitmap .555 .613
                    $lit = FixturePatch $shadowBitmap .68 .613
                    if ($lit.r - $shadow.r -lt 20 -or $lit.g - $shadow.g -lt 20) {
                        throw 'The partial-update fixture does not contain the expected sunlit floor and box shadow.'
                    }
                    [pscustomobject]@{shadow=$shadow; lit=$lit} | ConvertTo-Json |
                        Set-Content (Join-Path $outputDirectory "$startup-shadow-contrast.json")
                } finally { $shadowBitmap.Dispose() }
            } else {
                $shadowBitmap = [Drawing.Bitmap]::FromFile($previous.image.path)
                try {
                    $contrasts = @(AssertCascadeRangeShadows $shadowBitmap)
                    [pscustomobject]@{
                        initial_shadow_contrast_checked=$true
                        probes=$contrasts
                        note='Fixed startup floor patches establish shadows in all four cascade ranges; they do not quantify acne, contact bias or every blend-band pixel.'
                    } | ConvertTo-Json -Depth 5 |
                        Set-Content (Join-Path $outputDirectory "$startup-shadow-contrast.json")
                } finally { $shadowBitmap.Dispose() }
            }
            $partialSamples = @()
            for ($step=0; $step -lt 16 -and -not $script:shadowPartialObserved; ++$step) {
                try {
                    $null = Tool 'input.key' @{key='w';action='down'}
                    Start-Sleep -Milliseconds 15
                    $null = Tool 'input.key' @{key='w';action='up'}
                } finally { $null = Tool 'input.release_all' }
                $current = ShadowCacheCapture 'cascade' "partial-step-$step" $false
                $passes = $current.lighting.shadow_passes - $previous.lighting.shadow_passes
                $tiles = $current.lighting.shadow_tiles - $previous.lighting.shadow_tiles
                if ($passes -gt 0 -and $tiles -ge $passes -and $tiles -lt 4 * $passes) {
                    $script:shadowPartialObserved = $true
                }
                $partialSamples += [pscustomobject]@{step=$step; passes_delta=$passes; tiles_delta=$tiles; snapshot=$current}
                $previous = $current
            }
            [pscustomobject]@{partial_observed=$script:shadowPartialObserved; samples=$partialSamples} |
                ConvertTo-Json -Depth 8 | Set-Content (Join-Path $outputDirectory "$startup-partial-work.json")
            if (-not $script:shadowPartialObserved) {
                throw 'The procedural fixture did not exercise partial cascade work: no 0 < tiles_delta < 4 * passes_delta was observed.'
            }
            $full = ShadowCacheCapture 'full' 'partial-end-full'
            CompareShadowCaches $previous $full 'partial-image-check'
            $null = Tool 'render.set_shadow_cache' @{mode='cascade'}
            $null = ShadowCacheCapture 'cascade' 'partial-cascade-restored' $false
        }
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
        if ($state.structuredContent.render.anti_aliasing.msaa_2x_available) {
            $methods += 'msaa2x'
        }
        if ($state.structuredContent.render.anti_aliasing.msaa_4x_available) {
            $methods += 'msaa4x'
        }
        if ($state.structuredContent.render.anti_aliasing.fsr3_available) {
            $methods += 'fsr3'
        }
        for ($cycle = 0; $cycle -lt $Cycles; ++$cycle) {
            $null = Tool 'window.set_size' @{ width = 640 + 128 * $cycle; height = 360 + 72 * $cycle }
            $null = Tool 'render.set_present_mode' @{ mode = 'fifo' }
            $null = Tool 'render.set_present_mode' @{ mode = 'immediate' }
            if ($shadowCacheScenario) { CompareCurrentShadowCache "$cycle-resize" }
            foreach ($aa in $methods) {
                $state = Tool 'debug.get_state'
                if ($state.structuredContent.render.anti_aliasing.method -ne $aa) {
                    throw "AA switch did not select $aa"
                }
                if ($Scenario -ne 'rendering-pipeline' -and -not $shadowCacheScenario) {
                    $null = Tool 'input.mouse_move' @{ delta_x = 3; delta_y = -1 }
                }
                if ($shadowCacheScenario) {
                    ShadowCacheRoundTrip "$cycle-$aa-static"
                    $null = Tool 'render.set_shadow_cache' @{mode='cascade'}
                    $null = ShadowCacheCapture 'cascade' "$cycle-$aa-motion-start" $false
                    try {
                        $null = Tool 'input.mouse_move' @{delta_x=.5;delta_y=0}
                        # Capture completes a rendered frame, before release_all
                        # can clear the pending one-shot mouse delta.
                        $null = ShadowCacheCapture 'cascade' "$cycle-$aa-motion" $false
                    } finally { $null = Tool 'input.release_all' }
                    CompareCurrentShadowCache "$cycle-$aa-motion-result"
                    if ($aa -eq 'off') {
                        foreach ($resolution in @(1024,2048)) {
                            $null = Tool 'render.set_shadow_resolution' @{resolution=$resolution}
                            $applied = ShadowCacheCapture 'cascade' "$cycle-resolution-$resolution-applied" $false
                            if ($applied.lighting.shadow_resolution -ne $resolution -or
                                $applied.lighting.shadow_resolution_requested -ne $resolution) {
                                throw 'Shadow resolution was not applied during cache validation.'
                            }
                            CompareCurrentShadowCache "$cycle-resolution-$resolution"
                        }
                        $shadowHours = if ($Scenario -eq 'cascade-range') { @(7,17,14) } else { @(6,14) }
                        foreach ($hour in $shadowHours) {
                            $null = Tool 'lighting.set' @{hour=$hour;cycle=$false;gi=$false}
                            $script:shadowExpectedHour = $hour
                            CompareCurrentShadowCache "$cycle-hour-$hour"
                        }
                        if ($script:shadowRoofAvailable) {
                            foreach ($enabled in @($true,$false)) {
                                $beforeEdit = ShadowCacheState 'cascade'
                                $null = Tool 'test.set_roof' @{enabled=$enabled}
                                $edited = ShadowCacheCapture 'cascade' "$cycle-roof-$enabled-applied" $false
                                if ($edited.lighting.shadow_passes -le $beforeEdit.render.lighting.shadow_passes) {
                                    throw 'Roof scene invalidation did not refresh the shadow map.'
                                }
                                CompareCurrentShadowCache "$cycle-roof-$enabled"
                            }
                        }
                        [pscustomobject]@{
                            roof_scene_invalidation_checked=$script:shadowRoofAvailable
                            runtime_transform_invalidation_checked=$false
                            runtime_transform_note=$(if ($Scenario -eq 'cascade-range') {
                                'The runtime MCP profile exposes no transform-edit command; cascade-cache has no roof, so roof invalidation is not exercised.'
                            } else {
                                'The runtime MCP profile exposes no transform-edit command; roof scene invalidation is checked when available.'
                            })
                        } | ConvertTo-Json | Set-Content (Join-Path $outputDirectory "$startup-$cycle-invalidation-coverage.json")
                    }
                }
                if ($Scenario -eq 'rendering-pipeline') {
                    $before = PipelineCapture 'reinhard' $aa "$cycle-$aa-reinhard-before"
                    RejectInvalidMappers 'reinhard' $aa
                    $mapped = PipelineCapture 'gt7' $aa "$cycle-$aa-gt7"
                    RejectInvalidMappers 'gt7' $aa
                    $after = PipelineCapture 'reinhard' $aa "$cycle-$aa-reinhard-after"
                    if ($before.width -ne $mapped.width -or $before.height -ne $mapped.height -or
                        $before.width -ne $after.width -or $before.height -ne $after.height) {
                        throw "Tone mapper round trip changed capture dimensions: $aa"
                    }
                    $different = $before.pixel_sha256 -ne $mapped.pixel_sha256
                    $restored = $before.pixel_sha256 -eq $after.pixel_sha256
                    if (-not $different) { throw "Reinhard and GT7 produced identical pixels: $aa" }
                    if ($aa -ne 'fsr3' -and -not $restored) {
                        throw "Tone mapper round trip changed the static $aa image."
                    }
                    [pscustomobject]@{
                        aa=$aa; cycle=$cycle; reinhard_gt7_different=$different
                        restoration_identical=$restored; exact_restoration_required=($aa -ne 'fsr3')
                        temporal_history_note= $(if ($aa -eq 'fsr3') {
                            'FSR jitter/history can change restored pixels; exact equality is recorded but not required.'
                        } else { 'Static off/SMAA must restore every decoded pixel exactly.' })
                        before=$before; mapped=$mapped; after=$after
                    } | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $outputDirectory "$startup-$cycle-$aa-image-check.json")
                    $captures += 3
                }
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
                if ($Scenario -eq 'rendering-pipeline') {
                    $null = SavePipelineCapture $capture "$cycle-$aa-lifecycle" $true
                }
                if ($shadowCacheScenario) { $null = SavePipelineCapture $capture "$cycle-$aa-lifecycle" }
                $captures += 1
                $null = Tool 'input.key' @{ key = 'f6'; action = 'press' }
            }
            $null = Tool 'render.set_instances' @{ count = 100000 }
            $null = Tool 'render.set_mode' @{ mode = 'instancing' }
            if ($Scenario -eq 'shadow-resolution') { $null = ShadowCapture 1024 "$cycle-instancing" }
            $capture = Tool 'capture.screenshot'
            if ($Scenario -eq 'rendering-pipeline') { $null = SavePipelineCapture $capture "$cycle-instancing" }
            if ($shadowCacheScenario) { $null = SavePipelineCapture $capture "$cycle-instancing" }
            $null = Tool 'render.set_gpu_workload' @{ iterations = 8 }
            $null = Tool 'render.set_mode' @{ mode = 'gpu-saturation' }
            if ($Scenario -eq 'shadow-resolution') { $null = ShadowCapture 4096 "$cycle-saturation" }
            $capture = Tool 'capture.screenshot'
            if ($Scenario -eq 'rendering-pipeline') { $null = SavePipelineCapture $capture "$cycle-saturation" }
            if ($shadowCacheScenario) { $null = SavePipelineCapture $capture "$cycle-saturation" }
            $captures += 2
            $null = Tool 'render.set_gpu_workload' @{ iterations = 0 }
            $null = Tool 'render.set_mode' @{ mode = 'scene' }
            if ($Scenario -eq 'shadow-resolution') { $null = ShadowCapture 2048 "$cycle-scene"; $captures += 3 }
            if ($shadowCacheScenario) { CompareCurrentShadowCache "$cycle-scene-restored" }
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
            shadow_depth_requested = $ShadowDepth
            shadow_depth_actual = $script:validatedShadowDepth
            shadow_linear_filter = $script:validatedShadowLinearFilter
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
