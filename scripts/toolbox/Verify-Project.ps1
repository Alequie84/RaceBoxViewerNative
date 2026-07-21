param([switch]$Package)

$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'Build-Test-Native.ps1') -Package:$Package
& (Join-Path $PSScriptRoot 'Test-BrowserReference.ps1')
& (Join-Path $PSScriptRoot 'Get-ProjectState.ps1')
