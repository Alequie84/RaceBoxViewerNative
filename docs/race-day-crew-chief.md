# Race Day and Setup-Change Analytics

Formula contract: `racebox-setup-analytics-v3`

## Event-day model

A version-6 `.rbxday` file is a lightweight event book. It stores the
event/track/date, run order, file references, conditions, checklist, notes,
car-profile and immutable setup-revision references plus bounded snapshots,
Setup Sheet ON/OFF state, known/untracked changes, day-level conversation, and
bounded Setup Knowledge results; it does not keep every telemetry recording
resident in memory. Versions 1–5 remain readable and migrate without duplicating
legacy conversation turns.

Built-in run types are:

- Practice 1-3, with more practices addable;
- Q1-Q3, with Q4 or later qualifiers addable;
- A/B/C/D mains, each single or triple;
- arbitrary custom runs.

Every run stores:

- pre-run notes and a fillable RC-car safety/preparation checklist;
- the exact setup changes planned for that run;
- post-run driver-feel notes;
- ambient and track temperature;
- track condition;
- tire set, compound, and number of prior runs;
- sauce compound and minutes before the run;
- tire warmer minutes and temperature;
- battery pack/ID and optional voltage;
- references to VBO, RaceBox CSV, Sanwa CSV, GPX, or native session archives.
- setup-sheet state, immutable revision ancestry, and whether the physical setup
  is fully known, partially known, or awaiting reconciliation.

Race Day is the persistent left panel and Crew Chief is the persistent right
panel around the Run/Telemetry/Compare/Findings/Report center. The selected run
is always **Current**; the nearest earlier data-ready run becomes **Previous**
unless the driver overrides it. A run selection changes every visible context
together. Both runs are loaded on a worker only for analysis, converted to the
canonical aligned CSV, and released afterward. At narrow widths the panels are
temporary overlays rather than shrinking the telemetry graphs.

## Disclosure and transport

The collapsed advanced lap-to-lap Crew Chief check remains evidence-only.

## Garage, setup revisions, and PDFs

The reusable garage profile records chassis, electronics, radio, gearing, and
notes. The local setup library uses schema-versioned SQLite under
`%LOCALAPPDATA%\RaceBoxTelemetryViewer\2`. Imported PDFs are copied into a
SHA-256 content-addressed managed store; the original download location is not
needed afterward. Sources stay immutable and every saved physical setup is a
child revision. A bounded snapshot inside the event file keeps the day
understandable if the local library is missing.

With **Setup Sheet ON**, a run begins from the latest physical revision and save
creates the next immutable revision. With it OFF, free-text physical changes
carry forward as a notes-only, partially-known revision. Re-enabling exact setup
comparison requires the driver to reconcile the sheet with the real car.
PDFium renders pages and preserves AcroForm editing. Flat sheets support one-time
driver-confirmed rectangles, labels, and units; uncertain proposed mappings are
never accepted silently.

`racebox-race-day-request-v4` carries exact structured values and their diff.
Only the explicit Previous-versus-Current action may additionally render up to
two pages per run as bounded PNG/JPEG evidence under
`racebox-setup-sheet-vision-v1`. No PDF bytes or local paths leave the Viewer,
the gateway retains no image data, and structured values win over a visual
reading. The registered OpenClaw lane must pass a real red-image capability
probe before images are enabled; otherwise the editor and structured comparison
continue and the UI reports vision unavailable.

## Continuous Race Day conversation

The Crew Chief panel keeps one day-level timeline while the center view changes.
It filters Whole day or Selected run; every message stores a stable ID,
timestamp, origin, linked run IDs, and optional Previous-to-Current comparison
ID. The composer states whether it is sending Current only or Previous and
Current. The single-run request uses
`racebox-session-chat-request-v1` and includes one bounded
`racebox-session-analytics-csv-v1` document covering the complete recording.
The gateway reduces that CSV to `racebox-session-review-v5` before a language
model is called.

The single-run review contains every chronological lap or segment, its phase,
duration, average and maximum speed, 90th-percentile absolute lateral G,
longitudinal-G ranges, available steering/throttle/brake summaries, cautious
braking indicators, best/median lap deltas, consistency, and first-third versus
last-third trends. It also checks the whole run for a one-off abrupt near-stop
that is unusually slow compared with other laps at the same track progress.
When one clears the cautious threshold, the review calculates recent pre-event
pace, the incident-lap delta, every later complete lap, and the cumulative
impact-through-finish delta. Out-laps and in-laps are never presented as
complete laps.
Review v3 additionally builds one-percent progress profiles across all complete
laps and finds repeatable corner zones from the chassis lateral-G pattern. It
compares the fastest complete lap with the median of the other complete laps,
including corner time, entry/minimum/exit speed, GPS line difference, and where
GPS speed began falling before the apex. One whole-lap east/north translation
per lap is removed only for the line comparison; the original telemetry is not
changed. A local path shift above three metres is withheld as an alignment
outlier. A different line is not automatically a better line or proof of why
the lap was faster.
The model receives the calculated review and entered Race Day/session context;
it does not receive the raw CSV, original files, or local paths.

