# Development Codex Review Contract

`RACEBOX_ENABLE_CODEX_REVIEW` is a local-development compile option. It is ON
for `windows-release` and OFF for `windows-distribution`.

The review-enabled executable exposes **ANNOTATE**, **REFRESH REVIEW**, and F11.
After ImGui has rendered and before DX11 presentation, Refresh copies the app
back buffer through a staging texture and atomically replaces:

- `%LOCALAPPDATA%\RaceBoxTelemetryViewer\2-demo\codex-review\active-review.png`
- `%LOCALAPPDATA%\RaceBoxTelemetryViewer\2-demo\codex-review\active-review.json`

The version-1 JSON is bounded to app/capture version, capture time, viewport,
DPI/text scale/theme, center view, panel/focus/map state, selected run/lap roles,
cursor time, and normalized annotation positions/comments. It contains no raw
telemetry samples, source paths, credentials, PDFs, setup-sheet images, network
address, or command request.

This is a one-way local screenshot handoff. It creates no listener, URL, API
route, authentication method, remote-control action, or command-execution path.
Only the latest PNG/JSON pair is retained.

Release packaging scans the distribution executable and fails if it contains a
review control label, CLI/capture path, legacy annotation-review format, or the
`racebox-codex-review` contract string.
