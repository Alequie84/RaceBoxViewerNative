# RaceBox Telemetry Viewer 2 Architecture

Version 2 is a controlled rebuild around the proven native telemetry
engine. It is not a rewrite of the parser, clock alignment, lap timing, map
calibration, IMU analysis, or deterministic insight formulas.

## Product shell

The Windows release is `RaceBoxTelemetryViewer.exe`. Its guided shell has a
persistent Race Day panel, five center views, and a persistent Crew Chief panel:

1. **Race Day panel** — chronological Practice/Qualifying/Race runs, readiness,
   explicit import/source repair, selected Current run, and automatic/overridden
   nearest Previous run.
2. **Run** — one scrollable identity, tire preparation, setup, source-health,
   comments, and collapsed secondary-details page.
3. **Telemetry / Compare** — the compact map, inline playback, all-separate
   channel graphs, optional advanced panels, and distance-aligned comparison roles.
4. **Findings / Report** — deterministic insights/rules/notes and the existing
   summaries/exports.
5. **Crew Chief panel** — one filterable day timeline with explicit Current-only
   and Previous-to-Current actions. Changing the center view never resets it.

The default dock arrangement is locked to prevent accidental panel moves. The
map/graph divider deliberately remains draggable and defaults to 32/68. Side
panels resize/collapse to rails and become overlays at narrow widths. Text scale,
theme, center view, side-panel widths/visibility, map ratio, comparison choices,
graph order, palettes, focus state, and layout mode persist in UI-preferences
version 6.

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
Windows adapters          macOS preview / adapters
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
dependency. The `macos-release` preset now builds the first Apple Silicon shell:
SDL3 owns its window, high-DPI input and drag/drop, while Metal renders Dear
ImGui and ImPlot. It reads the bundled demo and user-supplied VBO/RaceBox/Sanwa
files through the shared domain target.

The next Apple Silicon milestones should add:

- an asynchronous native file-dialog adapter beyond the current Finder drop;
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
- Race Day version 6 stores privacy-bounded source identities, car-profile and
  setup-revision references plus bounded snapshots, Setup Sheet ON/OFF and
  known/untracked changes, and up to 2,000 day-level Crew Chief turns with ID,
  timestamp, role, content, origin, linked run IDs, and comparison ID. Versions
  1–5 migrate without duplicate legacy turns. Source identity contains basename,
  size, timestamp, and an optional algorithm-tagged fingerprint—never telemetry
  contents.
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
- UI-preferences version 6 migrates older layouts, validates dimensions and
  ratios, and recovers malformed state to the guided three-panel defaults.
- Normal and `--demo-profile` launches use different LocalAppData roots and
  different single-instance identities. The normal profile starts with a Race
  Day choice; the demo profile may load bundled/remembered fixtures without
  changing normal recents or layout.
- An unnamed event maintains a local recovery draft. A named `.rbxday` autosaves
  atomically after one quiet second, immediately on run changes and clean close,
  and retains five bounded recovery generations. A failed save leaves the event
  dirty and visible as failed.
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
   documentation, and all three demo inputs, while its executable contains no
   development Codex Review controls, flag/capture paths, or review contract;
5. launch the freshly built executable with VBO, RaceBox CSV, and Sanwa data;
6. visually check all five workspaces at normal and enlarged text sizes.

The React/Vite prototype is frozen historical reference material. It is not a
v2 target and is not part of the current build, test, or release workflow.
