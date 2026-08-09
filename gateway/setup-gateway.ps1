[CmdletBinding()]
param(
    [string]$TailscaleIp,
    [string]$Model,
    [switch]$InstallStartup,
    [switch]$DryRun,
    [string]$Rollback,
    [string]$InstallRoot
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$arguments = @((Join-Path $scriptRoot 'setup_common.py'), '--host', 'windows')
if ($TailscaleIp) { $arguments += @('--tailscale-ip', $TailscaleIp) }
if ($Model) { $arguments += @('--model', $Model) }
if ($InstallRoot) { $arguments += @('--install-root', $InstallRoot) }
if ($InstallStartup) { $arguments += '--install-startup' }
if ($DryRun) { $arguments += '--dry-run' }
if ($Rollback) { $arguments += @('--rollback', $Rollback) }

& python @arguments
if ($LASTEXITCODE -ne 0) { throw "RaceBox gateway setup failed with exit code $LASTEXITCODE" }
