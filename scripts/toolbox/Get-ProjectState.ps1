param([switch]$Json)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Project-Paths.ps1')

function Get-OptionalHash([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
}

$map = Join-Path $RaceBoxNativeRoot 'assets\rrr-map.png'
$browserMap = Join-Path $RaceBoxBrowserRoot 'public\assets\rrr-map.png'
$version = (Get-Content -LiteralPath (Join-Path $RaceBoxNativeRoot 'VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+\.\d{3}$') { throw "Invalid release VERSION: $version" }
$zip = Join-Path $RaceBoxNativeRoot "out\RaceBoxTelemetryViewer-$version-win64.zip"
$listener = Get-NetTCPConnection -LocalPort 5173 -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1
$nativeProcess = Get-Process -Name RaceBoxTelemetryViewer -ErrorAction SilentlyContinue | Select-Object -First 1

$state = [ordered]@{
    native_root = $RaceBoxNativeRoot
    release_version = $version
    browser_root = $RaceBoxBrowserRoot
    handoff_present = Test-Path -LiteralPath (Join-Path $RaceBoxNativeRoot 'HANDOFF.md')
    agent_guide_present = Test-Path -LiteralPath (Join-Path $RaceBoxNativeRoot 'AGENTS.md')
    native_executable_present = Test-Path -LiteralPath $RaceBoxExecutable
    golden_files_present = (Test-Path -LiteralPath $RaceBoxGoldenVbo) -and (Test-Path -LiteralPath $RaceBoxGoldenCsv) -and (Test-Path -LiteralPath $RaceBoxGoldenSanwa)
    native_demo_running = [bool]$nativeProcess
    browser_port_5173_listening = [bool]$listener
    native_map_sha256 = Get-OptionalHash $map
    browser_map_sha256 = Get-OptionalHash $browserMap
    portable_zip_sha256 = Get-OptionalHash $zip
    portable_zip_path = $zip
}

if ($Json) {
    [PSCustomObject]$state | ConvertTo-Json -Depth 3 -Compress
    exit 0
}

Write-Host 'RaceBox project state'
Write-Host "  Native source:   $($state.native_root)"
Write-Host "  Release version: $($state.release_version)"
Write-Host "  Browser source:  $($state.browser_root)"
Write-Host "  Handoff/guide:   $($state.handoff_present) / $($state.agent_guide_present)"
Write-Host "  Native build:    $($state.native_executable_present)"
Write-Host "  Golden fixture:  $($state.golden_files_present)"
Write-Host "  Native running:  $($state.native_demo_running)"
Write-Host "  Vite on 5173:    $($state.browser_port_5173_listening)"
Write-Host "  Native map SHA:  $($state.native_map_sha256)"
Write-Host "  Browser map SHA: $($state.browser_map_sha256)"
Write-Host "  Portable ZIP:    $($state.portable_zip_sha256)"
Write-Host "  ZIP path:        $($state.portable_zip_path)"
