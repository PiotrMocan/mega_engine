param(
    [string]$ExePath = '',
    [ValidateRange(320, 7680)][int]$Width = 1920,
    [ValidateRange(180, 4320)][int]$Height = 1080,
    [ValidateRange(1, 60)][double]$WarmupSeconds = 5,
    [ValidateRange(2, 120)][double]$DurationSeconds = 10,
    [ValidateRange(3, 12)][int]$Runs = 3,
    [ValidateSet(30, 60, 144)][int]$MotionFixtureFps = 60,
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$OutputName = 'motion-blur-benchmark'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$releaseTree = Join-Path $root 'build\windows-release'
if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $releaseTree 'bin\vulkan_cube.exe'
}
$exe = (Resolve-Path -LiteralPath $ExePath).Path
$expectedBin = Join-Path $releaseTree 'bin'
if ((Split-Path -Parent $exe) -ine $expectedBin -or
    (Split-Path -Leaf $exe) -ine 'vulkan_cube.exe') {
    throw 'Motion-blur performance measurements require build\windows-release\bin\vulkan_cube.exe.'
}

function Assert-NoCompetingRenderer {
    $active = @(Get-Process -Name vulkan_cube,vulkan_editor -ErrorAction SilentlyContinue)
    if ($active.Count -ne 0) {
        $description = ($active | ForEach-Object {
            "$($_.ProcessName) PID $($_.Id)"
        }) -join ', '
        throw "Close competing renderer/editor processes before measuring: $description"
    }
}

function Get-Statistics([double[]]$Values) {
    if ($Values.Count -eq 0) {
        throw 'Cannot calculate statistics for an empty sample set.'
    }
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    $median = if (($sorted.Count % 2) -eq 1) {
        [double]$sorted[$middle]
    } else {
        ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) * 0.5
    }
    return [ordered]@{
        median = $median
        minimum = [double]$sorted[0]
        maximum = [double]$sorted[-1]
    }
}

function Assert-FiniteMetric(
    [pscustomobject]$Result,
    [string]$Field,
    [string]$RunName,
    [bool]$RequirePositive
) {
    $property = $Result.PSObject.Properties[$Field]
    if ($null -eq $property -or $null -eq $property.Value) {
        throw "Missing motion-blur benchmark metric $Field in $RunName."
    }
    $value = [double]$property.Value
    if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or
        $value -lt 0.0 -or ($RequirePositive -and $value -le 0.0)) {
        throw "Invalid motion-blur benchmark metric $Field in $RunName."
    }
}

Assert-NoCompetingRenderer
$outputDirectory = Join-Path $releaseTree $OutputName
if (Test-Path -LiteralPath $outputDirectory) {
    throw 'Use a fresh OutputName; benchmark evidence is immutable.'
}
New-Item -ItemType Directory -Path $outputDirectory | Out-Null

$cases = @(
    [pscustomobject]@{ name = 'off'; mode = 'off'; quality = 'low' },
    [pscustomobject]@{ name = 'low'; mode = 'all'; quality = 'low' },
    [pscustomobject]@{ name = 'high'; mode = 'all'; quality = 'high' }
)
$metricFields = @(
    'average_fps',
    'one_percent_low_fps',
    'average_cpu_frame_ms',
    'average_gpu_frame_ms',
    'average_gpu_postprocess_ms',
    'average_gpu_motion_blur_ms'
)
$positiveMetricFields = @(
    'average_fps',
    'one_percent_low_fps',
    'average_cpu_frame_ms',
    'average_gpu_frame_ms'
)
$resolution = "$($Width)x$($Height)"
$warmupArgument = $WarmupSeconds.ToString(
    [Globalization.CultureInfo]::InvariantCulture)
$durationArgument = $DurationSeconds.ToString(
    [Globalization.CultureInfo]::InvariantCulture)
$benchmarkPath = Join-Path $outputDirectory 'benchmark.json'
$samples = @()

