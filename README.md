# RaceBox Viewer Native

Windows-native RaceBox and Sanwa telemetry analysis using C++20, DirectX 11, Dear ImGui docking, and ImPlot. Processing is local and the portable application does not require an internet connection, Chromium, or Node.js.

Developer continuation notes and the browser-reference comparison are in [HANDOFF.md](HANDOFF.md). Codex startup guidance is in [AGENTS.md](AGENTS.md), and reusable developer commands are in [scripts/toolbox/README.md](scripts/toolbox/README.md).

## Run

Open a VBO, RaceBox CSV, and Sanwa CSV together from **File > Open telemetry**, or pass all three paths to `RaceBoxViewer.exe`. The known Richmond fixture automatically loads the bundled clean aerial; the same Sanwa data is merged whether the files are opened from the menu, the command line, or the demo launcher.

The dockable workspace includes Track Map, Playback, Telemetry, Laps and Sectors, Radio Alignment, Analysis, G-G, Altitude, Insights, and Diagnostics. Window layout and application preferences are stored in `%LOCALAPPDATA%\RaceBoxViewerNative`.

## Lap roles and synchronized comparison

Compare mode has four independent roles:

- **Reference**: manually selected or automatically the fastest complete real lap;
- **Compare A**: amber trace;
- **Compare B**: red trace;
- **Playback**: the lap whose live values and moving dot are shown, without changing any comparison role.

The reference trace is green. All three comparison traces, map dots, telemetry channels, events, insight navigation, and relative-time curves use cumulative physical-distance progress, not sample index or equal elapsed time. Single Lap and Continuous modes keep their existing behavior.

The relative-time graph plots **Compare A minus Reference** and **Compare B minus Reference** at the same track distance. A positive value means that comparison lap is behind the reference; a negative value means it is ahead. Playback and cyan inspection cursors stay synchronized across the map and every graph.

## Deterministic driver analysis

`driver-analysis-v1` suggests editable corners from reference-path curvature, steering peaks, and speed troughs. Minor candidates below five percent of the strongest bend are rejected instead of filling the maximum count. Suggestions are automatically named and drawn in travel order as `T1`, `T2`, and so on; the verified Richmond layout resolves to nine significant turns. **Map settings > Auto-detect and number turns** reruns detection, and turn-label visibility is remembered. Each corner stores start, turn-in, apex, exit, and end boundaries. The engine deterministically detects sustained braking, brake release, turn-in, apex, first throttle, full throttle, and steering-correction events, then calculates:

- brake-point, turn-in, and apex timing deltas;
- minimum and exit-speed differences;
- throttle-pickup delay;
- entry-line outward deviation;
- relative-time change through each corner.

Rules are enabled and edited individually, include units and disclosed formulas, and can be reset individually or globally. Confidence uses GPS correction, satellite quality, sampling resolution, event ambiguity, and repeatability. Results below 40 are suppressed, 40-59 are labelled weak signals, and 60 or higher are actionable. Severity combines threshold exceedance with measured zone-time effect. The same evidence gates generate positive feedback for measurable gains.

Line analysis may remove one disclosed whole-lap east/north translation from a comparison calculation. This never changes the recorded or displayed GPS. Correction above 0.75 m reduces confidence, above 1.5 m reduces it strongly, and above 3 m disables line conclusions. Golden race lap R6 is raw lap 7; its roughly 1.8 m temporary drift is corrected only inside line metrics so it does not create a false wide-line claim. The application does not claim wheelspin without wheel-speed data; G/yaw findings are only possible-instability indicators.

## Analysis workspace

The Telemetry window has three tabs:

- **Telemetry**: relative time, speed, lateral G, longitudinal G, throttle/brake, and steering;
- **Events**: detected events by role, lap, corner, time, distance, and progress; selecting one synchronizes playback;
- **Sectors**: physical-sector/theoretical-best timing plus corner and phase metrics.

The Insights window has three tabs:

- **Insights**: deterministic cards with corner, comparison, confidence, impact, triggering rule, estimated time effect, and a derived-metrics table. Selecting a card moves to the event and highlights its corner; **Export analysis JSON** writes the disclosed rules, metrics, corrections, confidence, navigation targets, and insights;
- **Rules / Formula**: rule toggles, thresholds, units, formulas, resets, event detector settings, and editable corner phase boundaries;
- **Dev Notes**: a persistent **Notes for Codex** writing area and numbered location links.

Notes for Codex can hold setup details, tires, springs, damage, traffic, weather, deliberate test changes, requests, or questions. Select a numbered pin when the note refers to an exact UI or telemetry location, then use **Copy note for Codex** and paste the packaged note into the Codex task. The copied text includes the selected pin, surface, telemetry time, lap roles, and view mode; the native app never uploads or sends it automatically. `.rbxsession` and `.rbxlap` archives preserve lap roles, reference mode, view mode, rules, formula version, corner names and boundaries, start/finish line, notes, annotations, and map calibration. Numbered review JSON export remains available.

