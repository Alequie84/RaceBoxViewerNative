#include "ui_preferences.hpp"

#include <nlohmann/json.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <system_error>

namespace racebox::app {
namespace {

using Json = nlohmann::json;

bool boolean_or(const Json& object, const char* key, bool fallback) {
    const auto value = object.find(key);
    return value != object.end() && value->is_boolean() ? value->get<bool>() : fallback;
}

std::filesystem::path path_or(
    const Json& object,
    const char* key,
    const std::filesystem::path& fallback) {
    const auto value = object.find(key);
    if (value == object.end() || !value->is_string()) return fallback;
    const auto& encoded = value->get_ref<const std::string&>();
    if (encoded.empty() || encoded.size() > 8'192) return {};
    return std::filesystem::u8path(encoded);
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(encoded.data()),
        encoded.size(),
    };
}

float grid_spacing_or(const Json& object, float fallback) {
    const auto value = object.find("map_grid_spacing_m");
    if (value == object.end() || !value->is_number()) return fallback;
    return std::clamp(value->get<float>(), 5.0F, 25.0F);
}

float normalize_text_scale(float value) {
    constexpr std::array supported{1.0F, 1.15F, 1.30F, 1.50F};
    const auto requested = std::clamp(value, supported.front(), supported.back());
    return *std::min_element(supported.begin(), supported.end(), [requested](float left, float right) {
        return std::abs(left - requested) < std::abs(right - requested);
    });
}

float text_scale_or(const Json& object, float fallback) {
    const auto value = object.find("text_scale");
    return value != object.end() && value->is_number()
        ? normalize_text_scale(value->get<float>()) : fallback;
}

int layout_version_or(const Json& object, int fallback) {
    const auto value = object.find("layout_version");
    if (value == object.end() || !value->is_number_integer()) return fallback;
    return std::max(0, value->get<int>());
}

TelemetryDensity density_or(const Json& object, TelemetryDensity fallback) {
    const auto value = object.find("telemetry_density");
    if (value == object.end() || !value->is_string()) return fallback;
    const auto& name = value->get_ref<const std::string&>();
    if (name == "compact") return TelemetryDensity::Compact;
    if (name == "detailed") return TelemetryDensity::Detailed;
    return fallback;
}

UiWorkspace workspace_or(const Json& object, UiWorkspace fallback) {
    const auto value = object.find("active");
    if (value == object.end() || !value->is_string()) return fallback;
    const auto& name = value->get_ref<const std::string&>();
    if (name == "session" || name == "overview") return UiWorkspace::Session;
    if (name == "compare") return UiWorkspace::Compare;
    if (name == "crew_chief" || name == "analysis") return UiWorkspace::CrewChief;
    if (name == "reports" || name == "sectors") return UiWorkspace::Reports;
    if (name == "race_day") return UiWorkspace::RaceDay;
    return fallback;
}

const char* density_name(TelemetryDensity density) {
    return density == TelemetryDensity::Detailed ? "detailed" : "compact";
}

const char* workspace_name(UiWorkspace workspace) {
    switch (workspace) {
        case UiWorkspace::Session: return "session";
        case UiWorkspace::Compare: return "compare";
        case UiWorkspace::CrewChief: return "crew_chief";
        case UiWorkspace::Reports: return "reports";
        case UiWorkspace::RaceDay: return "race_day";
    }
    return "session";
}

std::string windows_error_message(DWORD error) {
    return std::system_category().message(static_cast<int>(error));
}

}  // namespace

UiPreferencesLoadResult load_ui_preferences(const std::filesystem::path& path) noexcept {
    UiPreferencesLoadResult result;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return result;

        const auto value = Json::parse(input);
        if (!value.is_object()) {
            result.warning = "Preferences root was not an object; safe defaults restored";
            return result;
        }

        result.loaded_from_disk = true;
        result.used_defaults = false;
        auto& preferences = result.preferences;
        preferences.light_theme = boolean_or(value, "light_theme", preferences.light_theme);
        preferences.metric_units = boolean_or(value, "metric_units", preferences.metric_units);
        preferences.speed_coloured_map = boolean_or(value, "speed_coloured_map", preferences.speed_coloured_map);
        preferences.average_track_guide = boolean_or(value, "average_track_guide", preferences.average_track_guide);
        preferences.show_turn_numbers = boolean_or(value, "show_turn_numbers", preferences.show_turn_numbers);
        preferences.show_map_grid = boolean_or(value, "show_map_grid", preferences.show_map_grid);
        preferences.map_grid_spacing_m = grid_spacing_or(value, preferences.map_grid_spacing_m);
        preferences.separate_compare_maps = boolean_or(value, "separate_compare_maps", preferences.separate_compare_maps);
        preferences.analysis_aligned_map_traces = boolean_or(
            value, "analysis_aligned_map_traces", preferences.analysis_aligned_map_traces);
        preferences.telemetry_import_folder = path_or(
            value, "telemetry_import_folder",
            preferences.telemetry_import_folder);
        preferences.auto_detect_sanwa_usb = boolean_or(
            value, "auto_detect_sanwa_usb",
            preferences.auto_detect_sanwa_usb);
        preferences.text_scale = text_scale_or(value, preferences.text_scale);

        result.source_layout_version = layout_version_or(value, 0);
        preferences.layout_version = result.source_layout_version;
        result.layout_rebuild_required = result.source_layout_version < kCurrentUiLayoutVersion;

        if (const auto order = value.find("telemetry_plot_order"); order != value.end() && order->is_array()) {
            std::vector<std::string> keys;
            keys.reserve(order->size());
            for (const auto& key : *order) {
                if (key.is_string()) keys.push_back(key.get<std::string>());
            }
            preferences.telemetry_plot_order = normalize_telemetry_plot_order(keys);
        }

        if (const auto workspace = value.find("workspace"); workspace != value.end() && workspace->is_object()) {
            preferences.workspace.active = workspace_or(*workspace, preferences.workspace.active);
            preferences.workspace.telemetry_density = density_or(*workspace, preferences.workspace.telemetry_density);
            preferences.workspace.compare_b_enabled = boolean_or(
                *workspace, "compare_b_enabled", preferences.workspace.compare_b_enabled);
            preferences.workspace.customize_layout = boolean_or(
                *workspace, "customize_layout", preferences.workspace.customize_layout);
            if (const auto panels = workspace->find("utility_panels"); panels != workspace->end() && panels->is_object()) {
                auto& visibility = preferences.workspace.utility_panels;
                visibility.radio_alignment = boolean_or(*panels, "radio_alignment", visibility.radio_alignment);
                visibility.theoretical_analysis = boolean_or(*panels, "theoretical_analysis", visibility.theoretical_analysis);
                visibility.diagnostics = boolean_or(*panels, "diagnostics", visibility.diagnostics);
                visibility.gg_plot = boolean_or(*panels, "gg_plot", visibility.gg_plot);
                visibility.altitude = boolean_or(*panels, "altitude", visibility.altitude);
            }
        }
        return result;
    } catch (const std::exception& exception) {
        result = {};
        result.warning = std::string("Preferences were invalid; safe defaults restored: ") + exception.what();
        return result;
    } catch (...) {
        result = {};
        result.warning = "Preferences were invalid; safe defaults restored";
        return result;
    }
}

