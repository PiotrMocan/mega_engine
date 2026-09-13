param(
    [string]$ExePath='build/windows-release/bin/vulkan_cube.exe',
    [ValidateRange(3,10)][int]$Repeats=3,
    [ValidateRange(1,30)][int]$Warmup=3,
    [ValidateRange(2,60)][int]$Duration=5,
    [ValidateRange(320,3840)][int]$Width=1920,
    [ValidateRange(180,2160)][int]$Height=1080,
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$OutputName='display-mapping-e13-v1'
)

$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$tree=Join-Path $root 'build/windows-release'
$exe=(Resolve-Path -LiteralPath $ExePath).Path
if ((Split-Path (Split-Path $exe -Parent) -Parent) -ne $tree -or
    (Split-Path $exe -Leaf) -ine 'vulkan_cube.exe') {
    throw 'Display-mapping performance measurements require the native Windows Release executable.'
}

function Assert-NoCompetingRenderer {
    $active=@(Get-Process vulkan_cube,vulkan_editor -ErrorAction SilentlyContinue)
    if ($active.Count -ne 0) {
        $description=($active | ForEach-Object { "$($_.ProcessName) PID $($_.Id)" }) -join ', '
        throw "Close competing renderer/editor processes before measuring: $description"
    }
}

Assert-NoCompetingRenderer
$output=Join-Path $tree $OutputName
if (Test-Path -LiteralPath $output) {
    throw 'Use a fresh OutputName; benchmark evidence is immutable.'
}
New-Item -ItemType Directory -Path $output | Out-Null

$cases=@(
    [pscustomobject]@{name='fixed-off-32';exposure='fixed';grading='off';size=32},
    [pscustomobject]@{name='fixed-trilinear-32';exposure='fixed';grading='trilinear';size=32},
    [pscustomobject]@{name='fixed-tetrahedral-32';exposure='fixed';grading='tetrahedral';size=32},
    [pscustomobject]@{name='fixed-trilinear-64';exposure='fixed';grading='trilinear';size=64},
    [pscustomobject]@{name='fixed-tetrahedral-64';exposure='fixed';grading='tetrahedral';size=64},
    [pscustomobject]@{name='auto-off-32';exposure='auto';grading='off';size=32}
)
$shaderEvidence=@()
foreach ($relative in @(
    'shaders/spv/exposure_histogram.comp.spv',
    'shaders/spv/exposure_adapt.comp.spv',
    'shaders/spv/tonemap.frag.spv')) {
    $path=Join-Path $root $relative
    $shaderEvidence += [pscustomobject]@{
        path=$relative
        sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    }
}
$environment=[ordered]@{
    DateUtc=[DateTime]::UtcNow.ToString('o');Suite='DisplayMappingE13';NativeWindows=$true
    Executable=$exe;ExecutableSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    Shaders=$shaderEvidence
    CPU=(Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name)
    GPU=(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion)
    PowerScheme=(& powercfg.exe /getactivescheme | Out-String).Trim()
    Clocks='Not locked by this suite; compare only within this uninterrupted, rotated-order series.'
    Resolution="$($Width)x$($Height)";Present='IMMEDIATE';Validation=$false
    Warmup=$Warmup;Duration=$Duration;Repeats=$Repeats
    Scene='Deterministic material-spheres fixture, dynamic lighting, GI/day cycle disabled, static camera.'
    CostScope='gpu_display_ms isolates the final tone-map/LUT draw. gpu_exposure_ms isolates histogram/adaptation and its required barriers; full-frame metrics include all work.'
    CaseOrderPolicy='Rotate all six cases by repetition to limit order and thermal bias.'
    CompetingProcessGuard='No vulkan_cube or vulkan_editor process may exist before the suite or between runs.'
}
$environment | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'DisplayMapping-environment.json')

$metricFields=@(
    'average_fps','one_percent_low_fps','average_cpu_frame_ms','average_command_record_ms',
    'average_gpu_frame_ms','average_gpu_geometry_ms','average_gpu_postprocess_ms',
    'average_gpu_display_ms','average_gpu_exposure_ms')
