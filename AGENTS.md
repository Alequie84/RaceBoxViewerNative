# RaceBox Telemetry Viewer 2 Agent Guide

This repository is the active C++ application. Version `0.1.0.029` is protected
on `main`/tag `v0.1.0.029`; current v2 work belongs on branch `v2`. Read
`HANDOFF.md` and `docs/v2-architecture.md` before changing code.

## Project Context

- Native application: `C:\CodexProjects\RaceBoxViewerNative`
- Frozen browser reference: `C:\CodexProjects\New project 2`
- The user may call the browser reference the "Java version." It is JavaScript/React, not Java.
- The browser application is frozen and read-only. It may be inspected for history, but do not edit, build, test, or extend it unless the user explicitly reverses that decision.
- The user is a mechanic and experienced troubleshooter, not a programmer. Explain findings in plain language and tie them to visible behavior.

## First Work In A New Task

1. Read `HANDOFF.md` and `scripts/toolbox/README.md`.
2. Run `scripts/toolbox/Get-ProjectState.ps1`.
3. Launch the current native demo with `scripts/toolbox/Start-NativeDemo.ps1`.
4. Inspect the current native demo before editing. Look for:
   - incorrect or unsynchronized map, playback, hover, lap, and plot cursors;
   - avoidable per-frame calculation, allocation, or geometry rebuilding;
   - crowded, clipped, overlapping, or hard-to-discover controls;
   - regressions from the documented native behavior;
   - dead code, duplicate state, and unnecessary data copies.
5. Report the most important findings first, ordered by severity, before proposing a broad refactor. Small, clearly requested fixes may be implemented directly.

Use archived browser behavior only as a read-only historical reference. The native product and its golden fixture are authoritative.

## Non-Negotiable Data Rules

- Run the golden tests before and after parser, timing, lap, Sanwa, sector, or persistence changes.
- RaceBox CSV is canonical for absolute time, lap timing, lap numbers, and speed.
- VBO supplies GPS, heading, satellites, and valid G-force channels.
- Sanwa supplies real throttle, brake, and steering. Do not reintroduce inferred controls.
- Throttle is positive and brake is negative on the combined control plot. Do not duplicate both as positive traces.
- Steering is negative left and positive right.
- Reject reverse start-line crossings.
- Keep map alignment rendering-only. Never alter GPS telemetry to force a visual fit.
- Preserve exact-aspect GPS geometry. Do not globally distort the aerial image or track trace.
- Do not silently merge weak or mismatched sources.

## Engineering Rules

- Keep parsing and analysis off the render thread.
- Cache heavy map and plot geometry. Playback should move lightweight overlays.
- Prefer contiguous channel buffers and bounded caches over cloned row objects.
- Keep `racebox_domain` and `racebox_application` free of Win32, DirectX,
  WinHTTP, WIC, registry, shell, and other platform-specific dependencies.
- Preserve the guided Race Day, Session, Compare, Crew Chief, and Reports
  workflow. The default dock layout stays locked until the user selects
  Customize.
- Preserve user files, image backups, and unrelated working-tree changes.
- Use `scripts/toolbox` for repeatable build, test, launch, and state checks.
- Update `HANDOFF.md`, the toolbox documentation, and tests when behavior or workflows change.

## Verification

Release versions come from the root `VERSION` file and use `major.minor.patch.build` with a three-digit final component. Version 2 starts at `2.0.0.001`, followed by `.002`, `.003`, and so on. Before creating a new user-facing release, run `scripts\bump-version.ps1` once; do not overwrite or delete earlier numbered ZIPs.

Run the native-only validation and package workflow:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\toolbox\Build-Test-Native.ps1 -Package
```

For an intermediate native-only check, run:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\toolbox\Build-Test-Native.ps1
```

Do not accept a visual improvement that breaks the golden alignment values documented in `HANDOFF.md`.

## Final Launch Rule

After any C++ code edit, finish the task by building and testing the native application, then launch the newly built executable with `scripts/toolbox/Start-NativeDemo.ps1`. Close an older running instance when it blocks the build, and always reopen the latest successful build before handing control back to the user. Report the launched PID so the user knows the visible window is current.
