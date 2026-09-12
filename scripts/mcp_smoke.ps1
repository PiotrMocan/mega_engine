param(
    [Parameter(Mandatory = $true)]
    [string]$ExePath,
    [switch]$Validation,
    [ValidateSet('off', 'smaa', 'fsr3')][string]$AntiAliasing = 'off',
    [string]$ScreenshotPath = "",
    [string]$ExtraArguments = "",
    [float]$CameraDeltaX = 0,
    [float]$CameraDeltaY = 0,
    [ValidateSet('','cpu','gpu','direct')][string]$ExpectedBackend=''
)

$ErrorActionPreference = "Stop"

$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $ExePath
$validationArgument = if ($Validation) {
    "--validation"
} else {
    "--no-validation"
}
$startInfo.Arguments =
    "--mcp-stdio $validationArgument --aa $AntiAliasing --width 640 --height 360 $ExtraArguments"
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

    $serialized = $Message | ConvertTo-Json -Compress -Depth 20
    $process.StandardInput.WriteLine($serialized)
    $process.StandardInput.Flush()
    if ($null -eq $ExpectedId) {
        return $null
    }

    $line = $process.StandardOutput.ReadLine()
    if ([string]::IsNullOrWhiteSpace($line)) {
        throw "MCP stdout closed before response $ExpectedId"
    }
    $response = $line | ConvertFrom-Json
    if ($response.id -ne $ExpectedId) {
        throw "Expected MCP response $ExpectedId, got $($response.id)"
    }
    if ($null -ne $response.error) {
        throw "MCP error $($response.error.code): $($response.error.message)"
    }
    return $response.result
}

try {
    $initialize = Send-McpMessage @{
        jsonrpc = "2.0"
        id = 1
        method = "initialize"
        params = @{
            protocolVersion = "2025-11-25"
            capabilities = @{}
            clientInfo = @{
                name = "vcr-windows-smoke"
                version = "1.0.0"
            }
        }
    } 1
    if ($initialize.protocolVersion -ne "2025-11-25") {
        throw "Unexpected MCP protocol version"
    }
    Send-McpMessage @{
        jsonrpc = "2.0"
        method = "notifications/initialized"
    }

    $toolList = Send-McpMessage @{
        jsonrpc = "2.0"
        id = 2
        method = "tools/list"
        params = @{}
    } 2
    $toolNames = @($toolList.tools | ForEach-Object { $_.name })
    foreach ($required in @(
        "debug.get_state",
        "test.run_self_check",
        "capture.screenshot",
        "input.key",
        "engine.quit"
    )) {
        if ($required -notin $toolNames) {
            throw "Required MCP tool is missing: $required"
        }
    }

    $selfCheck = Send-McpMessage @{
        jsonrpc = "2.0"
        id = 3
        method = "tools/call"
        params = @{
            name = "test.run_self_check"
            arguments = @{}
        }
    } 3
    if ($selfCheck.isError -or
        -not $selfCheck.structuredContent.passed) {
        throw "MCP runtime self-check failed"
    }

    if ($CameraDeltaX -ne 0 -or $CameraDeltaY -ne 0) {
        $null = Send-McpMessage @{
            jsonrpc = '2.0'; id = 30; method = 'tools/call'
            params = @{ name = 'input.mouse_move'; arguments = @{ delta_x = $CameraDeltaX; delta_y = $CameraDeltaY } }
        } 30
    }
    $screenshot = Send-McpMessage @{
        jsonrpc = "2.0"
        id = 4
        method = "tools/call"
        params = @{
            name = "capture.screenshot"
            arguments = @{}
        }
    } 4
    if ($screenshot.isError) {
        throw "MCP screenshot failed: $($screenshot.content[0].text)"
    }
    $image = $screenshot.content |
        Where-Object { $_.type -eq "image" } |
        Select-Object -First 1
    if ($null -eq $image -or $image.mimeType -ne "image/png") {
        throw "MCP screenshot did not return PNG ImageContent"
    }
    $png = [Convert]::FromBase64String($image.data)
    $pngSignature = [byte[]](
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)
    for ($index = 0; $index -lt $pngSignature.Length; ++$index) {
        if ($png[$index] -ne $pngSignature[$index]) {
            throw "MCP screenshot has an invalid PNG signature"
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($ScreenshotPath)) {
        [System.IO.File]::WriteAllBytes($ScreenshotPath, $png)
    }

    $state = Send-McpMessage @{
        jsonrpc='2.0'; id=31; method='tools/call'
        params=@{name='debug.get_state'; arguments=@{}}
    } 31
    if ($state.isError) { throw 'Unable to read renderer state after capture' }
    if ($ExpectedBackend -and $state.structuredContent.render.visibility_backend -ne $ExpectedBackend) {
        throw "Expected $ExpectedBackend visibility backend, got $($state.structuredContent.render.visibility_backend)"
    }

    $quit = Send-McpMessage @{
        jsonrpc = "2.0"
        id = 5
        method = "tools/call"
        params = @{
            name = "engine.quit"
            arguments = @{}
        }
    } 5
    if (-not $quit.structuredContent.shutting_down) {
        throw "engine.quit was not acknowledged"
    }
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(15000)) {
        throw "MCP renderer did not exit after engine.quit"
    }
    $stderr = $stderrTask.Result
    if ($process.ExitCode -ne 0) {
        throw "MCP renderer exited with $($process.ExitCode):`n$stderr"
    }
    if ($stderr -match '\[Vulkan ERROR\]|Validation Error|VUID-|SYNC-HAZARD|hazard detected|FidelityFX error:|AddressSanitizer|Live GPU allocations: [1-9]') {
        throw "Vulkan validation reported an error:`n$stderr"
    }

    [pscustomobject]@{
        protocol = $initialize.protocolVersion
        tools = $toolNames.Count
        self_check = $true
        screenshot_width = $screenshot.structuredContent.width
        screenshot_height = $screenshot.structuredContent.height
        screenshot_bytes = $png.Length
        validation = [bool]$Validation
        visibility_backend = $state.structuredContent.render.visibility_backend
        visibility = $state.structuredContent.render.visibility
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
