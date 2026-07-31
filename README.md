# RaceBox Telemetry Viewer 2

Native RaceBox and Sanwa telemetry analysis using C++20, Dear ImGui, and ImPlot. Windows uses DirectX 11; the first Apple Silicon preview uses SDL3 and Metal over the same portable telemetry, lap, alignment, and IMU core. Processing is local and neither native shell requires Chromium or Node.js.

The v2 architecture and platform boundary are documented in [docs/v2-architecture.md](docs/v2-architecture.md), with Mac build instructions in [docs/macos-build.md](docs/macos-build.md). Developer continuation notes are in [HANDOFF.md](HANDOFF.md). Codex startup guidance is in [AGENTS.md](AGENTS.md), and reusable developer commands are in [scripts/toolbox/README.md](scripts/toolbox/README.md). The React/Vite prototype is frozen history and is not built or tested as part of v2.

## Version 2 guided workflow

- **Race Day** owns file import and the Crew Chief. It puts each recording into a Practice, Qualifying, or Race run, detects its date, and keeps telemetry, the run checklist, setup changes, conditions, tire preparation/history, driver comments, and previous/current setup analysis together. It can scan a persistent user-selected folder, open the official RaceBox cloud export page without receiving account credentials, and detect a newly mounted Sanwa USB drive while still requiring the driver to confirm the matching run.
- **Session** is the normal map, playback, laps, telemetry, events, sectors, insights, and annotations workspace. Its always-visible **Race Day Run** selector clearly identifies and loads the recording being viewed; command-line/demo files are labelled as not linked to Race Day.
- **Compare** uses Reference plus Compare A, optional Compare B, and independent Playback selections.
- **Analysis** brings the deterministic Insights, disclosed Rules / Formula, and Dev Notes together without duplicating the Race Day Crew Chief.
- **Reports** summarizes the session, lap table, theoretical sectors, comparison roles, and deterministic findings with the existing export tools.

The safe default layout is locked. **LAYOUT: LOCKED / CUSTOM** deliberately enables dock and splitter editing. Theme and text size (100%, 115%, 130%, or 150%) are persistent, and errors/background status remain visible in the application header. At large text sizes or narrow widths, Theme, Layout, Annotation, and renderer status move into **TOOLS** so the five workspaces stay reachable.

## Built with Codex and GPT-5.6

RaceBox Telemetry Viewer was developed through a hands-on human/AI collaboration. The project owner supplied RC racing and mechanical expertise, real telemetry, visible troubleshooting observations, product decisions, and acceptance criteria. **OpenAI Codex powered by GPT-5.6** inspected the original React/Vite prototype, planned the native architecture, implemented and refactored the C++20 application, diagnosed synchronization and map-calibration problems, and produced the automated tests, packaging tools, developer handoff, and release documentation.

GPT-5.6 was used as an engineering reasoning partner rather than as a replacement for domain judgment. Each change followed a repeatable loop: inspect the running application, compare behavior with the recorded data, convert the owner's feedback into measurable rules, edit the smallest responsible part of the code, run the 14 Windows test targets and seven portable-core tests, rebuild, and visually verify the new executable with the real RaceBox/VBO/Sanwa session. Higher-reasoning Codex runs were especially useful for the browser-to-native migration, multi-clock telemetry alignment, distance-normalized three-lap comparison, analysis-only GPS correction, resize-safe map projection, deterministic insight evidence design, and mounting-independent IMU calibration.

The released application's viewer and deterministic Insights run locally and never upload telemetry automatically. Its optional Crew Chief calls the user's private Tailscale gateway only after **Ask Crew Chief About These Runs** or the advanced same-run lap check is clicked. Codex and GPT-5.6 were used to build and validate the product; the driver's visible insight cards remain local and are generated from disclosed formulas, thresholds, confidence gates, and measured telemetry.

The always-visible application header shows the exact packaged release number from the root `VERSION` file, making it easy to confirm that the open executable matches the latest numbered ZIP. The native UI uses a larger readable default Windows font, and the top header includes a visible **THEME: DARK / THEME: LIGHT** switch beside the workspace navigation or inside the responsive **TOOLS** menu.

## Run

The portable package is judge-ready: double-click `RaceBoxTelemetryViewer.exe` or `Start RaceBox Demo.cmd` and the bundled Richmond VBO, RaceBox CSV, and Sanwa CSV load automatically. The executable uses the demo only when all three files are present beside the packaged app and no user files were supplied. **File > Add recording to Race Day** opens the Race Day import flow, asks whether the recording was Practice, Qualifying, or Race, detects its recording date, and offers the next matching Race Day slot. **Scan saved import folder** searches a bounded directory tree and lists only recognized RaceBox, VBO, GPX, native archive, and Sanwa files. Passing paths on the command line remains the non-interactive launch path and overrides the demo.

