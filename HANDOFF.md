# RaceBox Viewer Native Handoff

Last updated: 2026-07-21

## Start here

Active native project:

`C:\CodexProjects\RaceBoxViewerNative`

Frozen JavaScript/React historical reference:

`C:\CodexProjects\New project 2`

The browser reference is React/Vite, not Java. Browser development and validation are frozen by user decision. Do not edit, build, test, or extend it unless the user explicitly reverses that decision. The live directory and `reference/browser-baseline-20260712.zip` are read-only history only.

New Codex tasks opened at the native root load `AGENTS.md`, `.codex/hooks.json`, and `scripts/toolbox/README.md`. The startup hook is read-only and may require one `/hooks` security review. There is no established git history in either project, so do not assume untracked files are disposable.

## Product and data invariants

RaceBox Viewer reviews RC-car GPS and radio telemetry. Accuracy takes priority over making a trace look visually plausible.

- RaceBox CSV is canonical for absolute time, lap timing, lap numbers, and speed.
- VBO supplies GPS position, heading, satellites, altitude, and valid G channels.
- Sanwa supplies real throttle, brake, and steering at its native 100 Hz rate.
- Throttle is positive and brake negative on the combined plot; steering is negative left and positive right.
- RaceBox data remains at its native sample rate. Controls are interpolated only for requested cursor/render times.
- Lap detection rejects reverse start-line crossings. The VBO physical start/finish line is retained; a user-placed replacement is accepted only when it yields at least two complete laps, then derived lap/sector/theoretical state is rebuilt. Physical sector markers are shared track locations, not separate distance thirds for each lap.
- Raw telemetry and the displayed GPS trace are immutable. Map calibration and the analysis-only line correction never rewrite them.
- Do not infer controls when compatible Sanwa data is present, silently combine weak sources, or hide data-quality failures with a plausible visualization.
- Do not claim wheelspin without wheel-speed data. Yaw/lateral-G patterns may only be labelled as possible instability indicators.

## Build, test, launch, and package

From Visual Studio 2022 Developer PowerShell:

```powershell
cd C:\CodexProjects\RaceBoxViewerNative
cmake --preset windows-release
cmake --build --preset release
ctest --preset release
powershell -ExecutionPolicy Bypass -File .\scripts\package.ps1
```

