param([int]$SetBuild = -1)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$versionPath = Join-Path $root 'VERSION'
if (-not (Test-Path -LiteralPath $versionPath)) { throw "VERSION was not found: $versionPath" }

$current = (Get-Content -LiteralPath $versionPath -Raw).Trim()
if ($current -notmatch '^(\d+)\.(\d+)\.(\d+)\.(\d{3})$') {
    throw "VERSION must use major.minor.patch.build with a three-digit build number: $current"
}

$major = [int]$Matches[1]
$minor = [int]$Matches[2]
$patch = [int]$Matches[3]
$currentBuild = [int]$Matches[4]
$nextBuild = if ($SetBuild -ge 0) { $SetBuild } else { $currentBuild + 1 }
if ($nextBuild -lt 0 -or $nextBuild -gt 999) { throw 'Release build number must be between 000 and 999.' }
if ($nextBuild -lt $currentBuild) { throw 'Release build number cannot move backwards.' }

$next = '{0}.{1}.{2}.{3:D3}' -f $major, $minor, $patch, $nextBuild
[IO.File]::WriteAllText($versionPath, $next + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
Write-Host "Release version: $current -> $next"
