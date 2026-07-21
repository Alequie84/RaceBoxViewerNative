$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Project-Paths.ps1')

Assert-RaceBoxPath -LiteralPath $RaceBoxBrowserRoot -Description 'Browser reference project'
$node = Get-RaceBoxNode
$testFiles = @(Get-ChildItem -LiteralPath (Join-Path $RaceBoxBrowserRoot 'tests') -Filter '*.test.js' | ForEach-Object FullName)
$vite = Join-Path $RaceBoxBrowserRoot 'node_modules\vite\bin\vite.js'

if ($testFiles.Count -eq 0) { throw 'No browser reference tests were found.' }
Assert-RaceBoxPath -LiteralPath $vite -Description 'Local Vite executable'

Push-Location $RaceBoxBrowserRoot
try {
    & $node --test @testFiles
    if ($LASTEXITCODE -ne 0) { throw 'Browser reference tests failed.' }

    & $node $vite build
    if ($LASTEXITCODE -ne 0) { throw 'Browser reference production build failed.' }
} finally {
    Pop-Location
}
