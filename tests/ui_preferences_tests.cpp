#include "ui_preferences.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / ("racebox-ui-preferences-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace

int main() {
    using namespace racebox::app;
    bool passed = true;
    TemporaryDirectory temporary;
    const auto preferences_path = temporary.path() / "preferences.json";

    const auto missing = load_ui_preferences(preferences_path);
    passed &= expect(!missing.loaded_from_disk && missing.used_defaults, "missing preferences use defaults");
    passed &= expect(missing.preferences.layout_version == kCurrentUiLayoutVersion, "fresh default is layout v3");
    passed &= expect(missing.preferences.workspace.telemetry_density == TelemetryDensity::Compact,
                     "fresh telemetry density is compact");
    passed &= expect(!missing.preferences.workspace.utility_panels.diagnostics,
                     "optional utility panels default hidden");
    passed &= expect(missing.preferences.show_turn_numbers, "turn numbers default visible");
    passed &= expect(missing.preferences.analysis_aligned_map_traces,
                     "analysis-aligned comparison traces default visible");

    {
        std::ofstream legacy(preferences_path);
        legacy << R"({
          "light_theme": true,
          "metric_units": false,
          "speed_coloured_map": false,
          "average_track_guide": false,
          "show_map_grid": false,
          "map_grid_spacing_m": 17.0,
          "separate_compare_maps": true,
          "layout_version": 2,
          "telemetry_plot_order": ["steering", "speed", "steering", "unknown"]
        })";
    }
    const auto legacy = load_ui_preferences(preferences_path);
    passed &= expect(legacy.loaded_from_disk && !legacy.used_defaults, "legacy preferences load");
    passed &= expect(legacy.layout_rebuild_required && legacy.source_layout_version == 2,
                     "v2 requests a v3 dock rebuild");
    passed &= expect(legacy.preferences.layout_version == 2, "loader does not mark migration complete early");
    passed &= expect(legacy.preferences.light_theme && !legacy.preferences.metric_units,
                     "legacy theme and unit fields are preserved");
    passed &= expect(!legacy.preferences.speed_coloured_map && !legacy.preferences.average_track_guide &&
                         !legacy.preferences.show_map_grid && legacy.preferences.separate_compare_maps,
                     "legacy map flags are preserved");
    passed &= expect(legacy.preferences.show_turn_numbers, "legacy preferences gain visible turn numbers safely");
    passed &= expect(legacy.preferences.map_grid_spacing_m == 17.0F, "legacy grid spacing is preserved");
    passed &= expect(legacy.preferences.telemetry_plot_order.front() == TelemetryPlotId::Steering &&
                         legacy.preferences.telemetry_plot_order[1] == TelemetryPlotId::Speed &&
                         legacy.preferences.telemetry_plot_order.size() == kDefaultTelemetryPlotOrder.size(),
                     "plot order is normalized without duplicates or missing plots");

    {
        std::ofstream invalid(preferences_path, std::ios::trunc);
        invalid << "{ definitely not JSON";
    }
    const auto invalid = load_ui_preferences(preferences_path);
    passed &= expect(invalid.used_defaults && !invalid.loaded_from_disk && !invalid.warning.empty(),
                     "malformed JSON safely restores defaults with a warning");
    passed &= expect(invalid.preferences.metric_units && !invalid.preferences.light_theme,
                     "malformed JSON cannot leak partial settings");

    UiPreferences saved;
    saved.light_theme = true;
    saved.metric_units = false;
    saved.map_grid_spacing_m = 999.0F;
    saved.show_turn_numbers = false;
    saved.workspace.active = UiWorkspace::Compare;
    saved.workspace.telemetry_density = TelemetryDensity::Detailed;
    saved.workspace.utility_panels.altitude = true;
    saved.workspace.utility_panels.theoretical_analysis = true;
    saved.analysis_aligned_map_traces = false;
    saved.telemetry_plot_order = {TelemetryPlotId::Steering, TelemetryPlotId::Speed};
    std::string save_error;
    passed &= expect(save_ui_preferences_atomic(preferences_path, saved, &save_error), "atomic preference save succeeds");
    passed &= expect(save_error.empty(), "successful atomic save clears the error");
    passed &= expect(!std::filesystem::exists(preferences_path.wstring() + L".tmp"),
                     "atomic save leaves no temporary file");

    const auto round_trip = load_ui_preferences(preferences_path);
    passed &= expect(round_trip.loaded_from_disk && round_trip.preferences.layout_version == 3,
                     "saved v3 preferences load without migration");
    passed &= expect(round_trip.preferences.workspace.active == UiWorkspace::Compare &&
                         round_trip.preferences.workspace.telemetry_density == TelemetryDensity::Detailed &&
                         round_trip.preferences.workspace.utility_panels.altitude &&
                         round_trip.preferences.workspace.utility_panels.theoretical_analysis,
                     "workspace, density, and utility visibility round trip");
    passed &= expect(round_trip.preferences.map_grid_spacing_m == 25.0F, "unsafe grid spacing is clamped");
    passed &= expect(!round_trip.preferences.show_turn_numbers, "turn-number visibility round trips");
    passed &= expect(!round_trip.preferences.analysis_aligned_map_traces,
                     "analysis-aligned trace visibility round trips");
    passed &= expect(round_trip.preferences.telemetry_plot_order.front() == TelemetryPlotId::Steering &&
                         round_trip.preferences.telemetry_plot_order[1] == TelemetryPlotId::Speed &&
                         round_trip.preferences.telemetry_plot_order.size() == kDefaultTelemetryPlotOrder.size(),
                     "incomplete saved plot order is normalized");

    const auto layout_path = temporary.path() / "layout.ini";
    {
        std::ofstream layout(layout_path);
        layout << "legacy-layout-v2";
    }
    const auto first_migration = prepare_layout_v3_migration(temporary.path(), 2);
    passed &= expect(first_migration.rebuild_required && first_migration.safe_to_rebuild && first_migration.backup_created,
                     "first v2 migration creates a safe backup");
    passed &= expect(read_text(first_migration.backup_path) == "legacy-layout-v2", "layout backup preserves exact content");

    {
        std::ofstream changed(layout_path, std::ios::trunc);
        changed << "changed-after-backup";
    }
    const auto repeated_migration = prepare_layout_v3_migration(temporary.path(), 2);
    passed &= expect(repeated_migration.safe_to_rebuild && !repeated_migration.backup_created,
                     "repeated migration reuses the one-time backup");
    passed &= expect(read_text(repeated_migration.backup_path) == "legacy-layout-v2",
                     "one-time backup is never overwritten");

    const auto current_layout = prepare_layout_v3_migration(temporary.path(), 3);
    passed &= expect(!current_layout.rebuild_required && current_layout.safe_to_rebuild,
                     "current v3 layout requires no migration");

    if (!passed) return 1;
    std::cout << "UI preference and layout migration tests passed\n";
    return 0;
}
