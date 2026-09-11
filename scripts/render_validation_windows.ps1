param(
    [Parameter(Mandatory = $true)][string]$ExePath,
    [ValidateRange(1, 100)][int]$Startups = 3,
    [ValidateRange(1, 100)][int]$Cycles = 2
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $ExePath).Path
$tree = Split-Path (Split-Path $exe -Parent) -Parent
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$allowedTrees = @('windows-debug', 'windows-release', 'windows-asan') |
    ForEach-Object { Join-Path $root "build\$_" }
if ($tree -notin $allowedTrees) {
    throw 'Use an executable in one of the three native Windows build trees.'
}
$outputDirectory = Join-Path $tree 'render-validation'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

for ($startup = 1; $startup -le $Startups; ++$startup) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $exe
    $info.WorkingDirectory = $outputDirectory
    $info.Arguments = '--mcp-stdio --validation --aa off --width 640 --height 360'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.EnvironmentVariables['VK_VALIDATION_VALIDATE_SYNC'] = 'true'
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $info
    [void]$process.Start()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $script:requestId = 0
    $captures = 0

    function Request([string]$Method, [hashtable]$Arguments) {
        $script:requestId += 1
        $request = @{
            jsonrpc = '2.0'; id = $script:requestId
            method = $Method; params = $Arguments
        } | ConvertTo-Json -Depth 12 -Compress
        $process.StandardInput.WriteLine($request)
        $process.StandardInput.Flush()
        $read = $process.StandardOutput.ReadLineAsync()
        if (-not $read.Wait(30000)) { throw "Timed out: $Method" }
        if ([string]::IsNullOrWhiteSpace($read.Result)) { throw 'MCP stdout closed.' }
        $response = $read.Result | ConvertFrom-Json
        if ($response.id -ne $script:requestId -or $null -ne $response.error) {
            throw "Invalid MCP response for $Method"
        }
        if ($response.result.isError) {
            throw "Tool failed: $($response.result.content[0].text)"
        }
        return $response.result
    }

    function Tool([string]$Name, [hashtable]$Arguments = @{}) {
        return Request 'tools/call' @{ name = $Name; arguments = $Arguments }
    }

    try {
        $null = Request 'initialize' @{
            protocolVersion = '2025-11-25'; capabilities = @{}
            clientInfo = @{ name = 'vcr-render-validation'; version = '1.0' }
        }
        $process.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized"}')
        $process.StandardInput.Flush()
        $state = Tool 'debug.get_state'
        $methods = @('off', 'smaa')
        if ($state.structuredContent.render.anti_aliasing.fsr3_available) {
            $methods += 'fsr3'
        }
        for ($cycle = 0; $cycle -lt $Cycles; ++$cycle) {
            $null = Tool 'window.set_size' @{ width = 640 + 128 * $cycle; height = 360 + 72 * $cycle }
            $null = Tool 'render.set_present_mode' @{ mode = 'fifo' }
            $null = Tool 'render.set_present_mode' @{ mode = 'immediate' }
            foreach ($aa in $methods) {
                $state = Tool 'debug.get_state'
                if ($state.structuredContent.render.anti_aliasing.method -ne $aa) {
                    throw "AA switch did not select $aa"
                }
                $null = Tool 'input.mouse_move' @{ delta_x = 3; delta_y = -1 }
                $capture = Tool 'capture.screenshot'
                if ($capture.structuredContent.width -le 0 -or
                    $capture.structuredContent.height -le 0) {
                    throw 'Capture did not complete after resize/AA change.'
                }
                $captures += 1
                $null = Tool 'input.key' @{ key = 'f6'; action = 'press' }
            }
            $null = Tool 'render.set_instances' @{ count = 100000 }
            $null = Tool 'render.set_mode' @{ mode = 'instancing' }
            $null = Tool 'capture.screenshot'
            $null = Tool 'render.set_gpu_workload' @{ iterations = 8 }
            $null = Tool 'render.set_mode' @{ mode = 'gpu-saturation' }
            $null = Tool 'capture.screenshot'
            $captures += 2
            $null = Tool 'render.set_gpu_workload' @{ iterations = 0 }
            $null = Tool 'render.set_mode' @{ mode = 'scene' }
        }
        $null = Tool 'engine.quit'
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(15000)) { throw 'Shutdown timed out.' }
        $stderr = $stderrTask.Result
        $logPath = Join-Path $outputDirectory "startup-$startup.log"
        [IO.File]::WriteAllText($logPath, $stderr)
        if ($process.ExitCode -ne 0 -or
            $stderr -match '\[Vulkan ERROR\]|Validation Error|VUID-|SYNC-HAZARD|hazard detected|FidelityFX error:|AddressSanitizer|Live GPU allocations: [1-9]') {
            throw "Renderer validation failed; see $logPath"
        }
        if ($stderr -match 'validation requested.*unavailable' -or
            $stderr -notmatch 'Live GPU allocations: 0 \(0 bytes\)') {
            throw "Validation layer or allocation check was not available; see $logPath"
        }
        [pscustomobject]@{
            startup = $startup; cycles = $Cycles; captures = $captures
            aa_methods = $methods
            validation_errors = 0; live_gpu_allocations = 0; exit_code = $process.ExitCode
        } | ConvertTo-Json -Compress
    }
    finally {
        if (-not $process.HasExited) {
            $process.Kill()
            $process.WaitForExit()
        }
        if ($stderrTask.IsCompleted) {
            [IO.File]::WriteAllText(
                (Join-Path $outputDirectory "startup-$startup.log"), $stderrTask.Result)
        }
        $process.Dispose()
    }
}
