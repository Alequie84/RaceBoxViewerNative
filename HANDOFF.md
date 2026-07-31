# RaceBox Telemetry Viewer 2 Handoff

Last updated: 2026-07-21

The protected `0.1.0.029` implementation is checkpointed on `main` and tagged
`v0.1.0.029`. Version 2 is developed side-by-side on branch `v2`; the first
release version is `2.0.0.001`. The golden telemetry engine and its raw-data
rules remain authoritative.

The current v2 architecture, guided workspaces, persistence migrations, and
future macOS adapter boundary are described in
[`docs/v2-architecture.md`](docs/v2-architecture.md).

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
- Raw telemetry is immutable. Map calibration and lap translation are derived rendering/analysis transforms only; Compare mode may display a clearly labelled aligned overlay, and the raw trace remains available from Map settings.
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

Current Windows CTest targets:

1. `racebox_core_golden` - golden parsing/alignment, map anchors, persistence, corruption checks, and core invariants;
2. `racebox_driver_analysis` - synthetic event, metric, rule, confidence, severity, positive-feedback, and line-correction fixtures, plus raw-lap-7 regression;
3. `racebox_insight_evidence_addon` - driver-analysis-v2 outcome, repeatability, payback, guardrail, and golden-session integration gates;
4. `racebox_dx11_warp` - DirectX 11 Microsoft WARP fallback;
5. `racebox_one_million_samples` - one-million-sample memory bound;
6. `racebox_plot_order` - stable graph-order normalization and moves with relative time hidden or shown;
7. `racebox_plot_decimation` - endpoint retention, point-budget enforcement, and multi-channel spike preservation;
8. `racebox_application_state` - portable workspace, lap-role, playback, job, and notification state;
9. `racebox_source_identity` - exact/probable/ambiguous/missing relink decisions and platform path-case rules;
10. `racebox_ui_preferences` - guided workspace, optional Compare B, text size, layout lock, atomic saving, and one-time layout-v4 migration;
11. `racebox_imu_analysis` - raw-channel availability, gyro-bias/yaw calibration, sustained airborne/landing indicators, exceptional-load and rapid-rotation gates, and empty compatibility channels;
12. `racebox_crew_chief` - bounded lap and race-day private-gateway request/response contracts;
13. `racebox_race_day` - event templates, single/triple mains, version-3 source identity, checklist/conditions/tire-run persistence, golden three-source loading, and canonical analytics CSV;
14. `racebox_import_discovery` - bounded recursive folder scanning, strict RaceBox/Sanwa CSV header recognition, unrelated/empty file rejection, and Sanwa-only removable-drive filtering.

The `portable-core-release` preset builds only the domain, application state,
CLI, and seven portable tests. It is the dependency-boundary check for a later
SDL3/Metal macOS shell; it does not claim that a Mac app bundle exists yet.

Current validation is native-only. `Verify-Project.ps1` is a legacy combined workflow and must not be used while the browser remains frozen. After every C++ edit, build and test with `Build-Test-Native.ps1`, then use `Start-NativeDemo.ps1`; do not leave an older executable open for review.

Outputs:

- GUI: `build\release\RaceBoxTelemetryViewer.exe`
- validation CLI: `build\release\racebox_cli.exe`
- portable folder: `package\RaceBoxTelemetryViewer`
- portable ZIP: `out\RaceBoxTelemetryViewer-<VERSION>-win64.zip`

The executable accepts VBO, RaceBox CSV, and Sanwa CSV paths on its command line or through **File > Add recording to Race Day**. Every interactive file/folder/USB import enters Race Day, loads only the newly selected files, asks whether the recording was Practice, Qualifying, or Race, preserves its canonical UTC timestamp, derives the event's local calendar date from that recording, and offers the next empty matching Race Day slot or an existing slot. Session exposes an always-visible **Race Day Run** selector that loads only data-ready runs, identifies the displayed run by stable run ID after a successful load, and labels command-line/demo telemetry as not linked to Race Day. A persistent import folder defaults to Windows Downloads. **Scan saved import folder** searches it on a worker with bounded depth/count, recognizes CSVs by RaceBox/Sanwa headers, and selects only the newest primary as a starting point; the driver explicitly chooses any companion files. **Open official RaceBox cloud export** launches `https://www.racebox.pro/webapp/login` in the default browser. The app does not collect credentials, reuse browser cookies, call an undocumented API, or scrape the account. **Auto-detect Sanwa on USB** polls only filesystem-mounted removable volumes, proposes the newest recognized Sanwa CSV when a volume appears, and never attaches it without confirmation. MTP-only devices have no stable filesystem path and are not supported by this detector. Command-line loading remains non-interactive for development/demo launchers. Version 2 packages install all three golden files under `demo`, plus `Start RaceBox Demo.cmd`. When launched without file arguments, the packaged executable auto-loads that demo only if all three files exist; explicit user paths always take priority. Ordinary development builds have no adjacent `demo` folder and therefore still start empty unless the toolbox launcher supplies the three golden files.

