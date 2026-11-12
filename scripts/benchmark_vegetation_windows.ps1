param(
    [string]$ExePath='build/windows-release/bin/vulkan_cube.exe',
    [string]$AssetRoot='assets',
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$OutputName='vegetation-baseline',
    [ValidateRange(1,10)][int]$Repeats=3,
    [ValidateRange(1,30)][int]$Warmup=5,
    [ValidateRange(2,60)][int]$Duration=15,
    [string[]]$ExtraArguments=@()
)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe=(Resolve-Path -LiteralPath $ExePath).Path
$assets=(Resolve-Path -LiteralPath $AssetRoot).Path
$release=Join-Path $root 'build/windows-release'
if (-not $exe.StartsWith($release+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
    (Split-Path $exe -Leaf) -ine 'vulkan_cube.exe') {
    throw 'Use a native renderer executable inside build/windows-release.'
}
$output=Join-Path $root "build/windows-release/$OutputName"
if (Test-Path -LiteralPath $output) { throw 'Use a fresh output name to preserve measurement evidence.' }
New-Item -ItemType Directory -Path $output | Out-Null
$arguments=@('--benchmark','--warmup',"$Warmup",'--duration',"$Duration",'--present','immediate',
    '--width','1920','--height','1080','--no-validation')+$ExtraArguments
[ordered]@{
    DateUtc=[DateTime]::UtcNow.ToString('o'); Executable=$exe
    ExecutableSha256=(Get-FileHash -LiteralPath $exe).Hash
    AssetRoot=$assets
    SceneSha256=(Get-FileHash -LiteralPath (Join-Path $assets 'scenes/editor_scene.json')).Hash
    CPU=(Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name)
    GPU=(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion)
    PowerScheme=(& powercfg.exe /getactivescheme | Out-String).Trim()
    Clocks='Driver managed; no clock lock applied.'
    Processes=(Get-Process | Sort-Object CPU -Descending | Select-Object -First 15 ProcessName,CPU)
    Arguments=$arguments; Repeats=$Repeats
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'environment.json')
for ($run=1;$run -le $Repeats;++$run) {
    if (@(Get-Process vulkan_cube,vulkan_editor -ErrorAction SilentlyContinue).Count) {
        throw 'Close competing renderer/editor processes before measuring.'
    }
    $info=[Diagnostics.ProcessStartInfo]::new()
    $info.FileName=$exe; $info.Arguments=$arguments -join ' '; $info.WorkingDirectory=$output
    $info.UseShellExecute=$false; $info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true; $info.RedirectStandardError=$true
    $info.EnvironmentVariables['VCR_ASSET_ROOT']=$assets
    $process=[Diagnostics.Process]::Start($info)
    try {
        $stdout=$process.StandardOutput.ReadToEndAsync(); $stderr=$process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(($Warmup+$Duration+180)*1000)) {
            $process.Kill(); $process.WaitForExit(); throw 'Benchmark timed out.'
        }
        [IO.File]::WriteAllText((Join-Path $output "run-$run.log"),$stdout.Result+$stderr.Result)
        if ($process.ExitCode -ne 0) { throw "Benchmark failed: run $run" }
    } finally { $process.Dispose() }
    $result=Get-Content -LiteralPath (Join-Path $output 'benchmark.json') -Raw | ConvertFrom-Json
    if ($result.build_type -cne 'Release' -or $result.present_mode -cne 'IMMEDIATE' -or
        $result.resolution -cne '1920x1080' -or $result.average_gpu_frame_ms -le 0) {
        throw 'Benchmark configuration or timings are invalid.'
    }
    Move-Item -LiteralPath (Join-Path $output 'benchmark.json') -Destination (Join-Path $output "run-$run.json")
    Write-Output "Run $run`: $($result.average_fps) FPS, GPU $($result.average_gpu_frame_ms) ms, geometry $($result.average_gpu_geometry_ms) ms, shadow $($result.average_gpu_shadow_ms) ms"
}
$runs=@(Get-ChildItem -LiteralPath $output -Filter 'run-*.json' | ForEach-Object {
    Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json
})
$summary=[ordered]@{}
foreach ($field in @('average_fps','one_percent_low_fps','average_cpu_frame_ms','average_fence_wait_ms',
    'average_acquire_ms','average_command_record_ms','average_queue_submit_ms','average_present_ms',
    'average_scene_prepare_ms','average_gpu_frame_ms','average_gpu_geometry_ms','average_gpu_shadow_ms',
    'average_gpu_visibility_ms','average_gpu_postprocess_ms')) {
    $values=@($runs | ForEach-Object { [double]$_.$field } | Sort-Object)
    $middle=[int][Math]::Floor($values.Count/2)
    $median=if ($values.Count%2) {$values[$middle]} else {($values[$middle-1]+$values[$middle])/2}
    $summary[$field]=[ordered]@{median=$median;minimum=$values[0];maximum=$values[-1]}
}
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'summary.json')
