param(
    [string]$ExePath = "",
    [ValidateRange(3, 9)][int]$Repeats = 3,
    [ValidateRange(1, 60)][double]$WarmupSeconds = 3,
    [ValidateRange(2, 120)][double]$DurationSeconds = 6,
    [ValidateSet('aa-wires', 'overdraw-4')][string]$RenderFixture = 'aa-wires'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $root 'build\windows-release\bin\vulkan_cube.exe'
}
$exe = (Resolve-Path -LiteralPath $ExePath).Path
$releaseTree = Join-Path $root 'build\windows-release'
if (-not $exe.StartsWith($releaseTree, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'MSAA performance measurements must use build\windows-release.'
}
$outputDirectory = Join-Path $releaseTree "msaa-benchmark\$RenderFixture"
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

function Assert-ExclusiveRendererProcess {
    $running = @(Get-Process -Name vulkan_cube,vulkan_editor -ErrorAction SilentlyContinue)
    if ($running.Count -ne 0) {
        throw "Benchmark contamination guard: renderer/editor already running: $($running.Name -join ', ')."
    }
}

function Invoke-Case([string]$Method, [int]$Repeat) {
    Assert-ExclusiveRendererProcess
    $aaMethod = if ($Method -eq 'smaa_t2x') { 'smaa' }
        elseif ($Method -eq 'fsr3_native') { 'fsr3' }
        else { $Method }
    $arguments = @(
        '--benchmark', '--no-validation', '--present', 'immediate',
        '--width', '1920', '--height', '1080', '--aa', $aaMethod,
        '--lighting', 'dynamic', '--gi', 'off', '--day-cycle', 'off',
        '--time-of-day', '14', '--exposure', 'fixed', '--exposure-ev', '0',
        '--render-fixture', $RenderFixture, '--subsurface', 'off',
        '--warmup', [string]$WarmupSeconds, '--duration', [string]$DurationSeconds
    )
    if ($Method -eq 'smaa_t2x') { $arguments += @('--smaa-mode', 't2x') }
    if ($Method -eq 'fsr3_native') { $arguments += @('--fsr-quality', 'native-aa') }
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $exe
    $info.WorkingDirectory = $outputDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $arguments) { [void]$info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    [void]$process.Start()
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    $logStem = Join-Path $outputDirectory ("{0}-{1}" -f $Repeat, $Method)
    [IO.File]::WriteAllText("$logStem.stdout.txt", $stdout)
    [IO.File]::WriteAllText("$logStem.stderr.txt", $stderr)
    if ($process.ExitCode -ne 0) {
        throw "AA $Method repeat $Repeat failed with exit code $($process.ExitCode)."
    }
    Assert-ExclusiveRendererProcess
    $benchmarkPath = Join-Path $outputDirectory 'benchmark.json'
    $result = Get-Content -LiteralPath $benchmarkPath -Raw | ConvertFrom-Json
    $expectedSamples = if ($Method -eq 'msaa4x') { 4 } elseif ($Method -eq 'msaa2x') { 2 } else { 1 }
    if ($result.build_type -cne 'Release' -or $result.present_mode -cne 'IMMEDIATE' -or
        $result.resolution -cne '1920x1080' -or $result.render_resolution -cne '1920x1080' -or
        $result.render_fixture -cne $RenderFixture -or
        $result.anti_aliasing_requested -cne $aaMethod -or
        $result.anti_aliasing_actual -cne $aaMethod -or
        $result.msaa_samples_requested -ne $expectedSamples -or
        $result.msaa_samples_actual -ne $expectedSamples) {
        throw "AA $Method repeat $Repeat did not preserve the controlled requested/actual configuration."
    }
    if ($Method.StartsWith('msaa')) {
        if ($result.msaa_resident_bytes -le 0) {
            throw "AA $Method did not retain multisampled color/depth attachments."
        }
    } elseif ($result.msaa_resident_bytes -ne 0) {
        throw "AA $Method retained MSAA attachments."
    }
    if ($Method -eq 'smaa_t2x' -and
        ($result.smaa_mode -cne 't2x' -or $result.smaa_temporal_samples -ne 2 -or
         $result.smaa_history_resident_bytes -le 0 -or
         $result.smaa_history_incremental_bytes -ne 0 -or
         -not $result.smaa_history_reuses_fsr_output)) {
        throw 'SMAA T2x benchmark metadata does not describe its strict shared history.'
    }
    if ($Method -eq 'fsr3_native' -and $result.anti_aliasing_quality -cne 'native-aa') {
        throw 'FSR 3 native-AA benchmark did not preserve the requested quality mode.'
    }
    Copy-Item -LiteralPath $benchmarkPath -Destination "$logStem.json" -Force
    return $result
}

function Median([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) * 0.5
}

function Range([double[]]$Values) {
    return [ordered]@{
        minimum = [double](($Values | Measure-Object -Minimum).Minimum)
        maximum = [double](($Values | Measure-Object -Maximum).Maximum)
    }
}

$methods = @('off', 'smaa', 'smaa_t2x', 'fsr3_native', 'msaa2x', 'msaa4x')
$results = @{}
foreach ($method in $methods) { $results[$method] = @() }
for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
    $order = if (($repeat % 2) -eq 1) { $methods } else { @('msaa4x', 'msaa2x', 'fsr3_native', 'smaa_t2x', 'smaa', 'off') }
    foreach ($method in $order) { $results[$method] += Invoke-Case $method $repeat }
}