## Architecture and source ownership

- `include/racebox/session.hpp`: structure-of-arrays telemetry/radio model, map calibration, and versioned workspace-state carrier.
- `include/racebox/domain.hpp`: platform-neutral public telemetry/analysis API.
- `include/racebox/application/state.hpp` and `services.hpp`: portable app state and OS-service contracts.
- `include/racebox/source_identity.hpp`: privacy-bounded source identity and deterministic relink decisions.
- `include/racebox/plot_decimation.hpp`: bounded multi-channel peak-preserving plot indices.
- `include/racebox/driver_analysis.hpp`: corner/event/metric/rule/confidence result contracts and formula version.
- `include/racebox/insight_evidence.hpp`: UI-independent v2 outcome/reliability/recommendation contract consumed by the native analysis pipeline.
- `include/racebox/telemetry_plot_order.hpp`: stable plot identifiers, validation, and order moves.
- `src/core/parsers.cpp`: VBO, RaceBox CSV, GPX, Sanwa parsing, source merge, and lap assignment.
- `src/core/analysis.cpp`: radio alignment, correlations, physical sectors, theoretical-best sector timing, and safe map display-crop resolution.
- `src/core/driver_analysis.cpp`: deterministic corner suggestion, event detectors, derived metrics, evidence gates, insights, and analysis-only line translation.
- `src/core/insight_evidence.cpp`: v2 evidence classifier for retained gains, compensation, repeatability, and recommendation guardrails.
- `include/racebox/imu_analysis.hpp` and `src/core/imu_analysis.cpp`: mounting-independent gyro calibration, derived vehicle yaw, vertical-load/landing, exceptional-load, and rapid-rotation indicators.
- `src/core/persistence.cpp`: backward-compatible `.rbxsession`/`.rbxlap`, atomic manifest-v2 archives, bounded workspace JSON, CSV export, map state, and embedded background.
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

The formula set is versioned as `driver-analysis-v2`. Reference-path curvature, steering peaks, and speed troughs produce deterministic, editable corner suggestions. Candidates below five percent of the strongest bend are discarded rather than filling the maximum count. Remaining corners are numbered in travel order; the known Richmond layout is capped at and resolves to nine turns so moving its start line cannot promote a minor ripple to `T10`. A corner stores stable ID/name plus start, turn-in, apex, exit, and end progress; its phases are braking, entry, exit, and acceleration.

Event detectors require sustained evidence so a single sample cannot fire them:

- brake begins above 10% for 80 ms;
- brake release remains below 5% for 80 ms;
- turn-in exceeds 15% steering with corresponding path curvature for 80 ms;
- apex is the closest registered approach inside the configured zone;
- first throttle exceeds 10% for 80 ms;
- full throttle exceeds 90% for 100 ms;
- post-apex steering reversal or secondary change exceeds 20%.

For Compare A and B, the engine calculates brake-point, turn-in, and apex timing delta; minimum-speed and exit-speed difference; throttle-pickup delay; entry-line outward deviation; and relative-time change through each configured zone. Rules default to `0.08 s`, `0.08 s`, `0.08 s`, `1.0 km/h`, `1.0 km/h`, `0.08 s`, `0.35 m`, and `0.05 s` respectively. Each rule has a stable ID, enable toggle, editable threshold, unit, formula explanation, individual reset, and global reset. A negative relative-time change beyond the final threshold produces deterministic positive feedback for reduced corner time loss.

Confidence is a deterministic 0-100 data-quality score using satellite quality, sampling resolution, event clarity, repeatability across complete laps, and required GPS translation. Below 40 is suppressed and 40-59 is a weak signal; 60+ can enter the evidence classifier but is not automatically actionable. Translation over 0.75 m reduces confidence, over 1.5 m reduces it strongly, and over 3 m disables line conclusions. Severity is Low below 1.5x threshold with under 0.05 s loss, High at 2.5x threshold or 0.15 s loss, and Medium between them.

