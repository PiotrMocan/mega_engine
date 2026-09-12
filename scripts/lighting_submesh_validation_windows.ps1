param([Parameter(Mandatory=$true)][string]$ExePath)
$ErrorActionPreference='Stop'
# This intentionally tiny model has 32 submeshes. Staging 32 separate 48-byte
# triangle payloads requires all alignment gaps, in addition to payload bytes.
$result=& (Join-Path $PSScriptRoot 'mcp_smoke.ps1') -ExePath $ExePath -Validation -ExtraArguments '--scene-count 1 --scene-walls off --scene-asset validation/lighting-submeshes/triangles.vcrasset.json --day-cycle off --gi voxel' | Where-Object {$_ -is [string] -and -not [string]::IsNullOrWhiteSpace($_)}
$parsed=$result | ConvertFrom-Json
if($parsed.visibility.submitted -ne 32) {throw 'The 32 fixture submeshes were not published.'}
$result
