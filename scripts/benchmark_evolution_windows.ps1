param(
    [ValidateSet('Compatibility','Visibility','Lod','Shadows','ShadowResolution','Default','Adaptive','Lighting','LightingStress','LightingVoxel','LightingQuality','LightingQualityStress')][string]$Suite='Visibility',
    [ValidateRange(3,20)][int]$Repeats=3,
    [ValidateRange(1,60)][int]$Warmup=5,
    [ValidateRange(2,300)][int]$Duration=15,
    [string]$BaselineExe='',
    [ValidateSet('legacy','direct','voxel')][string[]]$LightingModes=@('legacy','direct','voxel'),
    [string]$OutputName='evolution-benchmark'
)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$tree=Join-Path $root 'build/windows-release'
$exe=(Resolve-Path (Join-Path $tree 'bin/vulkan_cube.exe')).Path
if ($OutputName -notmatch '^[a-zA-Z0-9_-]+$') { throw 'OutputName must be a directory name, not a path.' }
$out=Join-Path $tree $OutputName
New-Item -ItemType Directory -Force -Path $out | Out-Null
$cases=@()
function Add-Case([string]$Name,[string]$Arguments,[string]$Executable=$exe) {
    $script:cases+=@{Name=$Name; Arguments=$Arguments; Executable=$Executable}
}
switch ($Suite) {
    'ShadowResolution' {
        if (-not $BaselineExe) { throw 'Provide the baseline executable with matching shaders/assets.' }
        $baseline=(Resolve-Path -LiteralPath $BaselineExe).Path
        # Camera motion exercises cascade refresh and atlas fill cost.
        $arguments='--aa off --lighting dynamic --gi off --animate-camera'
        Add-Case 'before-2048' $arguments $baseline
        foreach($resolution in @(2048,1024,4096,8192)) {
            Add-Case "after-$resolution" "$arguments --shadow-resolution $resolution"
        }
    }
    {$_ -in @('LightingQuality','LightingQualityStress')} {
        if (-not $BaselineExe) {throw 'Provide the baseline executable and matching shaders/assets.'}
        $baseline=(Resolve-Path -LiteralPath $BaselineExe).Path
        foreach ($mode in $LightingModes) {
            $arguments=switch($mode) {
                'legacy' {'--aa off --lighting legacy'}
                'direct' {'--aa off --lighting dynamic --gi off'}
                'voxel' {'--aa off --lighting dynamic --gi voxel'}
            }
            if($Suite -eq 'LightingQualityStress') {
                $arguments+=' --scene-count 100000 --animate-scene --animate-camera'
            }
            Add-Case "before-$mode" $arguments $baseline
            Add-Case "after-$mode" $arguments
        }
    }
    'Compatibility' {
        if (-not $BaselineExe) { throw 'Provide the baseline executable with its matching shaders and runtime DLL.' }
        $baseline=(Resolve-Path -LiteralPath $BaselineExe).Path
        foreach ($aa in @('off','smaa','fsr3')) {
            Add-Case "before-$aa" "--aa $aa" $baseline
            Add-Case "compat-$aa" "--aa $aa --culling none --lod off --dynamic-shadows off"
        }
    }
    'Lighting' {
        if (-not $BaselineExe) {throw 'Provide the baseline executable and matching shaders/assets.'}
        Add-Case 'before' '--aa off' (Resolve-Path -LiteralPath $BaselineExe).Path
        Add-Case 'legacy' '--aa off --lighting legacy'
        Add-Case 'dynamic-direct' '--aa off --gi off'
        Add-Case 'dynamic-voxel' '--aa off --gi voxel'
    }
    'LightingVoxel' {
        Add-Case 'dynamic-voxel' '--aa off --gi voxel'
        Add-Case 'stress-voxel' '--aa off --scene-count 100000 --animate-scene --animate-camera --gi voxel'
    }
    'LightingStress' {
        $scene='--aa off --scene-count 100000 --animate-scene --animate-camera'
        Add-Case 'stress-direct' "$scene --gi off"
        Add-Case 'stress-voxel' "$scene --gi voxel"
    }
    'Default' { Add-Case 'default-off' '--aa off' }
    'Adaptive' {
        $scene='--aa off --scene-count 100000 --animate-camera'
        Add-Case 'open-direct' "$scene --scene-walls off --visibility-backend cpu --culling none --lod off"
        Add-Case 'open-auto' "$scene --scene-walls off --visibility-backend auto"
        Add-Case 'walls-auto' "$scene --visibility-backend auto"
    }
    'Visibility' {
        $scene='--aa off --scene-count 100000 --animate-camera'
        Add-Case 'walls-direct' "$scene --visibility-backend cpu --culling none --lod off"
        Add-Case 'walls-frustum' "$scene --visibility-backend gpu --culling frustum"
        Add-Case 'walls-occlusion' "$scene --visibility-backend gpu --culling occlusion"
        Add-Case 'open-frustum' "$scene --scene-walls off --visibility-backend gpu --culling frustum"
        Add-Case 'open-occlusion' "$scene --scene-walls off --visibility-backend gpu --culling occlusion"
    }
    'Lod' {
        $scene='--aa off --scene-count 1024 --scene-walls off --scene-asset tree_stump_01_4k/tree_stump_01_4k.vcrasset.json --scene-scale .02 --camera-z 300 --culling frustum'
        Add-Case 'lod-off' "$scene --lod off"
        Add-Case 'lod-on' "$scene --lod on"
    }
    'Shadows' {
        Add-Case 'shadow-off' '--aa off --scene-count 100000 --animate-scene --dynamic-shadows off'
        Add-Case 'shadow-on' '--aa off --scene-count 100000 --animate-scene --dynamic-shadows on'
    }
}
$environment=@{
    DateUtc=[DateTime]::UtcNow.ToString('o'); Suite=$Suite
    CPU=(Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name)
    GPU=(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion)
    PowerScheme=(& powercfg.exe /getactivescheme | Out-String).Trim()
    Clocks='Driver controlled; clocks and temperature are not locked or sampled.'
    Processes=(Get-Process | Select-Object ProcessName,Id,CPU)
    Cases=$cases; Warmup=$Warmup; Duration=$Duration; Repeats=$Repeats
}
$environment | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out "$Suite-environment.json")
# Interleave cases by repetition to reduce drift between the two sides of A/B.
for ($run=1;$run -le $Repeats;++$run) {
    foreach ($case in $cases) {
        $name="$($case.Name)-$run"
        if (Test-Path (Join-Path $out "$name.json")) { throw "Result already exists: $name; use a fresh OutputName." }
        $info=New-Object System.Diagnostics.ProcessStartInfo
        $info.FileName=$case.Executable
        $info.Arguments="--benchmark --warmup $Warmup --duration $Duration --width 1920 --height 1080 --present immediate --no-validation $($case.Arguments)"
        $info.WorkingDirectory=$out
        $info.UseShellExecute=$false; $info.CreateNoWindow=$true
        $info.RedirectStandardOutput=$true; $info.RedirectStandardError=$true
        $process=[Diagnostics.Process]::Start($info)
        $stdout=$process.StandardOutput.ReadToEndAsync(); $stderr=$process.StandardError.ReadToEndAsync()
        try {
            if (-not $process.WaitForExit(($Warmup+$Duration+120)*1000)) {
                $process.Kill(); $process.WaitForExit()
                throw "Benchmark timed out: $name"
            }
            [IO.File]::WriteAllText((Join-Path $out "$name.log"),$stdout.Result+$stderr.Result)
            if ($process.ExitCode -ne 0) { throw "Benchmark failed: $name" }
            $result=Get-Content -Raw (Join-Path $out 'benchmark.json') | ConvertFrom-Json
            if ($result.build_type -ne 'Release' -or $result.present_mode -ne 'IMMEDIATE' -or $result.resolution -ne '1920x1080') {
                throw "Benchmark configuration changed: $name"
            }
            if ($Suite -eq 'ShadowResolution' -and $case.Name -match '^after-(\d+)$' -and
                $result.shadow_resolution -ne [int]$Matches[1]) {
                throw "Shadow resolution was not applied: $name"
            }
            Copy-Item -LiteralPath (Join-Path $out 'benchmark.json') -Destination (Join-Path $out "$name.json")
            '{0}: {1:N1} FPS; GPU {2:N4} ms' -f $name,$result.average_fps,$result.average_gpu_frame_ms
        } finally { $process.Dispose() }
    }
}
