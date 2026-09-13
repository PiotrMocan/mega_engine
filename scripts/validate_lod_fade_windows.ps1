param(
    [Parameter(Mandatory=$true)][string]$ExePath,
    [ValidateSet('coverage','geometry')][string]$Fixture='coverage',
    [ValidateSet('cpu','gpu')][string]$VisibilityBackend='cpu',
    [ValidatePattern('^[a-zA-Z0-9_-]*$')][string]$RunTag='',
    [ValidateRange(1,100)][int]$Startups=1,
    [ValidateRange(1,100)][int]$Cycles=1,
    [switch]$InventoryOnly,
    [switch]$CheckAssetRootGuards
)

$ErrorActionPreference = 'Stop'
$Fixture = $Fixture.ToLowerInvariant()
$VisibilityBackend = $VisibilityBackend.ToLowerInvariant()
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$exe = (Resolve-Path -LiteralPath $ExePath).Path
$tree = Split-Path (Split-Path $exe -Parent) -Parent
$allowedTrees = @('windows-debug','windows-release','windows-asan') |
    ForEach-Object { Join-Path $root "build\$_" }
if ($tree -notin $allowedTrees -or (Split-Path $exe -Leaf) -ine 'vulkan_cube.exe') {
    throw 'Use vulkan_cube.exe from an approved native Windows build tree.'
}
$fixtureDirectory = Join-Path $tree 'lod-fixture'
$manifestPath = Join-Path $fixtureDirectory 'fixture-manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.generator -cne 'vcr.lod-transition-fixture.v1') { throw 'Unknown fixture manifest.' }
$variant = @($manifest.variants | Where-Object {$_.variant -ceq $Fixture})
if ($variant.Count -ne 1) { throw 'The selected fixture must occur once in the manifest.' }
$variant = $variant[0]
$assetRoot = (Resolve-Path -LiteralPath (Join-Path $fixtureDirectory "$Fixture/assets")).Path
foreach ($file in $variant.files) {
    $path = [IO.Path]::GetFullPath((Join-Path $fixtureDirectory $file.path))
    if (-not $path.StartsWith($fixtureDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $file.sha256) {
        throw "Fixture file escaped its directory or differs from the manifest: $($file.path)"
    }
}
if ($variant.levels.Count -ne 3 -or ($variant.levels.triangles -join ',') -cne '128,32,2' -or
    $variant.levels[0].declared_error -ne 0 -or $variant.levels[1].declared_error -ne 0.0625 -or
    $variant.levels[2].declared_error -ne 0.25) {
    throw 'Validator requires the documented three-level fixture and error bounds.'
}
$canonicalPath = Join-Path $root 'assets/scenes/editor_scene.json'
$canonicalBefore = (Get-FileHash -LiteralPath $canonicalPath -Algorithm SHA256).Hash
$outputDirectory = Join-Path $tree "render-validation/lod-fade-$Fixture/$VisibilityBackend"
if ($RunTag) { $outputDirectory += "-$RunTag" }
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

# Identity transform has Frobenius scale sqrt(3); the plane's half extent is 1.
# lod_projection gives focal*(1+1/z). Solve error*scale*pixels/z=budget.
# GPU uses padded projection/guard terms; these distances are navigation hints,
# while actual LOD counts decide every gate. Native-AA FSR keeps the same size.
function LodBoundary([int]$Level, [double]$Budget, [int]$Height=360) {
    $error = @(0.0,0.0625,0.25)[$Level]
    $focal = $Height * 0.5 / [Math]::Tan(32.5 * [Math]::PI / 180)
    $a = $error * [Math]::Sqrt(3.0) * $focal
    return ($a + [Math]::Sqrt($a*$a + 4*$Budget*$a)) / (2*$Budget)
}
$inventory = [pscustomobject]@{
    executable=$exe; executable_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    fixture=$Fixture; asset_root=$assetRoot; manifest=$manifestPath; native_windows=$true
    triangles_per_lod=@(128,32,2); source_instances=1; visibility_backend=$VisibilityBackend
    dimensions=@(640,360); duration_seconds=2
    lod1_coarsen=(LodBoundary 1 0.8); lod1_refine=(LodBoundary 1 1.0)
    lod2_coarsen=(LodBoundary 2 0.8); lod2_refine=(LodBoundary 2 1.0)
    exact_coverage_modes=@('off','smaa'); fsr3='native-aa smoke only'
    asset_root_guards_requested=[bool]$CheckAssetRootGuards
    canonical_sha256=$canonicalBefore; gpu_launched=$false
}
$inventory | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $outputDirectory 'inventory.json')
if ($InventoryOnly) { $inventory | ConvertTo-Json -Depth 6; return }
Add-Type -AssemblyName System.Drawing