If Sanwa coverage is absent or incomplete, the review still uses RaceBox lap
timing, speed, GPS/progress, chassis G, and available calibrated yaw. It says
that steering, throttle, and brake commands are unavailable and forbids
inventing them. In this case the corner onset is described only as
speed-derived slowing--where GPS speed began falling--and never as a brake
trigger point. An unexplained abrupt stop is called only a possible incident;
Crew Chief asks whether it was contact, a spin/flip, traffic, marshalling, or an
intentional stop before assigning a cause. If the driver already reported an
impact or damage, the report can connect that context to the measured
before/after timing while retaining the causality warning. Single-run chat can
explain what happened across the run, but it cannot prove a setup cause,
measured tire temperature, tire load, suspension travel, or wheel lock. The
adaptive router caps this task below Ultra; deeper intelligence cannot replace
a controlled A/B comparison.

Review v4 adds the fastest lap's minimum, average, and exit speed differences
against the median of the other complete laps, plus speed-derived slowing time
before the apex and an inside/outside GPS-path description when alignment quality
supports it. For questions such as **what did I do best?** or **how can I go
faster?**, Crew Chief must explain what changed and what happened afterward, then
give one steering and one throttle/lift/brake instruction with a measurable
next-lap checkpoint. It says `began turning left/right into the corner`, not
`started steering` or a raw `(turn-in)` label. A single comparison is described
as an association, not proof that the input caused the result.

When Reference and Compare laps are selected in Viewer, Session chat also sends
the existing local `racebox-driver-analysis-v2` evidence for those laps inside
the open session context. The packet is distance-aligned and contains each
corner's event/relative-time metrics plus measured steering, throttle, brake,
speed, calibrated lateral/longitudinal/vertical G, and yaw summaries for both
laps and their differences. Retention, repeated-lap support, confidence, GPS
translation, and interpretation limits travel with the same packet. No source
path is included. This is optional context within the existing Session and
companion request contracts; it does not replace the whole-run chronological
review.

## Shared Viewer and phone session

The default Session panel can promote that same processed review and the
comparison selected when pairing begins into a
`racebox-companion-session-v1` room on the user's configured gateway. The gateway parses
the analytics CSV once, stores only the processed review and complete bounded
transcript in local SQLite, and discards the raw CSV. A five-minute one-use QR
or manual code gives the phone a revocable session-scoped token; it does not
expose the OpenClaw credential or another run.

Viewer and phone synchronize by stable message ID and cursor. Questions create
jobs with queued, thinking, completed, or failed state, allowing either client
to reconnect without duplicating a submission. Conversation is retained until
the driver explicitly clears it. Clear first creates a timestamped JSON backup.
The model receives recent turns plus a bounded summary of older context, while
the full transcript remains in storage.
Each shared assistant message contains the summary, measured observations,
limitations, next test, and causality note, so the phone and Viewer do not lose
the useful evidence behind a short headline.

The `.rbxday` version-6 day transcript stores ID, timestamp, role, content,
origin, linked run IDs, and comparison ID for up to 2,000 turns. Versions 1–5
migrate with generated stable legacy IDs and de-duplicate existing per-run
turns. Reaching 2,000 stops further saving until export or explicit clear; no
old turn is silently dropped.

Race-day analysis is a separate explicit-disclosure action. When the user
clicks **Analyze Previous vs Current**, the native app sends:

- two generated `racebox-session-analytics-csv-v1` documents;
- the entered event/run labels, conditions, checklist, and notes;
- tire preparation, car profile, setup revisions, structured field diff, and
  known or untracked physical changes;
- the user's question.

The original files and file paths are not sent. The private Tailscale gateway
parses the CSV in memory and sends only calculated analytics plus the entered
context to the GPT-5.6 Crew Chief lane. The raw CSV is not placed in the model
prompt.

## Setup Knowledge

After a successful previous/current analysis, the app automatically creates or
updates one local result identified by the compared run IDs, question, setup
change, and driver result. Each record stores:

- event, track, run labels, UTC creation time, and the handling question;
- the exact setup change and post-run driver feel;
- previous/current conditions, tires, preparation, and battery context;
- verdict, confidence, explanation, confounds, and controlled next test;
- formula version and a compact deterministic snapshot of lap outcome, matched
  lateral response, steering required for lateral load, matched full-throttle
  acceleration, straight top-speed attribution, braking-response indicators,
  overdriving/tire-scrub risk, tilt-corrected chassis-roll signature and
  roll-rate signature,
  and comparable-track quality;