The known Richmond fixture automatically loads the bundled clean aerial; the same native-rate Sanwa data is merged whether the files are opened from the menu, the command line, or the demo launcher.

### macOS preview

The source now includes a native Apple Silicon application bundle target. It automatically loads the same three demo files and can accept a VBO, RaceBox CSV, and optional Sanwa CSV by Finder drag/drop. Its initial read-only workspaces show the session summary, lap GPS trace, telemetry and controls, lap table, and calibrated IMU data. Build it on a Mac with `./scripts/build-macos.sh`; the script runs the portable tests and creates an unsigned ZIP. Race Day persistence, annotations, the aerial texture, Crew Chief networking, signing, and notarization remain Windows-only until their Mac adapters are completed.

The dockable workspace includes Track Map, Playback, Telemetry, Laps and Sectors, Radio Alignment, Analysis, G-G, Altitude, Insights, and Diagnostics. V2 stores its layout, preferences, cache, logs, and crash reports in `%LOCALAPPDATA%\RaceBoxTelemetryViewer\2`. On first launch it copies only the old preferences/layout when available; `.029` keeps its own folder unchanged.

## Lap roles and synchronized comparison

Compare mode has four independent roles:

- **Reference**: manually selected or automatically the fastest complete real lap;
- **Compare A**: amber trace;
- **Compare B**: red trace;
- **Playback**: the lap whose live values and moving dot are shown, without changing any comparison role.

The reference trace is green. All three comparison traces, map dots, telemetry channels, events, insight navigation, and relative-time curves use cumulative physical-distance progress, not sample index or equal elapsed time. Single Lap and Continuous modes keep their existing behavior.

The relative-time graph plots **Compare A minus Reference** and **Compare B minus Reference** at the same track distance. A positive value means that comparison lap is behind the reference; a negative value means it is ahead. Playback and cyan inspection cursors stay synchronized across the map and every graph.

## Deterministic driver analysis

`driver-analysis-v2` suggests editable corners from reference-path curvature, steering peaks, and speed troughs. Minor candidates below five percent of the strongest bend are rejected instead of filling the maximum count. Suggestions are automatically named and drawn in travel order as `T1`, `T2`, and so on; the verified Richmond layout resolves to nine significant turns. **Map settings > Auto-detect and number turns** reruns detection, and turn-label visibility is remembered. Each corner stores start, turn-in, apex, exit, and end boundaries. The engine deterministically detects sustained braking, brake release, turn-in, apex, first throttle, full throttle, and steering-correction events, then calculates:

- brake-point, turn-in, and apex timing deltas;
- minimum and exit-speed differences;
- throttle-pickup delay;
- entry-line outward deviation;
- relative-time change through each corner.

Rules are enabled and edited individually, include units and disclosed formulas, and can be reset individually or globally. Confidence uses GPS correction, satellite quality, sampling resolution, event ambiguity, and repeatability. Results below 40 are suppressed and 40-59 remain weak signals; confidence alone never makes a recommendation. Severity combines threshold exceedance with measured zone-time effect.

For every triggered candidate, v2 checks cumulative Delta-T before the action, through the corner, and at the next driver decision. It classifies the result separately as data limited, inconclusive, net loss, recovery/compensation, retained gain, or trade-off gain. A dynamic timing floor uses the larger of 0.05 s, two sample periods, and 1.5 times the session repeatability sigma. Matching complete laps are quality-screened, then a robust median, support rate, and interval determine whether the relationship is unproven, likely, or reliable. Only a reliable retained result with the guardrails satisfied becomes a technique or complete-sequence recommendation. A favorable-looking recovery after an earlier mistake is never called a gain.

Line analysis may remove one disclosed whole-lap east/north translation from a comparison calculation. **Analysis-aligned comparison traces** applies the same translation to the visible comparison polyline, playback/inspection dots, and sector markers, while the raw recording and archives remain unchanged. Map labels disclose the applied metres, and **Map settings** can switch back to raw GPS instantly. Correction above 0.75 m reduces confidence, above 1.5 m reduces it strongly, and above 3 m disables both line conclusions and visible alignment. Golden race lap R6 is raw lap 7; its roughly 1.7 m temporary drift is visibly aligned and corrected inside line metrics so it does not create a false wide-line claim. The application does not claim wheelspin without wheel-speed data; G/yaw findings are only possible-instability indicators.

## IMU motion analysis

