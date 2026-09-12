param(
    [Parameter(Mandatory = $true)]
    [string]$ExePath,
    [switch]$Validation,
    [ValidateRange(320, 16384)]
    [int]$Width = 800,
    [ValidateRange(240, 16384)]
    [int]$Height = 600,
    [string]$ScreenshotPath
)

$ErrorActionPreference = "Stop"

$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $ExePath
$validationArgument = if ($Validation) { "--validation" } else { "--no-validation" }
$startInfo.Arguments = "--mcp-stdio $validationArgument --width $Width --height $Height"
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true

$process = New-Object System.Diagnostics.Process
$process.StartInfo = $startInfo
if (-not $process.Start()) {
    throw "Unable to start $ExePath"
}
$stderrTask = $process.StandardError.ReadToEndAsync()

function Send-McpMessage {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$Message,
        [Nullable[int]]$ExpectedId = $null
    )
    $process.StandardInput.WriteLine(
        ($Message | ConvertTo-Json -Compress -Depth 20))
    $process.StandardInput.Flush()
    if ($null -eq $ExpectedId) {
        return $null
    }
    $line = $process.StandardOutput.ReadLine()
    if ([string]::IsNullOrWhiteSpace($line)) {
        throw "MCP stdout closed before response $ExpectedId"
    }
    $response = $line | ConvertFrom-Json
    if ($response.id -ne $ExpectedId -or $null -ne $response.error) {
        throw "Unexpected MCP response: $line"
    }
    return $response.result
}

