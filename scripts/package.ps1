param([ValidateSet('Release','Debug')][string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$versionPath = Join-Path $root 'VERSION'
$releaseVersion = (Get-Content -LiteralPath $versionPath -Raw).Trim()
if ($releaseVersion -notmatch '^\d+\.\d+\.\d+\.\d{3}$') {
    throw "VERSION must use major.minor.patch.build with a three-digit build number: $releaseVersion"
}
$preset = if ($Configuration -eq 'Release') { 'windows-distribution' } else { 'windows-debug' }
$buildPreset = if ($Configuration -eq 'Release') { 'distribution' } else { 'debug' }
$packageName = if ($Configuration -eq 'Release') { "RaceBoxTelemetryViewer-$releaseVersion-win64" } else { "RaceBoxTelemetryViewer-$releaseVersion-debug-win64" }
$packageRoot = Join-Path $root 'package'
$installRoot = Join-Path $packageRoot $(if ($Configuration -eq 'Release') {
    'RaceBoxTelemetryViewer'
} else {
    'RaceBoxTelemetryViewerDebug'
})
$outRoot = Join-Path $root 'out'
$zipPath = Join-Path $outRoot ($packageName + '.zip')
$developerShellCandidates = @()
$vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vsWhere) {
    $installationPath = (& $vsWhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1)
    if ($installationPath) {
        $developerShellCandidates += Join-Path $installationPath 'Common7\Tools\VsDevCmd.bat'
    }
}
$developerShellCandidates += @(
    'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
)
$developerShell = $developerShellCandidates |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1

if (-not $developerShell) {
    throw 'A Visual Studio 2022 installation with the C++ build tools was not found.'
}
$resolvedRoot = [IO.Path]::GetFullPath($root)
$resolvedPackage = [IO.Path]::GetFullPath($packageRoot)
$resolvedInstall = [IO.Path]::GetFullPath($installRoot)
$resolvedOut = [IO.Path]::GetFullPath($outRoot)
if (-not $resolvedPackage.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Package path escaped the project root.' }
if (-not $resolvedInstall.StartsWith($resolvedPackage + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Install path escaped the package root.'
}
if (-not $resolvedOut.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Output path escaped the project root.' }

$buildDirectory = Join-Path $root $(if ($Configuration -eq 'Release') {
    'build\distribution'
} else {
    'build\debug'
})
function Invoke-DeveloperCommand([string]$Command) {
    $line = 'call "' + $developerShell + '" -arch=x64 >nul && ' + $Command
    cmd.exe /d /c $line
    if ($LASTEXITCODE -ne 0) { throw "Command failed with exit code $LASTEXITCODE`: $Command" }
}

function New-PortableZipWithRetry(
    [string]$SourceDirectory,
    [string]$DestinationPath
) {
    $maximumAttempts = 5
    for ($attempt = 1; $attempt -le $maximumAttempts; $attempt++) {
        try {
            if (Test-Path -LiteralPath $DestinationPath) {
                Remove-Item -LiteralPath $DestinationPath -Force
            }
            Compress-Archive -Path (Join-Path $SourceDirectory '*') `
                -DestinationPath $DestinationPath -CompressionLevel Optimal `
                -ErrorAction Stop
            return
        } catch {
            if ($attempt -eq $maximumAttempts) { throw }
            Start-Sleep -Milliseconds (250 * $attempt)
        }
    }
}

Push-Location $root
try {
    Invoke-DeveloperCommand ('cmake --preset ' + $preset)
    Invoke-DeveloperCommand ('cmake --build --preset ' + $buildPreset + ' --parallel')
    Invoke-DeveloperCommand ('ctest --preset ' + $buildPreset)
    if (Test-Path -LiteralPath $resolvedInstall) {
        Remove-Item -LiteralPath $resolvedInstall -Recurse -Force
    }
    Invoke-DeveloperCommand ('cmake --install "' + $buildDirectory + '" --config ' + $Configuration)

    $requiredPackageFiles = @(
        'RaceBoxTelemetryViewer.exe',
        'pdfium.dll',
        'racebox_cli.exe',
        'VERSION',
        'README.md',
        'HANDOFF.md',
        'LICENSE',
        'THIRD-PARTY-NOTICES.md',
        'Start RaceBox Demo.cmd',
        'assets\rrr-map.png',
        'assets\rrr-map-original.png',
        'docs\race-day-crew-chief.md',
        'docs\public-connection-guide.md',
        'docs\codex-review.md',
        'docs\v2-architecture.md',
        'demo\session.vbo',
        'demo\session.csv',
        'demo\sanwa.csv',
        'licenses\imgui-LICENSE.txt',
        'licenses\implot-LICENSE.txt',
        'licenses\miniz-LICENSE.txt',
        'licenses\nlohmann-json-LICENSE.MIT',
        'licenses\pdfium-LICENSE.txt',
        'licenses\qrcodegen-LICENSE-AND-README.md'
    )
    foreach ($relativePath in $requiredPackageFiles) {
        $requiredPath = Join-Path $resolvedInstall $relativePath
        if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
            throw "Portable package is missing required file: $relativePath"
        }
    }
    $installedVersion = (Get-Content -LiteralPath (Join-Path $resolvedInstall 'VERSION') -Raw).Trim()
    if ($installedVersion -ne $releaseVersion) {
        throw "Packaged VERSION mismatch: expected $releaseVersion, found $installedVersion"
    }
    if ($Configuration -eq 'Release') {
        $viewerBytes = [IO.File]::ReadAllBytes((Join-Path $resolvedInstall 'RaceBoxTelemetryViewer.exe'))
        $viewerAscii = [Text.Encoding]::ASCII.GetString($viewerBytes)
        $viewerUnicode = [Text.Encoding]::Unicode.GetString($viewerBytes)
        foreach ($forbidden in @(
            'racebox-codex-review',
            'active-review.png',
            'Refresh Codex Review',
            'ANNOTATE',
            'racebox-annotation-review',
            'Annotation mode'
        )) {
            if ($viewerAscii.Contains($forbidden) -or $viewerUnicode.Contains($forbidden)) {
                throw "Distribution contains development-only review text: $forbidden"
            }
        }
    }

    New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
    New-PortableZipWithRetry -SourceDirectory $resolvedInstall `
        -DestinationPath $zipPath
    $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath
    Write-Host "Portable package: $zipPath"
    Write-Host "SHA256: $($hash.Hash)"
} finally {
    Pop-Location
}
