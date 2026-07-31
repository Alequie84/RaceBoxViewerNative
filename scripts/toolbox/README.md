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

# Advance the v2 release suffix (.001 -> .002) before the next user-facing release
powershell -ExecutionPolicy Bypass -File C:\CodexProjects\RaceBoxViewerNative\scripts\bump-version.ps1

# Launch the native application with the merged golden fixture
powershell -ExecutionPolicy Bypass -File C:\CodexProjects\RaceBoxViewerNative\scripts\toolbox\Start-NativeDemo.ps1
```

## Inventory

| Script | Purpose |
| --- | --- |
| `Project-Paths.ps1` | Shared paths, tool discovery, and process helpers. |
| `Get-ProjectState.ps1` | Read-only status for source, artifacts, map hashes, server port, and running demo. |
| `Build-Test-Native.ps1` | Configure, compile, and run all 14 Windows CTest targets with the Visual Studio developer environment. Add `-Package` for install and the portable ZIP. |
| `Test-BrowserReference.ps1` | Historical frozen-browser validation; do not run unless the user explicitly reopens browser development. |
| `Start-NativeDemo.ps1` | Launch the C++ UI with the golden VBO, RaceBox CSV, and Sanwa CSV. It quotes paths as one argument line so filenames containing spaces and parentheses work. |
| `Start-BrowserReference.ps1` | Historical frozen-browser launcher; not part of current development. |
| `Verify-Project.ps1` | Legacy combined native/browser verification. Use `Build-Test-Native.ps1` for current work. |
| `Codex-SessionStart.ps1` | Lightweight SessionStart hook that injects the handoff and demo-review checklist into a new Codex task. |

The root `VERSION` file is the single release-version source. Version 2 starts at `2.0.0.001` and keeps a three-digit release suffix (`2.0.0.002`, `2.0.0.003`, ...); `scripts\bump-version.ps1` advances it, and packaging retains earlier numbered ZIPs.

Interactive **File > Add recording to Race Day** enters the Race Day workspace,
loads only the newly selected recording, asks whether it was Practice,
Qualifying, or Race, and adds it with the detected UTC time and local event
date. Session then exposes an always-visible Race Day run selector; direct
command-line/demo files are clearly labelled as not linked to Race Day. Each
selection is self-contained and cannot reuse a file from the previously opened
run. The toolbox passes
the golden files on the command line, which remains non-interactive so startup
and automated review are never blocked by the classification prompt.

Existing specialized scripts remain authoritative:

- `scripts/package.ps1`: install and create the racer-facing portable ZIP.
- `scripts/render_map_calibration.ps1`: render map-calibration diagnostics.
- `scripts/render_google_trace_overlay.ps1`: regenerate the triangulated clean-aerial reference/trace diagnostic without changing the runtime background.
- `scripts/soak.ps1`: repeat-playback soak harness, including WARP mode.

After editing C++ code, always build/test first and finish by running `Start-NativeDemo.ps1` so the window left open for review is the latest successful build rather than a stale executable.

## What native validation covers

Windows CTest currently reports 14 tests:

- `racebox_core_golden`: parser/alignment golden values, three Google anchor reprojections and equal map-axis scale, archive round trips, and corruption rejection;
- `racebox_driver_analysis`: sustained event detectors, thresholds, confidence gates, severity bands, positive feedback, corner metrics, analysis-only GPS translation, and the raw-lap-7/R6 regression;
- `racebox_insight_evidence_addon`: v2 retained-gain, compensation, downstream-payback, repeatability, guardrail, and live driver-analysis integration gates;
- `racebox_dx11_warp`: Microsoft software-renderer availability;
- `racebox_one_million_samples`: bounded memory on a large synthetic session;
- `racebox_plot_order`: stable telemetry IDs, stored ordering, and relative-time visibility behavior;
- `racebox_plot_decimation`: bounded multi-channel peak and endpoint preservation;
- `racebox_application_state`: portable workspaces, lap roles, playback, jobs, and notifications;
- `racebox_source_identity`: deterministic exact/probable/ambiguous/missing source repair;
- `racebox_ui_preferences`: guided workspaces, optional Compare B, text size, layout lock, malformed-file recovery, atomic saving, and one-time layout-v4 backup;
- `racebox_import_discovery`: bounded saved-folder scans, strict RaceBox/Sanwa CSV header recognition, unrelated-file rejection, and Sanwa-only removable-drive filtering;
- `racebox_imu_analysis`: first-stationary-block zero calibration, IMU availability, mounting-independent yaw calibration, sustained low-load/landing detection, high-load and rapid-rotation gates, and empty legacy channels.
- `racebox_crew_chief`: bounded private-gateway report and evidence contracts.
- `racebox_race_day`: event/run persistence, pre-run checklist and tire history, golden multi-source loading, canonical analytics CSV, setup-analytics-v3 straight-speed attribution, brake-response indicators, overdriving/tire-scrub risk, chassis-roll amount, and roll-rate math.

The `portable-core-release` configure/build/test preset runs seven tests without
Win32, DirectX, WinHTTP, native archive storage, or the native UI. Use it to
guard the future macOS boundary; it does not build a Mac application yet.

After a successful UI-affecting change, manually review the launched native demo in Compare mode: Reference/Compare A/Compare B/Playback distance synchronization, Insights/Rules/Dev Notes, Events/Sectors, graph dragging and annotation attachment, plus triangulated-map drag/lock/reset across a resize.

## Startup Hook

The repository contains `.codex/config.toml` and `.codex/hooks.json`. Hooks are enabled for trusted projects. Codex may ask for one security review the first time this exact project hook is run; approve it through `/hooks` after checking that it points to `Codex-SessionStart.ps1` in this repository.

The hook does not build, modify source, or launch an application. It only reads project status and tells the new task to inspect the current native demo before broad changes.
