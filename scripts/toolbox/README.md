# RaceBox Developer Toolbox

These PowerShell scripts make the native build repeatable. Run them from any directory; each script resolves project paths itself. The native demo always receives the golden VBO, RaceBox CSV, and Sanwa CSV, so radio controls and the driver-analysis layer exercise the same merged fixture as automated validation. The React application is frozen and its scripts are retained only for history; do not run them during current native development.

## Common Commands

```powershell
# Fast, read-only health summary
powershell -ExecutionPolicy Bypass -File C:\CodexProjects\RaceBoxViewerNative\scripts\toolbox\Get-ProjectState.ps1

# Build and test the native application
powershell -ExecutionPolicy Bypass -File C:\CodexProjects\RaceBoxViewerNative\scripts\toolbox\Build-Test-Native.ps1

# Build, test, install, and create the portable ZIP
powershell -ExecutionPolicy Bypass -File C:\CodexProjects\RaceBoxViewerNative\scripts\toolbox\Build-Test-Native.ps1 -Package

# Advance the release suffix (.010 -> .011) before the next user-facing release
powershell -ExecutionPolicy Bypass -File C:\CodexProjects\RaceBoxViewerNative\scripts\bump-version.ps1

# Launch the native application with the merged golden fixture
powershell -ExecutionPolicy Bypass -File C:\CodexProjects\RaceBoxViewerNative\scripts\toolbox\Start-NativeDemo.ps1
```

## Inventory

| Script | Purpose |
| --- | --- |
| `Project-Paths.ps1` | Shared paths, tool discovery, and process helpers. |
| `Get-ProjectState.ps1` | Read-only status for source, artifacts, map hashes, server port, and running demo. |
| `Build-Test-Native.ps1` | Configure, compile, and run all seven CTest targets with the Visual Studio developer environment. Add `-Package` for install and the portable ZIP. |
| `Test-BrowserReference.ps1` | Historical frozen-browser validation; do not run unless the user explicitly reopens browser development. |
| `Start-NativeDemo.ps1` | Launch the C++ UI with the golden VBO, RaceBox CSV, and Sanwa CSV. It quotes paths as one argument line so filenames containing spaces and parentheses work. |
| `Start-BrowserReference.ps1` | Historical frozen-browser launcher; not part of current development. |
| `Verify-Project.ps1` | Legacy combined native/browser verification. Use `Build-Test-Native.ps1` for current work. |
| `Codex-SessionStart.ps1` | Lightweight SessionStart hook that injects the handoff and demo-review checklist into a new Codex task. |

The root `VERSION` file is the single release-version source. It uses a three-digit release suffix (`0.1.0.010`, `0.1.0.011`, ...); `scripts\bump-version.ps1` advances it, and packaging retains earlier numbered ZIPs.

Existing specialized scripts remain authoritative:

- `scripts/package.ps1`: install and create the racer-facing portable ZIP.
- `scripts/render_map_calibration.ps1`: render map-calibration diagnostics.
- `scripts/render_google_trace_overlay.ps1`: regenerate the triangulated clean-aerial reference/trace diagnostic without changing the runtime background.
- `scripts/soak.ps1`: repeat-playback soak harness, including WARP mode.

After editing C++ code, always build/test first and finish by running `Start-NativeDemo.ps1` so the window left open for review is the latest successful build rather than a stale executable.

## What native validation covers

CTest currently reports seven tests:

- `racebox_core_golden`: parser/alignment golden values, three Google anchor reprojections and equal map-axis scale, archive round trips, and corruption rejection;
- `racebox_driver_analysis`: sustained event detectors, thresholds, confidence gates, severity bands, positive feedback, corner metrics, analysis-only GPS translation, and the raw-lap-7/R6 regression;
- `racebox_insight_evidence_addon`: isolated v2 retained-gain, compensation, downstream-payback, repeatability, guardrail, and golden-session gates; the app does not consume this module yet;
- `racebox_dx11_warp`: Microsoft software-renderer availability;
- `racebox_one_million_samples`: bounded memory on a large synthetic session;
- `racebox_plot_order`: stable telemetry IDs, stored ordering, and relative-time visibility behavior.
- `racebox_ui_preferences`: dark defaults, workspace/density/panel persistence, malformed-file recovery, atomic saving, and one-time layout-v3 backup.

After a successful UI-affecting change, manually review the launched native demo in Compare mode: Reference/Compare A/Compare B/Playback distance synchronization, Insights/Rules/Dev Notes, Events/Sectors, graph dragging and annotation attachment, plus triangulated-map drag/lock/reset across a resize.

## Startup Hook

The repository contains `.codex/config.toml` and `.codex/hooks.json`. Hooks are enabled for trusted projects. Codex may ask for one security review the first time this exact project hook is run; approve it through `/hooks` after checking that it points to `Codex-SessionStart.ps1` in this repository.

The hook does not build, modify source, or launch an application. It only reads project status and tells the new task to inspect the current native demo before broad changes.