bool save_ui_preferences_atomic(const std::filesystem::path& path, const UiPreferences& preferences,
                                std::string* error) noexcept {
    const auto fail = [error](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };

    std::filesystem::path temporary = path;
    temporary += L".tmp";
    try {
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());

        auto plot_order = Json::array();
        const auto normalized_order = [&preferences] {
            std::vector<std::string> keys;
            keys.reserve(preferences.telemetry_plot_order.size());
            for (const auto id : preferences.telemetry_plot_order) keys.emplace_back(telemetry_plot_key(id));
            return normalize_telemetry_plot_order(keys);
        }();
        for (const auto id : normalized_order) plot_order.push_back(std::string(telemetry_plot_key(id)));

        const auto& panels = preferences.workspace.utility_panels;
        const Json value{
            {"light_theme", preferences.light_theme},
            {"metric_units", preferences.metric_units},
            {"speed_coloured_map", preferences.speed_coloured_map},
            {"average_track_guide", preferences.average_track_guide},
            {"show_turn_numbers", preferences.show_turn_numbers},
            {"show_map_grid", preferences.show_map_grid},
            {"map_grid_spacing_m", std::clamp(preferences.map_grid_spacing_m, 5.0F, 25.0F)},
            {"separate_compare_maps", preferences.separate_compare_maps},
            {"analysis_aligned_map_traces", preferences.analysis_aligned_map_traces},
            {"telemetry_import_folder",
             path_utf8(preferences.telemetry_import_folder)},
            {"auto_detect_sanwa_usb",
             preferences.auto_detect_sanwa_usb},
            {"text_scale", normalize_text_scale(preferences.text_scale)},
            {"layout_version", std::max(0, preferences.layout_version)},
            {"telemetry_plot_order", std::move(plot_order)},
            {"workspace",
             {{"active", workspace_name(preferences.workspace.active)},
              {"telemetry_density", density_name(preferences.workspace.telemetry_density)},
              {"compare_b_enabled", preferences.workspace.compare_b_enabled},
              {"customize_layout", preferences.workspace.customize_layout},
              {"utility_panels",
               {{"radio_alignment", panels.radio_alignment},
                {"theoretical_analysis", panels.theoretical_analysis},
                {"diagnostics", panels.diagnostics},
                {"gg_plot", panels.gg_plot},
                {"altitude", panels.altitude}}}}},
        };

        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return fail("Could not open the temporary preferences file");
            }
            output << value.dump(2) << '\n';
            output.flush();
            if (!output) {
                output.close();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return fail("Could not completely write the temporary preferences file");
            }
        }

        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const auto message = windows_error_message(GetLastError());
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return fail("Could not replace preferences atomically: " + message);
        }
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return fail(std::string("Could not save preferences: ") + exception.what());
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return fail("Could not save preferences");
    }
}

LayoutMigrationPreparation prepare_layout_v4_migration(const std::filesystem::path& settings_directory,
                                                        int source_layout_version) noexcept {
    LayoutMigrationPreparation result;
    result.rebuild_required = source_layout_version < kCurrentUiLayoutVersion;
    result.backup_path = settings_directory / L"layout-pre-v4.ini";
    if (!result.rebuild_required) return result;

    try {
        const auto layout_path = settings_directory / L"layout.ini";
        if (!std::filesystem::exists(layout_path) || std::filesystem::exists(result.backup_path)) return result;
        std::filesystem::create_directories(settings_directory);
        std::error_code copy_error;
        result.backup_created = std::filesystem::copy_file(layout_path, result.backup_path,
                                                           std::filesystem::copy_options::none, copy_error);
        if (copy_error || !result.backup_created) {
            result.safe_to_rebuild = false;
            result.error = "Could not back up the legacy dock layout: " + copy_error.message();
        }
        return result;
    } catch (const std::exception& exception) {
        result.safe_to_rebuild = false;
        result.error = std::string("Could not prepare the dock-layout migration: ") + exception.what();
        return result;
    } catch (...) {
        result.safe_to_rebuild = false;
        result.error = "Could not prepare the dock-layout migration";
        return result;
    }
}

}  // namespace racebox::app