$script:lodProcess=$null; $script:lodStderr=$null; $script:lodCanRequest=$false; $script:lodStarted=$false
$script:lodRequest=0; $script:lodCaptures=0; $script:lodWidth=640; $script:lodHeight=360
$script:lodTrace=New-Object 'System.Collections.Generic.List[object]'
$script:lodEvidence=New-Object 'System.Collections.Generic.List[object]'
$script:lodInvalidRequests=0

function Request([string]$Method, [hashtable]$Arguments) {
    $script:lodRequest++
    $wire=@{jsonrpc='2.0';id=$script:lodRequest;method=$Method;params=$Arguments} | ConvertTo-Json -Depth 16 -Compress
    try {
        $script:lodProcess.StandardInput.WriteLine($wire)
        $script:lodProcess.StandardInput.Flush()
        $read=$script:lodProcess.StandardOutput.ReadLineAsync()
        if (-not $read.Wait(30000)) { throw "MCP timeout: $Method" }
        if ([string]::IsNullOrWhiteSpace($read.Result)) { throw 'MCP stdout closed.' }
        $response=$read.Result | ConvertFrom-Json
        if ($response.id -ne $script:lodRequest -or $null -ne $response.error) { throw "Invalid MCP response: $($read.Result)" }
        return $response.result
    } catch {
        # Never start a second stdout read while a timed-out ReadLineAsync lives.
        $script:lodCanRequest=$false
        throw
    }
}
function Tool([string]$Name, [hashtable]$Arguments=@{}, [bool]$AllowError=$false) {
    $result=Request 'tools/call' @{name=$Name;arguments=$Arguments}
    $script:lodTrace.Add([pscustomobject]@{
        request=$script:lodRequest; tool=$Name; arguments=$Arguments; is_error=[bool]$result.isError
        structured=$result.structuredContent
    })
    if ($result.isError -and -not $AllowError) { throw "Tool failed: $Name : $($result.content[0].text)" }
    return $result
}
function State { return (Tool 'debug.get_state').structuredContent }
function TotalLods($Snapshot) { return [int](($Snapshot.render.visibility.lod_instances | Measure-Object -Sum).Sum) }
function CameraKey($Snapshot) { return $Snapshot.camera | ConvertTo-Json -Depth 5 -Compress }
function AssertFixture($Snapshot, [string]$Pattern, [string]$Prepass, [string]$Aa) {
    $r=$Snapshot.render
    if ($r.mode -cne 'scene' -or $r.render_fixture -cne 'none' -or $r.assets_pending -ne 0 -or
        $r.visibility.submitted -ne 1 -or $r.visibility.visible -ne 1 -or
        $r.visibility_backend -cne $VisibilityBackend -or $r.depth_convention -cne 'reverse-z' -or
        $r.lod_fade_requested -cne $Pattern -or $r.lod_fade_actual -cne $Pattern -or
        $r.lod_fade_seconds_requested -ne 2 -or $r.lod_fade_seconds_actual -ne 2 -or
        $r.depth_prepass -cne $Prepass -or $r.anti_aliasing.method -cne $Aa -or
        $r.lighting.dynamic -ne $false -or $r.lighting.voxel_gi -ne $false -or
        $Snapshot.virtual_keys_held -ne 0) { throw 'Unexpected fixture/backend/fade/prepass/AA state.' }
    if ($Snapshot.window.pixel_width -ne $script:lodWidth -or $Snapshot.window.pixel_height -ne $script:lodHeight -or
        [Math]::Abs($Snapshot.camera.position[0]) -gt 0.01 -or [Math]::Abs($Snapshot.camera.position[1]) -gt 0.01 -or
        [Math]::Abs($Snapshot.camera.forward[0]) -gt 0.00001 -or [Math]::Abs($Snapshot.camera.forward[1]) -gt 0.00001 -or
        $Snapshot.camera.forward[2] -gt -0.9999) { throw 'Fixture camera orientation or dimensions changed.' }
    if ($Aa -eq 'fsr3' -and $r.anti_aliasing.quality -cne 'native-aa') { throw 'FSR smoke requires native-AA quality.' }
}
function WaitSettled([int]$Lod=-1) {
    $timer=[Diagnostics.Stopwatch]::StartNew(); $consecutive=0
    do {
        $s=State
        $ready=$s.render.assets_pending -eq 0 -and $s.render.visibility.submitted -eq 1 -and
            $s.render.visibility.visible -eq 1 -and (TotalLods $s) -eq 1 -and $s.virtual_keys_held -eq 0
        if ($Lod -ge 0) { $ready=$ready -and $s.render.visibility.lod_instances[$Lod] -eq 1 }
        if ($ready) { $consecutive++ } else { $consecutive=0 }
        if ($consecutive -ge 3) { return $s }
        Start-Sleep -Milliseconds 20
    } while ($timer.Elapsed.TotalSeconds -lt 10)
    throw "Fixture did not settle to one unique draw at LOD $Lod."
}
function MoveTo([double]$Distance) {
    $s=State; $start=[double]$s.camera.position[2]
    if ([Math]::Abs($Distance-$start) -lt 0.05) { return $s }
    $outward=$Distance -gt $start
    $key=if ($outward) {'s'} else {'w'}
    $timer=[Diagnostics.Stopwatch]::StartNew(); $previous=$start
    $null=Tool 'input.key' @{key='shift';action='down'}
    $null=Tool 'input.key' @{key=$key;action='down'}
    try {
        do {
            Start-Sleep -Milliseconds 15
            $s=State; $z=[double]$s.camera.position[2]
            # This stronger sampled bound excludes any >25m/frame camera cut.
            if ([Math]::Abs($z-$previous) -ge 25) { throw 'Camera motion exceeded the fade cut bound.' }
            $previous=$z
            if ($z -lt 1 -or $z -gt 400) { throw 'Camera left the bounded fixture range.' }
            if (($outward -and $z -ge $Distance) -or (-not $outward -and $z -le $Distance)) { break }
        } while ($timer.Elapsed.TotalSeconds -lt 30)
        if ($timer.Elapsed.TotalSeconds -ge 30) { throw 'Camera movement timed out.' }
    } finally { if ($script:lodCanRequest) { $null=Tool 'input.release_all' } }
    $a=State; $b=State
    if ((CameraKey $a) -cne (CameraKey $b)) { throw 'Camera continued moving after release_all.' }
    return $b
}
function SetAa([string]$Aa) {
    for ($attempt=0; $attempt -lt 4; ++$attempt) {
        $s=State
        if ($s.render.anti_aliasing.method -ceq $Aa) { return }
        $null=Tool 'input.key' @{key='f6';action='press'}
        $null=Tool 'input.release_all'
    }
    throw "AA mode did not select $Aa."
}
function SetFade([string]$Pattern) { $null=Tool 'render.set_lod_fade' @{pattern=$Pattern;seconds=2} }
function Capture([string]$Name, [bool]$RequireDual=$false) {
    $before=State
    if ($RequireDual -and (TotalLods $before) -ne 2) { throw "No active two-LOD draw before capture $Name." }
    $result=Tool 'capture.screenshot'
    $after=State
    if ((CameraKey $before) -cne (CameraKey $after) -or $after.virtual_keys_held -ne 0) { throw 'Capture moved the camera.' }
    if ($RequireDual -and (TotalLods $after) -ne 2) { throw "Fade retired/reset during capture $Name; this is not transition evidence." }
    $images=@($result.content | Where-Object {$_.type -eq 'image'})
    if ($images.Count -ne 1 -or $images[0].mimeType -cne 'image/png' -or
        $result.structuredContent.width -ne $script:lodWidth -or $result.structuredContent.height -ne $script:lodHeight) {
        throw 'Missing PNG or incorrect capture dimensions.'
    }
    $path=Join-Path $outputDirectory "$startup-$Name.png"
    [IO.File]::WriteAllBytes($path,[Convert]::FromBase64String($images[0].data))
    $bitmap=[Drawing.Bitmap]::FromFile($path)
    try {
        $center=$bitmap.GetPixel([int]($bitmap.Width/2),[int]($bitmap.Height/2))
        $background=$bitmap.GetPixel(0,$bitmap.Height-1)
        if (($center.R+$center.G+$center.B) -le ($background.R+$background.G+$background.B+20)) { throw 'Fixture surface is not visible at image center.' }
        $rectangle=New-Object Drawing.Rectangle(0,0,$bitmap.Width,$bitmap.Height)
        $locked=$bitmap.LockBits($rectangle,[Drawing.Imaging.ImageLockMode]::ReadOnly,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $rowBytes=$bitmap.Width*4; $pixels=New-Object byte[] ($rowBytes*$bitmap.Height)
            for ($y=0; $y -lt $bitmap.Height; ++$y) {
                [Runtime.InteropServices.Marshal]::Copy([IntPtr]::Add($locked.Scan0,$y*$locked.Stride),$pixels,$y*$rowBytes,$rowBytes)
            }
        } finally { $bitmap.UnlockBits($locked) }
        $sha=[Security.Cryptography.SHA256]::Create()
        try { $hash=[BitConverter]::ToString($sha.ComputeHash($pixels)).Replace('-','') }
        finally { $sha.Dispose() }
    } finally { $bitmap.Dispose() }
    $script:lodCaptures++
    $saved=[pscustomobject]@{path=$path;pixel_sha256=$hash;width=$script:lodWidth;height=$script:lodHeight;before=$before;after=$after;dual_required=$RequireDual}
    $saved | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $outputDirectory "$startup-$Name.json")
    return $saved
}
function CompareCapture($Fading, $Reference, [string]$Name) {
    if ((CameraKey $Fading.after) -cne (CameraKey $Reference.after) -or
        $Fading.width -ne $Reference.width -or $Fading.height -ne $Reference.height -or
        $Fading.after.render.anti_aliasing.method -cne $Reference.after.render.anti_aliasing.method -or
        $Fading.after.render.depth_prepass -cne $Reference.after.render.depth_prepass) { throw 'Comparison changed pose, size, AA or prepass.' }
    $exactRequired=$Fixture -eq 'coverage' -and $Fading.after.render.anti_aliasing.method -ne 'fsr3'
    $identical=$Fading.pixel_sha256 -ceq $Reference.pixel_sha256
    $check=[pscustomobject]@{
        name=$Name;exact_required=$exactRequired;exact=$identical;passed=(-not $exactRequired -or $identical)
        fading=$Fading.path;reference=$Reference.path;fading_lod_counts=$Fading.after.render.visibility.lod_instances
        reference_lod_counts=$Reference.after.render.visibility.lod_instances;camera=$Reference.after.camera
        note=$(if ($exactRequired) {'Every decoded RGBA pixel must match.'} else {'Smoke only: temporal history or differing geometry has no exact-pixel contract.'})
    }
    $script:lodEvidence.Add($check)
    $check | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $outputDirectory "$startup-$Name-comparison.json")
    if (-not $check.passed) { throw "Coverage pixels differ during fade: $Name" }
}
function InvalidPayloads {
    $null=Tool 'render.set_lod_fade' @{pattern='bayer4';seconds=0.75}
    $null=Tool 'render.set_lod_fade' @{pattern='bayer8'}
    $s=State
    if ($s.render.lod_fade_seconds_actual -ne 0.75 -or $s.render.lod_fade_requested -cne 'bayer8') { throw 'Omitted duration was not preserved.' }
    SetFade 'bayer8'; $before=State
    foreach ($bad in @(
        @{}, @{pattern=''}, @{pattern='Bayer4'}, @{pattern='bayer16'}, @{pattern='off '},
        @{pattern=$null}, @{pattern=$true}, @{pattern=1}, @{pattern=@('off')},
        @{pattern=@{value='off'}}, @{mode='off'}, @{pattern='off';extra=1},
        @{pattern='bayer4';seconds=0}, @{pattern='bayer4';seconds=0.049999}, @{pattern='bayer4';seconds=2.000001},
        @{pattern='bayer4';seconds=-1}, @{pattern='bayer4';seconds=$null}, @{pattern='bayer4';seconds=$true},
        @{pattern='bayer4';seconds='0.25'}, @{pattern='bayer4';seconds=@(0.25)}
    )) {
        $r=Tool 'render.set_lod_fade' $bad $true
        $after=State
        if (-not $r.isError -or $after.render.lod_fade_requested -cne 'bayer8' -or
            $after.render.lod_fade_seconds_actual -ne 2 -or (CameraKey $before) -cne (CameraKey $after)) { throw 'Invalid fade payload mutated state or was accepted.' }
        $script:lodInvalidRequests++
    }
    SetFade 'off'
}
function Transition([int]$TargetLod, [string]$Pattern, [string]$Prepass, [string]$Aa, [string]$Name) {
    SetFade 'off'
    # First remove incoming hysteresis in a guaranteed previous-LOD zone.
    $anchor=if ($TargetLod -eq 1) {12.0} else {((LodBoundary 1 0.8)+(LodBoundary 2 1.0))*0.5}
    $null=MoveTo $anchor
    $null=WaitSettled ($TargetLod-1)
    $boundary=LodBoundary $TargetLod 0.8
    $null=MoveTo ($boundary-4)
    $start=WaitSettled ($TargetLod-1)
    SetFade $Pattern
    $null=WaitSettled ($TargetLod-1)
    $stopped=MoveTo ($boundary+3)
    AssertFixture $stopped $Pattern $Prepass $Aa
    if ([Math]::Abs($stopped.camera.position[2]-$start.camera.position[2]) -ge 25 -or
        (TotalLods $stopped) -ne 2 -or $stopped.render.visibility.lod_instances[$TargetLod] -ne 1 -or
        $stopped.render.visibility.lod_instances[$TargetLod-1] -ne 1) { throw 'The bounded camera move did not leave a live two-LOD transition.' }
    # Both forms must preserve an already active transition, not reset/snap it.
    $null=Tool 'render.set_lod_fade' @{pattern=$Pattern}
    $null=Tool 'render.set_lod_fade' @{pattern=$Pattern;seconds=2}
    $noop=State
    if ((TotalLods $noop) -ne 2 -or $noop.render.lod_fade_seconds_actual -ne 2) { throw 'No-op fade request reset the transition or duration.' }
    $fading=Capture "$Name-fading" $true
    SetFade 'off'
    $settled=WaitSettled $TargetLod
    AssertFixture $settled 'off' $Prepass $Aa
    $reference=Capture "$Name-reference"
    CompareCapture $fading $reference $Name
    return $reference
}
function StartRenderer([string]$RootMode='fixture') {
    $info=New-Object Diagnostics.ProcessStartInfo
    $info.FileName=$exe; $info.WorkingDirectory=$outputDirectory
    $info.Arguments="--mcp-stdio --validation --width 640 --height 360 --present immediate --aa off --fsr-quality native-aa --lighting legacy --gi off --day-cycle off --depth-prepass off --lod on --lod-fade off --lod-fade-seconds 2 --visibility-backend $VisibilityBackend --culling frustum"
    $info.UseShellExecute=$false; $info.CreateNoWindow=$true
    $info.RedirectStandardInput=$true; $info.RedirectStandardOutput=$true; $info.RedirectStandardError=$true
    $info.EnvironmentVariables['VK_VALIDATION_VALIDATE_SYNC']='true'
    if ($RootMode -eq 'fixture') { $info.EnvironmentVariables['VCR_ASSET_ROOT']=$assetRoot }
    elseif ($RootMode -eq 'invalid') { $info.EnvironmentVariables['VCR_ASSET_ROOT']=Join-Path $outputDirectory 'missing-assets-root' }
    else { $null=$info.EnvironmentVariables.Remove('VCR_ASSET_ROOT') }
    $script:lodProcess=New-Object Diagnostics.Process
    $script:lodProcess.StartInfo=$info
    $script:lodRequest=0; $script:lodCanRequest=$false; $script:lodStderr=$null; $script:lodStarted=$false
    if (-not $script:lodProcess.Start()) { throw 'Renderer did not start.' }
    $script:lodStarted=$true
    $script:lodStderr=$script:lodProcess.StandardError.ReadToEndAsync()
    if ($RootMode -ne 'invalid') {
        $null=Request 'initialize' @{protocolVersion='2025-11-25';capabilities=@{};clientInfo=@{name='vcr-lod-fade-validation';version='1.0'}}
        $script:lodProcess.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized"}')
        $script:lodProcess.StandardInput.Flush(); $script:lodCanRequest=$true
    }
}
function StopRenderer([string]$LogPath, [bool]$ExpectedFailure=$false) {
    if (-not $ExpectedFailure) { $null=Tool 'input.release_all'; $null=Tool 'engine.quit'; $script:lodProcess.StandardInput.Close() }
    if (-not $script:lodProcess.WaitForExit(15000)) { throw 'Renderer shutdown timed out.' }
    $script:lodCanRequest=$false
    $stderr=$script:lodStderr.Result; [IO.File]::WriteAllText($LogPath,$stderr)
    if ($ExpectedFailure) {
        if ($script:lodProcess.ExitCode -eq 0 -or $stderr -notmatch 'Unable to locate the runtime assets directory' -or
            $stderr -match 'Loaded scene:') { throw 'Invalid explicit asset root did not fail before scene fallback.' }
    } elseif ($script:lodProcess.ExitCode -ne 0 -or $stderr -notmatch 'Live GPU allocations: 0 \(0 bytes\)' -or
              $stderr -match 'validation requested.*unavailable') { throw 'Shutdown, validation availability or live allocation gate failed.' }
    if ($stderr -match '\[Vulkan ERROR\]|Validation Error|VUID-|SYNC-HAZARD|hazard detected|FidelityFX error:|AddressSanitizer|Live GPU allocations: [1-9]') {
        throw "Vulkan/ASan validation failure: $LogPath"
    }
    return $stderr
}
function DisposeRenderer([string]$LogPath) {
    if ($null -eq $script:lodProcess) { return }
    if ($script:lodStarted -and -not $script:lodProcess.HasExited) {
        if ($script:lodCanRequest) {
            try { $null=Tool 'input.release_all'; $null=Tool 'engine.quit'; $null=$script:lodProcess.WaitForExit(5000) } catch { }
        }
        if (-not $script:lodProcess.HasExited) { $script:lodProcess.Kill(); $script:lodProcess.WaitForExit() }
    }
    if ($null -ne $script:lodStderr -and $script:lodStderr.IsCompleted) { [IO.File]::WriteAllText($LogPath,$script:lodStderr.Result) }
    $script:lodProcess.Dispose(); $script:lodProcess=$null; $script:lodCanRequest=$false; $script:lodStarted=$false
}