- the disclosed OpenClaw model, thinking level, and selected lane.

Records never contain telemetry samples, attachment paths, credentials, or
unrestricted agent memory. The file keeps at most 200 results. Before any Race
Day analysis or ordinary Crew Chief chat, the app deterministically ranks local
records by track, handling/setup terms, tire/condition match, confidence, and
recency. Only the top eight bounded records are sent. The model must cite their
record IDs and treat them as historical associations, not universal truths.

## Intelligence validation

The trusted test baseline for setup-memory reasoning is
`openai/gpt-5.6-sol` at `xhigh` on the correlation lane. The live benchmark
replays the identical evidence and saved history through Luna low and Terra
medium/high. A lower lane is approved only when it preserves the xhigh verdict,
confidence band, evidence IDs, causality boundary, and controlled next test.
Ultra is not the baseline and remains benchmark-gated.

## Comparable-track gate

Every sample includes physical cumulative lap progress plus raw latitude and
longitude. The gateway compares median GPS position in five-percent progress
bins. It removes one disclosed whole-run east/north translation for this check:

```text
offset = median(current_position - previous_position)
residual_rms = RMS((current_position - previous_position) - offset)
```

At least ten shared progress bins are required. Residual RMS above `5 m`
rejects the comparison as a different/mismatched layout. This accepts the known
whole-lap translation behavior without rewriting raw GPS.

## Whole-run outcome

For each run:

```text
run outcome = median(fastest three complete lap times)
delta = current outcome - previous outcome
```

Negative delta is faster. The spread of those three laps is disclosed. A local
dynamics change is not a retained gain when the repeated whole-run outcome is
slower or within ordinary spread.

## Lateral-response indicator

Eligible samples are complete-lap samples with:

- speed at least `8 km/h`;
- absolute steering at least `8%`;
- brake at most `5%`;
- valid aligned Sanwa inputs.

Samples are matched into cells of:

- `2%` physical lap progress;
- `3 km/h` speed;
- `5%` absolute steering input.

For each cell with at least three samples in both runs:

```text
cell delta = median(abs(lateral G) current) - median(abs(lateral G) previous)
cell weight = min(previous samples, current samples, 100)
matched delta = sum(cell delta * cell weight) / sum(cell weight)
```

The indicator requires at least ten shared cells and a matched weight of 40.
A positive value means more **chassis lateral response at the same part of the
track, speed, and steering input**. It is not direct tire load and does not by
itself prove more side grip.

## Forward-bite indicator

Speed-derived acceleration is:

```text
acceleration_g = ((speed_current - speed_previous) / 3.6) / delta_time / 9.80665
```

Eligible samples require:

- throttle at least `90%`;
- brake at most `5%`;
- absolute steering at most `20%`;
- speed at least `5 km/h`;
- `15-250 ms` between adjacent samples in the same complete lap.

Cells match `4%` physical progress, `5 km/h` speed, and `10%` absolute steering.
The weighted-median-difference formula is the same as lateral response and
requires at least five cells / 25 matched weight. A positive value is a
candidate for stronger forward acceleration. Battery, gearing, line, surface,
and tire condition remain alternative explanations.

## Straight top-speed attribution

The native v2 summary separates each lap's strongest low-steering full-throttle
straight into:

- straight-entry speed;
- straight top speed;
- speed-derived acceleration across the straight.

If top speed improves and most of the gain is already present at straight
entry, the app labels it `exit_or_line_driven`. If entry speed is similar but
straight acceleration improves, it labels it `forward_bite_or_power_delivery`.
If exit is worse but acceleration recovers later, it labels the result
`recovery_after_poor_exit`, not a clean corner gain. Later braking can raise
terminal speed and must be discussed as braking commitment, not acceleration.

## Corner balance indicators

The v2 summary tracks input/output balance across complete-lap cornering
samples:

- lateral G per steering input;
- steering required for the same lateral load;
- yaw rate per steering input when IMU/GPS yaw calibration is valid.

More yaw per steering can support "more rotation." More steering required for
similar lateral load supports "more steering demand" or possible push/fade,
not more grip by itself.

## Overdriving / tire-scrub risk

The v3 summary adds a driver-effort guardrail. It does not blame the driver; it
looks for the car being asked for more than the tire is giving back.

Eligible samples require:

- complete-lap samples;
- speed at least `8 km/h`;
- absolute steering at least `15%`;
- brake at most `15%`;
- valid Sanwa steering/brake.

For each eligible sample the app calculates:

```text
efficient_response = 75th percentile(abs(lateral G) / abs(steering))
expected_lateral = efficient_response * abs(steering)
weak_response = clamp((expected_lateral - abs(lateral G)) / expected_lateral)
steering_stress = clamp((abs(steering) - 25) / 50)
scrub_decel = clamp(no-brake speed-derived deceleration G / 0.25)
correction = 1 when steering changes at least 15% sample-to-sample

overdriving_score =
  100 * steering_stress *
  (0.50 * weak_response + 0.35 * scrub_decel + 0.15 * correction)
```

The run-level index is the 75th percentile score. The risk-sample percentage is
the share of samples with score at least `20`. A current-minus-previous index
change of `+8` or risk-sample change of `+6%` is flagged. If this happens while
the top-three lap outcome is not meaningfully faster, the status is
`more_overdriving_no_lap_gain`. If the second half of the current run rises by
`+10` score points, it is a late-run tire-scrub risk.

This is a **possible scrub / pushing past the tire window** signal. It is not
measured tire temperature, not proof of driver fault, and not a power-sag
diagnosis.

## Tilt-corrected chassis-roll signature

The v3 summary also estimates whether the chassis attitude changed between
runs. First it learns an asphalt/sensor tilt reference:

1. Use stopped samples at or below `1.5 km/h` when at least 25 samples exist.
2. Otherwise use straight samples with speed at least `8 km/h`, absolute
   lateral G at most `0.20 g`, steering at most `5%`, and brake at most `5%`.
3. If neither reference exists, the roll result is `data_limited`.

For raw lateral and vertical G:

```text
surface_roll_deg = median(atan2(raw_lateral_g, abs(raw_vertical_g)))
corner_roll_signature =
  median(abs(atan2(raw_lateral_g, abs(raw_vertical_g)) - surface_roll_deg))
roll_per_lateral_g = corner_roll_signature / max(0.35, abs(lateral G))
```

The status changes at about `0.75 deg` run-to-run. If the two runs' surface
tilt references differ by `2 deg` or more, the result is marked
`surface_tilt_changed_check_reference` instead of pretending the comparison is
clean.

This is a **chassis roll/loading signature**, not exact shock travel,
roll-center height, tire load, or tire temperature.

## Tilt-corrected roll-rate signature

Roll-rate answers a different question from roll amount. Roll amount asks
**how far the chassis loading signature moved**. Roll-rate asks **how quickly
that loading signature built** as the car took a set.

The analyzer reuses the same stopped-point or straight-line surface reference,
then calculates a signed corrected roll angle for every valid sample:

```text
corrected_roll_angle_deg =
  atan2(raw_lateral_g, abs(raw_vertical_g)) - surface_roll_deg
```

The corrected angle is smoothed over a `200 ms` local window. During complete
lap cornering samples with speed at least `8 km/h`, steering at least `15%`,
brake at most `15%`, and lateral G at least `0.35 g`, the rate is:

```text
roll_rate_dps =
  (smoothed_roll_angle_after - smoothed_roll_angle_before) / delta_time_s
```

The derivative window is about `160 ms`. The run value is the 90th percentile
absolute roll-rate in `deg/s`, plus the same value normalized by lateral G.
The current run is flagged as `faster_roll_build` or `slower_roll_build` when
the delta clears:

```text
threshold = max(8 deg/s, 15% of previous p90 roll-rate)
```

This range is intentionally not capped to a `0-100` score. If a real sensor
clips at its physical limit, software cannot recover the missing peak; the
right conclusion is that the recording is range-limited and needs a higher
range sensor/configuration before trusting peak claims.

Racing meaning: faster roll build can mean the car takes a set quicker or loads
the outside tires faster. It is not proof that a droop screw, shock oil, spring,
roll center, or tire temperature caused the change unless repeated A/B data
supports that connection.

## Possible braking lockup / low-grip indicator

The detector first requires at least `80 ms` of:

- brake command at least `70%`;
- throttle at most `5%`;
- speed at least `10 km/h`.

It then measures the delay from brake command to at least `0.20 g` of
speed-derived deceleration, plus the peak deceleration reached while brake
remains high. A delay of `80 ms` or more, missing decel response, or weak peak
deceleration counts as a possible brake/low-grip indicator. It separately
reports whether the same event includes any of:

- yaw at least `45 deg/s`;
- absolute lateral G at least `0.45 g`;
- steering range at least `20%`.

This is always called a **possible lockup or low-grip indicator**. Chassis
GNSS/IMU plus brake command cannot confirm which tire locked. Confirmation
requires individual wheel-speed data.

## Recommendation gate

The Crew Chief must separate:

1. measured difference;
2. whole-run outcome;
3. repeatability beyond normal spread;
4. confounds;
5. causality;
6. one controlled next test.

One previous/current pair can support a candidate association, not causation.
The preferred confirmation is an A/B/A or B/A/B sequence with at least three
complete clean laps per run while holding tire run count/preparation, battery,
track temperature, traffic, and damage state as constant as practical.