RaceBox VBO and CSV imports retain vertical acceleration and all three gyroscope axes; these channels also survive session/lap archives and lap CSV export. The first continuous two-second block at or below 1.5 km/h is treated as the car's on-track stationary zero. Its longitudinal, lateral, vertical, and gyro medians are subtracted in the derived graphs, live readings, G-G plot, yaw calibration, and IMU detectors. The source recording, archives, and exports remain untouched. If no qualifying stationary block exists, the app discloses that acceleration remains uncalibrated instead of inventing a zero.

The **IMU** tab graphs these zero-calibrated vertical-G and X/Y/Z gyro readings at the recording's actual output rate. Sensor axes are deliberately not called roll, pitch, or yaw because the logger can be mounted at an arbitrary angle.

A separate vehicle-yaw estimate learns the mounted axis mixture from gyro motion versus GPS heading rate. It is shown only with at least 100 matched moving samples and correlation of 0.60 or better; the bundled recording calibrates at approximately `r=0.962` over 9,000 samples. Stationary samples provide gyro bias when available, with a disclosed whole-session fallback otherwise.

Deterministic indicators flag sustained low vertical load (possible airborne), a following vertical impulse (possible landing), exceptional combined loads above 3.5 g, and calibrated rotation above 220 degrees/second. Labels remain cautious because these signals cannot alone prove a crash, jump, or loss of control. Selecting **Go** opens the containing lap at the event. This layer never rewrites GPS; inertial position fusion remains disabled until device mounting and clock uncertainty are validated.

## Analysis workspace

The Telemetry window has four tabs:

- **Telemetry**: relative time, speed, lateral G, longitudinal G, throttle/brake, and steering;
- **IMU**: vertical acceleration, raw gyro axes, gated vehicle-yaw estimate, and navigable motion indicators;
- **Events**: detected events by role, lap, corner, time, distance, and progress; selecting one synchronizes playback;
- **Sectors**: physical-sector/theoretical-best timing plus corner and phase metrics.

The Insights window has three tabs:

- **Insights**: deterministic cards written in plain English first, with standard motorsport terms such as brake point, turn-in, apex, entry line, and throttle pickup included in parentheses. Each card separately explains the result, repeatability, driver advice, retained/local/prior timing, normal variation, repeated-lap support, recording quality, and evidence reasons. Selecting a card moves to the event and highlights its corner; **Export analysis JSON** writes the complete v2 evidence record;
- **Rules / Formula**: metric rules, event detector settings, editable recommendation gates, formulas, resets, and corner phase boundaries;
- **Dev Notes**: a persistent **Notes for Codex** writing area and numbered location links.

The separate **Race Day workspace** is a unified event book for Practice, Q1-Q4, single/triple A/B/C/D mains, and custom runs, and the home of the optional private Crew Chief for setup-change A/B testing. A prominent **Import Files** flow loads VBO/RaceBox/Sanwa files, asks Practice/Qualifying/Race, records the detected UTC time, fills the event date from that recording in the computer's local time zone, and assigns the data to the next empty matching slot or a selected slot. A persistent import folder defaults to Windows Downloads, can be changed to any filesystem folder, and is scanned recursively with strict file-count/depth bounds on a worker thread. CSV discovery checks real RaceBox or Sanwa headers instead of treating every CSV as telemetry. The folder picker selects only the newest primary file as a starting point; optional VBO/RaceBox/Sanwa pairing remains an explicit driver choice. **RaceBox Cloud Export** opens the official RaceBox sign-in page in the system browser. Credentials and browser cookies never enter the native app, and no undocumented cloud API or scraping is used. With **Auto-detect Sanwa on USB** enabled, a newly mounted filesystem USB volume is scanned for Sanwa headers and the newest candidate is proposed; it is never silently attached, and MTP-only devices are not claimed as supported. Every run opens on a **Run Data** tab with source-type/status rows, add/replace/remove/repair actions, **Add from saved import folder**, **Use telemetry currently open**, clear readiness messages, and **Open this run in Telemetry Viewer**. At narrow widths or enlarged text, the workspace becomes **Runs / Selected Run / Crew Chief** tabs instead of squeezing three columns. Each run also retains a fillable before-run checklist, planned changes, driver after-run notes, temperatures, tire set/compound/run count, sauce/warmer preparation, and battery context. Source updates are atomic: one file per logical type, archives remain exclusive, and replacement captures a fresh privacy-bounded identity without disturbing unrelated inputs. Replacing or removing telemetry invalidates saved comparisons that used the old source. Version-3 `.rbxday` files persist the detected run time and source identities while remaining compatible with older files. Missing attachments can be repaired only when the saved content fingerprint matches; a different recording must use Add/replace. **Ask Crew Chief About These Runs** sends two generated aligned analytics CSV documents and the entered run context to the private Tailscale gateway; original paths/files are not sent, and the raw CSV is not placed in the language-model prompt. The assistant converts verified evidence into plain crew-chief wording, separates association from causation, proposes a controlled next test, and locally retrieves up to eight relevant saved results. A completed analysis automatically creates or updates a bounded **Setup Knowledge** record containing the exact change, driver result, conditions, deterministic evidence, confidence, cautions, Crew Chief conclusion, and next test. Up to 200 records persist inside the `.rbxday` file. An advanced collapsed control preserves the older same-run Reference-versus-Compare lap question without presenting it as the primary setup workflow.

