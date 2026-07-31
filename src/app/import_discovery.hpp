#pragma once

#include "racebox/race_day.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace racebox::app {

struct DiscoveredTelemetryFile {
    std::filesystem::path path;
    race_day::TelemetrySourceKind kind{
        race_day::TelemetrySourceKind::Unsupported};
    std::uintmax_t size_bytes{};
    std::filesystem::file_time_type modified_at{};
};

struct TelemetryFolderScan {
    std::filesystem::path root;
    std::vector<DiscoveredTelemetryFile> files;
    std::size_t files_examined{};
    bool truncated{};
    std::string error;
};

// Scans a user-selected directory without following symlinks. Search depth and
// file count are bounded so a removable drive or Downloads folder cannot keep
// the UI worker busy indefinitely.
TelemetryFolderScan scan_telemetry_folder(
    const std::filesystem::path& root,
    bool sanwa_only = false,
    std::size_t max_files_examined = 4'000,
    std::size_t max_depth = 5) noexcept;

// Returns only filesystem-mounted removable volumes. MTP-only phones and
// devices are intentionally excluded because they do not provide stable paths.
std::vector<std::filesystem::path> removable_drive_roots() noexcept;

std::string discovered_file_time_label(
    std::filesystem::file_time_type modified_at);

}  // namespace racebox::app
