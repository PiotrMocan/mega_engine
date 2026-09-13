param(
    [string]$ExePath='build/windows-release/bin/vulkan_cube.exe',
    [ValidateSet(1,16,64,256,4096)][int[]]$Counts=@(1,16,64,256,4096),
    [ValidateSet('separated','overlap')][string[]]$Layouts=@('separated','overlap'),
    [ValidateRange(3,10)][int]$Repeats=3,
    [ValidateRange(1,30)][int]$Warmup=3,
    [ValidateRange(2,60)][int]$Duration=5,
    [ValidateRange(320,3840)][int]$Width=1280,
    [ValidateRange(180,2160)][int]$Height=720,
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$OutputName='local-lights-ab'
)

$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$tree=Join-Path $root 'build/windows-release'
$exe=(Resolve-Path -LiteralPath $ExePath).Path
if ((Split-Path (Split-Path $exe -Parent) -Parent) -ne $tree -or
    (Split-Path $exe -Leaf) -ine 'vulkan_cube.exe') {
    throw 'Local-light performance measurements require the native Windows Release executable.'
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
$backends=@('forward','clustered')
$shaderEvidence=@()
foreach ($relative in @(
    'shaders/spv/cluster_lights.comp.spv',
    'shaders/spv/scene.frag.spv',
    'shaders/spv/scene_aa.frag.spv',
    'shaders/spv/editor_model.frag.spv',
    'shaders/spv/editor_model_aa.frag.spv')) {
    $path=Join-Path $root $relative
    $shaderEvidence += [pscustomobject]@{
        path=$relative
        sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    }
}
$environment=[ordered]@{
    DateUtc=[DateTime]::UtcNow.ToString('o');Suite='LocalLights';NativeWindows=$true
    Executable=$exe;ExecutableSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    Shaders=$shaderEvidence
    CPU=(Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name)
    GPU=(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion)
    PowerScheme=(& powercfg.exe /getactivescheme | Out-String).Trim()
    Clocks='Controlled by the caller. Compare only within one uninterrupted series.'
    Resolution="$($Width)x$($Height)";Present='IMMEDIATE';Validation=$false
    Warmup=$Warmup;Duration=$Duration;Repeats=$Repeats;Counts=$Counts
    Layouts=$Layouts;Backends=$backends
    Camera='Static built-in camera at z=12 with the deterministic light-swarm fixture.'
    CostScope='GPU frame and FPS include local-light shading. GPU geometry contains light evaluation; gpu_light_culling_ms separately measures clustered list construction and statistics copy.'
    CaseOrderPolicy='Forward/clustered order alternates for every layout/count/repetition group.'
    CompetingProcessGuard='No vulkan_cube or vulkan_editor process may exist before the suite or between runs.'
}
$environment | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'LocalLights-environment.json')

$metricFields=@(
    'average_fps','one_percent_low_fps','average_cpu_frame_ms','average_scene_prepare_ms',
    'average_command_record_ms','average_gpu_frame_ms','average_gpu_geometry_ms',
    'average_gpu_light_culling_ms','average_gpu_shadow_ms','average_gpu_postprocess_ms')