Its **setup-change dynamics analytics** use `racebox-setup-analytics-v3` to verify the same track shape after one disclosed translation, compare top-three complete-lap outcome, and calculate input/output-matched lateral response, steering needed for the same lateral load, full-throttle acceleration, per-straight top speed, straight-entry speed, acceleration attribution, cautious braking-response anomalies, overdriving/tire-scrub risk, tilt-corrected chassis-roll amount, and tilt-corrected roll-rate. It separates a faster straight caused by better corner exit or line from one caused by stronger acceleration, and flags extra steering effort without matching cornering return as possible scrub rather than a guaranteed driver mistake. Roll-rate is measured in uncapped deg/sec so repeated high values do not hit a software score ceiling; physically clipped sensors are treated as range-limited instead of trusted peaks. The preloaded OpenClaw vehicle-dynamics skill forbids direct tire-load, tire-temperature, shock-travel, roll-center, or confirmed-lock claims without the needed sensors. Exact formulas and gates are in [`docs/race-day-crew-chief.md`](docs/race-day-crew-chief.md).

The default Crew Chief address is the user's Tailscale-only
`http://100.73.60.87:18804/v1/crew-chief/chat` service. Override it with
`RACEBOX_CREW_CHIEF_URL`; deployments that enable a second client bearer token
can provide `RACEBOX_CREW_CHIEF_TOKEN`. When Tailscale or the private service is
unavailable, the app shows a connection message while every local telemetry,
graph, event, sector, IMU, and deterministic Insight feature continues to work.
The dedicated server route is `openclaw/racebox-crew-chief`, backed by
`openai/gpt-5.6-sol` with `ultra` thinking and all tools disabled.

Notes for Codex can hold setup details, tires, springs, damage, traffic, weather, deliberate test changes, requests, or questions. Select a numbered pin when the note refers to an exact UI or telemetry location, then use **Copy note for Codex** and paste the packaged note into the Codex task. The copied text includes the selected pin, surface, telemetry time, lap roles, and view mode; the native app never uploads or sends it automatically. `.rbxsession` and `.rbxlap` archives preserve lap roles, reference mode, view mode, rules, formula version, corner names and boundaries, start/finish line, notes, annotations, and map calibration. New archives use an atomic manifest-v2 format with a bounded separate workspace document while remaining backward compatible with manifest v1. Numbered review JSON can be exported and imported; a session-name mismatch is rejected to prevent misplaced pins.

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

CTest currently runs 14 Windows targets: golden/core and archive tests, IMU calibration/event fixtures, integrated driver-analysis-v2 fixtures, the outcome/reliability/recommendation classifier and golden-session integration harness, Crew Chief evidence/response contracts, Race Day v3 persistence/source identity/setup knowledge, bounded telemetry-folder and Sanwa USB discovery, DirectX 11 WARP, one-million-sample memory, telemetry plot order, multi-channel peak-preserving decimation, portable application state, deterministic source relinking, and UI-preference/layout-v4 migration. The separate `portable-core-release` preset passes seven tests without Win32, DirectX, WinHTTP, miniz storage, or the native UI. The deterministic insight evidence contract is documented in `docs/driver-analysis-v2-evidence-addon.md`; lap evidence uses `racebox-crew-chief-evidence-v1`, while Race Day setup-memory requests use `racebox-race-day-request-v3`.

The repeatable full check and portable package commands are:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/toolbox/Build-Test-Native.ps1 -Package
```

The GUI is `build/release/RaceBoxTelemetryViewer.exe`; the validation CLI is `build/release/racebox_cli.exe`. Run repeat-playback soak testing with `scripts/soak.ps1`; add `-Warp` for the Microsoft software renderer.

The portable ZIP includes `demo/session.vbo`, `demo/session.csv`, `demo/sanwa.csv`, and `Start RaceBox Demo.cmd`. These demo files are not copied beside ordinary development builds, so an unparameterized development executable still starts empty.

Golden validation can also be run directly:

```powershell
build/release/racebox_cli.exe golden/session.vbo golden/session.csv golden/sanwa.csv
```

The browser application is frozen and is not part of current development or validation. Its archived baseline remains in `reference/browser-baseline-20260712.zip` for read-only history. Telemetry, preferences, rotating logs, and crash dumps stay on the computer and are never uploaded automatically.