$runs=@()
for ($run=1;$run -le $Repeats;++$run) {
    for ($offset=0;$offset -lt $cases.Count;++$offset) {
        $case=$cases[($run-1+$offset)%$cases.Count]
        $name="$($case.name)-$run"
        Assert-NoCompetingRenderer
        $arguments="--benchmark --warmup $Warmup --duration $Duration --width $Width --height $Height --present immediate --no-validation --aa off --render-fixture material-spheres --lighting dynamic --gi off --day-cycle off --time-of-day 14 --shadow-cache full --depth-prepass off --lod off --visibility-backend cpu --culling frustum --tone-mapper gt7 --exposure $($case.exposure) --exposure-metering uniform --grading $($case.grading) --grading-lut-size $($case.size)"
        $info=New-Object Diagnostics.ProcessStartInfo
        $info.FileName=$exe;$info.Arguments=$arguments;$info.WorkingDirectory=$output
        $info.UseShellExecute=$false;$info.CreateNoWindow=$true
        $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
        $process=[Diagnostics.Process]::Start($info)
        $stdout=$process.StandardOutput.ReadToEndAsync()
        $stderr=$process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(($Warmup+$Duration+180)*1000)) {
            $process.Kill();$process.WaitForExit()
            throw "Display-mapping benchmark timed out: $name"
        }
        Assert-NoCompetingRenderer
        [IO.File]::WriteAllText((Join-Path $output "$name.log"),$stdout.Result+$stderr.Result)
        if ($process.ExitCode -ne 0) { throw "Display-mapping benchmark failed: $name" }
        $result=Get-Content -LiteralPath (Join-Path $output 'benchmark.json') -Raw | ConvertFrom-Json
        if ($result.build_type -cne 'Release' -or $result.present_mode -cne 'IMMEDIATE' -or
            $result.resolution -cne "$($Width)x$($Height)" -or $result.anti_aliasing -cne 'off' -or
            $result.render_fixture -cne 'material-spheres' -or $result.dynamic_lighting -ne $true -or
            $result.voxel_gi -ne $false -or $result.day_cycle -ne $false -or
            $result.tone_mapper -cne 'gt7' -or
            $result.exposure_mode_requested -cne $case.exposure -or
            $result.exposure_mode_actual -cne $case.exposure -or
            $result.color_grading_requested -cne $case.grading -or
            $result.color_grading_actual -cne $case.grading -or
            $result.color_grading_lut_size -ne $case.size) {
            throw "Display-mapping benchmark configuration differs: $name"
        }
        if ($case.exposure -eq 'auto') {
            if ($result.average_gpu_exposure_ms -le 0 -or
                $result.exposure_sample_weight -le 0 -or
                $result.exposure_multiplier -le 0) {
                throw "Automatic exposure timing or state is unavailable: $name"
            }
        } elseif ($result.average_gpu_exposure_ms -ne 0 -or
                  $result.exposure_sample_weight -ne 0) {
            throw "Fixed exposure unexpectedly ran histogram/adaptation: $name"
        }
        $sample=[ordered]@{
            name=$name;source_json="$name.json";case=$case.name
            exposure=$case.exposure;grading=$case.grading;lut_size=$case.size
            exposure_multiplier=[double]$result.exposure_multiplier
            exposure_average_luminance=[double]$result.exposure_average_luminance
            exposure_sample_weight=[uint32]$result.exposure_sample_weight
        }
        foreach ($field in $metricFields) {
            $value=$result.$field
            if ($null -eq $result.PSObject.Properties[$field] -or $null -eq $value -or
                [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value) -or
                $value -lt 0 -or
                ($field -in @('average_fps','one_percent_low_fps','average_cpu_frame_ms',
                              'average_gpu_frame_ms','average_gpu_geometry_ms','average_gpu_display_ms') -and
                 $value -eq 0)) {
                throw "Missing display-mapping benchmark metric $field in $name"
            }
            $sample[$field]=[double]$value
        }
        Copy-Item -LiteralPath (Join-Path $output 'benchmark.json') -Destination (Join-Path $output "$name.json")
        $runs += [pscustomobject]$sample
        Write-Output "$name`: $([Math]::Round($result.average_fps,1)) FPS; GPU $([Math]::Round($result.average_gpu_frame_ms,4)) ms; display $([Math]::Round($result.average_gpu_display_ms,4)) ms; exposure $([Math]::Round($result.average_gpu_exposure_ms,4)) ms"
    }
}

function Statistics([double[]]$Values) {
    $sorted=@($Values | Sort-Object);$middle=[int]($sorted.Count/2)
    $median=if ($sorted.Count%2) {$sorted[$middle]} else {($sorted[$middle-1]+$sorted[$middle])/2}
    return [ordered]@{median=$median;min=$sorted[0];max=$sorted[-1]}
}
$groups=@()
foreach ($case in $cases) {
    $samples=@($runs | Where-Object {$_.case -eq $case.name})
    $metrics=[ordered]@{}
    foreach ($field in $metricFields) { $metrics[$field]=Statistics @($samples.$field) }
    $groups += [pscustomobject]@{case=$case.name;exposure=$case.exposure;grading=$case.grading
        lut_size=$case.size;repeats=$samples.Count;metrics=$metrics}
}
$baseline=$groups | Where-Object {$_.case -eq 'fixed-off-32'}
$comparisons=@()
foreach ($candidate in $groups | Where-Object {$_.case -ne 'fixed-off-32'}) {
    $delta=[ordered]@{}
    foreach ($field in $metricFields) {
        $before=[double]$baseline.metrics.$field.median
        $after=[double]$candidate.metrics.$field.median
        $delta[$field]=[ordered]@{
            absolute=$after-$before
            percent=if ($before -eq 0) {$null} else {100*($after-$before)/$before}
        }
    }
    $comparisons += [pscustomobject]@{baseline='fixed-off-32';candidate=$candidate.case;delta=$delta}
}
$summary=[ordered]@{runs=$runs;groups=$groups;comparisons=$comparisons}
$summary | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath (Join-Path $output 'DisplayMapping-summary.json')
Write-Output "Display-mapping benchmark complete: $($runs.Count) clean runs."
