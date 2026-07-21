param([ValidateSet('Release','Debug')][string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$versionPath = Join-Path $root 'VERSION'
$releaseVersion = (Get-Content -LiteralPath $versionPath -Raw).Trim()
if ($releaseVersion -notmatch '^\d+\.\d+\.\d+\.\d{3}$') {
    throw "VERSION must use major.minor.patch.build with a three-digit build number: $releaseVersion"
}
$preset = if ($Configuration -eq 'Release') { 'windows-release' } else { 'windows-debug' }
$buildPreset = if ($Configuration -eq 'Release') { 'release' } else { 'debug' }
$packageName = if ($Configuration -eq 'Release') { "RaceBoxViewerNative-$releaseVersion-win64" } else { "RaceBoxViewerNative-$releaseVersion-debug-win64" }
$packageRoot = Join-Path $root 'package'
$installRoot = Join-Path $packageRoot 'RaceBoxViewer'
$outRoot = Join-Path $root 'out'
$zipPath = Join-Path $outRoot ($packageName + '.zip')
$developerShell = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'

if (-not (Test-Path -LiteralPath $developerShell)) { throw 'Visual Studio 2022 Community developer shell was not found.' }
$resolvedRoot = [IO.Path]::GetFullPath($root)
$resolvedPackage = [IO.Path]::GetFullPath($packageRoot)
$resolvedOut = [IO.Path]::GetFullPath($outRoot)
if (-not $resolvedPackage.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Package path escaped the project root.' }
if (-not $resolvedOut.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Output path escaped the project root.' }

$buildDirectory = Join-Path $root ('build\' + $Configuration.ToLowerInvariant())
function Invoke-DeveloperCommand([string]$Command) {
    $line = 'call "' + $developerShell + '" -arch=x64 >nul && ' + $Command
    cmd.exe /d /c $line
    if ($LASTEXITCODE -ne 0) { throw "Command failed with exit code $LASTEXITCODE`: $Command" }
}
Push-Location $root
try {
    Invoke-DeveloperCommand ('cmake --preset ' + $preset)
    Invoke-DeveloperCommand ('cmake --build --preset ' + $buildPreset + ' --parallel')
    Invoke-DeveloperCommand ('ctest --preset ' + $buildPreset)
    Invoke-DeveloperCommand ('cmake --install "' + $buildDirectory + '" --config ' + $Configuration)
    New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Compress-Archive -Path (Join-Path $installRoot '*') -DestinationPath $zipPath -CompressionLevel Optimal
    $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath
    Write-Host "Portable package: $zipPath"
    Write-Host "SHA256: $($hash.Hash)"
} finally {
    Pop-Location
}
