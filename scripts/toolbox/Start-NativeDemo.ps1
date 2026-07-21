param(
    [string]$Vbo,
    [string]$RaceBoxCsv,
    [string]$SanwaCsv,
    [switch]$BuildIfMissing,
    [switch]$Wait
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Project-Paths.ps1')

if (-not $Vbo) { $Vbo = $RaceBoxGoldenVbo }
if (-not $RaceBoxCsv) { $RaceBoxCsv = $RaceBoxGoldenCsv }
if (-not $SanwaCsv) { $SanwaCsv = $RaceBoxGoldenSanwa }

if (-not (Test-Path -LiteralPath $RaceBoxExecutable)) {
    if (-not $BuildIfMissing) {
        throw "Native executable was not found. Run Build-Test-Native.ps1 or add -BuildIfMissing: $RaceBoxExecutable"
    }
    & (Join-Path $PSScriptRoot 'Build-Test-Native.ps1')
}

Assert-RaceBoxPath -LiteralPath $Vbo -Description 'VBO fixture'
Assert-RaceBoxPath -LiteralPath $RaceBoxCsv -Description 'RaceBox CSV fixture'
Assert-RaceBoxPath -LiteralPath $SanwaCsv -Description 'Sanwa CSV fixture'

$quotedArguments = @($Vbo, $RaceBoxCsv, $SanwaCsv) |
    ForEach-Object { ConvertTo-RaceBoxArgument ([IO.Path]::GetFullPath($_)) }
$argumentLine = [string]::Join(' ', $quotedArguments)

$process = Start-Process -FilePath $RaceBoxExecutable -WorkingDirectory $RaceBoxNativeRoot -ArgumentList $argumentLine -PassThru
Write-Host "Native demo PID $($process.Id)"
Write-Host "Fixture: $Vbo | $RaceBoxCsv | $SanwaCsv"
if ($Wait) { $process.WaitForExit() }