for ($startup=1; $startup -le $Startups; ++$startup) {
    $logPath=Join-Path $outputDirectory "startup-$startup.log"
    $script:lodCaptures=0; $script:lodInvalidRequests=0; $script:lodTrace.Clear(); $script:lodEvidence.Clear()
    $script:lodWidth=640; $script:lodHeight=360; $passed=$false; $failure=$null
    try {
        StartRenderer
        $capabilities=(Tool 'debug.get_capabilities').structuredContent
        if (-not $capabilities.validation_enabled -or -not $capabilities.gpu_timestamps -or -not $capabilities.screenshot_supported) { throw 'Required validation/timestamp/capture capability is unavailable.' }
        $null=Tool 'window.set_mouse_capture' @{enabled=$false}
        $null=Tool 'input.release_all'
        $initial=WaitSettled 0
        AssertFixture $initial 'off' 'off' 'off'
        if ($initial.render.triangles -ne 128 -or [Math]::Abs($initial.camera.position[2]-$variant.camera_z) -gt 0.001) {
            throw 'Generated asset root was not applied: initial topology or authored camera differs.'
        }
        $initial | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $outputDirectory "$startup-initial-state.json")
        InvalidPayloads
        $methods=@('off','smaa'); if ($initial.render.anti_aliasing.fsr3_available) { $methods+='fsr3' }
        for ($cycle=0; $cycle -lt $Cycles; ++$cycle) {
            foreach ($aa in $methods) {
                SetAa $aa
                foreach ($prepass in @('off','partial','full')) {
                    $null=Tool 'render.set_depth_prepass' @{mode=$prepass}
                    foreach ($pattern in @('bayer4','bayer8')) {
                        foreach ($level in @(1,2)) {
                            $null=Transition $level $pattern $prepass $aa "$cycle-$aa-$prepass-$pattern-lod$level"
                        }
                    }
                }
            }
            SetAa 'off'; SetFade 'off'
            $null=Tool 'render.set_depth_prepass' @{mode='off'}
            $null=MoveTo 12; $null=WaitSettled 0
            $baseline=Capture "$cycle-reset-baseline"
            SetFade 'bayer8'
            $null=Tool 'window.set_size' @{width=1280;height=720}
            $script:lodWidth=1280; $script:lodHeight=720
            $null=WaitSettled 0
            $null=Capture "$cycle-resized"
            $null=Tool 'window.set_size' @{width=640;height=360}
            $script:lodWidth=640; $script:lodHeight=360
            $null=WaitSettled 0
            $restored=Capture "$cycle-resize-restored"
            CompareCapture $restored $baseline "$cycle-resize"
            $null=Tool 'render.set_instances' @{count=8}
            $null=Tool 'render.set_mode' @{mode='instancing'}
            $direct=State
            if ($direct.render.lod_fade_requested -cne 'bayer8' -or $direct.render.lod_fade_actual -cne 'off') { throw 'Instancing must retain requested fade with actual off.' }
            $null=Tool 'render.set_mode' @{mode='scene'}
            $scene=WaitSettled 0
            AssertFixture $scene 'bayer8' 'off' 'off'
            $restored=Capture "$cycle-mode-restored"
            CompareCapture $restored $baseline "$cycle-mode"
        }
        $null=StopRenderer $logPath
        $passed=$true
    } catch { $failure=$_.Exception.Message; throw }
    finally {
        DisposeRenderer $logPath
        $script:lodTrace | ConvertTo-Json -Depth 16 | Set-Content (Join-Path $outputDirectory "$startup-mcp-trace.json")
        $summary=[pscustomobject]@{
            passed=$passed;failure=$failure;startup=$startup;cycles=$Cycles;fixture=$Fixture;visibility_backend=$VisibilityBackend
            captures=$script:lodCaptures;invalid_requests_rejected=$script:lodInvalidRequests
            comparisons=@($script:lodEvidence.ToArray());asset_root=$assetRoot
            canonical_unchanged=((Get-FileHash -LiteralPath $canonicalPath -Algorithm SHA256).Hash -ceq $canonicalBefore)
            limits='Reversal continuity and third-target timing are covered by math tests; this script currently gates coarsening captures, settled reference, resize and mode restoration.'
        }
        $summary | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $outputDirectory "$startup-summary.json")
        if (-not $summary.canonical_unchanged) { throw 'Canonical scene changed during validation.' }
        $summary | Select-Object passed,startup,cycles,fixture,visibility_backend,captures,invalid_requests_rejected,canonical_unchanged | ConvertTo-Json -Compress
    }
}