for ($run = 1; $run -le $Runs; ++$run) {
    for ($offset = 0; $offset -lt $cases.Count; ++$offset) {
        $currentCase = $cases[($run - 1 + $offset) % $cases.Count]
        $runName = "$($currentCase.name)-$run"
        Assert-NoCompetingRenderer
        if (Test-Path -LiteralPath $benchmarkPath) {
            Remove-Item -LiteralPath $benchmarkPath
        }

        $arguments = @(
            '--benchmark',
            '--warmup', $warmupArgument,
            '--duration', $durationArgument,
            '--width', [string]$Width,
            '--height', [string]$Height,
            '--present', 'immediate',
            '--no-validation',
            '--aa', 'off',
            '--render-fixture', 'motion-bars',
            '--animate-scene',
            '--motion-fixture-fps', [string]$MotionFixtureFps,
            '--lighting', 'dynamic',
            '--gi', 'off',
            '--day-cycle', 'off',
            '--exposure', 'fixed',
            '--motion-blur', $currentCase.mode,
            '--motion-blur-quality', $currentCase.quality,
            '--motion-blur-shutter', 'centered',
            '--motion-blur-angle', '180',
            '--motion-blur-radius', '32',
            '--motion-blur-camera-fade-start', '0',
            '--motion-blur-camera-fade-end', '0'
        )
        $startInfo = [Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $exe
        $startInfo.WorkingDirectory = $outputDirectory
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.Arguments = $arguments -join ' '

        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        [void]$process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $timeoutMilliseconds = [int](($WarmupSeconds + $DurationSeconds + 180.0) * 1000.0)
        if (-not $process.WaitForExit($timeoutMilliseconds)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Motion-blur benchmark timed out: $runName"
        }
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
        [IO.File]::WriteAllText(
            (Join-Path $outputDirectory "$runName.stdout.txt"), $stdout)
        [IO.File]::WriteAllText(
            (Join-Path $outputDirectory "$runName.stderr.txt"), $stderr)
        if ($process.ExitCode -ne 0) {
            throw "Motion-blur benchmark failed with exit code $($process.ExitCode): $runName"
        }
        Assert-NoCompetingRenderer
        if (-not (Test-Path -LiteralPath $benchmarkPath -PathType Leaf)) {
            throw "Motion-blur benchmark did not write benchmark.json: $runName"
        }

        $result = Get-Content -LiteralPath $benchmarkPath -Raw | ConvertFrom-Json
        if ($result.build_type -cne 'Release' -or
            $result.present_mode -cne 'IMMEDIATE' -or
            $result.resolution -cne $resolution -or
            $result.render_resolution -cne $resolution -or
            $result.render_fixture -cne 'motion-bars' -or
            [uint32]$result.motion_fixture_fps -ne [uint32]$MotionFixtureFps -or
            $result.animated -ne $true -or
            $result.anti_aliasing_requested -cne 'off' -or
            $result.anti_aliasing_actual -cne 'off' -or
            $result.motion_blur_requested -cne $currentCase.mode -or
            $result.motion_blur_actual -cne $currentCase.mode -or
            $result.motion_blur_quality -cne $currentCase.quality -or
            $result.motion_blur_shutter -cne 'centered' -or
            [double]$result.motion_blur_shutter_angle_degrees -ne 180.0 -or
            [double]$result.motion_blur_maximum_radius_pixels -ne 32.0 -or
            [double]$result.motion_blur_camera_fade_start -ne 0.0 -or
            [double]$result.motion_blur_camera_fade_end -ne 0.0) {
            throw "Motion-blur benchmark configuration differs from the requested case: $runName"
        }
        if ($result.percentile_scope -cne 'full_measurement' -or
            [uint64]$result.percentile_samples -ne [uint64]$result.frames_rendered) {
            throw "Motion-blur benchmark percentile coverage is incomplete: $runName"
        }
        foreach ($field in $metricFields) {
            Assert-FiniteMetric $result $field $runName ($field -in $positiveMetricFields)
        }
        if ($currentCase.mode -eq 'off') {
            if ([double]$result.average_gpu_motion_blur_ms -ne 0.0) {
                throw "Motion blur ran in the off baseline: $runName"
            }
        } elseif ([double]$result.average_gpu_motion_blur_ms -le 0.0) {
            throw "Motion-blur GPU timing is unavailable for the active case: $runName"
        }

        $rawJsonName = "$runName.json"
        Copy-Item -LiteralPath $benchmarkPath -Destination (
            Join-Path $outputDirectory $rawJsonName)
        $sample = [ordered]@{
            name = $runName
            source_json = $rawJsonName
            run = $run
            case = $currentCase.name
            motion_blur_requested = $result.motion_blur_requested
            motion_blur_actual = $result.motion_blur_actual
            motion_blur_quality = $result.motion_blur_quality
            motion_fixture_fps = [uint32]$result.motion_fixture_fps
            average_fps = [double]$result.average_fps
            one_percent_low_fps = [double]$result.one_percent_low_fps
            average_cpu_frame_ms = [double]$result.average_cpu_frame_ms
            average_gpu_frame_ms = [double]$result.average_gpu_frame_ms
            average_gpu_postprocess_ms = [double]$result.average_gpu_postprocess_ms
            average_gpu_motion_blur_ms = [double]$result.average_gpu_motion_blur_ms
        }
        $samples += [pscustomobject]$sample
        Write-Output ($sample | ConvertTo-Json -Compress)
    }
}

$caseSummaries = [ordered]@{}
foreach ($currentCase in $cases) {
    $caseSamples = @($samples | Where-Object { $_.case -eq $currentCase.name })
    if ($caseSamples.Count -ne $Runs) {
        throw "Motion-blur case $($currentCase.name) has an incomplete sample set."
    }
    $statistics = [ordered]@{}
    foreach ($field in $metricFields) {
        $statistics[$field] = Get-Statistics @($caseSamples.$field)
    }
    $caseSummaries[$currentCase.name] = [ordered]@{
        motion_blur_mode = $currentCase.mode
        motion_blur_quality = $currentCase.quality
        runs = $caseSamples.Count
        metrics = $statistics
    }
}

$deltaVsOff = [ordered]@{}
foreach ($currentCase in $cases | Where-Object { $_.name -ne 'off' }) {
    $delta = [ordered]@{}
    foreach ($field in $metricFields) {
        $baseline = [double]$caseSummaries['off'].metrics[$field].median
        $candidate = [double]$caseSummaries[$currentCase.name].metrics[$field].median
        $delta[$field] = [ordered]@{
            absolute = $candidate - $baseline
            percent = if ($baseline -eq 0.0) {
                $null
            } else {
                100.0 * ($candidate / $baseline - 1.0)
            }
        }
    }
    $deltaVsOff[$currentCase.name] = $delta
}

$summary = [ordered]@{
    suite = 'MotionBlur'
    date_utc = [DateTime]::UtcNow.ToString('o')
    executable = $exe
    executable_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    gpu = $result.gpu
    driver = $result.driver
    resolution = $resolution
    present_mode = 'IMMEDIATE'
    validation = $false
    render_fixture = 'motion-bars'
    animated_scene = $true
    motion_fixture_fps = $MotionFixtureFps
    anti_aliasing = 'off'
    shutter = 'centered'
    shutter_angle_degrees = 180.0
    maximum_radius_pixels = 32.0
    warmup_seconds = $WarmupSeconds
    duration_seconds = $DurationSeconds
    runs_per_case = $Runs
    case_order_policy = 'Rotate off/low/high by repetition to limit order and thermal bias.'
    process_guard = 'No vulkan_cube or vulkan_editor process may exist before the suite or between runs.'
    samples = $samples
    cases = $caseSummaries
    delta_vs_off = $deltaVsOff
}
$summaryJson = $summary | ConvertTo-Json -Depth 12
$summaryPath = Join-Path $outputDirectory 'MotionBlur-summary.json'
[IO.File]::WriteAllText($summaryPath, $summaryJson + "`n")
Write-Output $summaryJson