Text is generated only from verified metric results; there is no language-model interpretation of raw graphs.

### Integrated recommendation evidence

`driver-analysis-v2-evidence-addon-1` remains a UI-independent classifier but is linked into `racebox_core` and consumed by `RaceBoxViewer`. It separates measured outcome, repeatability, and recommendation instead of reusing the old `positive` Boolean. For every card, the engine samples cumulative distance-based Delta-T before the action, at corner end, and at the next driver decision. Matching complete laps that pass telemetry-gap, confidence, and applicable GPS-line gates form the comparison population. Median absolute deviation supplies robust repeatability sigma; support, median, and the reliability interval are calculated from the real session.

The dynamic timing floor is `max(0.05 s, 2 x median sample period, 1.5 x repeatability sigma)`. A favorable local value followed by a net loss becomes recovery/compensation and can never recommend a technique. One-lap retained gains remain observations. The default reliable gate requires confidence 75, eight comparable laps, five supporting laps, 75% support, a median and upper interval beyond the noise floor, no driving guardrail, and no more than 50% downstream payback. A slower-entry/faster-exit result can recommend only the complete sequence. The bundled R15/R6/R8 harness currently classifies 107 cards, including six compensation cases and 24 retained/trade-off observations; none passes the strict reliable-recommendation gates.

The current clean-lap screen means complete, no telemetry gap, sufficient confidence, and line correction within limits when applicable. The recordings do not include a track-limit channel or structured setup/conditions markers, so those two guardrails remain false unless future source data provides them; do not describe the population as FIA-style validated clean laps.

### Analysis UI

The Telemetry window contains:

- **Telemetry**: three-trace relative time, speed, lateral G, longitudinal G, signed throttle/brake, and steering;
- **IMU**: untouched vertical G and X/Y/Z gyro output, gated GPS-heading-calibrated vehicle yaw, and navigable possible-airborne/landing, exceptional-load, and rapid-rotation indicators;
- **Events**: Reference/A/B detected events with lap, corner, event, elapsed time, distance, and progress. Selecting a row synchronizes the cursor;
- **Sectors**: existing physical-sector/theoretical-best timing followed by new per-corner/zone metrics.

The Insights window contains:

- **Insights**: cards use plain English as the primary wording and teach standard motorsport terms in parentheses. They separately show result, repeatability, driver advice, prior/local/retained timing, normal variation, repeated-lap support, recording quality, evidence reasons, technical metric, and trigger. The derived table deliberately says favorable/changed/within rather than calling a local value a gain. Selecting a card navigates to its event and selected corner;
- **Rules / Formula**: metric rules, event thresholds, recommendation-evidence gates, formulas/units, corner auto-suggestion, corner names, and distance-timeline phase boundaries;
- **Dev Notes**: persistent **Notes for Codex** and numbered location links. Annotation mode is also exposed by the permanent **ANNOTATE** top-navigation button; its active label reads **ANNOTATION ON (ESC)**.

Notes for Codex are multiline and intended for setup, tires, springs, damage, traffic, weather, deliberate changes, questions, and requested fixes. Numbered annotations now act as location links instead of separate mini-documents. Selecting a pin links it to the main note; **Copy note for Codex** places the note plus pin surface, telemetry time, Reference/A/B/Playback roles, and view mode on the clipboard for pasting into the active Codex task. The offline native app never uploads or sends the note automatically. Annotation mode accepts pins on the complete interface, blocks underlying controls, and exits with Esc. Map/plot pins retain telemetry time; interface pins retain window/workspace anchors. **Go to pin** restores Reference, A, B, Playback, view mode, and cursor.

### Workspace and archive persistence

Before saving `.rbxsession` or `.rbxlap`, the application serializes a versioned workspace document containing:

- manual/fastest reference mode and all four lap roles;
- view mode;
- analysis formula version, enabled metric rules, event thresholds, and recommendation-evidence gates;
- corner IDs, names, and all phase boundaries;
- Session Notes;
- numbered annotations, stable plot IDs, lap context, cursor, and comments.

The archive manifest also persists the physical start/finish endpoints, map lock, east/north displacement, reference coordinate/pixel, metres-per-pixel, rotation, opacity, and the embedded background image. Loading an archive restores the workspace. Archive entries have realistic memory limits, telemetry/radio columns and time order are checked, lap indices and markers are bounded, and ZIP resources close on every failure path. Annotation review JSON can now be exported and imported independently; it requires an open matching session and stages the complete batch before committing, so malformed later pins cannot leave a partial import.

