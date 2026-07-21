param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$Package
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Project-Paths.ps1')

$configurePreset = if ($Configuration -eq 'Release') { 'windows-release' } else { 'windows-debug' }
$buildPreset = if ($Configuration -eq 'Release') { 'release' } else { 'debug' }

Push-Location $RaceBoxNativeRoot
try {
    Invoke-RaceBoxDeveloperCommand "cmake --preset $configurePreset"
    Invoke-RaceBoxDeveloperCommand "cmake --build --preset $buildPreset --parallel"
    if ($Configuration -eq 'Release') {
        Invoke-RaceBoxDeveloperCommand 'ctest --preset release'
    } else {
        $debugDirectory = Join-Path $RaceBoxNativeRoot 'build\debug'
        Invoke-RaceBoxDeveloperCommand ('ctest --test-dir ' + (ConvertTo-RaceBoxArgument $debugDirectory) + ' --output-on-failure')
    }

    if ($Package) {
        & (Join-Path $RaceBoxNativeRoot 'scripts\package.ps1') -Configuration $Configuration
        if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
    }
} finally {
    Pop-Location
}
