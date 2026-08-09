# RaceBox Developer Toolbox

These PowerShell scripts make the native build repeatable. Run them from any directory; each script resolves project paths itself. The native demo always receives the golden VBO, RaceBox CSV, and Sanwa CSV, so radio controls and the driver-analysis layer exercise the same merged fixture as automated validation. The React application is frozen and its scripts are retained only for history; do not run them during current native development.

## Common Commands

```powershell
# Fast, read-only health summary
powershell -ExecutionPolicy Bypass -File .\scripts\toolbox\Get-ProjectState.ps1

# Build and test the native application
powershell -ExecutionPolicy Bypass -File .\scripts\toolbox\Build-Test-Native.ps1

# Build, test, install, and create the portable ZIP
powershell -ExecutionPolicy Bypass -File .\scripts\toolbox\Build-Test-Native.ps1 -Package

# Advance the v2 release suffix (.001 -> .002) before the next user-facing release
powershell -ExecutionPolicy Bypass -File .\scripts\bump-version.ps1

# Launch the native application with the merged golden fixture
powershell -ExecutionPolicy Bypass -File .\scripts\toolbox\Start-NativeDemo.ps1
```

## Inventory

| Script | Purpose |
| --- | --- |
| `Project-Paths.ps1` | Shared paths, tool discovery, and process helpers. |
| `Get-ProjectState.ps1` | Read-only status for source, artifacts, map hashes, server port, and running demo. |
| `Build-Test-Native.ps1` | Configure, compile, and run all 17 Windows CTest targets with the Visual Studio developer environment. Add `-Package` for install and the portable ZIP. |
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

The normal executable remembers the exact sources from its latest successful
load and reopens them on a later argument-free start when every file is still
readable and nonempty. Explicit command-line sources take priority, followed by
remembered files and then the complete packaged demo. Fresh preferences default
to light mode. The toolbox launcher deliberately passes the golden trio, so it
continues to override remembered user files during repeatable engineering checks.

Existing specialized scripts remain authoritative:

- `scripts/package.ps1`: install and create the racer-facing portable ZIP.
- `scripts/render_map_calibration.ps1`: render map-calibration diagnostics.
- `scripts/render_google_trace_overlay.ps1`: regenerate the triangulated clean-aerial reference/trace diagnostic without changing the runtime background.
- `scripts/soak.ps1`: repeat-playback soak harness, including WARP mode.

After editing C++ code, always build/test first and finish by running `Start-NativeDemo.ps1` so the window left open for review is the latest successful build rather than a stale executable.

## What native validation covers

Windows CTest currently reports 17 tests:

- `racebox_core_golden`: parser/alignment golden values, synchronized Sanwa/GPS start-clock anchoring without signal-fit drift, stale-clock fallback, explicit empty-Sanwa rejection, three Google anchor reprojections and equal map-axis scale, archive round trips, and corruption rejection;
- `racebox_driver_analysis`: sustained event detectors, thresholds, confidence gates, severity bands, positive feedback, corner metrics, analysis-only GPS translation, and the raw-lap-7/R6 regression;
- `racebox_insight_evidence_addon`: v2 retained-gain, compensation, downstream-payback, repeatability, guardrail, and live driver-analysis integration gates;
- `racebox_dx11_warp`: Microsoft software-renderer availability;
- `racebox_one_million_samples`: bounded memory on a large synthetic session;
- `racebox_plot_order`: the four-plot default, stable telemetry IDs, stored ordering, and relative-time visibility behavior;
- `racebox_plot_decimation`: bounded multi-channel peak and endpoint preservation;
- `racebox_application_state`: portable workspaces, lap roles, playback, jobs, and notifications;
- `racebox_source_identity`: deterministic exact/probable/ambiguous/missing source repair;
- `racebox_ui_preferences`: light fresh defaults, remembered sources, persistent three-panel shell, panel widths/visibility, center view, map ratio, text scale, malformed-file recovery, atomic saving, and UI-preferences v5 migration;
- `racebox_import_discovery`: bounded saved-folder scans, strict RaceBox/Sanwa CSV header recognition, unrelated-file rejection, and Sanwa-only removable-drive filtering;
- `racebox_imu_analysis`: first-stationary-block zero calibration, IMU availability, mounting-independent yaw calibration, sustained low-load/landing detection, high-load and rapid-rotation gates, and empty legacy channels.
- `racebox_crew_chief`: bounded private-gateway report/evidence contracts, v4 setup-image hashing/limits/path privacy, response vision status, selected-lap context preservation, and distance-aligned per-corner control/G-force evidence.
- `racebox_crew_chief_connection`: offline demo, user-owned gateway URL policy, token-free local settings, and Windows Credential Manager storage.
- `racebox_race_day`: v1–v6 event/run migration, day-level conversation de-duplication, car/setup snapshots, pre-run checklist/tire history, atomic source changes, golden loading, canonical analytics CSV, and setup-analytics-v3 dynamics.
- `racebox_setup_library`: schema backup/migration, managed content-addressed PDFs, immutable revision ancestry, duplicate imports, Setup OFF carry-forward, and reconciliation.
- `racebox_pdf_setup`: PDFium rendering/edit/save, AcroForm and flat-field mappings, immutable source preservation, and bounded in-memory PNG encoding.

The `portable-core-release` configure/build/test preset runs seven tests without
Win32, DirectX, WinHTTP, native archive storage, or the native UI. Use it to
guard the future macOS boundary; it does not build a Mac application yet.

After a successful UI-affecting change, manually review the launched native demo: Race Day and Crew Chief from every center view, run-context propagation, Panel/Graph Focus restore, narrow/large-text collapse behavior, light/dark tables and chat surfaces, the 32/68 map divider, the all-separate default graphs, comparison synchronization, and setup-PDF edit/reopen behavior. Development builds may use **Refresh Codex Review**; the distribution package must prove that review controls and contract strings are absent.

## Startup Hook

The repository contains `.codex/config.toml` and `.codex/hooks.json`. Hooks are enabled for trusted projects. Codex may ask for one security review the first time this exact project hook is run; approve it through `/hooks` after checking that it points to `Codex-SessionStart.ps1` in this repository.

The hook does not build, modify source, or launch an application. It only reads project status and tells the new task to inspect the current native demo before broad changes.