try {
    $initialize = Send-McpMessage @{
        jsonrpc = "2.0"; id = 1; method = "initialize"
        params = @{
            protocolVersion = "2025-11-25"; capabilities = @{}
            clientInfo = @{ name = "vcr-editor-smoke"; version = "1.0.0" }
        }
    } 1
    Send-McpMessage @{ jsonrpc = "2.0"; method = "notifications/initialized" }

    $toolList = Send-McpMessage @{
        jsonrpc = "2.0"; id = 2; method = "tools/list"; params = @{}
    } 2
    $toolNames = @($toolList.tools | ForEach-Object { $_.name })
    foreach ($required in @(
        "editor.get_state",
        "editor.scene.list_nodes",
        "editor.scene.create_node",
        "editor.scene.duplicate_node",
        "editor.selection.set",
        "editor.camera.focus_selection",
        "editor.landscape.create_flat_ground",
        "editor.landscape.scatter_model",
        "editor.assets.list",
        "editor.assets.import",
        "editor.history.undo",
        "capture.screenshot",
        "engine.quit"
    )) {
        if ($required -notin $toolNames) {
            throw "Required editor MCP tool is missing: $required"
        }
    }

    $before = Send-McpMessage @{
        jsonrpc = "2.0"; id = 3; method = "tools/call"
        params = @{ name = "editor.get_state"; arguments = @{} }
    } 3
    if ($before.isError) { throw "Unable to read editor state" }

    $assets = Send-McpMessage @{
        jsonrpc = "2.0"; id = 4; method = "tools/call"
        params = @{
            name = "editor.assets.list"
            arguments = @{ directory = ""; offset = 0; limit = 20 }
        }
    } 4
    if ($assets.isError) { throw "Unable to list editor assets" }

    $folder = Send-McpMessage @{
        jsonrpc = "2.0"; id = 5; method = "tools/call"
        params = @{
            name = "editor.scene.create_node"
            arguments = @{ name = "MCP Smoke"; kind = "folder" }
        }
    } 5
    if ($folder.isError) { throw "Unable to create editor node" }
    $folderId = $folder.structuredContent.node.id

    $ground = Send-McpMessage @{
        jsonrpc = "2.0"; id = 6; method = "tools/call"
        params = @{
            name = "editor.landscape.create_flat_ground"
            arguments = @{
                name = "MCP Ground"; parent = $folderId
                center = @(0, -1, 0); width = 20; depth = 12
            }
        }
    } 6
    if ($ground.isError) { throw "Unable to create flat terrain" }
    $groundId = $ground.structuredContent.node.id

    $update = Send-McpMessage @{
        jsonrpc = "2.0"; id = 7; method = "tools/call"
        params = @{
            name = "editor.scene.update_node"
            arguments = @{
                id = $groundId
                base_color = @(0.2, 0.45, 0.1, 1.0)
                roughness = 0.9
            }
        }
    } 7
    if ($update.isError -or -not $update.structuredContent.changed) {
        throw "Unable to update authored ground material"
    }

    $selection = Send-McpMessage @{
        jsonrpc = "2.0"; id = 8; method = "tools/call"
        params = @{
            name = "editor.selection.set"
            arguments = @{ id = $groundId }
        }
    } 8
    if ($selection.isError -or
        $selection.structuredContent.selection.index -ne $groundId.index) {
        throw "Unable to select the authored ground"
    }

    $duplicate = Send-McpMessage @{
        jsonrpc = "2.0"; id = 9; method = "tools/call"
        params = @{
            name = "editor.scene.duplicate_node"
            arguments = @{ id = $groundId }
        }
    } 9
    if ($duplicate.isError -or
        $duplicate.structuredContent.duplicated_nodes -ne 1) {
        throw "Unable to duplicate the selected authored ground"
    }

    $focus = Send-McpMessage @{
        jsonrpc = "2.0"; id = 10; method = "tools/call"
        params = @{
            name = "editor.camera.focus_selection"
            arguments = @{}
        }
    } 10
    if ($focus.isError) { throw "Unable to focus the editor selection" }

    $nodes = Send-McpMessage @{
        jsonrpc = "2.0"; id = 11; method = "tools/call"
        params = @{
            name = "editor.scene.list_nodes"
            arguments = @{ offset = 0; limit = 100 }
        }
    } 11
    if ($nodes.isError -or
        $nodes.structuredContent.total -ne $before.structuredContent.node_count + 3) {
        throw "Editor scene mutations are not visible through list_nodes"
    }

    $screenshot = Send-McpMessage @{
        jsonrpc = "2.0"; id = 12; method = "tools/call"
        params = @{ name = "capture.screenshot"; arguments = @{} }
    } 12
    $image = $screenshot.content |
        Where-Object { $_.type -eq "image" } |
        Select-Object -First 1
    if ($screenshot.isError -or $null -eq $image -or $image.mimeType -ne "image/png") {
        throw "Editor screenshot did not return PNG ImageContent"
    }
    if (-not [string]::IsNullOrWhiteSpace($ScreenshotPath)) {
        [System.IO.File]::WriteAllBytes(
            $ScreenshotPath,
            [Convert]::FromBase64String($image.data))
    }

    $undo = Send-McpMessage @{
        jsonrpc = "2.0"; id = 13; method = "tools/call"
        params = @{ name = "editor.history.undo"; arguments = @{} }
    } 13
    if ($undo.isError -or -not $undo.structuredContent.undone) {
        throw "Editor undo failed"
    }

    $quit = Send-McpMessage @{
        jsonrpc = "2.0"; id = 14; method = "tools/call"
        params = @{ name = "engine.quit"; arguments = @{} }
    } 14
    if (-not $quit.structuredContent.shutting_down) {
        throw "engine.quit was not acknowledged"
    }
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(15000)) {
        throw "MCP editor did not exit after engine.quit"
    }
    $stderr = $stderrTask.Result
    if ($process.ExitCode -ne 0) {
        throw "MCP editor exited with $($process.ExitCode):`n$stderr"
    }
    if ($stderr -match '\[Vulkan ERROR\]|Validation Error|VUID-|SYNC-HAZARD|AddressSanitizer|Live GPU allocations: [1-9]') {
        throw "Editor validation reported an error:`n$stderr"
    }

    [pscustomobject]@{
        protocol = $initialize.protocolVersion
        tools = $toolNames.Count
        initial_nodes = $before.structuredContent.node_count
        listed_nodes = $nodes.structuredContent.total
        listed_assets = $assets.structuredContent.total
        screenshot_width = $screenshot.structuredContent.width
        screenshot_height = $screenshot.structuredContent.height
        validation = [bool]$Validation
        exit_code = $process.ExitCode
    } | ConvertTo-Json -Compress
}
finally {
    if (-not $process.HasExited) {
        $process.Kill()
        $process.WaitForExit()
    }
    $process.Dispose()
}
