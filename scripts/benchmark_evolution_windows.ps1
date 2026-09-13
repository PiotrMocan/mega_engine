param(
    [ValidateSet('Compatibility','Visibility','Lod','Shadows','ShadowResolution','Default','Adaptive','Lighting','LightingStress','LightingVoxel','LightingQuality','LightingQualityStress','RenderingPipeline','ToneMapping','ShadowCache','ShadowDepth','DepthPrepass','FrontToBack','CpuHierarchy','MaterialABI','CpuPackets')][string]$Suite='Visibility',
    [ValidateRange(3,20)][int]$Repeats=3,
    [ValidateRange(1,60)][int]$Warmup=5,
    [ValidateRange(2,300)][int]$Duration=15,
    [string]$BaselineExe='',
    [ValidateSet('legacy','direct','voxel')][string[]]$LightingModes=@('legacy','direct','voxel'),
    [string]$OutputName='evolution-benchmark',
    [string]$ModelAsset=''
)
$ErrorActionPreference='Stop'
$materialPacketSuite=$Suite -in @('MaterialABI','CpuPackets')
if ($Suite -eq 'Lod' -and [string]::IsNullOrWhiteSpace($ModelAsset)) {
    throw 'The Lod suite requires -ModelAsset with a local model path relative to assets/.'
}
if ($ModelAsset.Contains('"')) { throw 'ModelAsset must not contain double quotes.' }
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$tree=Join-Path $root 'build/windows-release'
$exe=(Resolve-Path (Join-Path $tree 'bin/vulkan_cube.exe')).Path
if ($OutputName -notmatch '^[a-zA-Z0-9_-]+$') { throw 'OutputName must be a directory name, not a path.' }
$out=Join-Path $tree $OutputName
New-Item -ItemType Directory -Force -Path $out | Out-Null
$cases=@()
function Add-Case([string]$Name,[string]$Arguments,[string]$Executable=$exe,[hashtable]$Expected=@{}) {
    $script:cases+=@{Name=$Name; Arguments=$Arguments; Executable=$Executable; Expected=$Expected}
}
switch ($Suite) {
    { $materialPacketSuite } {
        if ([string]::IsNullOrWhiteSpace($BaselineExe)) {
            throw "$Suite requires -BaselineExe with its matching shaders, runtime DLLs, and unchanged default-IOR scene assets."
        }
        $baseline=(Resolve-Path -LiteralPath $BaselineExe).Path
        if ($baseline -eq $exe) { throw "$Suite requires a preserved baseline executable, separate from the current executable." }
        $baselineDirectory=Split-Path -Parent $baseline
        if (-not (Test-Path -LiteralPath $baseline -PathType Leaf) -or
            -not (Test-Path -LiteralPath (Join-Path $baselineDirectory 'shaders') -PathType Container) -or
            -not (Test-Path -LiteralPath (Join-Path $baselineDirectory 'amd_fidelityfx_vk.dll') -PathType Leaf)) {
            throw 'The baseline bundle must contain the executable, matching shaders directory, and amd_fidelityfx_vk.dll.'
        }
        $lighting='--lighting dynamic --gi off --day-cycle off --time-of-day 14 --aa off --depth-prepass off --front-to-back off --cpu-culling linear --tone-mapper reinhard --shadow-cache full --shadow-resolution 2048 --shadow-depth d32 --dynamic-shadows on'
        $materialPacketWorkloads=@('canonical-cpu','canonical-gpu','stress-cpu')
        foreach ($workload in $materialPacketWorkloads) {
            $backend=if ($workload -eq 'canonical-gpu') { 'gpu' } else { 'cpu' }
            $arguments="$lighting --visibility-backend $backend --culling frustum"
            $expected=@{
                mode='scene'; cpu_culling_requested='linear'; cpu_culling_actual=$(if ($backend -eq 'cpu') { 'linear' } else { 'off' })
                visibility_backend=$backend; frustum_culling=$true; occlusion_culling=$false
                depth_prepass_requested='off'; depth_prepass_actual='off'; average_gpu_depth_prepass_ms=0
                front_to_back_requested='off'; front_to_back_actual='off'; render_fixture='none'
                anti_aliasing='off'; render_resolution='1920x1080'; tone_mapper='reinhard'
                dynamic_lighting=$true; voxel_gi=$false; day_cycle=$false; time_of_day_end=14
                animated=$false; lighting_room=$false; dynamic_shadows=$true; percentile_scope='full_measurement'
                cascade_cache=$false; shadow_resolution=2048; shadow_depth_requested='d32'; shadow_depth_actual='d32'
            }
            if ($workload -eq 'stress-cpu') {
                $arguments+=' --scene-count 100000 --scene-walls off --camera-z 12 --animate-camera --lod off'
                $expected.scene_count=100000; $expected.source_instances=100000; $expected.scene_walls=$false
                $expected.camera_z=12; $expected.animated_camera=$true; $expected.lod=$false
            } else {
                $arguments+=' --lod on'
                $expected.scene_count=0; $expected.animated_camera=$false; $expected.lod=$true
            }
            Add-Case "before-$workload" $arguments $baseline $expected
            Add-Case "after-$workload" $arguments $exe $expected
        }
    }
    'CpuHierarchy' {
        $lighting='--lighting dynamic --gi off --day-cycle off --time-of-day 14 --aa off --depth-prepass off --front-to-back off --tone-mapper reinhard --shadow-cache full --shadow-resolution 2048 --shadow-depth d32'
        $visibility='--visibility-backend cpu --culling frustum --lod off --scene-walls off --camera-z 12 --animate-camera'
        $cpuHierarchyWorkloads=@(16,1024,25000,100000)
        foreach ($count in $cpuHierarchyWorkloads) {
            foreach ($culling in @('linear','hierarchy')) {
                $arguments="$lighting $visibility --scene-count $count --cpu-culling $culling"
                $expected=@{
                    cpu_culling_requested=$culling; cpu_culling_actual=$culling
                    visibility_backend='cpu'; frustum_culling=$true; occlusion_culling=$false; lod=$false
                    depth_prepass_requested='off'; depth_prepass_actual='off'
                    front_to_back_requested='off'; front_to_back_actual='off'; render_fixture='none'
                    anti_aliasing='off'; render_resolution='1920x1080'; tone_mapper='reinhard'
                    dynamic_lighting=$true; voxel_gi=$false; day_cycle=$false; time_of_day_end=14
                    animated_camera=$true; animated=$false; camera_z=12; scene_count=$count; scene_walls=$false
                    cascade_cache=$false; shadow_resolution=2048; shadow_depth_requested='d32'; shadow_depth_actual='d32'
                }
                Add-Case "$count-$culling" $arguments $exe $expected
            }
        }
    }
    'FrontToBack' {
        $lighting='--lighting dynamic --gi off --day-cycle off --time-of-day 14 --aa off --depth-prepass off --tone-mapper reinhard --shadow-cache full --shadow-resolution 2048 --shadow-depth d32'
        $frontToBackWorkloads=@('canonical','stress-cpu')
        foreach ($workload in $frontToBackWorkloads) {
            foreach ($sorting in @('off','coarse')) {
                $arguments="$lighting --visibility-backend cpu --front-to-back $sorting"
                $expected=@{
                    front_to_back_requested=$sorting; front_to_back_actual=$sorting
                    depth_prepass_requested='off'; depth_prepass_actual='off'; render_fixture='none'
                    anti_aliasing='off'; render_resolution='1920x1080'; tone_mapper='reinhard'
                    dynamic_lighting=$true; voxel_gi=$false; day_cycle=$false; time_of_day_end=14
                    animated=$false; visibility_backend='cpu'; occlusion_culling=$false
                    cascade_cache=$false; shadow_resolution=2048; shadow_depth_requested='d32'; shadow_depth_actual='d32'
                }
                if ($workload -eq 'canonical') {
                    $arguments+=' --culling frustum --lod on'
                    $expected.animated_camera=$false; $expected.scene_count=0
                    $expected.frustum_culling=$true; $expected.lod=$true
                } else {
                    $arguments+=' --scene-count 100000 --scene-walls off --camera-z 12 --animate-camera --culling none --lod off'
                    $expected.animated_camera=$true; $expected.scene_count=100000
                    $expected.camera_z=12; $expected.scene_walls=$false
                    $expected.frustum_culling=$false; $expected.lod=$false
                }
                Add-Case "$workload-$sorting" $arguments $exe $expected
            }
        }
    }
    'DepthPrepass' {
        $lighting='--lighting dynamic --gi off --day-cycle off --time-of-day 14 --aa off --tone-mapper reinhard --shadow-cache full --shadow-resolution 2048 --shadow-depth d32'
        $visibility='--camera-z 12 --visibility-backend cpu --culling frustum --lod off'
        $depthPrepassWorkloads=@('overdraw-1','overdraw-16')
        foreach ($workload in $depthPrepassWorkloads) {
            foreach ($prepass in @('off','partial','full')) {
                $arguments="$lighting $visibility --render-fixture $workload --depth-prepass $prepass"
                $expected=@{
                    render_fixture=$workload; depth_prepass_requested=$prepass; depth_prepass_actual=$prepass
                    anti_aliasing='off'; render_resolution='1920x1080'; tone_mapper='reinhard'
                    dynamic_lighting=$true; voxel_gi=$false; day_cycle=$false; time_of_day_end=14
                    animated_camera=$false; animated=$false; camera_z=12; scene_count=0
                    visibility_backend='cpu'; frustum_culling=$true; occlusion_culling=$false; lod=$false
                    cascade_cache=$false; shadow_resolution=2048; shadow_depth_requested='d32'; shadow_depth_actual='d32'
                }
                Add-Case "$workload-$prepass" $arguments $exe $expected
            }
        }
    }
    'ShadowDepth' {
        $lighting='--lighting dynamic --gi off --day-cycle off --time-of-day 14 --aa off --animate-camera --shadow-cache full'
        foreach ($resolution in @(2048,4096)) {
            foreach ($depth in @('d32','d16')) {
                $arguments="$lighting --shadow-resolution $resolution --shadow-depth $depth"
                $expected=@{anti_aliasing='off'; dynamic_lighting=$true; voxel_gi=$false; day_cycle=$false; time_of_day_end=14; animated_camera=$true; cascade_cache=$false; shadow_resolution=$resolution; shadow_depth_requested=$depth; shadow_depth_actual=$depth}
                Add-Case "$resolution-$depth" $arguments $exe $expected
            }
        }
    }
    'ShadowCache' {
        $lighting='--lighting dynamic --gi off --day-cycle off --time-of-day 14 --aa off --animate-camera'
        foreach ($workload in @('canonical','stress-gpu')) {
            foreach ($cache in @('full','cascade')) {
                $arguments="$lighting --shadow-cache $cache"
                $expected=@{anti_aliasing='off'; dynamic_lighting=$true; voxel_gi=$false; day_cycle=$false; time_of_day_end=14; animated_camera=$true; cascade_cache=($cache -eq 'cascade')}
                if ($workload -eq 'stress-gpu') {
                    $arguments+=' --scene-count 100000 --visibility-backend gpu'
                    $expected.scene_count=100000
                    $expected.visibility_backend='gpu'
                }
                Add-Case "$workload-$cache" $arguments $exe $expected
            }
        }
    }
    'RenderingPipeline' {
        if ([string]::IsNullOrWhiteSpace($BaselineExe)) {
            throw 'RenderingPipeline requires -BaselineExe with its matching shaders, runtime DLLs, and unchanged scene assets.'
        }
        $baseline=(Resolve-Path -LiteralPath $BaselineExe).Path
        if ($baseline -eq $exe) { throw 'RenderingPipeline requires a preserved baseline executable, separate from the current executable.' }
        $baselineDirectory=Split-Path -Parent $baseline
        if (-not (Test-Path -LiteralPath $baseline -PathType Leaf) -or
            -not (Test-Path -LiteralPath (Join-Path $baselineDirectory 'shaders') -PathType Container) -or
            -not (Test-Path -LiteralPath (Join-Path $baselineDirectory 'amd_fidelityfx_vk.dll') -PathType Leaf)) {
            throw 'The baseline bundle must contain the executable, matching shaders directory, and amd_fidelityfx_vk.dll.'
        }
        $lighting='--lighting dynamic --time-of-day 14 --day-cycle off'
        $aaNames=@{off='off'; smaa='SMAA 1x'; fsr3='FSR 3.1.4'}
        foreach ($gi in @('off','voxel')) {
            foreach ($aa in @('off','smaa','fsr3')) {
                $arguments="$lighting --gi $gi --aa $aa"
                $expected=@{anti_aliasing=$aaNames[$aa]; dynamic_lighting=$true; voxel_gi=($gi -eq 'voxel'); day_cycle=$false; time_of_day_end=14; animated_camera=$false; animated=$false}
                Add-Case "before-static-$gi-$aa" $arguments $baseline $expected
                Add-Case "after-static-$gi-$aa" $arguments $exe $expected
            }
        }
        $arguments="$lighting --gi off --aa off --scene-count 100000 --animate-camera --visibility-backend gpu --culling occlusion"
        $expected=@{anti_aliasing='off'; dynamic_lighting=$true; voxel_gi=$false; day_cycle=$false; time_of_day_end=14; scene_count=100000; animated_camera=$true; visibility_backend='gpu'}
        Add-Case 'before-stress-gpu' $arguments $baseline $expected
        Add-Case 'after-stress-gpu' $arguments $exe $expected
    }
    'ToneMapping' {
        # Exposure is fixed at 1 by the renderer; both cases use the same executable.
        $lighting='--lighting dynamic --gi off --time-of-day 14 --day-cycle off'
        $aaNames=@{off='off'; smaa='SMAA 1x'; fsr3='FSR 3.1.4'}
        foreach ($aa in @('off','smaa','fsr3')) {
            foreach ($mapper in @('reinhard','gt7')) {
                $arguments="$lighting --aa $aa --tone-mapper $mapper"
                $expected=@{anti_aliasing=$aaNames[$aa]; dynamic_lighting=$true; voxel_gi=$false; day_cycle=$false; time_of_day_end=14; animated_camera=$false; animated=$false; tone_mapper=$mapper}
                Add-Case "$mapper-$aa" $arguments $exe $expected
            }
        }
    }
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
        $scene="--aa off --scene-count 1024 --scene-walls off --scene-asset `"$ModelAsset`" --scene-scale .02 --camera-z 300 --culling frustum"
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
if ($materialPacketSuite) {
    $environment.CaseOrderPolicy='Alternate before/after ordering by repetition within each workload; identical CLI settings and unchanged scene assets.'
    $environment.CameraComparison='Canonical CPU/GPU cases are static and require matching geometry within each workload. Stress repeats one time-based camera trajectory; final visible/frustum/triangle counts are not an equality gate.'
    $environment.CostScope=if ($Suite -eq 'CpuPackets') {
        'Compare CPU packets that duplicate 144-byte runtime instances with source plus visible-index packets. Both binaries keep the 144-byte GPU instance ABI, default material precision and shaders; canonical GPU visibility is the control workload. CPU culling is forced linear, sorting and prepass off. Timings include packet preparation, upload and draw-path work; a bandwidth cause requires separate hardware evidence.'
    } else {
        'Compare the complete old/new material pipeline with CPU culling forced linear, sorting and prepass off. Runtime instance ABI changes from 128 to 144 bytes; this is not an isolated hierarchy or memory-bandwidth benchmark.'
    }
    $environment.MaterialContract='Unchanged canonical/procedural assets use default IOR 1.5 and standard shading. There is no IOR CLI override or benchmark IOR field; authoring/import defaults and asset contents must be verified separately.'
    $environment.GeometryTimingScope='average_gpu_geometry_ms includes depth prepass (disabled here); GPU intervals must not be added to CPU timings as independent sequential work.'
    $environment.ExecutableHashes=@{
        before=@{path=$baseline; sha256=(Get-FileHash -LiteralPath $baseline -Algorithm SHA256).Hash}
        after=@{path=$exe; sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash}
    }
    $materialPacketGeometry=@{}
    $materialPacketRuns=@()
    $materialPacketMetricFields=@('average_fps','one_percent_low_fps','average_cpu_frame_ms','average_scene_prepare_ms',
        'average_fence_wait_ms','average_acquire_ms','average_command_record_ms','average_queue_submit_ms','average_present_ms',
        'average_gpu_frame_ms','average_gpu_geometry_ms','average_gpu_workload_ms','average_gpu_shadow_ms',
        'average_gpu_postprocess_ms','average_gpu_depth_prepass_ms','average_gpu_voxel_geometry_ms',
        'average_gpu_voxel_light_ms','average_gpu_visibility_ms')
    function MaterialPacketSummary {
        foreach ($workload in $materialPacketWorkloads) {
            foreach ($side in @('before','after')) {
                $samples=@($materialPacketRuns | Where-Object {$_.workload -eq $workload -and $_.side -eq $side})
                if ($samples.Count -eq 0) { continue }
                $statistics=[ordered]@{}
                foreach ($field in $materialPacketMetricFields) {
                    $values=@($samples | Where-Object {$null -ne $_.$field} | ForEach-Object {[double]$_.$field} | Sort-Object)
                    if ($values.Count -eq 0) {
                        $statistics[$field]=@{median=$null; min=$null; max=$null; available_samples=0; unavailable_samples=$samples.Count}
                        continue
                    }
                    $middle=[int][Math]::Floor($values.Count/2)
                    $median=if ($values.Count % 2) { $values[$middle] } else { ($values[$middle-1]+$values[$middle])/2 }
                    $statistics[$field]=@{median=$median; min=$values[0]; max=$values[-1]; available_samples=$values.Count; unavailable_samples=($samples.Count-$values.Count)}
                }
                @{workload=$workload; side=$side; repeats=$samples.Count; metrics=$statistics}
            }
        }
    }
}
if ($Suite -eq 'DepthPrepass') {
    $environment.CaseOrderPolicy='Within each fixture rotate off/partial/full by repetition; all variants use the same executable and static scene.'
    $environment.GeometryTimingScope='average_gpu_geometry_ms includes average_gpu_depth_prepass_ms; do not add the subset twice.'
    $depthPrepassGeometry=@{}
    $depthPrepassRuns=@()
}
if ($Suite -eq 'FrontToBack') {
    $environment.CaseOrderPolicy='Alternate off/coarse ordering by repetition within each workload; same executable and unchanged scene assets.'
    $environment.CameraComparison='Canonical is static. Stress repeats the same time-based camera trajectory; terminal frame poses need not match. Culling and LOD are disabled so geometry counts remain constant.'
    $environment.CostScope='scene_prepare_ms and CPU frame include CPU visibility, packing and coarse sorting work; GPU frame and FPS include the resulting draw order.'
    $frontToBackGeometry=@{}
    $frontToBackRuns=@()
    $frontToBackMetricFields=@('average_scene_prepare_ms','average_cpu_frame_ms','average_gpu_frame_ms','average_fps','one_percent_low_fps')
    function FrontToBackSummary {
        foreach ($workload in $frontToBackWorkloads) {
            foreach ($sorting in @('off','coarse')) {
                $samples=@($frontToBackRuns | Where-Object {$_.workload -eq $workload -and $_.front_to_back_actual -eq $sorting})
                if ($samples.Count -eq 0) { continue }
                $statistics=[ordered]@{}
                foreach ($field in $frontToBackMetricFields) {
                    $values=@($samples | ForEach-Object {[double]$_.$field} | Sort-Object)
                    $middle=[int][Math]::Floor($values.Count/2)
                    $median=if ($values.Count % 2) { $values[$middle] } else { ($values[$middle-1]+$values[$middle])/2 }
                    $statistics[$field]=@{median=$median; min=$values[0]; max=$values[-1]}
                }
                @{workload=$workload; front_to_back=$sorting; repeats=$samples.Count; metrics=$statistics}
            }
        }
    }
}
if ($Suite -eq 'CpuHierarchy') {
    $environment.CaseOrderPolicy='Alternate linear/hierarchy order by repetition within each scene count; same executable and fixed CLI settings.'
    $environment.CameraComparison='Runs repeat one time-based camera trajectory. Terminal frame poses and final visible/frustum-rejected/triangle counts may differ; equality of these counters across runs is not a gate.'
    $environment.CostScope='scene_prepare_ms and CPU frame include CPU culling and packet preparation; report GPU geometry/frame and FPS alongside CPU cost. Visibility counters describe the final sampled state, not frame averages.'
    $cpuHierarchyRuns=@()
    $cpuHierarchyMetricFields=@('average_scene_prepare_ms','average_cpu_frame_ms','average_fps','one_percent_low_fps',
        'average_gpu_geometry_ms','average_gpu_frame_ms','visible_instances','frustum_rejected')
    function CpuHierarchySummary {
        foreach ($count in $cpuHierarchyWorkloads) {
            foreach ($culling in @('linear','hierarchy')) {
                $samples=@($cpuHierarchyRuns | Where-Object {$_.scene_count -eq $count -and $_.cpu_culling_actual -eq $culling})
                if ($samples.Count -eq 0) { continue }
                $statistics=[ordered]@{}
                foreach ($field in $cpuHierarchyMetricFields) {
                    $values=@($samples | ForEach-Object {[double]$_.$field} | Sort-Object)
                    $middle=[int][Math]::Floor($values.Count/2)
                    $median=if ($values.Count % 2) { $values[$middle] } else { ($values[$middle-1]+$values[$middle])/2 }
                    $statistics[$field]=@{median=$median; min=$values[0]; max=$values[-1]}
                }
                @{scene_count=$count; cpu_culling=$culling; repeats=$samples.Count; metrics=$statistics}
            }
        }
    }
}
$environment | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out "$Suite-environment.json")
# Interleave cases by repetition to reduce drift between the two sides of A/B.
for ($run=1;$run -le $Repeats;++$run) {
    $runCases=$cases
    if ($materialPacketSuite) {
        $runCases=@()
        foreach ($workload in $materialPacketWorkloads) {
            $workloadCases=@($cases | Where-Object {$_.Name -in @("before-$workload","after-$workload")})
            for ($offset=0;$offset -lt $workloadCases.Count;++$offset) {
                $runCases+=$workloadCases[($run-1+$offset) % $workloadCases.Count]
            }
        }
    }
    if ($Suite -eq 'DepthPrepass') {
        $runCases=@()
        foreach ($workload in $depthPrepassWorkloads) {
            $workloadCases=@($cases | Where-Object { $_.Expected.render_fixture -eq $workload })
            for ($offset=0;$offset -lt $workloadCases.Count;++$offset) {
                $runCases+=$workloadCases[($run-1+$offset) % $workloadCases.Count]
            }
        }
    }
    if ($Suite -eq 'FrontToBack') {
        $runCases=@()
        foreach ($workload in $frontToBackWorkloads) {
            $workloadCases=@($cases | Where-Object {$_.Name.StartsWith("$workload-")})
            for ($offset=0;$offset -lt $workloadCases.Count;++$offset) {
                $runCases+=$workloadCases[($run-1+$offset) % $workloadCases.Count]
            }
        }
    }
    if ($Suite -eq 'CpuHierarchy') {
        $runCases=@()
        foreach ($count in $cpuHierarchyWorkloads) {
            $workloadCases=@($cases | Where-Object {$_.Expected.scene_count -eq $count})
            for ($offset=0;$offset -lt $workloadCases.Count;++$offset) {
                $runCases+=$workloadCases[($run-1+$offset) % $workloadCases.Count]
            }
        }
    }
    foreach ($case in $runCases) {
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
            foreach ($field in $case.Expected.Keys) {
                if ($null -eq $result.PSObject.Properties[$field] -or $result.$field -ne $case.Expected[$field]) {
                    throw "Benchmark setting was not applied: $name, $field expected '$($case.Expected[$field])', got '$($result.$field)'."
                }
            }
            if ($Suite -in @('ShadowCache','ShadowDepth')) {
                if ($null -eq $result.PSObject.Properties['shadow_passes_total'] -or
                    $null -eq $result.PSObject.Properties['shadow_tiles_total'] -or
                    $result.shadow_passes_total -le 0 -or
                    $result.shadow_tiles_total -lt $result.shadow_passes_total -or
                    $result.shadow_tiles_total -gt 4 * $result.shadow_passes_total) {
                    throw "Invalid shadow work counters: $name"
                }
                if (-not $result.cascade_cache -and $result.shadow_tiles_total -ne 4 * $result.shadow_passes_total) {
                    throw "Full shadow cache did not update all four tiles per pass: $name"
                }
            }
            if ($Suite -eq 'ShadowDepth' -and $result.shadow_linear_filter -isnot [bool]) {
                throw "Missing or invalid shadow filter metadata: $name"
            }
            if ($Suite -eq 'DepthPrepass') {
                $available=[ordered]@{}
                foreach ($field in @('average_gpu_frame_ms','average_gpu_geometry_ms',
                                      'average_gpu_postprocess_ms','average_gpu_depth_prepass_ms')) {
                    $value=$result.$field
                    $available[$field]=($null -ne $result.PSObject.Properties[$field] -and
                        $null -ne $value -and -not [double]::IsNaN([double]$value) -and
                        -not [double]::IsInfinity([double]$value) -and $value -ge 0)
                    if (-not $available[$field]) { throw "GPU timing unavailable: $name, $field" }
                }
                if (($result.depth_prepass_actual -eq 'off' -and $result.average_gpu_depth_prepass_ms -ne 0) -or
                    ($result.depth_prepass_actual -ne 'off' -and $result.average_gpu_depth_prepass_ms -le 0)) {
                    throw "Depth prepass timing disagrees with the active pass: $name"
                }
                if ($result.average_gpu_depth_prepass_ms -gt $result.average_gpu_geometry_ms + 0.000001) {
                    throw "Depth prepass timing exceeds its enclosing geometry interval: $name"
                }
                $geometry=[ordered]@{}
                foreach ($field in @('source_instances','visible_instances','triangles','frustum_rejected',
                                      'occlusion_rejected','lod0_instances','lod1_instances','lod2_instances')) {
                    if ($null -eq $result.PSObject.Properties[$field] -or $null -eq $result.$field -or $result.$field -lt 0) {
                        throw "Geometry counter unavailable: $name, $field"
                    }
                    $geometry[$field]=$result.$field
                }
                if ($geometry.visible_instances -le 0 -or $geometry.triangles -le 0) {
                    throw "Depth prepass fixture rendered no geometry: $name"
                }
                $workload=$result.render_fixture
                if ($depthPrepassGeometry.ContainsKey($workload)) {
                    foreach ($field in $geometry.Keys) {
                        if ($geometry[$field] -ne $depthPrepassGeometry[$workload][$field]) {
                            throw "Depth prepass variants changed geometry: $name, $field"
                        }
                    }
                } else { $depthPrepassGeometry[$workload]=$geometry }
                $depthPrepassRuns+=@{
                    name=$name; source_json="$name.json"; render_fixture=$workload
                    depth_prepass_requested=$result.depth_prepass_requested; depth_prepass_actual=$result.depth_prepass_actual
                    average_fps=$result.average_fps; one_percent_low_fps=$result.one_percent_low_fps
                    average_cpu_frame_ms=$result.average_cpu_frame_ms; average_gpu_frame_ms=$result.average_gpu_frame_ms
                    average_gpu_geometry_ms=$result.average_gpu_geometry_ms
                    average_gpu_depth_prepass_ms=$result.average_gpu_depth_prepass_ms
                    average_gpu_postprocess_ms=$result.average_gpu_postprocess_ms
                    gpu_timing_available=$available; geometry=$geometry
                }
            }
            if ($Suite -eq 'FrontToBack') {
                $workload=if ($result.scene_count -eq 100000) { 'stress-cpu' } else { 'canonical' }
                $geometry=[ordered]@{}
                foreach ($field in @('source_instances','visible_instances','triangles')) {
                    if ($null -eq $result.PSObject.Properties[$field] -or $null -eq $result.$field -or $result.$field -le 0) {
                        throw "Front-to-back geometry counter unavailable or empty: $name, $field"
                    }
                    $geometry[$field]=$result.$field
                }
                if ($frontToBackGeometry.ContainsKey($workload)) {
                    foreach ($field in $geometry.Keys) {
                        if ($geometry[$field] -ne $frontToBackGeometry[$workload][$field]) {
                            throw "Front-to-back variants changed geometry: $name, $field"
                        }
                    }
                } else { $frontToBackGeometry[$workload]=$geometry }
                $sample=@{
                    name=$name; source_json="$name.json"; workload=$workload; geometry=$geometry
                    front_to_back_requested=$result.front_to_back_requested; front_to_back_actual=$result.front_to_back_actual
                    animated_camera=$result.animated_camera
                }
                foreach ($field in $frontToBackMetricFields) {
                    $value=$result.$field
                    if ($null -eq $result.PSObject.Properties[$field] -or $null -eq $value -or
                        [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value) -or
                        $value -lt 0 -or ($field -ne 'average_scene_prepare_ms' -and $value -eq 0)) {
                        throw "Front-to-back timing or FPS unavailable: $name, $field"
                    }
                    $sample[$field]=$value
                }
                $frontToBackRuns+=$sample
            }
            if ($Suite -eq 'CpuHierarchy') {
                $sample=@{
                    name=$name; source_json="$name.json"; scene_count=$result.scene_count
                    cpu_culling_requested=$result.cpu_culling_requested; cpu_culling_actual=$result.cpu_culling_actual
                    animated_camera=$result.animated_camera
                }
                foreach ($field in ($cpuHierarchyMetricFields + @('source_instances','triangles'))) {
                    $value=$result.$field
                    if ($null -eq $result.PSObject.Properties[$field] -or $null -eq $value -or
                        [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value) -or
                        $value -lt 0 -or ($field -in @('average_cpu_frame_ms','average_gpu_frame_ms','average_fps','one_percent_low_fps','source_instances') -and $value -eq 0)) {
                        throw "CPU hierarchy metric or counter unavailable: $name, $field"
                    }
                    $sample[$field]=$value
                }
                # Do not compare final visibility counts across moving-camera
                # runs: elapsed-time trajectories can end at different poses.
                $cpuHierarchyRuns+=$sample
            }
            if ($materialPacketSuite) {
                if ($case.Name -notmatch '^(before|after)-(.+)$') { throw "Invalid $Suite case: $name" }
                $side=$Matches[1]; $workload=$Matches[2]
                $geometry=[ordered]@{}
                foreach ($field in @('source_instances','visible_instances','triangles','frustum_rejected',
                                      'occlusion_rejected','lod0_instances','lod1_instances','lod2_instances')) {
                    $value=$result.$field
                    if ($null -eq $result.PSObject.Properties[$field] -or $null -eq $value -or
                        [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value) -or $value -lt 0) {
                        throw "$Suite geometry counter unavailable: $name, $field"
                    }
                    $geometry[$field]=$value
                }
                if ($geometry.source_instances -le 0 -or $geometry.visible_instances -le 0 -or $geometry.triangles -le 0) {
                    throw "$Suite fixture rendered no geometry: $name"
                }
                if ($workload -ne 'stress-cpu') {
                    if ($materialPacketGeometry.ContainsKey($workload)) {
                        foreach ($field in $geometry.Keys) {
                            if ($geometry[$field] -ne $materialPacketGeometry[$workload][$field]) {
                                throw "$Suite static variants changed geometry: $name, $field"
                            }
                        }
                    } else { $materialPacketGeometry[$workload]=$geometry }
                }
                if ($result.shadow_linear_filter -isnot [bool] -or
                    $null -eq $result.PSObject.Properties['shadow_passes_total'] -or
                    $null -eq $result.PSObject.Properties['shadow_tiles_total'] -or
                    $result.shadow_passes_total -lt 0 -or $result.shadow_tiles_total -ne 4*$result.shadow_passes_total) {
                    throw "$Suite full-shadow metadata is invalid: $name"
                }
                $sample=@{
                    name=$name; source_json="$name.json"; workload=$workload; side=$side; geometry=$geometry
                    shadow_passes_total=$result.shadow_passes_total; shadow_tiles_total=$result.shadow_tiles_total
                    settings=$case.Expected; shadow_linear_filter=$result.shadow_linear_filter
                }
                foreach ($field in $materialPacketMetricFields) {
                    $value=$result.$field
                    if ($null -eq $result.PSObject.Properties[$field] -or
                        ($null -eq $value -and $field -ne 'average_gpu_workload_ms') -or
                        ($null -ne $value -and ([double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value) -or
                            $value -lt 0 -or ($field -in @('average_fps','one_percent_low_fps','average_cpu_frame_ms','average_gpu_frame_ms','average_gpu_geometry_ms') -and $value -eq 0)))) {
                        throw "$Suite timing unavailable or invalid: $name, $field"
                    }
                    # Scene mode may not issue the gpu-saturation compute workload.
                    $sample[$field]=$value
                }
                $materialPacketRuns+=$sample
            }
            Copy-Item -LiteralPath (Join-Path $out 'benchmark.json') -Destination (Join-Path $out "$name.json")
            if ($materialPacketSuite) {
                @{
                    schema_version=1; expected_runs=($cases.Count*$Repeats)
                    complete=($materialPacketRuns.Count -eq $cases.Count*$Repeats)
                    camera_comparison=$environment.CameraComparison; cost_scope=$environment.CostScope
                    material_contract=$environment.MaterialContract
                    runs=$materialPacketRuns; summaries=@(MaterialPacketSummary)
                } | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out "$Suite-metrics.json")
            }
            if ($Suite -eq 'CpuHierarchy') {
                @{
                    schema_version=1; expected_runs=($cases.Count*$Repeats)
                    complete=($cpuHierarchyRuns.Count -eq $cases.Count*$Repeats)
                    camera_comparison=$environment.CameraComparison; cost_scope=$environment.CostScope
                    runs=$cpuHierarchyRuns; summaries=@(CpuHierarchySummary)
                } | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'CpuHierarchy-metrics.json')
            }
            if ($Suite -eq 'FrontToBack') {
                @{
                    schema_version=1; expected_runs=($cases.Count*$Repeats)
                    complete=($frontToBackRuns.Count -eq $cases.Count*$Repeats)
                    camera_comparison=$environment.CameraComparison; cost_scope=$environment.CostScope
                    runs=$frontToBackRuns; summaries=@(FrontToBackSummary)
                } | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'FrontToBack-metrics.json')
            }
            if ($Suite -eq 'DepthPrepass') {
                @{
                    schema_version=1; expected_runs=($cases.Count*$Repeats)
                    complete=($depthPrepassRuns.Count -eq $cases.Count*$Repeats)
                    geometry_timing_scope=$environment.GeometryTimingScope; runs=$depthPrepassRuns
                } | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'DepthPrepass-metrics.json')
                '{0}: {1:N1} FPS; GPU {2:N4} ms; geometry {3:N4} ms (includes prepass {4:N4} ms); {5} visible, {6} triangles' -f $name,$result.average_fps,$result.average_gpu_frame_ms,$result.average_gpu_geometry_ms,$result.average_gpu_depth_prepass_ms,$result.visible_instances,$result.triangles
            } else {
                '{0}: {1:N1} FPS; GPU {2:N4} ms' -f $name,$result.average_fps,$result.average_gpu_frame_ms
            }
        } finally { $process.Dispose() }
    }
}