## Reorderable telemetry graphs

Every telemetry plot has a dedicated drag handle above it. Drag handles move plots vertically without interfering with plot pan or zoom. The stable identifiers are `relative_time`, `speed`, `lateral_g`, `longitudinal_g`, `controls`, and `steering`; preferences remember their order, and **Reset graph order** restores the default. Relative time is hidden outside Compare mode without losing its stored position. Graph annotations use these identifiers, so pins stay attached when graphs move and a saved workspace is restored.

Hover inspection shows the channel's actual reading at the trace: speed with the selected unit, signed lateral/longitudinal G, named throttle or brake, and signed steering direction. Compare mode labels Reference, Compare A, and Compare B separately. Lap progress remains available as a compact percentage badge in the graph-header strip directly above the cyan cursor, outside the plotting area, and the graph-header values switch from playback to the synchronized inspection position while hovering.

## Triangulated Richmond aerial

**Import triangulated Google map** loads the clean bundled `assets/rrr-map-original.png` and its verified three-point calibration:

| Anchor | Image pixel | GPS |
| --- | --- | --- |
| A | `(495, 73)` | `49.184236, -123.145111` |
| B | `(105, 273)` | `49.184060, -123.145634` |
| C | `(815, 339)` | `49.184003, -123.144682` |

Scale is fixed at `0.0974800703 m/pixel` and rotation at `0.091559 degrees`. Image X and Y always use the same metres-per-pixel. The aerial, raw GPS, grid, corner/sector markers, and playback dots share one world-to-screen projection, so resizing or redocking the map preserves exact 1:1 alignment.

The visible aerial is source-cropped to pixels `(138, 94)` through `(837, 409)`, the useful track boundary marked by review pins 1-4. This removes surrounding parking/runway clutter without editing the bundled original, changing its three calibration anchors, or altering the displayed raw GPS trace. Archives preserve the crop and older archives without crop metadata continue to show their full image.

While unlocked, drag the aerial directly with the left mouse button; its displacement is stored in east/north metres. **Lock background / Unlock background** controls dragging, and **Reset position** restores the verified calibration. Annotation mode takes priority over dragging. **Uncalibrated image (advanced)** remains available for other images, is clearly marked as not GPS calibrated, and permits only uniform zoom so the source aspect ratio is never stretched.

## Placeable start/finish and numbered turns

Select **Place S/F**, then click the centre of the track at the desired crossing. The app derives an eight-metre line perpendicular to local travel, previews it as a white/red `S/F` line, and rebuilds lap boundaries only when the crossing yields at least two complete laps. Invalid placements are rejected without changing the previous assignments. A successful move rebuilds physical sectors, theoretical-best timing, lap roles, average-track geometry, and the numbered corner suggestions; raw GPS and telemetry samples never move. Annotation mode overrides placement, **Esc** cancels it, and session/lap archives retain the physical line coordinates.

## Annotation mode

Use the always-visible **ANNOTATE** button in the top navigation, or open **Insights > Dev Notes** and enable **Annotation mode**. The top button changes to **ANNOTATION ON (ESC)** while review input is active, so the mode cannot be mistaken for a missing feature. Click anywhere in the interface to place a numbered location link; review clicks do not activate ordinary controls underneath. Map and graph pins retain telemetry context, while other pins remain anchored to their window or workspace surface. Select a pin to link it to **Notes for Codex**, use **Go to pin** to restore all four lap roles and the cursor, and use **Copy note for Codex** to prepare a clipboard handoff. **Export review JSON** remains available for structured review files. Press **Esc** to leave annotation mode.

## Build and validate

From a Visual Studio developer terminal:

```powershell
cmake --preset windows-release
cmake --build --preset release
ctest --preset release
```

CTest currently runs seven targets: golden/core and archive tests, driver-analysis-v1 fixtures, the isolated pre-integration driver-analysis-v2 evidence add-on, DirectX 11 WARP, one-million-sample memory, telemetry plot order, and UI-preference/layout migration. The v2 add-on is not connected to the visible app yet; its verification contract is documented in `docs/driver-analysis-v2-evidence-addon.md`.

The repeatable full check and portable package commands are:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/toolbox/Build-Test-Native.ps1 -Package
```

The GUI is `build/release/RaceBoxViewer.exe`; the validation CLI is `build/release/racebox_cli.exe`. Run repeat-playback soak testing with `scripts/soak.ps1`; add `-Warp` for the Microsoft software renderer.

Golden validation can also be run directly:

```powershell
build/release/racebox_cli.exe golden/session.vbo golden/session.csv golden/sanwa.csv
```

The browser application is frozen and is not part of current development or validation. Its archived baseline remains in `reference/browser-baseline-20260712.zip` for read-only history. Telemetry, preferences, rotating logs, and crash dumps stay on the computer and are never uploaded automatically.
