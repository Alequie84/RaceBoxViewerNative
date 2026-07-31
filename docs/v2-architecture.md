# RaceBox Telemetry Viewer 2 Architecture

Version `2.0.0.001` is a controlled rebuild around the proven native telemetry
engine. It is not a rewrite of the parser, clock alignment, lap timing, map
calibration, IMU analysis, or deterministic insight formulas.

## Product shell

The Windows release is `RaceBoxTelemetryViewer.exe`. Its guided shell exposes
five driver-facing workspaces:

1. **Race Day** — unified session import and Practice/Qualifying/Race
   classification, canonical UTC recording time plus its local event date,
   persistent bounded folder discovery, confirmed Sanwa removable-drive
   discovery, browser-managed official RaceBox cloud export, responsive
   Runs/Selected Run/Crew Chief navigation, per-run source management,
   pre-run checklist, setup and conditions, post-run notes, tire history, and
   previous/current setup analysis through the primary Crew Chief workflow.
2. **Session** — the normal map, playback, lap list, telemetry, events, sectors,
   notes, and annotations, with a visible Race Day run selector that identifies
   and loads the displayed recording.
3. **Compare** — Reference plus Compare A, with optional Compare B and an
   independent Playback lap.
4. **Analysis** — deterministic insights, disclosed rules/formulas, and
   developer notes. The private Crew Chief is intentionally kept in Race Day
   beside the setup and run context it analyzes.
5. **Reports** — session summary, lap table, theoretical sectors,
   deterministic findings, and the existing export actions.

The default dock arrangement is locked to prevent accidental panel moves.
**Layout: Locked / Custom** deliberately enables panel and splitter editing.
Text scale, theme, comparison choices, graph order, and layout mode persist.
Errors and background-job state are shown in the global header instead of being
hidden inside the Playback panel.

## Target boundaries

```text
racebox_domain
  telemetry, parsing, synchronization, map math, IMU and deterministic analysis
        |
racebox_application
  application state, lap roles, selections, jobs, notifications, OS contracts
        |
  +-----+---------------------+
  |                           |
Windows adapters          future macOS adapters
Win32/DX11/WARP           SDL3/Metal
WinHTTP                   native/portable HTTP transport
Windows file dialogs      asynchronous SDL/native file dialogs
LocalAppData              Application Support / Caches / Logs
```

`racebox_core` remains a compatibility aggregate while the existing UI adopts
the new boundaries incrementally. This keeps the golden engine stable and
avoids a high-risk, all-at-once migration.

The portable service contracts are in:

- `include/racebox/application/state.hpp`
- `include/racebox/application/services.hpp`

They cover file dialogs, HTTP, application directories, atomic writing, asset
lookup, logging, protected secret lookup, and system appearance. Network and
secret services are optional: opening, analyzing, comparing, reporting, and
saving telemetry must continue to work offline.

## Portable build check

Windows presets continue to build the complete app. A portable preset configures
only targets that have no Win32, DirectX, WinHTTP, shell, registry, or WIC
dependency. It is an architecture check, not a claim that the macOS window is
already implemented.

The eventual Apple Silicon shell should add:

- SDL3 window, input, DPI, clipboard, and asynchronous file-dialog adapters;
- a Metal renderer for Dear ImGui and ImPlot;
- macOS Application Support, Caches, and Logs directory adapters;
- a Keychain-backed optional secret provider;
- a macOS HTTP transport for the optional Crew Chief connection;
- package signing, notarization, and a universal application bundle only after
  the Apple Silicon build is stable.

The domain and application targets must stay free of platform headers. macOS
work should implement adapters behind the existing contracts instead of adding
`#ifdef` branches to telemetry formulas.

## Data and migration

- Existing `.rbxsession` and `.rbxlap` version-1 archives remain readable.
- New archives use manifest version 2 and store the versioned workspace as a
  bounded `workspace.json` entry. Writes use a same-directory temporary file
  and atomic replacement so a failed save does not destroy the previous file.
- Race Day version 3 stores privacy-bounded source identities beside local
  relative paths. The identity contains basename, size, timestamp, and an
  optional algorithm-tagged fingerprint—never telemetry contents.
- A run also stores its detected UTC recording time. Every interactive file,
  folder, or USB import enters Race Day, classifies the loaded recording, and
  assigns it to a Race Day slot; source
  updates are validated and staged atomically so duplicate types, mixed
  archives, or a failed replacement cannot partially alter the event book.
- Session stores the displayed Race Day run as a stable in-memory run ID only
  after a verified load succeeds. Changing or opening an event book clears that
  association, so unrelated demo or command-line telemetry cannot be mislabeled.
- Source repair distinguishes **Exact**, **Probable**, **Ambiguous**, and
  **Missing**. Only one unique exact fingerprint may relink automatically;
  probable or ambiguous choices require the driver to confirm.
- UI layout version 4 migrates older layouts once, preserves a pre-v4 backup,
  and starts with the guided layout locked.
- Local UI preferences retain the telemetry import folder and Sanwa USB
  auto-detect toggle. Folder and removable-drive scans stay in the Windows app
  adapter, are bounded and read-only, recognize CSV sources by header, and
  never infer that optional source files belong to the same run.

Raw GPS and telemetry remain immutable. Display alignment, stationary IMU zero,
corner analysis, and other derived corrections remain disclosed,
reversible calculations.

## Validation gates

Every Windows release must:

1. configure and build all release targets;
2. pass the golden parser/alignment/lap/map checks;
3. pass analysis, IMU, Crew Chief, Race Day, archive migration, source matching,
   application-state, UI-preference, WARP, plot-order, and memory tests;
4. verify the portable package contains the executable, CLI, version, launcher,
   documentation, and all three demo inputs;
5. launch the freshly built executable with VBO, RaceBox CSV, and Sanwa data;
6. visually check all five workspaces at normal and enlarged text sizes.

The React/Vite prototype is frozen historical reference material. It is not a
v2 target and is not part of the current build, test, or release workflow.