$summary = [ordered]@{
    gpu = $results.off[0].gpu
    driver = $results.off[0].driver
    resolution = '1920x1080'
    present_mode = 'IMMEDIATE'
    render_fixture = $RenderFixture
    exposure = 'fixed EV 0'
    repeats = $Repeats
    warmup_seconds = $WarmupSeconds
    duration_seconds = $DurationSeconds
    process_guard = 'no vulkan_cube or vulkan_editor before and after every run'
    cases = [ordered]@{}
}
foreach ($method in $methods) {
    $case = $results[$method]
    $summary.cases[$method] = [ordered]@{
        median_fps = Median @($case.average_fps)
        fps_range = Range @($case.average_fps)
        median_one_percent_low_fps = Median @($case.one_percent_low_fps)
        median_cpu_frame_ms = Median @($case.average_cpu_frame_ms)
        median_cpu_record_ms = Median @($case.average_command_record_ms)
        median_gpu_frame_ms = Median @($case.average_gpu_frame_ms)
        median_gpu_geometry_ms = Median @($case.average_gpu_geometry_ms)
        median_gpu_postprocess_ms = Median @($case.average_gpu_postprocess_ms)
        samples = $case[0].msaa_samples_actual
        resident_bytes = $case[0].msaa_resident_bytes
        smaa_mode = $case[0].smaa_mode
        smaa_temporal_samples = $case[0].smaa_temporal_samples
        smaa_history_resident_bytes = $case[0].smaa_history_resident_bytes
        smaa_history_incremental_bytes = $case[0].smaa_history_incremental_bytes
    }
}
$summary.delta_vs_off = [ordered]@{}
foreach ($method in @('smaa', 'smaa_t2x', 'fsr3_native', 'msaa2x', 'msaa4x')) {
    $summary.delta_vs_off[$method] = [ordered]@{
        fps_percent = 100.0 * ($summary.cases[$method].median_fps / $summary.cases.off.median_fps - 1.0)
        gpu_frame_ms = $summary.cases[$method].median_gpu_frame_ms - $summary.cases.off.median_gpu_frame_ms
        gpu_geometry_ms = $summary.cases[$method].median_gpu_geometry_ms - $summary.cases.off.median_gpu_geometry_ms
        gpu_postprocess_ms = $summary.cases[$method].median_gpu_postprocess_ms - $summary.cases.off.median_gpu_postprocess_ms
    }
}
$json = $summary | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText((Join-Path $outputDirectory 'summary.json'), $json + "`n")
$json