if ($CheckAssetRootGuards) {
    $guards=@()
    foreach ($mode in @('invalid','unset')) {
        $logPath=Join-Path $outputDirectory "asset-root-$mode.log"
        try {
            StartRenderer $mode
            if ($mode -eq 'invalid') { $null=StopRenderer $logPath $true }
            else {
                $timer=[Diagnostics.Stopwatch]::StartNew()
                do { $s=State; if ($s.render.assets_pending -eq 0) { break }; Start-Sleep -Milliseconds 50 } while ($timer.Elapsed.TotalSeconds -lt 15)
                if ($s.render.assets_pending -ne 0) { throw 'Canonical no-override load did not settle.' }
                $log=StopRenderer $logPath
                $normalizedLog=$log.Replace('\\','\').Replace('/','\')
                if (-not $normalizedLog.Contains($canonicalPath)) { throw 'No-override process did not report the canonical scene path.' }
            }
            $guards+=[pscustomobject]@{mode=$mode;passed=$true;log=$logPath}
        } finally { DisposeRenderer $logPath }
    }
    $guards | ConvertTo-Json | Set-Content (Join-Path $outputDirectory 'asset-root-guards.json')
    if ((Get-FileHash -LiteralPath $canonicalPath -Algorithm SHA256).Hash -cne $canonicalBefore) { throw 'Asset-root guards changed the canonical scene.' }
}