From an ordinary PowerShell window, the repository toolbox discovers the Visual Studio environment:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\toolbox\Build-Test-Native.ps1 -Package
powershell -ExecutionPolicy Bypass -File .\scripts\toolbox\Start-NativeDemo.ps1
```

Current native CTest targets:

1. `racebox_core_golden` - golden parsing/alignment, map anchors, persistence, corruption checks, and core invariants;
2. `racebox_driver_analysis` - synthetic event, metric, rule, confidence, severity, positive-feedback, and line-correction fixtures, plus raw-lap-7 regression;
3. `racebox_insight_evidence_addon` - isolated driver-analysis-v2 outcome, repeatability, payback, guardrail, and golden-session recommendation gates;
4. `racebox_dx11_warp` - DirectX 11 Microsoft WARP fallback;
5. `racebox_one_million_samples` - one-million-sample memory bound;
6. `racebox_plot_order` - stable graph-order normalization and moves with relative time hidden or shown.
7. `racebox_ui_preferences` - dark defaults, workspace/density/panel persistence, malformed-file recovery, atomic saving, and one-time layout-v3 backup.

Current validation is native-only. `Verify-Project.ps1` is a legacy combined workflow and must not be used while the browser remains frozen. After every C++ edit, build and test with `Build-Test-Native.ps1`, then use `Start-NativeDemo.ps1`; do not leave an older executable open for review.

Outputs:

- GUI: `build\release\RaceBoxViewer.exe`
- validation CLI: `build\release\racebox_cli.exe`
- portable folder: `package\RaceBoxViewer`
- portable ZIP: `out\RaceBoxViewerNative-<VERSION>-win64.zip` (currently `.015`)

The executable accepts VBO, RaceBox CSV, and Sanwa CSV paths on its command line or through **File > Open telemetry**. Portable `.015` and later packages install all three golden files under `demo`, plus `Start RaceBox Demo.cmd`. When launched without file arguments, the packaged executable auto-loads that demo only if all three files exist; explicit user paths always take priority. Ordinary development builds have no adjacent `demo` folder and therefore still start empty unless the toolbox launcher supplies the three golden files.

## Architecture and source ownership

- `include/racebox/session.hpp`: structure-of-arrays telemetry/radio model, map calibration, and versioned workspace-state carrier.
- `include/racebox/driver_analysis.hpp`: corner/event/metric/rule/confidence result contracts and formula version.
- `include/racebox/insight_evidence.hpp`: isolated v2 outcome/reliability/recommendation contract; not yet connected to the app.
- `include/racebox/telemetry_plot_order.hpp`: stable plot identifiers, validation, and order moves.
- `src/core/parsers.cpp`: VBO, RaceBox CSV, GPX, Sanwa parsing, source merge, and lap assignment.
- `src/core/analysis.cpp`: radio alignment, correlations, physical sectors, theoretical-best sector timing, and safe map display-crop resolution.
- `src/core/driver_analysis.cpp`: deterministic corner suggestion, event detectors, derived metrics, evidence gates, insights, and analysis-only line translation.
- `src/core/insight_evidence.cpp`: pre-integration v2 evidence classifier for retained gains, compensation, repeatability, and recommendation guardrails.
- `src/core/persistence.cpp`: `.rbxsession`, `.rbxlap`, CSV export, map state, embedded background, and versioned workspace JSON.
- `src/core/settings.cpp`: application preferences, rotating logs, and local paths.
- `src/app/native_app.cpp`: docking, lap roles, playback, distance synchronization, plots, map interaction, analysis UI, notes, and annotations.
- `src/app/texture_loader.cpp`: WIC decoding and DirectX texture upload.
- `src/app/main.cpp`: Win32/DirectX startup, hardware/WARP selection, and local crash dumps.
- `src/cli/main.cpp`: machine-readable golden validation output.

Keep parsing and expensive analysis out of per-frame geometry paths. Cache map/plot geometry and update only lightweight cursors during playback.

The Richmond aerial remains the untouched `989 x 513` source image. Review pins 1-4 define a display-only source crop of left `138`, top `94`, right `837`, bottom `409` pixels (`699 x 315`). The full-image reference pixel and three calibration anchors remain canonical. Projection bounds, rendered quad, and UVs use the same resolved crop so resizing cannot change the GPS/image ratio; missing or invalid crop metadata safely falls back to the full image.

## Current native behavior

### Four lap roles

Compare mode has explicit **Reference**, **Compare A**, **Compare B**, and **Playback** selections. The reference can be manual or automatically the fastest complete real lap. Playback is independent: choosing the lap used for live values and the moving playback dot does not change any of the three analytical roles.

Stable colors are green Reference, amber Compare A, and red Compare B. All three maps and telemetry traces use cumulative physical-distance normalization in Compare mode. Map dots, plot cursors, event navigation, and insight navigation therefore represent the same track position despite differing lap durations or sample counts. Single Lap and Continuous behavior remains unchanged.

The relative-time curves are:

```text
Compare A elapsed at distance - Reference elapsed at distance
Compare B elapsed at distance - Reference elapsed at distance
```

Positive means the comparison is behind; negative means it is ahead. This replaces the old two-lap/equal-elapsed-time behavior. Yellow is the playback cursor and cyan is hover inspection; hovering does not pause playback.

### Driver-analysis core

The formula set is versioned as `driver-analysis-v1`. Reference-path curvature, steering peaks, and speed troughs produce deterministic, editable corner suggestions. Candidates below five percent of the strongest bend are discarded rather than filling the maximum count. Remaining corners are numbered in travel order; the known Richmond layout is capped at and resolves to nine turns so moving its start line cannot promote a minor ripple to `T10`. A corner stores stable ID/name plus start, turn-in, apex, exit, and end progress; its phases are braking, entry, exit, and acceleration.

Event detectors require sustained evidence so a single sample cannot fire them:

- brake begins above 10% for 80 ms;
- brake release remains below 5% for 80 ms;
- turn-in exceeds 15% steering with corresponding path curvature for 80 ms;
- apex is the closest registered approach inside the configured zone;
- first throttle exceeds 10% for 80 ms;
- full throttle exceeds 90% for 100 ms;
- post-apex steering reversal or secondary change exceeds 20%.

For Compare A and B, the engine calculates brake-point, turn-in, and apex timing delta; minimum-speed and exit-speed difference; throttle-pickup delay; entry-line outward deviation; and relative-time change through each configured zone. Rules default to `0.08 s`, `0.08 s`, `0.08 s`, `1.0 km/h`, `1.0 km/h`, `0.08 s`, `0.35 m`, and `0.05 s` respectively. Each rule has a stable ID, enable toggle, editable threshold, unit, formula explanation, individual reset, and global reset. A negative relative-time change beyond the final threshold produces deterministic positive feedback for reduced corner time loss.

Confidence is a deterministic 0-100 score using satellite quality, sampling resolution, event clarity, repeatability across complete laps, and required GPS translation. Below 40 is suppressed, 40-59 is a weak signal, and 60+ is actionable. Translation over 0.75 m reduces confidence, over 1.5 m reduces it strongly, and over 3 m disables line conclusions. Severity is Low below 1.5x threshold with under 0.05 s loss, High at 2.5x threshold or 0.15 s loss, and Medium between them. Positive feedback uses the same evidence and confidence gates as corrective feedback.

Text is generated only from verified metric results; there is no language-model interpretation of raw graphs.

### Pre-integration recommendation-evidence add-on

`driver-analysis-v2-evidence-addon-1` is compiled and tested as a separate static library but is intentionally not linked into `RaceBoxViewer`. It separates measured outcome, repeatability, and recommendation instead of reusing the v1 `positive` Boolean. A selected lap cannot become a recommendation without repeated aggregate evidence; a favorable-looking metric followed by a net time loss becomes recovery/compensation; and a slower-entry/faster-exit gain can recommend only the complete sequence.

The golden R15/R6/R8 pre-integration harness reviewed 107 existing v1 cards, identified eight compensation cases and 28 retained-gain observations awaiting repeats, and allowed zero recommendations. The contract, default gates, verified counterexamples, and integration hold point are documented in `docs/driver-analysis-v2-evidence-addon.md`. The visible app remains on `driver-analysis-v1` until phase checkpoints, clean-lap aggregation, robust intervals, persistence, and UI wording are implemented and verified.

### Analysis UI

The Telemetry window contains:

- **Telemetry**: three-trace relative time, speed, lateral G, longitudinal G, signed throttle/brake, and steering;
- **Events**: Reference/A/B detected events with lap, corner, event, elapsed time, distance, and progress. Selecting a row synchronizes the cursor;
- **Sectors**: existing physical-sector/theoretical-best timing followed by new per-corner/zone metrics.

The Insights window contains:

- **Insights**: cards with title, corner, comparison, measured result, confidence, positive/severity status, rule, threshold, and estimated time effect; a derived-metrics table shows A and B against Reference with pass/gain/flag state. Selecting a card navigates to its event and selected corner;
- **Rules / Formula**: rule controls, event thresholds, formulas/units, corner auto-suggestion, corner names, and distance-timeline phase boundaries;
- **Dev Notes**: persistent **Notes for Codex** and numbered location links. Annotation mode is also exposed by the permanent **ANNOTATE** top-navigation button; its active label reads **ANNOTATION ON (ESC)**.

Notes for Codex are multiline and intended for setup, tires, springs, damage, traffic, weather, deliberate changes, questions, and requested fixes. Numbered annotations now act as location links instead of separate mini-documents. Selecting a pin links it to the main note; **Copy note for Codex** places the note plus pin surface, telemetry time, Reference/A/B/Playback roles, and view mode on the clipboard for pasting into the active Codex task. The offline native app never uploads or sends the note automatically. Annotation mode accepts pins on the complete interface, blocks underlying controls, and exits with Esc. Map/plot pins retain telemetry time; interface pins retain window/workspace anchors. **Go to pin** restores Reference, A, B, Playback, view mode, and cursor.

### Workspace and archive persistence

Before saving `.rbxsession` or `.rbxlap`, the application serializes a versioned workspace document containing:

- manual/fastest reference mode and all four lap roles;
- view mode;
- analysis formula version, enabled rules, and thresholds;
- corner IDs, names, and all phase boundaries;
- Session Notes;
- numbered annotations, stable plot IDs, lap context, cursor, and comments.

The archive manifest also persists the physical start/finish endpoints, map lock, east/north displacement, reference coordinate/pixel, metres-per-pixel, rotation, opacity, and the embedded background image. Loading an archive restores the workspace. JSON review export remains a separate sharing format; standalone annotation JSON import is not implemented.

### Reorderable telemetry plots

Plot IDs are `relative_time`, `speed`, `lateral_g`, `longitudinal_g`, `controls`, and `steering`. Each plot has a dedicated drag handle and drop positions between plots, so reordering does not steal pan/zoom gestures. Order is stored in application preferences; **Reset graph order** restores the default. Relative time disappears outside Compare mode without losing its stored position. Annotations store the stable plot ID, so their attachment survives reordering and restart/archive restore.

Telemetry Y ranges include padding and expand to include all three visible traces. Signed controls and steering keep a labelled `-120..120%` display around their full `-100..100%` input range.

The cyan inspection cursor reports real channel values rather than using its label for lap percentage. The hovered graph annotates speed with units, signed G, named throttle/brake, or signed steering; Compare mode shows separate R/A/B readings and the relative-time plot shows both comparison deltas. A compact percentage badge stays in the graph-header strip directly above the cyan cursor and outside the plotting rectangle, while graph-header values follow the synchronized inspection position.

## Triangulated Richmond aerial

The one-click **Import triangulated Google map** workflow uses the clean, unchanged `assets/rrr-map-original.png`, not a diagnostic image containing a baked GPS trace and not the older localized-warp experiment.

Verified anchors against the clean image:

| Anchor | Pixel | Latitude, longitude |
| --- | --- | --- |
| A | `(495, 73)` | `49.184236, -123.145111` |
| B | `(105, 273)` | `49.184060, -123.145634` |
| C | `(815, 339)` | `49.184003, -123.144682` |

Calibration constants:

- reference coordinate `49.18407486000555, -123.14511168306714` at pixel `(494.5, 256.5)`;
- `0.09748007033810534 m/pixel` on both image axes;
- `0.09155874851718505 degrees` rotation;
- three anchor reprojections must remain within one pixel.

The aerial, unmodified GPS coordinates, grid, sector/corner markers, and playback/inspection dots use the same metric world-to-screen projection. Map resize and dock changes therefore cannot alter the aerial-to-GPS ratio or stretch one image axis.

**Place S/F** is a one-click map mode. The clicked point snaps to the closest reference-path sample and creates an eight-metre crossing perpendicular to the local tangent. The core rejects zero-length or invalid placements and atomically restores the previous derived lap state if fewer than two complete laps result. Success rebuilds lap roles, physical sectors, theoretical best, average-track geometry, and auto-numbered corners without rewriting GPS. Annotation mode has priority and `Esc` cancels placement. The map draws the stored line and optional `T1...Tn` apex labels on the same projection.

The triangulated image opens unlocked. Left-drag moves it directly, stored as east/north metres; lock/unlock and reset-position buttons replace the old X/Y alignment sliders. Locking prevents editing but does not change resize behavior. Annotation mode has input priority over map dragging.

**Uncalibrated image (advanced)** remains for arbitrary pictures. It is labelled not GPS calibrated and uses panel-relative placement with uniform zoom only; its source aspect remains fixed. Do not reintroduce independent width/height controls.

The clean source image SHA256 is:

`8628CCFA389C4BA28B1A0AB5470475CE32F2D76C6C96CCDBCDA7F523D40F22E3`

`assets/rrr-map.png` remains packaged for compatibility/history, but the triangulated Richmond workflow intentionally selects `rrr-map-original.png`. Diagnostic overlays under `out` must never become runtime backgrounds.

## Golden fixture and raw-lap-7 rule

Golden inputs:

- `golden/session.vbo`
- `golden/session.csv`
- `golden/sanwa.csv`

Core acceptance values:

- 13,863 RaceBox rows and 43,199 Sanwa samples;
- 20 laps;
- radio anchor `+123.780 s` within 2 ms;
- fine correction `-399 ms` within 3 ms;
- trigger correlation greater than `0.32`;
- absolute steering/yaw correlation greater than `0.55`;
- lap steering correlation greater than `0.70`;
- raw lap 17 at `16.280 s` within 2 ms;
- nonzero theoretical-best timing;
- RaceBox-only loading without invented radio samples;
- archive round trips, corrupt-archive rejection, WARP availability, and under 250 MB for one million synthetic samples.

Golden race lap R6 is native raw lap 7 (`2026-07-11T21:54:00.040Z` through `21:54:16.480Z`). Its RaceBox sources agree within 1 cm and sampling/satellite quality is normal, but the complete lap has a temporary whole-lap GPS translation of roughly 1.8 m north. The driver-analysis regression fits and discloses one east/north translation for line metrics only, leaves the displayed raw path untouched, and verifies the correction does not manufacture a wide-entry insight. Never add a per-lap display alignment for this case.

Run the CLI check directly with:

```powershell
build\release\racebox_cli.exe golden\session.vbo golden\session.csv golden\sanwa.csv
```

## Browser-to-native status

| Capability | Native status |
| --- | --- |
| VBO, RaceBox CSV, GPX, and Sanwa inputs | Implemented |
| VBO-only, CSV-only, and paired-source loading | Implemented |
| Sanwa clock/response alignment and diagnostics | Implemented |
| Single Lap, Continuous, repeat, and drill-down | Implemented |
| Reference plus Compare A/B and independent Playback | Implemented |
| Three-lap physical-distance synchronization | Implemented |
| Two relative-time curves vs Reference | Implemented |
| Speed, G, altitude, real controls, and steering | Implemented |
| Deterministic events, corner metrics, rules, confidence, severity, and gains | Implemented |
| Insights / Rules / Dev Notes and Telemetry / Events / Sectors | Implemented |
| Reorderable persistent plots and stable annotation attachment | Implemented |
| Three-point triangulated Richmond aerial with direct drag/lock/reset | Implemented |
| Placeable/persisted start-finish line and auto-numbered map turns | Implemented |
| Concept-inspired native dashboard, fixed app/context header, compact playback, dock layout, dark/light modes | Implemented |
| Units, map grid, average GPS, and equal-scale separate compare maps | Implemented |
| `.rbxsession`, `.rbxlap`, lap CSV, and annotation JSON export | Implemented |
| Physical sector timing and theoretical-best sector sources | Implemented |
| Theoretical-best coherent composite telemetry trace/map | Deferred |
| Full Reports workspace and exact concept-image recreation | Deferred |

## Known gaps and next priorities

1. A theoretical best currently combines sector times/source-lap identities, not one coherent drivable telemetry trace. Keep theoretical-composite and averaged-clean telemetry references deferred until their data contract is explicit.
2. A full Reports workspace and pixel-for-pixel reproduction of the supplied concept are deferred. **Export analysis JSON** already emits the rules, metrics, corrections, confidence, navigation targets, and insight cards for a later reporting layer.
3. Standalone annotation-review JSON import/reopen is not implemented; use a saved session/lap archive for persistence.
4. Record final long-run performance evidence: hardware and WARP frame-time percentiles, repeated load/unload growth, and a two-hour repeat-playback soak.
5. Continue small-screen and high-DPI layout review after functional changes.

## Distribution and next-developer rules

The supported artifact is `out\RaceBoxViewerNative-<VERSION>-win64.zip`. `VERSION` is the single source and uses a three-digit release sequence: `0.1.0.010`, `0.1.0.011`, `0.1.0.012`, and so on. Run `scripts\bump-version.ps1` once before each new user-facing release and retain prior numbered ZIPs. Record the new artifact's SHA256 after every package build:

```powershell
$version = (Get-Content .\VERSION -Raw).Trim()
Get-FileHash -Algorithm SHA256 ".\out\RaceBoxViewerNative-$version-win64.zip"
```

The package must remain offline, portable, and usable without an installer, administrator access, Node.js, Chromium, or an online map service.

Before changing parser, timing, lap, radio, map, or analysis logic, read the golden and driver-analysis tests. Preserve the current C++ architecture, original telemetry, and both map-image backups. Treat browser files as read-only history. Update this handoff and the README whenever behavior changes.