Race Day v3 stores each run's detected UTC recording time plus UTF-8 relative source paths, basename, size, modified time, and an algorithm-tagged content fingerprint. The **Run Data** tab exposes named status rows and add/replace/remove/repair actions. Source changes are staged atomically, duplicate logical source types are rejected, native archives stay exclusive, and replacing one input captures a fresh identity without disturbing unrelated inputs. A Sanwa-only run is correctly labelled as controls-only and cannot be opened or analyzed until primary RaceBox/VBO/GPX/archive telemetry is present. An existing file that changed at the same path is not accepted or silently blessed during save. Missing or changed sources must be exactly matched or explicitly confirmed through **Find moved file / Review file** before loading or analysis; identities and plaintext `.writing` files are cleaned up safely on failure.

The saved import-folder path and Sanwa USB auto-detect toggle live in local UI preferences, not the Race Day archive. Folder/USB scans are native Windows adapters and remain outside the portable domain/application targets. USB detection is insertion-based and read-only: it scans mounted removable storage, never writes to the device, and does not claim support for MTP or vendor-specific serial protocols.

### Reorderable telemetry plots

Plot IDs are `relative_time`, `speed`, `lateral_g`, `longitudinal_g`, `controls`, and `steering`. Each plot has a dedicated drag handle and drop positions between plots, so reordering does not steal pan/zoom gestures. Order is stored in application preferences; **Reset graph order** restores the default. Relative time disappears outside Compare mode without losing its stored position. Annotations store the stable plot ID, so their attachment survives reordering and restart/archive restore.

Telemetry Y ranges include padding and expand to include all three visible traces. Signed controls and steering keep a labelled `-120..120%` display around their full `-100..100%` input range.

The cyan inspection cursor reports real channel values rather than using its label for lap percentage. The hovered graph annotates speed with units, signed G, named throttle/brake, or signed steering; Compare mode shows separate R/A/B readings and the relative-time plot shows both comparison deltas. A compact percentage badge stays in the graph-header strip directly above the cyan cursor and outside the plotting rectangle, while graph-header values follow the synchronized inspection position.

### IMU channels and interpretation

VBO field 8 and fields 9-11, plus RaceBox CSV `GForceZ` and `GyroX/Y/Z`, are retained as vertical G and raw gyro X/Y/Z in `TelemetrySeries`. Telemetry payload version 2 appends these four arrays; version-1 archives load with zero-filled compatibility arrays. Session/lap archives and lap CSV export preserve the channels.

The first continuous two-second block at or below 1.5 km/h is the on-track stationary zero. Median longitudinal, lateral, vertical, and gyro readings from that exact block are subtracted only in derived display/analysis channels. Main G plots, live G readings, G-G, the IMU plots, yaw calibration, and IMU event loads therefore read zero while the car is stationary. Raw telemetry, persistence, and exports are never rewritten. With the golden fixture the chosen block begins at sample 104 / about 4.16 seconds and yields approximately `-0.017 g` longitudinal, `+0.051 g` lateral, and `+0.994 g` vertical. If no qualifying block exists, acceleration display remains uncalibrated with a visible warning; gyro yaw calibration may use its disclosed whole-session median fallback.

The logger's installed orientation is not assumed. A deterministic calibration estimates gyro bias, then learns a normalized X/Y/Z projection against smoothed GPS heading rate using moving samples. Derived vehicle yaw is shown only with at least 100 matches and `r >= 0.60`; the golden recording yields about `r=0.962` over 9,000 samples. Raw axes remain visible regardless. Sustained low vertical load below 40% of the resting channel for 80 ms creates a **Possible airborne period** indicator; a following vertical impulse may create **Possible landing impact**. Combined dynamic load must exceed 3.5 g and derived yaw 220 deg/s before their neutral indicators appear. None is presented as proof of a crash, jump, or spin.

In Compare mode, **Analysis-aligned comparison traces** defaults on and applies each valid whole-lap translation to the displayed comparison polyline, its playback/inspection dots, and its sector markers. Labels disclose the magnitude (R6/raw lap 7 is about 1.72 m), while **Map settings** can show raw GPS. Corrections over 3 m remain disabled. Projection and hover hit-testing use the same derived coordinate, and tests verify applying it does not mutate `TelemetrySeries`.

No inertial position fusion is currently applied. Raw GPS remains immutable; the optional aligned display is only one disclosed whole-lap translation. A future derived fused path needs a validated installation transform, sensor/GNSS clock model, uncertainty output, and preferably RTK-quality absolute position before it may be used for line conclusions.

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

