# macOS preview build

The macOS target is the first native Mac milestone. It uses the same C++20
telemetry parsers, lap logic, Sanwa alignment and IMU analysis as Windows, with
an SDL3 window/input layer and Apple's Metal renderer for Dear ImGui and ImPlot.
It is a native application and does not embed Chromium or Node.js.

## Current scope

- Loads the bundled VBO, RaceBox CSV and Sanwa CSV demo automatically.
- Accepts a VBO, RaceBox CSV and optional Sanwa CSV together by Finder drag/drop
  or as command-line arguments.
- Shows overview, selected-lap GPS trace, telemetry, laps and calibrated IMU.
- Supports native high-DPI resizing and dark/light appearance.

This first preview is intentionally read-only. Race Day persistence, image map
textures, annotations, Crew Chief networking, Keychain storage, signing and
notarization remain disabled until their macOS service adapters are completed.

## Requirements

- Apple Silicon Mac running macOS 13 or newer.
- Apple Command Line Tools (`xcode-select --install`). The full Xcode app is
  not required for this preview.
- CMake 3.25 or newer.
- Internet access for the first configure so CMake can fetch the pinned SDL3,
  Dear ImGui, ImPlot and JSON source dependencies.

No Mac toolchain or binary has been claimed as verified from Windows. Run the
following commands on the Mac:

```bash
./scripts/build-macos.sh
```

The script configures, compiles, runs the seven platform-neutral tests, and
creates an unsigned ZIP under `out/`. To run without packaging:

```bash
open build/macos-release/RaceBoxTelemetryViewer.app
```

An unsigned local build may need Control-click, Open on its first launch. A
publicly distributed build will require an Apple Developer ID, signing,
notarization and a clean-machine Gatekeeper test.

## Hosted build fallback

If Apple Software Update does not offer Command Line Tools and Apple Developer
downloads are temporarily unavailable, the manual GitHub Actions workflow
`.github/workflows/macos-preview.yml` builds on a fresh Apple-silicon
`macos-15` runner. It runs the same portable tests, validates the application
bundle and bundled demo inputs, and uploads an unsigned ZIP plus its SHA256 as
a private workflow artifact. It never publishes a GitHub release automatically.
