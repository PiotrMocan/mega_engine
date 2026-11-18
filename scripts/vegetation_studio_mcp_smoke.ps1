param(
    [string]$ExePath = "build\windows-debug\bin\vulkan_vegetation_studio.exe",
    [switch]$Validation
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $ExePath)) {
    throw "Vegetation Studio executable not found: $ExePath"
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("vcr_veg_mcp_smoke_" + [System.Guid]::NewGuid().ToString("N"))
$tempProject = Join-Path $tempRoot "sample_project"
$screenshotPath = Join-Path $tempRoot "veg_lod_lineup.png"
New-Item -ItemType Directory -Path (Join-Path $tempProject "assets") -Force | Out-Null

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = (Resolve-Path -LiteralPath $ExePath).Path
$valFlag = if ($Validation) { "--validation" } else { "--no-validation" }
$psi.Arguments = "--mcp-stdio $valFlag --width 1280 --height 720"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true
$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8

$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $psi
if (-not $proc.Start()) {
    throw "Failed to start $ExePath"
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$stdin = New-Object System.IO.StreamWriter($proc.StandardInput.BaseStream, $utf8NoBom)
$stdin.AutoFlush = $true
$stderrTask = $proc.StandardError.ReadToEndAsync()

function Invoke-Mcp {
    param(
        [int]$Id,
        [string]$Method,
        $Params
    )
    $msg = [ordered]@{
        jsonrpc = "2.0"
        id = $Id
        method = $Method
    }
    if ($null -ne $Params) {
        $msg.params = $Params
    }
    $line = ($msg | ConvertTo-Json -Depth 12 -Compress)
    $stdin.WriteLine($line)

    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $deadline) {
        $readTask = $proc.StandardOutput.ReadLineAsync()
        while (-not $readTask.Wait(200)) {
            if ($proc.HasExited) {
                throw "Process exited unexpectedly with code $($proc.ExitCode)"
            }
            if ([DateTime]::UtcNow -ge $deadline) {
                throw "Timed out waiting for response to $Method (id=$Id)"
            }
        }
        $raw = $readTask.Result
        if ($null -eq $raw) {
            throw "EOF on stdout waiting for $Method (id=$Id)"
        }
        if ([string]::IsNullOrWhiteSpace($raw)) {
            continue
        }
        $obj = $raw | ConvertFrom-Json
        if ($obj.PSObject.Properties["id"] -and [int]$obj.id -eq $Id) {
            if ($obj.PSObject.Properties["error"] -and $null -ne $obj.error) {
                throw "JSON-RPC error for ${Method}: $($obj.error | ConvertTo-Json -Compress)"
            }
            return $obj.result
        }
    }
    throw "Timed out waiting for response to $Method"
}

function Send-Notification {
    param([string]$Method)
    $line = (@{ jsonrpc = "2.0"; method = $Method } | ConvertTo-Json -Compress)
    $stdin.WriteLine($line)
}

function Call-Tool {
    param(
        [int]$Id,
        [string]$Name,
        $Arguments
    )
    $res = Invoke-Mcp -Id $Id -Method "tools/call" -Params ([ordered]@{
        name = $Name
        arguments = if ($null -eq $Arguments) { @{} } else { $Arguments }
    })
    if ($res.PSObject.Properties["isError"] -and $res.isError -eq $true) {
        throw "Tool '$Name' returned isError=true: $($res.content[0].text)"
    }
    return $res
}

try {
    # 1. initialize + notifications/initialized
    $init = Invoke-Mcp -Id 1 -Method "initialize" -Params ([ordered]@{
        protocolVersion = "2025-11-25"
        capabilities = @{}
        clientInfo = @{ name = "veg_mcp_smoke"; version = "1.0.0" }
    })
    if ($init.protocolVersion -ne "2025-11-25") {
        throw "Unexpected protocolVersion: $($init.protocolVersion)"
    }
    Send-Notification -Method "notifications/initialized"

    # 2. tools/list -> assert all 15 tools
    $toolsRes = Invoke-Mcp -Id 2 -Method "tools/list" -Params @{}
    $expectedTools = @(
        "veg.list_presets",
        "veg.get_state",
        "veg.apply_preset",
        "veg.set_recipe",
        "veg.randomize_seed",
        "veg.paint_foliage",
        "veg.clear_paint",
        "veg.set_arena",
        "veg.set_camera",
        "veg.export_to_project",
        "veg.open_from_project",
        "veg.list_project_vegetation",
        "render.get_metrics",
        "capture.screenshot",
        "engine.quit"
    )
    $actualNames = @($toolsRes.tools | ForEach-Object { $_.name })
    foreach ($tool in $expectedTools) {
        if ($actualNames -notcontains $tool) {
            throw "Missing MCP tool in tools/list: $tool"
        }
    }
    if ($actualNames.Count -ne 15) {
        throw "Expected 15 tools, got $($actualNames.Count)"
    }

    # 3. veg.list_presets
    $presetsCall = Call-Tool -Id 3 -Name "veg.list_presets" -Arguments @{}
    $presetsJson = $presetsCall.content[0].text | ConvertFrom-Json
    if ($presetsJson.presets.Count -lt 14) {
        throw "Expected >= 14 presets, got $($presetsJson.presets.Count)"
    }

    # 4. veg.apply_preset
    $null = Call-Tool -Id 4 -Name "veg.apply_preset" -Arguments @{
        preset_id = "tropical_coconut_palm"
        seed = 4242
    }

    # 5. veg.paint_foliage
    $null = Call-Tool -Id 5 -Name "veg.paint_foliage" -Arguments @{
        mode = "autumn"
        center = @(0.0, 5.5, 0.0)
        radius = 2.0
        strength = 0.9
    }

    # 6. veg.set_arena
    $null = Call-Tool -Id 6 -Name "veg.set_arena" -Arguments @{
        mode = "lod_lineup"
        spacing = 5.0
    }

    # 7. veg.get_state
    $stateCall = Call-Tool -Id 7 -Name "veg.get_state" -Arguments @{}
    $state = $stateCall.content[0].text | ConvertFrom-Json
    if ($state.arena.mode -ne "lod_lineup") {
        throw "Expected arena.mode == 'lod_lineup', got '$($state.arena.mode)'"
    }
    if ([int]$state.arena.submitted_instances -ne 6 -or $state.report.triangles.Count -ne 6) {
        throw "Expected six preview models and six LOD budgets."
    }
    if ([int]$state.paint_brush.stroke_count -ne 1) {
        throw "Expected stroke_count == 1, got $($state.paint_brush.stroke_count)"
    }

    # 8. veg.export_to_project
    $exportCall = Call-Tool -Id 8 -Name "veg.export_to_project" -Arguments @{
        project_dir = $tempProject
        category_subdir = "models/vegetation"
        asset_slug = "smoke_coconut_palm"
    }
    $exportPayload = $exportCall.content[0].text | ConvertFrom-Json
    if (-not (Test-Path -LiteralPath $exportPayload.lod0_model_file)) {
        throw "Missing exported .vcrasset.json: $($exportPayload.lod0_model_file)"
    }
    $model = Get-Content -Raw -LiteralPath $exportPayload.lod0_model_file | ConvertFrom-Json
    if ($model.lods.Count -ne 5 -or $model.detail_aa -ne 'coverage') {
        throw "Exported asset requires five LOD references and filtered coverage."
    }
    if (-not (Test-Path -LiteralPath $exportPayload.recipe_file)) {
        throw "Missing exported .vcrveg.json: $($exportPayload.recipe_file)"
    }
    $meshFiles = Get-ChildItem -LiteralPath $exportPayload.package_directory -Filter "*.vcrmesh"
    if ($meshFiles.Count -lt 4) {
        throw "Expected at least 4 .vcrmesh files in $($exportPayload.package_directory)"
    }

    # 9. veg.list_project_vegetation
    $listCall = Call-Tool -Id 9 -Name "veg.list_project_vegetation" -Arguments @{
        project_dir = $tempProject
    }
    $listJson = $listCall.content[0].text | ConvertFrom-Json
    $listedSlugs = @($listJson.assets | ForEach-Object { $_.asset_slug })
    if ($listedSlugs -notcontains "smoke_coconut_palm") {
        throw "Expected 'smoke_coconut_palm' in project vegetation listing."
    }

    # 10. render.get_metrics (allow brief wait for steady-state frames & arena submission)
    $metricsJson = $null
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        $metricsCall = Call-Tool -Id (100 + $attempt) -Name "render.get_metrics" -Arguments @{}
        $metricsJson = $metricsCall.content[0].text | ConvertFrom-Json
        if ([int]$metricsJson.frames_rendered -ge 2 -and
            [double]$metricsJson.one_second_average_fps -gt 0.0 -and
            [double]$metricsJson.cpu_frame_ms -gt 0.0 -and
            [int]$metricsJson.visibility.submitted -ge 6) {
            break
        }
        Start-Sleep -Milliseconds 50
    }
    if ([int]$metricsJson.frames_rendered -lt 2 -or
        [double]$metricsJson.one_second_average_fps -le 0.0 -or
        [double]$metricsJson.cpu_frame_ms -le 0.0 -or
        [int]$metricsJson.visibility.submitted -lt 6) {
        throw "Unexpected metrics after arena update: frames=$($metricsJson.frames_rendered) fps=$($metricsJson.one_second_average_fps) cpu_ms=$($metricsJson.cpu_frame_ms) submitted=$($metricsJson.visibility.submitted)"
    }

    # 11. capture.screenshot
    $shotCall = Call-Tool -Id 200 -Name "capture.screenshot" -Arguments @{
        output_path = $screenshotPath
    }
    $imageBlock = $shotCall.content | Where-Object { $_.type -eq "image" } | Select-Object -First 1
    if ($null -eq $imageBlock -or $imageBlock.mimeType -ne "image/png" -or $imageBlock.data.Length -lt 100) {
        throw "capture.screenshot did not return inline image/png MCP block."
    }
    if (-not (Test-Path -LiteralPath $screenshotPath)) {
        throw "Screenshot file not created: $screenshotPath"
    }
    $bytes = [System.IO.File]::ReadAllBytes($screenshotPath)
    if ($bytes.Length -lt 8 -or $bytes[0] -ne 0x89 -or $bytes[1] -ne 0x50 -or $bytes[2] -ne 0x4E -or $bytes[3] -ne 0x47) {
        throw "Saved screenshot does not start with valid PNG header (89 50 4E 47)."
    }

    # 12. engine.quit
    $null = Call-Tool -Id 201 -Name "engine.quit" -Arguments @{}
    $stdin.Close()

    if (-not $proc.WaitForExit(15000)) {
        throw "vulkan_vegetation_studio did not exit within 15 seconds after engine.quit."
    }
    $stderrText = $stderrTask.GetAwaiter().GetResult()
    if ($proc.ExitCode -ne 0) {
        throw "Process exited with non-zero code $($proc.ExitCode). Stderr:`n$stderrText"
    }
    if ($stderrText -match "validation layer:|VK_ERROR_|VUID-") {
        throw "Vulkan validation errors detected in stderr:`n$stderrText"
    }

    Write-Output "vegetation_studio_mcp_smoke.ps1 PASSED (15 tools, fps=$([math]::Round([double]$metricsJson.one_second_average_fps, 1)), LOD lineup submitted=$($metricsJson.visibility.submitted), PNG $($bytes.Length) bytes, exit 0)."
}
finally {
    if (-not $proc.HasExited) {
        try { $proc.Kill() } catch {}
    }
    $proc.Dispose()
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