Golden race lap R6 is native raw lap 7 (`2026-07-11T21:54:00.040Z` through `21:54:16.480Z`). Its RaceBox sources agree within 1 cm and sampling/satellite quality is normal, but the complete lap has a temporary whole-lap GPS translation of roughly 1.7 m. The driver-analysis regression fits and discloses one east/north translation, uses it for line metrics, and—when **Analysis-aligned comparison traces** is enabled—uses the identical transform for the visible comparison trace and dots. Raw GPS remains stored unchanged and selectable; tests verify the display helper cannot mutate it or manufacture a wide-entry insight.

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
| Speed, three-axis G, raw gyro X/Y/Z, altitude, real controls, and steering | Implemented |
| Gated vehicle-yaw estimate and cautious jump/impact/rotation indicators | Implemented |
| IMU/GNSS fused derived position trace with uncertainty | Deferred |
| Deterministic events, corner metrics, rules, confidence, severity, and gains | Implemented |
| Evidence-constrained Crew Chief chat over private Tailscale/OpenClaw lane | Implemented inside Race Day; optional connection |
| Race Day event book, run checklist/conditions/tire history, and previous/current Crew Chief analysis | Implemented; primary Crew Chief workflow |
| Insights / Rules / Dev Notes and Telemetry / Events / Sectors | Implemented in Analysis/Session |
| Session Race Day run selector with explicit unlinked-demo state | Implemented |
| Reorderable persistent plots and stable annotation attachment | Implemented |
| Three-point triangulated Richmond aerial with direct drag/lock/reset | Implemented |
| Placeable/persisted start-finish line and auto-numbered map turns | Implemented |
| Concept-inspired native dashboard, larger readable default font, fixed app/context header with visible dark/light switch, compact playback, dock layout, dark/light modes | Implemented |
| Units, map grid, average GPS, and equal-scale separate compare maps | Implemented |
| `.rbxsession`, `.rbxlap`, lap CSV, and annotation JSON export | Implemented |
| Physical sector timing and theoretical-best sector sources | Implemented |
| Theoretical-best coherent composite telemetry trace/map | Deferred |
| Guided Reports workspace with summary, lap table, findings, sectors, and exports | Implemented |
| Exact pixel-for-pixel concept-image recreation | Deferred; native guided layout retained |

## Known gaps and next priorities

1. A theoretical best currently combines sector times/source-lap identities, not one coherent drivable telemetry trace. Keep theoretical-composite and averaged-clean telemetry references deferred until their data contract is explicit.
2. Exact pixel-for-pixel reproduction of the supplied concept remains deferred; v2 uses its guided native layout and working Reports workspace.
3. Record final long-run performance evidence: hardware and WARP frame-time percentiles, repeated load/unload growth, and a two-hour repeat-playback soak.
4. Continue small-screen and high-DPI layout review after functional changes, especially 150% text on narrow displays.

## Distribution and next-developer rules

The v2 artifact is `out\RaceBoxTelemetryViewer-<VERSION>-win64.zip`. `VERSION` is the single source and uses a three-digit release sequence: `2.0.0.001`, `2.0.0.002`, and so on. Run `scripts\bump-version.ps1` once before each later user-facing release and retain prior numbered ZIPs. Record the new artifact's SHA256 after every package build:

```powershell
$version = (Get-Content .\VERSION -Raw).Trim()
Get-FileHash -Algorithm SHA256 ".\out\RaceBoxTelemetryViewer-$version-win64.zip"
```

The package must remain offline, portable, and usable without an installer, administrator access, Node.js, Chromium, or an online map service.

The Crew Chief is an optional add-on and must never weaken that offline
baseline. Native code builds `racebox-crew-chief-evidence-v1` from the selected
Reference (before) and Compare A/B (after) laps. It includes calculated channel
summaries/deltas, zero-calibrated chassis G, gated yaw, controls, driver-analysis
metrics, retained timing, confidence, GPS correction, and repeatability. It
excludes file paths, raw files, screenshots, notes, archives, and credentials.
The default private endpoint is
`http://100.73.60.87:18804/v1/crew-chief/chat`; overrides are
`RACEBOX_CREW_CHIEF_URL` and optional `RACEBOX_CREW_CHIEF_TOKEN`.

