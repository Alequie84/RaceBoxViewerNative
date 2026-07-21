#pragma once

#include "racebox/telemetry_plot_order.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace racebox::app {

inline constexpr int kCurrentUiLayoutVersion = 3;

enum class TelemetryDensity {
    Compact,
    Detailed,
};

enum class UiWorkspace {
    Overview,
    Analysis,
    Compare,
    Sectors,
};

struct UtilityPanelPreferences {
    bool radio_alignment{};
    bool theoretical_analysis{};
    bool diagnostics{};
    bool gg_plot{};
    bool altitude{};
};

struct WorkspacePreferences {
    UiWorkspace active{UiWorkspace::Overview};
    TelemetryDensity telemetry_density{TelemetryDensity::Compact};
    UtilityPanelPreferences utility_panels;
};

// Defaults are deliberately safe for a fresh install: dark, metric, calibrated
// map aids enabled, a compact Overview workspace, and optional utility windows
// hidden until the user asks for them.
struct UiPreferences {
    bool light_theme{};
    bool metric_units{true};
    bool speed_coloured_map{true};
    bool average_track_guide{true};
    bool show_turn_numbers{true};
    bool show_map_grid{true};
    float map_grid_spacing_m{10.0F};
    bool separate_compare_maps{};
    int layout_version{kCurrentUiLayoutVersion};
    std::vector<TelemetryPlotId> telemetry_plot_order{default_telemetry_plot_order()};
    WorkspacePreferences workspace;
};

struct UiPreferencesLoadResult {
    UiPreferences preferences;
    bool loaded_from_disk{};
    bool used_defaults{true};
    bool layout_rebuild_required{true};
    int source_layout_version{};
    std::string warning;
};

struct LayoutMigrationPreparation {
    bool rebuild_required{};
    bool safe_to_rebuild{true};
    bool backup_created{};
    std::filesystem::path backup_path;
    std::string error;
};

// Missing and malformed files return usable defaults and never throw.
UiPreferencesLoadResult load_ui_preferences(const std::filesystem::path& path) noexcept;

// Writes beside the destination and atomically replaces preferences.json. The
// caller receives false on failure and the old destination is left untouched.
bool save_ui_preferences_atomic(const std::filesystem::path& path, const UiPreferences& preferences,
                                std::string* error = nullptr) noexcept;

// Before rebuilding an older dock layout, preserve layout.ini exactly once as
// layout-pre-v3.ini. The original remains in place until DockBuilder succeeds.
LayoutMigrationPreparation prepare_layout_v3_migration(const std::filesystem::path& settings_directory,
                                                        int source_layout_version) noexcept;

}  // namespace racebox::app
