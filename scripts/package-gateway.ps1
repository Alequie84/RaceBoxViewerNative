param([string]$Version = '2.0.0.020')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$gatewaySource = Join-Path $root 'gateway'
$stage = Join-Path $root 'package\RaceBoxCrewChiefGateway'
$out = Join-Path $root 'out'
$archive = Join-Path $out "RaceBoxCrewChiefGateway-$Version.zip"
$resolvedRoot = [IO.Path]::GetFullPath($root)
$resolvedStage = [IO.Path]::GetFullPath($stage)
if (-not $resolvedStage.StartsWith($resolvedRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Gateway package staging path escaped the repository.'
}
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage,$out | Out-Null
$files = @(
    'gateway.py', 'companion_store.py', 'intelligence_router.py',
    'telemetry_analytics.py', 'benchmark_lanes.py', 'compare_single_agent.py',
    'racebox-vehicle-dynamics.SKILL.md', 'racebox-agent-instructions.md',
    'run_gateway.py', 'setup_common.py', 'setup-gateway.ps1', 'setup-gateway.sh',
    'test_gateway.py', 'test_setup_common.py', '.env.example'
)
foreach ($file in $files) {
    Copy-Item -LiteralPath (Join-Path $gatewaySource $file) -Destination (Join-Path $stage $file)
}
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $stage 'LICENSE')
Copy-Item -LiteralPath (Join-Path $root 'THIRD-PARTY-NOTICES.md') -Destination (Join-Path $stage 'THIRD-PARTY-NOTICES.md')
Copy-Item -LiteralPath (Join-Path $root 'docs\public-connection-guide.md') -Destination (Join-Path $stage 'README.md')
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -CompressionLevel Optimal
Write-Output $archive
