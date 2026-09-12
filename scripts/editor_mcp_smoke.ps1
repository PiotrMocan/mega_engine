param(
    [Parameter(Mandatory = $true)]
    [string]$ExePath,
    [switch]$Validation
)

$ErrorActionPreference = "Stop"

$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $ExePath
$validationArgument = if ($Validation) { "--validation" } else { "--no-validation" }
$startInfo.Arguments = "--mcp-stdio $validationArgument --width 800 --height 600"
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

    $nodes = Send-McpMessage @{
        jsonrpc = "2.0"; id = 8; method = "tools/call"
        params = @{
            name = "editor.scene.list_nodes"
            arguments = @{ offset = 0; limit = 100 }
        }
    } 8
    if ($nodes.isError -or
        $nodes.structuredContent.total -ne $before.structuredContent.node_count + 2) {
        throw "Editor scene mutations are not visible through list_nodes"
    }

    $undo = Send-McpMessage @{
        jsonrpc = "2.0"; id = 9; method = "tools/call"
        params = @{ name = "editor.history.undo"; arguments = @{} }
    } 9
    if ($undo.isError -or -not $undo.structuredContent.undone) {
        throw "Editor undo failed"
    }

    $screenshot = Send-McpMessage @{
        jsonrpc = "2.0"; id = 10; method = "tools/call"
        params = @{ name = "capture.screenshot"; arguments = @{} }
    } 10
    $image = $screenshot.content |
        Where-Object { $_.type -eq "image" } |
        Select-Object -First 1
    if ($screenshot.isError -or $null -eq $image -or $image.mimeType -ne "image/png") {
        throw "Editor screenshot did not return PNG ImageContent"
    }

    $quit = Send-McpMessage @{
        jsonrpc = "2.0"; id = 11; method = "tools/call"
        params = @{ name = "engine.quit"; arguments = @{} }
    } 11
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
