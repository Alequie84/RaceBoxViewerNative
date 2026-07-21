$ErrorActionPreference = 'Stop'

# Consume the hook payload even though this hook does not need to mutate it.
$null = [Console]::In.ReadToEnd()
$stateJson = & (Join-Path $PSScriptRoot 'Get-ProjectState.ps1') -Json 2>$null | Out-String
$state = $null
try { $state = $stateJson | ConvertFrom-Json } catch { }

$status = if ($state) {
    "Native build present: $($state.native_executable_present); native demo running: $($state.native_demo_running); browser port 5173: $($state.browser_port_5173_listening)."
} else {
    'Run scripts/toolbox/Get-ProjectState.ps1 for current build and server status.'
}

$context = @"
RaceBox Viewer Native startup context:
- Read C:\CodexProjects\RaceBoxViewerNative\HANDOFF.md and AGENTS.md before editing.
- Use C:\CodexProjects\RaceBoxViewerNative\scripts\toolbox for repeatable build, test, launch, and health checks.
- Before broad code or layout work, launch the current native demo with Start-NativeDemo.ps1 and inspect it using the golden fixture.
- When parity matters, launch C:\CodexProjects\New project 2 with Start-BrowserReference.ps1 and compare the same fixture side by side.
- Look for correctness problems, avoidable per-frame work or memory copies, dead code, crowded/clipped layout, weak control discoverability, and missing browser-reference behavior.
- Report prioritized findings before a broad refactor. Preserve the golden timing, Sanwa correlation, lap, map, and GPS invariants in HANDOFF.md.
- Current status: $status
"@

$payload = [ordered]@{
    hookSpecificOutput = [ordered]@{
        hookEventName = 'SessionStart'
        additionalContext = $context.Trim()
    }
}

[PSCustomObject]$payload | ConvertTo-Json -Depth 5 -Compress
