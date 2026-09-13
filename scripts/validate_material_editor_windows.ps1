param(
    [Parameter(Mandatory=$true)][string]$ExePath,
    [ValidatePattern('^[a-zA-Z0-9_-]*$')][string]$RunTag='',
    [ValidateRange(640,16384)][int]$Width=1280,
    [ValidateRange(480,16384)][int]$Height=720
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = (Resolve-Path -LiteralPath $ExePath).Path
$tree = Split-Path (Split-Path $exe -Parent) -Parent
$allowedTrees = @('windows-debug','windows-release','windows-asan') |
    ForEach-Object { Join-Path $root "build\$_" }
if ($tree -notin $allowedTrees -or (Split-Path $exe -Leaf) -ine 'vulkan_editor.exe') {
    throw 'Use vulkan_editor.exe from one of the three approved native Windows build trees.'
}
$outputDirectory = Join-Path $tree 'render-validation/material-editor'
if ($RunTag) { $outputDirectory += "-$RunTag" }
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$scenePath = Join-Path $root 'assets/scenes/editor_scene.json'
$sceneHashBefore = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
$logPath = Join-Path $outputDirectory 'editor.log'
$tracePath = Join-Path $outputDirectory 'mcp-checks.json'
Add-Type -AssemblyName System.Drawing

$info = New-Object Diagnostics.ProcessStartInfo
$info.FileName = $exe
$info.WorkingDirectory = $outputDirectory
$info.Arguments = "--mcp-stdio --validation --width $Width --height $Height --present immediate --aa off --gi off --day-cycle off --time-of-day 14"
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardInput = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$info.EnvironmentVariables['VK_VALIDATION_VALIDATE_SYNC'] = 'true'
$process = New-Object Diagnostics.Process
$process.StartInfo = $info
$started = $false
$stderrTask = $null
$script:requestId = 0
$script:canRequest = $false
$createdIds = New-Object 'System.Collections.Generic.List[object]'
$checks = New-Object 'System.Collections.Generic.List[object]'
$failures = New-Object 'System.Collections.Generic.List[string]'
$initial = $null
$finalState = $null
$captureEvidence = $null
$quitAccepted = $false
$cleanupComplete = $false
$invalidRequests = 0
$exitCode = $null
$stderr = ''

function Request([string]$Method, [hashtable]$Parameters) {
    $script:requestId++
    $wire = @{jsonrpc='2.0'; id=$script:requestId; method=$Method; params=$Parameters} |
        ConvertTo-Json -Depth 20 -Compress
    try {
        $process.StandardInput.WriteLine($wire)
        $process.StandardInput.Flush()
        $read = $process.StandardOutput.ReadLineAsync()
        if (-not $read.Wait(30000)) { throw "MCP timeout: $Method" }
        if ([string]::IsNullOrWhiteSpace($read.Result)) { throw 'MCP stdout closed.' }
        $response = $read.Result | ConvertFrom-Json
        if ($response.id -ne $script:requestId -or $null -ne $response.error) {
            throw "Invalid MCP response: $($read.Result)"
        }
        return $response.result
    } catch {
        # A pending timed-out read must not race a second stdout reader during cleanup.
        $script:canRequest = $false
        throw
    }
}
function Tool([string]$Name, [hashtable]$Arguments=@{}, [bool]$AllowError=$false) {
    $result = Request 'tools/call' @{name=$Name; arguments=$Arguments}
    $checks.Add([pscustomobject]@{
        request=$script:requestId; tool=$Name; arguments=$Arguments
        is_error=[bool]$result.isError; structured_content=$result.structuredContent
        text=@($result.content | Where-Object {$_.type -eq 'text'} | ForEach-Object {$_.text})
    })
    # Also track an erroneously accepted invalid create, so it is cleaned up.
    if ($Name -eq 'editor.scene.create_node' -and -not $result.isError -and
        $null -ne $result.structuredContent.node.id) {
        $createdIds.Add($result.structuredContent.node.id)
    }
    if ($result.isError -and -not $AllowError) {
        throw "MCP tool failed: $Name : $($result.content[0].text)"
    }
    return $result
}
function ReadNode($Id) { return (Tool 'editor.scene.get_node' @{id=$Id}).structuredContent.node }
function State { return (Tool 'editor.get_state').structuredContent }
function AssertMaterial($Node, [hashtable]$Expected) {
    if ($null -eq $Node.pbr -or $Node.pbr.model -cne $Expected.model) {
        throw 'Missing material or incorrect exact model token.'
    }
    foreach ($key in @('ior','metallic','roughness','ambient_occlusion')) {
        if ($null -eq $Node.pbr.$key -or [single]$Node.pbr.$key -ne [single]$Expected[$key]) {
            throw "Material $key differs: expected=$($Expected[$key]), actual=$($Node.pbr.$key)"
        }
    }
    if (@($Node.pbr.base_color).Count -ne 4) { throw 'Material base_color must have four channels.' }
    for ($channel=0; $channel -lt 4; ++$channel) {
        if ([single]$Node.pbr.base_color[$channel] -ne [single]$Expected.base_color[$channel]) {
            throw "Material base_color channel $channel was not preserved."
        }
    }
}
function AssertStableState($Before, $After, [string]$Context) {
    foreach ($key in @('node_count','content_revision','structure_revision','dirty','can_undo','can_redo','mode')) {
        if ($Before.$key -ne $After.$key) { throw "$Context changed editor state: $key" }
    }
}
function AssertSameNode($Before, $After, [string]$Context) {
    if (($Before | ConvertTo-Json -Depth 16 -Compress) -cne
        ($After | ConvertTo-Json -Depth 16 -Compress)) {
        throw "$Context changed the authored node."
    }
}
function PatchMaterial($Id, [hashtable]$Patch, [hashtable]$Expected) {
    $before = ReadNode $Id
    $stateBefore = State
    $arguments = $Patch.Clone()
    # Windows PowerShell serializes Single.MaxValue as 3.4028235E+38, which
    # exceeds the strict C++ float bound. Promote to Double before JSON output.
    if ($arguments.ContainsKey('ior')) { $arguments.ior=[double]$arguments.ior }
    $arguments.id = $Id
    $updated = (Tool 'editor.scene.update_node' $arguments).structuredContent
    if ($updated.changed -ne $true) { throw 'Material patch was not marked changed.' }
    AssertMaterial $updated.node $Expected
    $after = ReadNode $Id
    AssertSameNode $updated.node $after 'Reading an updated material'
    if (($before | Select-Object * -ExcludeProperty pbr | ConvertTo-Json -Depth 16 -Compress) -cne
        ($after | Select-Object * -ExcludeProperty pbr | ConvertTo-Json -Depth 16 -Compress)) {
        throw 'A partial material patch changed identity, hierarchy or transform.'
    }
    $stateAfter = State
    if ($stateAfter.content_revision -le $stateBefore.content_revision -or
        $stateAfter.structure_revision -ne $stateBefore.structure_revision -or
        $stateAfter.node_count -ne $stateBefore.node_count) {
        throw 'Material patch has incorrect content/structure revision behavior.'
    }
}
function InvokeMaterialHistory([string]$Operation, $Id, [hashtable]$Expected) {
    $result = (Tool "editor.history.$Operation").structuredContent
    $field = if ($Operation -eq 'undo') { 'undone' } else { 'redone' }
    if ($result.$field -ne $true) { throw "History $Operation was not acknowledged." }
    AssertMaterial (ReadNode $Id) $Expected
}

try {
    $started = $process.Start()
    if (-not $started) { throw 'Unable to start the native editor.' }
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $script:canRequest = $true
    $initialize = Request 'initialize' @{
        protocolVersion='2025-11-25'; capabilities=@{}
        clientInfo=@{name='vcr-material-editor-validation'; version='1.0.0'}
    }
    $process.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized"}')
    $process.StandardInput.Flush()
    $toolList = Request 'tools/list' @{}
    foreach ($required in @('editor.get_state','editor.scene.create_node','editor.scene.update_node',
        'editor.scene.get_node','editor.scene.list_nodes','editor.scene.delete_node',
        'editor.history.undo','editor.history.redo','editor.selection.set',
        'editor.camera.focus_selection','capture.screenshot','engine.quit')) {
        if ($required -notin @($toolList.tools.name)) { throw "Missing MCP tool: $required" }
    }
    foreach ($name in @('editor.scene.create_node','editor.scene.update_node')) {
        $schema = @($toolList.tools | Where-Object {$_.name -eq $name})[0].inputSchema.properties
        if ($schema.ior.type -cne 'number' -or $schema.ior.minimum -ne 1 -or
            [single]$schema.ior.maximum -ne [single]::MaxValue -or
            $schema.model.type -cne 'string' -or
            ($schema.model.enum -join ',') -cne 'standard,unlit') {
            throw "IOR/model schema is incorrect: $name"
        }
    }
    $initial = State
    if ($initial.mode -cne 'edit') { throw 'Material validation requires editor edit mode.' }
    if ([IO.Path]::GetFullPath($initial.asset_root) -ine (Join-Path $root 'assets')) {
        throw 'Editor resolved a different asset root than the hashed canonical scene.'
    }
    $defaultNode = (Tool 'editor.scene.create_node' @{
        name='MCP E08 Default'; kind='ground'; translation=@(-3,0,0)
    }).structuredContent.node
    $defaults = @{ior=1.5; model='standard'; metallic=0; roughness=0.65;
        ambient_occlusion=1; base_color=@(1,1,1,1)}
    AssertMaterial $defaultNode $defaults
    AssertMaterial (ReadNode $defaultNode.id) $defaults

    $expected = @{ior=[single]1.0001234; model='standard'; metallic=0.25; roughness=0.375;
        ambient_occlusion=0.75; base_color=@(0.25,0.5,0.75,1)}
    $arguments = $expected.Clone()
    $arguments.name='MCP E08 Authored'; $arguments.kind='cube'; $arguments.translation=@(2,1,0)
    $authored = (Tool 'editor.scene.create_node' $arguments).structuredContent.node
    AssertMaterial $authored $expected
    AssertMaterial (ReadNode $authored.id) $expected

    $expected.ior=[single]1.7654321
    PatchMaterial $authored.id @{ior=$expected.ior} $expected
    $expected.model='unlit'
    PatchMaterial $authored.id @{model='unlit'} $expected
    $expected.model='standard'
    InvokeMaterialHistory 'undo' $authored.id $expected
    $beforeNoOp = State
    if ($beforeNoOp.can_redo -ne $true) { throw 'Undo did not create redo history.' }
    $nodeBeforeNoOp = ReadNode $authored.id
    foreach ($patch in @(@{id=$authored.id; ior=$expected.ior}, @{id=$authored.id; model=$expected.model},
        @{id=$authored.id; ior=$expected.ior; model=$expected.model})) {
        $noOp = (Tool 'editor.scene.update_node' $patch).structuredContent
        if ($noOp.changed -ne $false) { throw 'An identical material patch must report changed=false.' }
        AssertSameNode $nodeBeforeNoOp $noOp.node 'No-op material patch'
        AssertStableState $beforeNoOp (State) 'No-op material patch'
    }
    $expected.model='unlit'
    InvokeMaterialHistory 'redo' $authored.id $expected
    $expected.model='standard'
    InvokeMaterialHistory 'undo' $authored.id $expected
    $expected.ior=[single]1.0001234
    InvokeMaterialHistory 'undo' $authored.id $expected
    $expected.ior=[single]1.7654321
    InvokeMaterialHistory 'redo' $authored.id $expected
    $expected.model='unlit'
    InvokeMaterialHistory 'redo' $authored.id $expected

    foreach ($ior in @([single]1.0, [single]::MaxValue, [single]2.417123)) {
        $expected.ior=$ior
        PatchMaterial $authored.id @{ior=$ior} $expected
    }
    # The other supported primitive kind also preserves independent defaults.
    $defaults.model='unlit'
    PatchMaterial $defaultNode.id @{model='unlit'} $defaults
    $defaults.ior=[single]1.3333333
    PatchMaterial $defaultNode.id @{ior=$defaults.ior} $defaults

    $invalidCases = @(
        @{key='ior'; value=0}, @{key='ior'; value=0.99}, @{key='ior'; value=1e100},
        @{key='ior'; value=$true}, @{key='ior'; value='1.5'},
        @{key='ior'; value=@(1.5)}, @{key='ior'; value=$null},
        @{key='model'; value='unknown'}, @{key='model'; value=1}, @{key='model'; value=$null}
    )
    foreach ($operation in @('create_node','update_node')) {
        foreach ($case in $invalidCases) {
            $stateBefore = State
            $nodeBefore = ReadNode $authored.id
            $bad = if ($operation -eq 'create_node') {
                @{name='MCP E08 Rejected'; kind='cube'}
            } else { @{id=$authored.id} }
            $bad[$case.key]=$case.value
            $result = Tool "editor.scene.$operation" $bad $true
            if ($result.isError -ne $true -or
                ($result.content[0].text -notmatch $case.key)) {
                throw "Invalid $($case.key) accepted or rejected for an unrelated reason: $operation"
            }
            ++$invalidRequests
            AssertStableState $stateBefore (State) 'Rejected material request'
            AssertSameNode $nodeBefore (ReadNode $authored.id) 'Rejected material request'
        }
    }

    $listed = (Tool 'editor.scene.list_nodes' @{offset=0; limit=1}).structuredContent
    if ($listed.total -ne $initial.node_count + 2) { throw 'Unexpected scene node count after material edits.' }
    $null = Tool 'editor.selection.set' @{id=$authored.id}
    $null = Tool 'editor.camera.focus_selection'
    $capture = Tool 'capture.screenshot'
    $images = @($capture.content | Where-Object {$_.type -eq 'image'})
    if ($images.Count -ne 1 -or $images[0].mimeType -cne 'image/png' -or
        $capture.structuredContent.width -ne $Width -or $capture.structuredContent.height -ne $Height) {
        throw 'Editor capture did not return one PNG with the requested dimensions.'
    }
    $pngPath = Join-Path $outputDirectory 'material-editor.png'
    [IO.File]::WriteAllBytes($pngPath, [Convert]::FromBase64String($images[0].data))
    $bitmap = [Drawing.Bitmap]::FromFile($pngPath)
    try {
        if ($bitmap.Width -ne $Width -or $bitmap.Height -ne $Height -or
            $bitmap.RawFormat.Guid -ne [Drawing.Imaging.ImageFormat]::Png.Guid) {
            throw 'Saved editor screenshot is not a valid PNG at the requested dimensions.'
        }
    } finally { $bitmap.Dispose() }
    $captureEvidence = [pscustomobject]@{
        path=$pngPath; width=$Width; height=$Height
        sha256=(Get-FileHash -LiteralPath $pngPath -Algorithm SHA256).Hash
        note='Editor snapshot evidence; no pixel-level lighting equivalence is asserted.'
    }
} catch {
    $failures.Add($_.Exception.Message)
} finally {
    if ($started -and -not $process.HasExited -and $script:canRequest) {
        try {
            $cleanupFailed=$false
            for ($index=$createdIds.Count-1; $index -ge 0; --$index) {
                if (-not $script:canRequest) { break }
                try {
                    $deleted = (Tool 'editor.scene.delete_node' @{id=$createdIds[$index]}).structuredContent.deleted
                    if ($deleted.index -ne $createdIds[$index].index -or
                        $deleted.generation -ne $createdIds[$index].generation) {
                        throw 'Deletion did not acknowledge the created test node.'
                    }
                } catch {
                    $cleanupFailed=$true
                    $failures.Add("Node deletion failed: $($_.Exception.Message)")
                }
            }
            if (-not $script:canRequest) { throw 'MCP connection unavailable during cleanup.' }
            $finalState = State
            if ($null -ne $initial -and $finalState.node_count -ne $initial.node_count) {
                throw 'Test nodes remain after cleanup.'
            }
            $cleanupComplete=-not $cleanupFailed
        } catch { $failures.Add("Node cleanup failed: $($_.Exception.Message)") }
        if ($script:canRequest) {
            try {
                $quitAccepted = (Tool 'engine.quit').structuredContent.shutting_down -eq $true
                if (-not $quitAccepted) { throw 'engine.quit was not acknowledged.' }
                $process.StandardInput.Close()
                if (-not $process.WaitForExit(15000)) { throw 'Editor shutdown timed out.' }
            } catch { $failures.Add($_.Exception.Message) }
        }
    }
    if ($started -and -not $process.HasExited) {
        $failures.Add('Editor required forced termination; graceful shutdown was not verified.')
        $process.Kill()
        $process.WaitForExit()
    }
    if ($started) { $exitCode=$process.ExitCode }
    if ($null -ne $stderrTask) {
        if ($stderrTask.Wait(5000)) { $stderr=$stderrTask.Result }
        else { $failures.Add('Editor stderr did not close after shutdown.') }
    }
    [IO.File]::WriteAllText($logPath,$stderr)
    $process.Dispose()
    try {
        $sceneHashAfter = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
        if ($sceneHashAfter -cne $sceneHashBefore) { $failures.Add('Canonical editor_scene.json changed.') }
    } catch { $sceneHashAfter=$null; $failures.Add("Scene hash check failed: $($_.Exception.Message)") }
}

if ($exitCode -ne 0) { $failures.Add("Editor exit code was $exitCode.") }
if ($stderr -match '\[Vulkan ERROR\]|Validation Error|VUID-|SYNC-HAZARD|hazard detected|AddressSanitizer|Live GPU allocations: [1-9]') {
    $failures.Add('Validation, synchronization, ASan or live-allocation errors appeared in editor.log.')
}
if ($stderr -match 'validation requested.*unavailable' -or
    $stderr -notmatch 'Live GPU allocations: 0 \(0 bytes\)') {
    $failures.Add('Validation layer or explicit zero live GPU allocation gate was unavailable.')
}
if (-not $cleanupComplete -or -not $quitAccepted) { $failures.Add('Cleanup or graceful shutdown gate did not complete.') }
$summary = [pscustomobject]@{
    passed=($failures.Count -eq 0); executable=$exe; native_windows=$true
    validation_requested=$true; synchronization_validation_requested=$true
    asan_build=((Split-Path $tree -Leaf) -eq 'windows-asan')
    invalid_requests_rejected=$invalidRequests; checks=$checks.Count
    initial_nodes=$initial.node_count; final_nodes=$finalState.node_count
    created_nodes=$createdIds.Count; cleanup_complete=$cleanupComplete; quit_acknowledged=$quitAccepted
    scene_path=$scenePath; scene_sha256_before=$sceneHashBefore; scene_sha256_after=$sceneHashAfter
    scene_unchanged=($sceneHashBefore -ceq $sceneHashAfter); screenshot=$captureEvidence
    exit_code=$exitCode; log=$logPath; failures=@($failures.ToArray())
}
$checks.ToArray() | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $tracePath
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outputDirectory 'summary.json')
$summary | ConvertTo-Json -Depth 8 -Compress
if ($failures.Count -ne 0) { throw "Material editor validation failed: $($failures -join '; ')" }
