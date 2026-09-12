param(
    [Parameter(Mandatory=$true)][string]$ExePath,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('off','smaa','fsr3')][string[]]$Methods=@('off','smaa','fsr3'),
    [ValidateSet('off','voxel')][string[]]$GiModes=@('off','voxel'),
    [ValidateRange(2,32)][int]$Frames=6,
    [string]$ExtraArguments='',
    [float]$CameraDeltaX=0,
    [float]$CameraDeltaY=0,
    [switch]$Validation
)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outputPath=[IO.Path]::GetFullPath($OutputDirectory)
$allowed=@('windows-release','windows-debug','windows-asan') | ForEach-Object {
    [IO.Path]::GetFullPath((Join-Path $root "build/$_"))+[IO.Path]::DirectorySeparatorChar
}
if (-not @($allowed | Where-Object {$outputPath.StartsWith($_,[StringComparison]::OrdinalIgnoreCase)}).Count) {
    throw 'Capture output must be inside an approved native Windows build tree.'
}
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$exe=(Resolve-Path -LiteralPath $ExePath).Path
Add-Type -AssemblyName System.Drawing
foreach($gi in $GiModes) { foreach($aa in $Methods) {
    $name="gi-$gi-aa-$aa"
    $info=New-Object Diagnostics.ProcessStartInfo
    $info.FileName=$exe
    $validationFlag=if($Validation){'--validation'}else{'--no-validation'}
    $info.Arguments="--mcp-stdio $validationFlag --width 1280 --height 720 --aa $aa --lighting dynamic --gi $gi --day-cycle off --time-of-day 14 $ExtraArguments"
    $info.WorkingDirectory=$outputPath
    $info.UseShellExecute=$false; $info.CreateNoWindow=$true
    $info.RedirectStandardInput=$true; $info.RedirectStandardOutput=$true; $info.RedirectStandardError=$true
    $info.EnvironmentVariables['VK_VALIDATION_VALIDATE_SYNC']='true'
    $process=[Diagnostics.Process]::Start($info)
    $stderr=$process.StandardError.ReadToEndAsync()
    $script:request=0
    function Request([string]$Method,[hashtable]$Arguments) {
        $script:request++
        $message=@{jsonrpc='2.0';id=$script:request;method=$Method;params=$Arguments} | ConvertTo-Json -Depth 12 -Compress
        $process.StandardInput.WriteLine($message);$process.StandardInput.Flush()
        $read=$process.StandardOutput.ReadLineAsync()
        if(-not $read.Wait(30000)){throw "Timed out: $Method"}
        $response=$read.Result | ConvertFrom-Json
        if($response.error -or $response.id -ne $script:request -or $response.result.isError){throw "MCP failure: $($read.Result)"}
        return $response.result
    }
    function Tool([string]$Name,[hashtable]$Arguments=@{}) {return Request 'tools/call' @{name=$Name;arguments=$Arguments}}
    try {
        $null=Request 'initialize' @{protocolVersion='2025-11-25';capabilities=@{};clientInfo=@{name='shadow-regression';version='1'}}
        $process.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized"}')
        $process.StandardInput.Flush()
        Start-Sleep -Milliseconds 1500
        if($CameraDeltaX -ne 0 -or $CameraDeltaY -ne 0) {
            $null=Tool 'input.mouse_move' @{delta_x=$CameraDeltaX;delta_y=$CameraDeltaY}
            Start-Sleep -Milliseconds 500
        }
        $state=(Tool 'debug.get_state').structuredContent
        if($state.render.anti_aliasing.method -ne $aa){throw "Unavailable AA: $aa"}
        if($state.render.lighting.cycle){throw 'Static shadow validation requires stopped daylight.'}
        if(-not $state.render.lighting.dynamic -or $state.render.lighting.voxel_gi -ne ($gi -eq 'voxel')) {
            throw 'Requested dynamic lighting/GI configuration was not applied.'
        }
        $pairs=@()
        $reference=$null
        try {
            for($frame=0;$frame -lt $Frames;$frame++) {
                $capture=Tool 'capture.screenshot'
                $image=@($capture.content | Where-Object {$_.type -eq 'image'})[0]
                $path=Join-Path $outputPath "$name-$frame.png"
                [IO.File]::WriteAllBytes($path,[Convert]::FromBase64String($image.data))
                $bitmap=[Drawing.Bitmap]::FromFile($path)
                if($null -eq $reference){$reference=$bitmap;continue}
                try {
                    $sum=0.0;$changed=0;$samples=0;$maximum=0
                    # Exclude the HUD; sample a fixed grid over the actual scene.
                    for($y=72;$y -lt $bitmap.Height;$y+=4){for($x=0;$x -lt $bitmap.Width;$x+=4){
                        $a=$reference.GetPixel($x,$y);$b=$bitmap.GetPixel($x,$y)
                        $difference=[Math]::Max([Math]::Abs([int]$a.R-$b.R),[Math]::Max([Math]::Abs([int]$a.G-$b.G),[Math]::Abs([int]$a.B-$b.B)))
                        $sum+=$difference;$maximum=[Math]::Max($maximum,$difference)
                        if($difference -gt 2){$changed++};$samples++
                    }}
                    $pairs+=@{frame=$frame;mean_max_channel_delta=$sum/$samples;changed_over_2_fraction=$changed/$samples;max_channel_delta=$maximum}
                } finally {$bitmap.Dispose()}
                Start-Sleep -Milliseconds 80
            }
        } finally {if($null -ne $reference){$reference.Dispose()}}
        $endState=(Tool 'debug.get_state').structuredContent
        if($state.render.lighting.hour -ne $endState.render.lighting.hour){throw 'Paused time changed during capture.'}
        $null=Tool 'engine.quit';$process.StandardInput.Close()
        if(-not $process.WaitForExit(15000)){throw 'Shutdown timed out'}
        if($process.ExitCode -ne 0 -or $stderr.Result -match 'VUID-|SYNC-HAZARD|\[Vulkan ERROR\]|AddressSanitizer|Live GPU allocations: [1-9]'){throw 'Renderer validation failed'}
        if($Validation -and ($stderr.Result -match 'validation requested.*unavailable' -or $stderr.Result -notmatch 'Live GPU allocations: 0 \(0 bytes\)')) {
            throw 'Validation layer or allocation check was unavailable.'
        }
        @{case=$name;before=$state;after=$endState;differences=$pairs} | ConvertTo-Json -Depth 15 | Set-Content (Join-Path $outputPath "$name.json")
        Write-Output "$name : $($pairs | ConvertTo-Json -Compress)"
        # Off/SMAA have no temporal sampling and must be identical on this static
        # fixture. FSR deliberately jitters: retain its measured differences as
        # diagnostics, without treating arbitrary jitter as a shadow regression.
        if($aa -ne 'fsr3' -and @($pairs | Where-Object {$_.max_channel_delta -ne 0}).Count) {
            throw "Static image changed with $aa; inspect $name captures."
        }
    } finally {
        if(-not $process.HasExited){$process.Kill();$process.WaitForExit()}
        [IO.File]::WriteAllText((Join-Path $outputPath "$name.log"),$stderr.Result)
        $process.Dispose()
    }
}}
