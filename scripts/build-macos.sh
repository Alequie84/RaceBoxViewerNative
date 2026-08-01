#!/usr/bin/env bash
set -euo pipefail

export PATH="$HOME/.local/bin:$PATH"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This script must run on macOS with Xcode installed." >&2
  exit 2
fi

xcrun --find clang++ >/dev/null 2>&1 || { echo "Apple Command Line Tools are required. Run: xcode-select --install" >&2; exit 2; }
command -v cmake >/dev/null || { echo "CMake 3.25 or newer is required." >&2; exit 2; }

root="$(cd "$(dirname "$0")/.." && pwd)"
version="$(tr -d '[:space:]' < "$root/VERSION")"
build="$root/build/macos-release"
out="$root/out"

cmake --preset macos-release
cmake --build --preset macos-release
ctest --preset macos-release
mkdir -p "$out"
ditto -c -k --sequesterRsrc --keepParent \
  "$build/RaceBoxTelemetryViewer.app" \
  "$out/RaceBoxTelemetryViewer-${version}-macos-arm64-unsigned.zip"
shasum -a 256 "$out/RaceBoxTelemetryViewer-${version}-macos-arm64-unsigned.zip"