$runs=@()
for ($run=1;$run -le $Repeats;++$run) {
    for ($layoutIndex=0;$layoutIndex -lt $Layouts.Count;++$layoutIndex) {
        $layout=$Layouts[$layoutIndex]
        for ($countIndex=0;$countIndex -lt $Counts.Count;++$countIndex) {
            $count=$Counts[$countIndex]
            $order=if ((($run+$layoutIndex+$countIndex) % 2) -eq 0) {
                @('forward','clustered')
            } else {
                @('clustered','forward')
            }
            foreach ($backend in $order) {
                $name="$layout-$count-$backend-$run"
                Assert-NoCompetingRenderer
                $arguments="--benchmark --warmup $Warmup --duration $Duration --width $Width --height $Height --present immediate --no-validation --aa off --render-fixture light-swarm --local-light-count $count --light-swarm-layout $layout --local-lighting $backend --lighting dynamic --gi off --day-cycle off --time-of-day 14 --shadow-cache full --shadow-resolution 2048 --shadow-depth d32 --depth-prepass off --lod off --visibility-backend cpu --culling frustum --tone-mapper reinhard"
                $info=New-Object Diagnostics.ProcessStartInfo
                $info.FileName=$exe;$info.Arguments=$arguments;$info.WorkingDirectory=$output
                $info.UseShellExecute=$false;$info.CreateNoWindow=$true
                $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
                $process=[Diagnostics.Process]::Start($info)
                $stdout=$process.StandardOutput.ReadToEndAsync()
                $stderr=$process.StandardError.ReadToEndAsync()
                if (-not $process.WaitForExit(($Warmup+$Duration+180)*1000)) {
                    $process.Kill();$process.WaitForExit()
                    throw "Local-light benchmark timed out: $name"
                }
                Assert-NoCompetingRenderer
                [IO.File]::WriteAllText((Join-Path $output "$name.log"),$stdout.Result+$stderr.Result)
                if ($process.ExitCode -ne 0) { throw "Local-light benchmark failed: $name" }
                $result=Get-Content -LiteralPath (Join-Path $output 'benchmark.json') -Raw | ConvertFrom-Json
                if ($result.build_type -cne 'Release' -or $result.present_mode -cne 'IMMEDIATE' -or
                    $result.resolution -cne "$($Width)x$($Height)" -or $result.anti_aliasing -cne 'off' -or
                    $result.render_fixture -cne 'light-swarm' -or $result.light_swarm_layout -cne $layout -or
                    $result.local_light_count -ne $count -or
                    $result.local_light_backend_requested -cne $backend -or
                    $result.local_light_backend_actual -cne $backend -or
                    $result.local_light_dropped -ne 0 -or $result.source_instances -ne 4 -or
                    $result.depth_prepass_actual -cne 'off' -or $result.voxel_gi -ne $false -or
                    $result.day_cycle -ne $false) {
                    throw "Local-light benchmark configuration or counters differ: $name"
                }
                if ($backend -eq 'clustered') {
                    if ($result.average_gpu_light_culling_ms -le 0 -or
                        $result.local_light_assignments -le 0 -or
                        $result.local_light_maximum_list -le 0) {
                        throw "Clustered-light timing or statistics are unavailable: $name"
                    }
                } elseif ($result.average_gpu_light_culling_ms -ne 0 -or
                          $result.local_light_assignments -ne 0 -or
                          $result.local_light_overflow_clusters -ne 0) {
                    throw "Forward reference unexpectedly ran cluster culling: $name"
                }
                $sample=[ordered]@{
                    name=$name;source_json="$name.json";layout=$layout;count=$count;backend=$backend
                    assignments=[uint32]$result.local_light_assignments
                    maximum_list=[uint32]$result.local_light_maximum_list
                    overflow_clusters=[uint32]$result.local_light_overflow_clusters
                    dropped=[uint32]$result.local_light_dropped
                }
                foreach ($field in $metricFields) {
                    $value=$result.$field
                    if ($null -eq $result.PSObject.Properties[$field] -or $null -eq $value -or
                        [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value) -or
                        $value -lt 0 -or
                        ($field -in @('average_fps','one_percent_low_fps','average_cpu_frame_ms',
                                      'average_gpu_frame_ms','average_gpu_geometry_ms') -and $value -eq 0)) {
                        throw "Missing local-light benchmark metric $field in $name"
                    }
                    $sample[$field]=[double]$value
                }
                Copy-Item -LiteralPath (Join-Path $output 'benchmark.json') -Destination (Join-Path $output "$name.json")
                $runs += [pscustomobject]$sample
                Write-Output "$name`: $([Math]::Round($result.average_fps,1)) FPS; GPU $([Math]::Round($result.average_gpu_frame_ms,4)) ms; geometry $([Math]::Round($result.average_gpu_geometry_ms,4)) ms; cull $([Math]::Round($result.average_gpu_light_culling_ms,4)) ms"
            }
        }
    }
}

function Statistics([double[]]$Values) {
    $sorted=@($Values | Sort-Object)
    $middle=[int]($sorted.Count/2)
    $median=if ($sorted.Count%2) {$sorted[$middle]} else {($sorted[$middle-1]+$sorted[$middle])/2}
    return [ordered]@{median=$median;min=$sorted[0];max=$sorted[-1]}
}

$groups=@()
foreach ($layout in $Layouts) {
    foreach ($count in $Counts) {
        foreach ($backend in $backends) {
            $samples=@($runs | Where-Object {
                $_.layout -eq $layout -and $_.count -eq $count -and $_.backend -eq $backend})
            $metrics=[ordered]@{}
            foreach ($field in $metricFields) { $metrics[$field]=Statistics @($samples.$field) }
            $groups += [pscustomobject]@{
                layout=$layout;count=$count;backend=$backend;repeats=$samples.Count
                metrics=$metrics
                assignments=Statistics @($samples.assignments)
                maximum_list=Statistics @($samples.maximum_list)
                overflow_clusters=Statistics @($samples.overflow_clusters)
            }
        }
    }
}

$comparisons=@()
foreach ($layout in $Layouts) {
    foreach ($count in $Counts) {
        $forward=$groups | Where-Object {$_.layout -eq $layout -and $_.count -eq $count -and $_.backend -eq 'forward'}
        $clustered=$groups | Where-Object {$_.layout -eq $layout -and $_.count -eq $count -and $_.backend -eq 'clustered'}
        $delta=[ordered]@{}
        foreach ($field in $metricFields) {
            $before=[double]$forward.metrics.$field.median
            $after=[double]$clustered.metrics.$field.median
            $delta[$field]=[ordered]@{
                absolute=$after-$before
                percent=if ($before -eq 0) {$null} else {100*($after-$before)/$before}
            }
        }
        $comparisons += [pscustomobject]@{
            layout=$layout;count=$count;baseline='forward';candidate='clustered';delta=$delta
        }
    }
}

$summary=[ordered]@{runs=$runs;groups=$groups;comparisons=$comparisons}
$summary | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath (Join-Path $output 'LocalLights-summary.json')
Write-Output "Local-light benchmark complete: $($runs.Count) runs."
