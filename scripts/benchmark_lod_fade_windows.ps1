param(
    [string]$ExePath='build/windows-release/bin/vulkan_cube.exe',
    [ValidateSet(1,1024)][int[]]$Counts=@(1,1024),
    [ValidateRange(3,10)][int]$Repeats=3,
    [ValidateRange(1,30)][int]$Warmup=5,
    [ValidateRange(2,60)][int]$Duration=10,
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$OutputName='lod-fade-ab'
)
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$tree=Join-Path $root 'build/windows-release'
$exe=(Resolve-Path -LiteralPath $ExePath).Path
if ((Split-Path (Split-Path $exe -Parent) -Parent) -ne $tree -or
    (Split-Path $exe -Leaf) -ine 'vulkan_cube.exe') {
    throw 'LOD fade performance measurements require the native Windows Release executable.'
}
function Assert-NoCompetingRenderer {
    $active=@(Get-Process vulkan_cube,vulkan_editor -ErrorAction SilentlyContinue)
    if ($active.Count -ne 0) {
        $description=($active | ForEach-Object { "$($_.ProcessName) PID $($_.Id)" }) -join ', '
        throw "Close competing renderer/editor processes before measuring: $description"
    }
}
Assert-NoCompetingRenderer
$fixtureRoot=Join-Path $tree 'lod-benchmark'
$manifestPath=Join-Path $fixtureRoot 'fixture-manifest.json'
$manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.generator -cne 'vcr.lod-benchmark.v1' -or $manifest.capacity_stress -ne $false) {
    throw 'Generate the regular Release LOD benchmark fixture first.'
}
$variants=@{}
foreach ($variant in $manifest.variants) {
    $count=[int]$variant.source_instances
    $assetRoot=(Resolve-Path -LiteralPath $variant.asset_root).Path
    foreach ($file in $variant.files) {
        $path=[IO.Path]::GetFullPath((Join-Path $fixtureRoot $file.path))
        if (-not $path.StartsWith($fixtureRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $file.sha256) {
            throw "LOD benchmark input differs from its manifest: $($file.path)"
        }
    }
    $variants[$count]=$assetRoot
}
foreach ($count in $Counts) { if (-not $variants.ContainsKey($count)) { throw "Missing generated count-$count fixture." } }
$output=Join-Path $tree $OutputName
if (Test-Path -LiteralPath $output) { throw 'Use a fresh OutputName; benchmark evidence is immutable.' }
New-Item -ItemType Directory -Path $output | Out-Null
$patterns=@('off','bayer4','bayer8')
$backends=@('cpu','gpu')
$cases=@()
foreach ($count in $Counts) {
    foreach ($backend in $backends) {
        foreach ($pattern in $patterns) {
            $cases += [pscustomobject]@{count=$count;backend=$backend;pattern=$pattern;asset_root=$variants[$count]}
        }
    }
}
$environment=[ordered]@{
    DateUtc=[DateTime]::UtcNow.ToString('o');Suite='LodFade';NativeWindows=$true
    Executable=$exe;ExecutableSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    FixtureManifest=$manifestPath;FixtureManifestSha256=(Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
    CPU=(Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name)
    GPU=(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion)
    PowerScheme=(& powercfg.exe /getactivescheme | Out-String).Trim()
    Clocks='Controlled by the caller. Compare only within one uninterrupted series.'
    Resolution='1920x1080';Present='IMMEDIATE';Validation=$false
    Warmup=$Warmup;Duration=$Duration;Repeats=$Repeats;Counts=$Counts
    Patterns=$patterns;Backends=$backends
    Camera='Built-in --animate-camera: x=4*sin(.6*t), y=4, z=12, pitch -12 degrees.'
    CostScope='Full frame cost of off/Bayer4/Bayer8, including CPU/GPU LOD selection, dual draw emission, material shading and always-on GPU history dispatch while fade is enabled. Synthetic overlapping instances are a capacity/dispatch stress, not representative shading.'
    CaseOrderPolicy='For each count/backend rotate off/Bayer4/Bayer8 by repetition.'
    CompetingProcessGuard='No vulkan_cube or vulkan_editor process may exist before the suite or between runs.'
}
$environment | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'LodFade-environment.json')
$metricFields=@('average_fps','one_percent_low_fps','average_cpu_frame_ms','average_scene_prepare_ms',
    'average_command_record_ms','average_gpu_frame_ms','average_gpu_geometry_ms','average_gpu_visibility_ms',
    'average_gpu_shadow_ms','average_gpu_postprocess_ms')
$runs=@()
for ($run=1;$run -le $Repeats;++$run) {
    foreach ($count in $Counts) {
        foreach ($backend in $backends) {
            $group=@($cases | Where-Object {$_.count -eq $count -and $_.backend -eq $backend})
            for ($offset=0;$offset -lt $group.Count;++$offset) {
                $case=$group[($run-1+$offset)%$group.Count]
                $name="$($case.count)-$($case.backend)-$($case.pattern)-$run"
                Assert-NoCompetingRenderer
                $arguments="--benchmark --warmup $Warmup --duration $Duration --width 1920 --height 1080 --present immediate --no-validation --aa off --lighting legacy --gi off --day-cycle off --depth-prepass off --front-to-back off --cpu-culling linear --visibility-backend $($case.backend) --culling frustum --lod on --lod-fade $($case.pattern) --lod-fade-seconds 0.25 --camera-z 12 --animate-camera --dynamic-shadows off"
                $info=New-Object Diagnostics.ProcessStartInfo
                $info.FileName=$exe;$info.Arguments=$arguments;$info.WorkingDirectory=$output
                $info.UseShellExecute=$false;$info.CreateNoWindow=$true
                $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
                $info.EnvironmentVariables['VCR_ASSET_ROOT']=$case.asset_root
                $process=[Diagnostics.Process]::Start($info)
                $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
                if (-not $process.WaitForExit(($Warmup+$Duration+120)*1000)) {
                    $process.Kill();$process.WaitForExit();throw "LOD benchmark timed out: $name"
                }
                Assert-NoCompetingRenderer
                [IO.File]::WriteAllText((Join-Path $output "$name.log"),$stdout.Result+$stderr.Result)
                if ($process.ExitCode -ne 0) { throw "LOD benchmark failed: $name" }
                $result=Get-Content -LiteralPath (Join-Path $output 'benchmark.json') -Raw | ConvertFrom-Json
                if ($result.build_type -cne 'Release' -or $result.present_mode -cne 'IMMEDIATE' -or
                    $result.resolution -cne '1920x1080' -or $result.visibility_backend -cne $backend -or
                    $result.lod_fade_requested -cne $case.pattern -or $result.lod_fade_actual -cne $case.pattern -or
                    $result.source_instances -ne $count -or $result.animated_camera -ne $true -or
                    $result.depth_prepass_actual -cne 'off' -or $result.anti_aliasing -cne 'off') {
                    throw "LOD benchmark configuration or loaded scene differs: $name"
                }
                $sample=[ordered]@{name=$name;source_json="$name.json";count=$count;backend=$backend;pattern=$case.pattern
                    lod0_instances=$result.lod0_instances;lod1_instances=$result.lod1_instances;lod2_instances=$result.lod2_instances
                    visible_instances=$result.visible_instances;triangles=$result.triangles}
                foreach ($field in $metricFields) {
                    $value=$result.$field
                    if ($null -eq $result.PSObject.Properties[$field] -or $null -eq $value -or
                        [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value) -or $value -lt 0 -or
                        ($field -in @('average_fps','one_percent_low_fps','average_cpu_frame_ms','average_gpu_frame_ms','average_gpu_geometry_ms') -and $value -eq 0)) {
                        throw "Missing LOD benchmark metric $field in $name"
                    }
                    $sample[$field]=[double]$value
                }
                Copy-Item -LiteralPath (Join-Path $output 'benchmark.json') -Destination (Join-Path $output "$name.json")
                $runs += [pscustomobject]$sample
                Write-Output "$name`: $([Math]::Round($result.average_fps,1)) FPS; GPU $([Math]::Round($result.average_gpu_frame_ms,4)) ms"
            }
        }
    }
}
function Statistics([double[]]$Values) {
    $sorted=@($Values | Sort-Object);$middle=[int]($sorted.Count/2)
    $median=if ($sorted.Count%2) {$sorted[$middle]} else {($sorted[$middle-1]+$sorted[$middle])/2}
    return [ordered]@{median=$median;min=$sorted[0];max=$sorted[-1]}
}
$groups=@()
foreach ($count in $Counts) { foreach ($backend in $backends) { foreach ($pattern in $patterns) {
    $samples=@($runs | Where-Object {$_.count -eq $count -and $_.backend -eq $backend -and $_.pattern -eq $pattern})
    $metrics=[ordered]@{};foreach ($field in $metricFields) {$metrics[$field]=Statistics @($samples.$field)}
    $groups += [pscustomobject]@{count=$count;backend=$backend;pattern=$pattern;repeats=$samples.Count;metrics=$metrics}
}}}
$comparisons=@()
foreach ($count in $Counts) { foreach ($backend in $backends) {
    $baseline=$groups | Where-Object {$_.count -eq $count -and $_.backend -eq $backend -and $_.pattern -eq 'off'}
    foreach ($pattern in @('bayer4','bayer8')) {
        $candidate=$groups | Where-Object {$_.count -eq $count -and $_.backend -eq $backend -and $_.pattern -eq $pattern}
        $delta=[ordered]@{};foreach ($field in $metricFields) {
            $before=[double]$baseline.metrics.$field.median;$after=[double]$candidate.metrics.$field.median
            $delta[$field]=if ($before -eq 0) {$null} else {100*($after-$before)/$before}
        }
        $comparisons += [pscustomobject]@{count=$count;backend=$backend;baseline='off';candidate=$pattern;percent_change=$delta}
    }
}}
$summary=[ordered]@{runs=$runs;groups=$groups;comparisons=$comparisons}
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output 'LodFade-summary.json')
Write-Output "LOD fade benchmark complete: $($runs.Count) runs."
