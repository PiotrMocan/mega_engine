param([string]$ExePath = "$PSScriptRoot/../build/windows-release/bin/vulkan_editor.exe")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$assetDestination = Join-Path $root 'assets/neon_city'
New-Item -ItemType Directory -Force $assetDestination | Out-Null

$info = [System.Diagnostics.ProcessStartInfo]::new()
$info.FileName = (Resolve-Path $ExePath).Path
$info.WorkingDirectory = $root
$info.Arguments = '--mcp-stdio --no-validation --width 960 --height 640 --gi off --day-cycle off'
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardInput = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $info
[void]$process.Start()
$stderr = $process.StandardError.ReadToEndAsync()
$script:rpcId = 0

function Rpc([string]$method, [hashtable]$parameters) {
    $script:rpcId++
    $message = @{ jsonrpc = '2.0'; id = $script:rpcId; method = $method; params = $parameters }
    $process.StandardInput.WriteLine(($message | ConvertTo-Json -Depth 20 -Compress))
    $process.StandardInput.Flush()
    $pending = $process.StandardOutput.ReadLineAsync()
    if (-not $pending.Wait(60000)) { throw "MCP timeout: $method" }
    $response = $pending.Result | ConvertFrom-Json
    if ($null -eq $response -or $response.error -or $response.result.isError) {
        throw "MCP failure: $($pending.Result)"
    }
    return $response.result
}

try {
    $null = Rpc 'initialize' @{
        protocolVersion = '2025-11-25'
        capabilities    = @{}
        clientInfo      = @{ name = 'neon-city-import'; version = '1' }
    }
    $process.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized"}')
    $process.StandardInput.Flush()

    foreach ($source in Get-ChildItem (Join-Path $root 'art/neon_city/exports') -Filter '*.glb' | Sort-Object Name) {
        $expected = Join-Path $assetDestination "$($source.BaseName)/$($source.BaseName).vcrasset.json"
        if (Test-Path -LiteralPath $expected) {
            $importedSource = Join-Path $assetDestination "$($source.BaseName)/source/$($source.Name)"
            if ((Test-Path -LiteralPath $importedSource) -and
                (Get-FileHash -LiteralPath $source.FullName).Hash -eq (Get-FileHash -LiteralPath $importedSource).Hash) {
                Write-Output "Exists: $($source.BaseName)"
                continue
            }
            $oldPackage = [IO.Path]::GetFullPath((Join-Path $assetDestination $source.BaseName))
            $archiveRoot = [IO.Path]::GetFullPath((Join-Path $root 'build/windows-release/neon-import-history'))
            $archive = [IO.Path]::GetFullPath((Join-Path $archiveRoot ($source.BaseName + '-' + [DateTime]::UtcNow.Ticks)))
            if (-not $oldPackage.StartsWith($assetDestination + [IO.Path]::DirectorySeparatorChar) -or
                -not $archive.StartsWith($archiveRoot + [IO.Path]::DirectorySeparatorChar)) {
                throw 'Import archive escaped workspace'
            }
            New-Item -ItemType Directory -Force $archiveRoot | Out-Null
            Move-Item -LiteralPath $oldPackage -Destination $archive
        }

        $null = Rpc 'tools/call' @{
            name      = 'editor.assets.import'
            arguments = @{ source_path = $source.FullName; destination_directory = 'neon_city' }
        }

        $deadline = [DateTime]::UtcNow.AddSeconds(90)
        do {
            Start-Sleep -Milliseconds 150
            $state = Rpc 'tools/call' @{ name = 'editor.get_state'; arguments = @{} }
            if (Test-Path -LiteralPath $expected) { break }
            if (-not $state.structuredContent.asset_import_pending -and
                $state.structuredContent.asset_import_status -match '[1-9][0-9]* failed') {
                throw "Import failed for $($source.Name): $($state.structuredContent.asset_import_status)"
            }
            if ([DateTime]::UtcNow -gt $deadline) {
                throw "Import did not produce $expected"
            }
        } while ($true)
        Write-Output "Imported: $($source.BaseName)"
    }

    $null = Rpc 'tools/call' @{ name = 'engine.quit'; arguments = @{} }
    if (-not $process.WaitForExit(15000)) { throw 'Editor shutdown timed out' }
    if ($process.ExitCode -ne 0) { throw "Editor exit $($process.ExitCode)" }
} finally {
    if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    $stderr.Result | Set-Content (Join-Path $root 'build/windows-release/neon-import.log')
    $process.Dispose()
}