The server implementation lives in the separate family-server workspace at
`C:\Users\aalex\OneDrive\Documentos\Minecraft Java Server\racebox-crew-chief-gateway`.
It exposes only the host's Tailscale address, forwards to the loopback OpenClaw
gateway, and uses the `racebox-adaptive-intelligence-v1` pipeline. Its xhigh
routing brain chooses an evidence-gated Luna-low, Terra-medium/high, or
Sol-xhigh worker; Sol-ultra is benchmark-locked and is not the normal test
baseline. Every lane has broad tool groups disabled. Both client and server
validate and bound the structured response.
The wording prompt forbids causal claims from a lap pair, distinguishes retained
gains from recoveries/payback, and requires disclosure that chassis IMU G is not
measured tire or suspension load. Server deployment creates a timestamped
OpenClaw/service rollback backup before changing the route.

The top-level **RACE DAY** workspace persists a lightweight `.rbxday` event book
without retaining all telemetry in memory. It supports P1-Pn, Q1-Qn (including
Q4 trophy-race use), single/triple A/B/C/D mains, and custom runs. Each run
stores a fillable pre-run checklist, planned changes, post-run driver feel,
ambient/track temperature, tire set/compound/prior-run count, sauce/warmer
preparation, battery context, detected recording time, and attached telemetry
references. **Import Session Data** unifies normal telemetry loading with this
event book: after parsing, it asks Practice/Qualifying/Race and assigns the
recording to the next matching empty slot or a selected slot. The selected
run's **Run Data** tab can also reuse the telemetry currently open or load that
run back into the normal viewer. Session's header selector lists every run and
its readiness, verifies the saved source identity before loading, and changes
the displayed run label only after the load succeeds.

The primary Crew Chief is now part of Race Day beside the run context it uses.
**Ask Crew Chief About These Runs** is an explicit higher-disclosure action:
the app loads both runs on a worker, produces two bounded
`racebox-session-analytics-csv-v1` documents, and sends those plus the entered
context to the private Tailscale gateway. Original files/paths are not sent and
the raw CSV is not placed in the model prompt. The native app now calculates a
deterministic `racebox-setup-analytics-v3` summary before asking the gateway:
top-three lap outcome, input/output lateral response, steering required for
lateral load, full-throttle speed-derived acceleration, straight-entry speed,
straight top speed, straight acceleration attribution, brake decel
response/delay, possible braking lockup/low-grip indicators, overdriving/tire
scrub risk, tilt-corrected chassis-roll amount, and tilt-corrected roll-rate
signature. Roll-rate is measured in uncapped degrees per second with an
adaptive threshold, so repeated high derived values do not flatten at a
software score ceiling; true physical sensor clipping remains range-limited
and cannot be reconstructed after recording. The row-level
CSV contract remains `racebox-session-analytics-csv-v1` for gateway
compatibility and adds a `#setup_contract,racebox-setup-analytics-v3` metadata
line plus optional raw lateral/vertical G columns for tilt correction.
The OpenClaw lane preloads `racebox-vehicle-dynamics`; broad tools remain denied.
Exact formulas and sensor limits are in `docs/race-day-crew-chief.md`.

Successful Race Day analysis also creates a bounded Setup Knowledge record in
the version-2 `.rbxday` file. It stores the setup change, driver result,
conditions, deterministic evidence summary, confidence, confounds, agent
conclusion, and next test, but no raw telemetry or file path. Deterministic
local retrieval supplies at most eight matching records to both Race Day
analysis and ordinary Crew Chief chat. The live lane benchmark uses Sol-xhigh
as the trusted setup-memory baseline and tests lower lanes against the same
packet before allowing them to handle that task class.

The always-visible native header displays `v<VERSION>` from the same generated version header used by the executable metadata and portable ZIP. The five top workspaces are **Race Day / Session / Compare / Analysis / Reports**; Crew Chief is intentionally inside Race Day, while Analysis contains the deterministic Insights, Rules / Formula, and Dev Notes tabs. The header exposes the persistent **THEME: DARK / THEME: LIGHT** switch beside the top workspace navigation at normal widths; at 130-150% text or narrow widths, Theme, Layout, Annotation, and renderer status collapse into **TOOLS** so every workspace remains reachable. Keep the version tied to the root `VERSION` source; never hard-code a separate UI version string.

Before changing parser, timing, lap, radio, map, or analysis logic, read the golden and driver-analysis tests. Preserve the current C++ architecture, original telemetry, and both map-image backups. Treat browser files as read-only history. Update this handoff and the README whenever behavior changes.
