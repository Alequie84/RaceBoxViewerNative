$ErrorActionPreference = 'Stop'

$script:RaceBoxNativeRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$script:RaceBoxBrowserRoot = 'C:\CodexProjects\New project 2'
$script:RaceBoxReleaseDirectory = Join-Path $script:RaceBoxNativeRoot 'build\release'
$script:RaceBoxExecutable = Join-Path $script:RaceBoxReleaseDirectory 'RaceBoxViewer.exe'
$script:RaceBoxCliExecutable = Join-Path $script:RaceBoxReleaseDirectory 'racebox_cli.exe'
$script:RaceBoxGoldenVbo = Join-Path $script:RaceBoxNativeRoot 'golden\session.vbo'
$script:RaceBoxGoldenCsv = Join-Path $script:RaceBoxNativeRoot 'golden\session.csv'
$script:RaceBoxGoldenSanwa = Join-Path $script:RaceBoxNativeRoot 'golden\sanwa.csv'
$script:RaceBoxDeveloperShell = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat'

function Assert-RaceBoxPath {
    param(
        [Parameter(Mandatory = $true)][string]$LiteralPath,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if (-not (Test-Path -LiteralPath $LiteralPath)) {
        throw "$Description was not found: $LiteralPath"
    }
}

function Get-RaceBoxNode {
    $command = Get-Command node.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $runtimeRoot = Join-Path $env:LOCALAPPDATA 'OpenAI\Codex\runtimes\cua_node'
    $candidate = Get-ChildItem -LiteralPath $runtimeRoot -Filter node.exe -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($candidate) { return $candidate.FullName }

    throw 'Node.js was not found on PATH or in the Codex bundled runtime.'
}

function Get-RaceBoxDeveloperShell {
    if (Test-Path -LiteralPath $script:RaceBoxDeveloperShell) {
        return $script:RaceBoxDeveloperShell
    }

    $candidate = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022' -Filter VsDevCmd.bat -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($candidate) { return $candidate.FullName }

    throw 'A Visual Studio 2022 developer shell was not found.'
}

function Invoke-RaceBoxDeveloperCommand {
    param([Parameter(Mandatory = $true)][string]$Command)

    $developerShell = Get-RaceBoxDeveloperShell
    $line = 'call "' + $developerShell + '" -arch=x64 >nul && ' + $Command
    & cmd.exe /d /c $line
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE`: $Command"
    }
}

function ConvertTo-RaceBoxArgument {
    param([Parameter(Mandatory = $true)][string]$Value)
    return '"' + $Value.Replace('"', '\"') + '"'
}
