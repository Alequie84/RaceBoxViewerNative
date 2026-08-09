#pragma once

#include "racebox/telemetry_plot_order.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace racebox::app {

inline constexpr int kCurrentUiLayoutVersion = 6;

enum class GraphDisplayMode {
    Paired,
    AllSeparate,
};

struct GraphColor {
    float red{};
    float green{};
    float blue{};
    float alpha{1.0F};

    auto operator<=>(const GraphColor&) const = default;
};

struct ChannelPalette {
    GraphColor speed{0.18F, 0.55F, 0.95F, 1.0F};
    GraphColor lateral_g{0.96F, 0.65F, 0.12F, 1.0F};
    GraphColor steering{0.64F, 0.40F, 0.94F, 1.0F};
    GraphColor longitudinal_g{0.08F, 0.70F, 0.66F, 1.0F};
    GraphColor controls{0.96F, 0.38F, 0.32F, 1.0F};
};

struct ComparisonPalette {
    GraphColor reference{0.20F, 0.78F, 0.40F, 1.0F};
    GraphColor compare_a{1.0F, 0.68F, 0.12F, 1.0F};
    GraphColor compare_b{0.95F, 0.25F, 0.28F, 1.0F};
};

enum class TelemetryDensity {
    Compact,
    Detailed,
};

enum class UiWorkspace {
    Run,
    Telemetry,
    Compare,
    Findings,
    Report,
};

struct UtilityPanelPreferences {
    bool radio_alignment{};
    bool theoretical_analysis{};
    bool diagnostics{};
    bool gg_plot{};
    bool altitude{};
};

struct WorkspacePreferences {
    UiWorkspace active{UiWorkspace::Telemetry};
    TelemetryDensity telemetry_density{TelemetryDensity::Compact};
    bool compare_b_enabled{};
    bool customize_layout{};
    bool race_day_panel_open{true};
    bool crew_chief_panel_open{true};
    bool panel_focus{};
    bool graph_focus{};
    float race_day_panel_width{320.0F};
    float crew_chief_panel_width{380.0F};
    float map_height_ratio{0.32F};
    UtilityPanelPreferences utility_panels;
};

// Defaults are deliberately safe for a fresh install: light, metric, calibrated
// map aids enabled, a compact Session workspace, and optional utility windows
// hidden until the user asks for them.
struct UiPreferences {
    bool light_theme{true};
    bool metric_units{true};
    bool speed_coloured_map{true};
    bool average_track_guide{true};
    bool show_turn_numbers{true};
    bool show_map_grid{true};
    float map_grid_spacing_m{10.0F};
    bool separate_compare_maps{};
    bool analysis_aligned_map_traces{true};
    std::filesystem::path telemetry_import_folder;
    std::vector<std::filesystem::path> recent_session_files;
    std::vector<std::filesystem::path> recent_race_day_files;
    bool auto_detect_sanwa_usb{true};
    float text_scale{1.0F};
    int layout_version{kCurrentUiLayoutVersion};
    std::vector<TelemetryPlotId> telemetry_plot_order{default_telemetry_plot_order()};
    std::vector<TelemetryPlotId> compare_plot_order{default_telemetry_plot_order(true)};
    GraphDisplayMode telemetry_graph_mode{GraphDisplayMode::AllSeparate};
    GraphDisplayMode compare_graph_mode{GraphDisplayMode::AllSeparate};
    ChannelPalette channel_palette;
    ComparisonPalette comparison_palette;
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
// layout-pre-v6.ini. The original remains in place until DockBuilder succeeds.
LayoutMigrationPreparation prepare_layout_v6_migration(const std::filesystem::path& settings_directory,
                                                        int source_layout_version) noexcept;

}  // namespace racebox::app
