param(
    [string]$ExePath = "",
    [ValidateRange(3, 9)][int]$Repeats = 3,
    [ValidateRange(1, 60)][double]$WarmupSeconds = 3,
    [ValidateRange(2, 120)][double]$DurationSeconds = 6,
    [ValidateSet('off', 'smaa', 'fsr3')][string]$AntiAliasing = 'off'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $root 'build\windows-release\bin\vulkan_cube.exe'
}
$exe = (Resolve-Path -LiteralPath $ExePath).Path
$releaseTree = Join-Path $root 'build\windows-release'
if (-not $exe.StartsWith($releaseTree, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Subsurface performance measurements must use build\windows-release.'
}
$outputDirectory = Join-Path $releaseTree 'subsurface-benchmark'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

function Assert-ExclusiveRendererProcess {
    $running = @(Get-Process -Name vulkan_cube,vulkan_editor -ErrorAction SilentlyContinue)
    if ($running.Count -ne 0) {
        throw "Benchmark contamination guard: renderer/editor already running: $($running.Name -join ', ')."
    }
}

function Invoke-Case([string]$Mode, [int]$Repeat) {
    Assert-ExclusiveRendererProcess
    $arguments = @(
        '--benchmark', '--no-validation', '--present', 'immediate',
        '--width', '1920', '--height', '1080', '--aa', $AntiAliasing,
        '--lighting', 'dynamic', '--gi', 'off', '--day-cycle', 'off',
        '--time-of-day', '14', '--exposure', 'fixed', '--exposure-ev', '0',
        '--render-fixture', 'subsurface-profiles', '--subsurface', $Mode,
        '--warmup', [string]$WarmupSeconds, '--duration', [string]$DurationSeconds
    )
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
    $logStem = Join-Path $outputDirectory ("{0}-{1}" -f $Repeat, $Mode)
    [IO.File]::WriteAllText("$logStem.stdout.txt", $stdout)
    [IO.File]::WriteAllText("$logStem.stderr.txt", $stderr)
    if ($process.ExitCode -ne 0) {
        throw "Subsurface $Mode repeat $Repeat failed with exit code $($process.ExitCode)."
    }
    $benchmarkPath = Join-Path $outputDirectory 'benchmark.json'
    $result = Get-Content -LiteralPath $benchmarkPath -Raw | ConvertFrom-Json
    if ($result.build_type -cne 'Release' -or $result.present_mode -cne 'IMMEDIATE' -or
        $result.resolution -cne '1920x1080' -or
        $result.render_fixture -cne 'subsurface-profiles' -or
        $result.subsurface_requested -cne $Mode -or
        $result.subsurface_actual -cne $Mode) {
        throw "Subsurface $Mode repeat $Repeat did not preserve the controlled configuration."
    }
    if ($Mode -eq 'off' -and
        ($result.subsurface_provisioned -or $result.subsurface_resident_bytes -ne 0 -or
         $result.average_gpu_subsurface_ms -ne 0)) {
        throw 'Default-off path allocated resources or executed SSS passes.'
    }
    if ($Mode -eq 'full' -and
        (-not $result.subsurface_provisioned -or
         $result.subsurface_resident_bytes -ne 66355200 -or
         $result.average_gpu_subsurface_ms -le 0)) {
        throw 'Full SSS path did not provision the expected 63.28125 MiB or record its passes.'
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

$results = @{ off = @(); full = @() }
for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
    $order = if (($repeat % 2) -eq 1) { @('off', 'full') } else { @('full', 'off') }
    foreach ($mode in $order) {
        $results[$mode] += Invoke-Case $mode $repeat
    }
}

$summary = [ordered]@{
    gpu = $results.off[0].gpu
    driver = $results.off[0].driver
    resolution = '1920x1080'
    present_mode = 'IMMEDIATE'
    anti_aliasing = $AntiAliasing
    repeats = $Repeats
    warmup_seconds = $WarmupSeconds
    duration_seconds = $DurationSeconds
    process_guard = 'no vulkan_cube or vulkan_editor before every run'
    off = [ordered]@{
        median_fps = Median @($results.off.average_fps)
        median_gpu_frame_ms = Median @($results.off.average_gpu_frame_ms)
        median_gpu_geometry_ms = Median @($results.off.average_gpu_geometry_ms)
        median_gpu_postprocess_ms = Median @($results.off.average_gpu_postprocess_ms)
        median_gpu_subsurface_ms = Median @($results.off.average_gpu_subsurface_ms)
        resident_bytes = $results.off[0].subsurface_resident_bytes
    }
    full = [ordered]@{
        median_fps = Median @($results.full.average_fps)
        median_gpu_frame_ms = Median @($results.full.average_gpu_frame_ms)
        median_gpu_geometry_ms = Median @($results.full.average_gpu_geometry_ms)
        median_gpu_postprocess_ms = Median @($results.full.average_gpu_postprocess_ms)
        median_gpu_subsurface_ms = Median @($results.full.average_gpu_subsurface_ms)
        resident_bytes = $results.full[0].subsurface_resident_bytes
    }
}
$summary.delta = [ordered]@{
    fps_percent = 100.0 * ($summary.full.median_fps / $summary.off.median_fps - 1.0)
    gpu_frame_ms = $summary.full.median_gpu_frame_ms - $summary.off.median_gpu_frame_ms
    gpu_geometry_ms = $summary.full.median_gpu_geometry_ms - $summary.off.median_gpu_geometry_ms
    gpu_postprocess_ms = $summary.full.median_gpu_postprocess_ms - $summary.off.median_gpu_postprocess_ms
}
$json = $summary | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText((Join-Path $outputDirectory 'summary.json'), $json + "`n")
$json
