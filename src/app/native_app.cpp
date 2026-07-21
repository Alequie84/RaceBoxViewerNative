#include "native_app.hpp"
#include "ui_preferences.hpp"
#include "ui_theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <nlohmann/json.hpp>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <format>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace racebox::app {
namespace {

constexpr Timestamp kSecond = 1'000'000;

float application_header_height() {
    const auto* viewport = ImGui::GetMainViewport();
    return 88.0F * std::clamp(viewport ? viewport->DpiScale : 1.0F, 1.0F, 2.0F);
}

std::string recorded_time_label(const Session* session) {
    if (!session || session->telemetry.absolute_time_us.empty() || session->telemetry.absolute_time_us.front() <= 0) {
        return "Not recorded";
    }
    constexpr std::uint64_t unix_to_filetime_ticks = 116'444'736'000'000'000ULL;
    const auto unix_microseconds = static_cast<std::uint64_t>(session->telemetry.absolute_time_us.front());
    const auto filetime_ticks = unix_microseconds * 10ULL + unix_to_filetime_ticks;
    FILETIME file_time{static_cast<DWORD>(filetime_ticks & 0xFFFFFFFFULL), static_cast<DWORD>(filetime_ticks >> 32U)};
    SYSTEMTIME system_time{};
    if (!FileTimeToSystemTime(&file_time, &system_time)) return "Not recorded";
    return std::format("{:04}-{:02}-{:02} {:02}:{:02} UTC", system_time.wYear, system_time.wMonth,
        system_time.wDay, system_time.wHour, system_time.wMinute);
}

std::filesystem::path executable_directory() {
    std::wstring buffer(32'768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

bool uses_parking_track_background(const Session& session) {
    if (session.telemetry.latitude.empty() || session.telemetry.longitude.empty()) return false;
    const auto [min_lat, max_lat] = std::minmax_element(session.telemetry.latitude.begin(), session.telemetry.latitude.end());
    const auto [min_lon, max_lon] = std::minmax_element(session.telemetry.longitude.begin(), session.telemetry.longitude.end());
    const auto center_lat = (*min_lat + *max_lat) * 0.5;
    auto center_lon = (*min_lon + *max_lon) * 0.5;
    if (center_lon > 0.0) center_lon = -center_lon;
    return center_lat > 49.1834 && center_lat < 49.1847 && center_lon > -123.1460 && center_lon < -123.1442 &&
        (*max_lat - *min_lat) < 0.002 && (*max_lon - *min_lon) < 0.003;
}

std::size_t automatic_corner_limit(const Session& session) {
    // The verified Richmond parking layout has nine real turns. Capping this
    // known layout prevents a moved start line from promoting a minor ripple
    // into a tenth label; unknown tracks retain the general twelve-turn cap.
    return uses_parking_track_background(session) ? std::size_t{9} : std::size_t{12};
}

std::filesystem::path triangulated_map_path() {
    const auto installed = executable_directory() / L"assets" / L"rrr-map-original.png";
    if (std::filesystem::exists(installed)) return installed;
    const auto source_tree = executable_directory().parent_path().parent_path() / L"assets" / L"rrr-map-original.png";
    if (std::filesystem::exists(source_tree)) return source_tree;
    const auto development = std::filesystem::current_path() / L"assets" / L"rrr-map-original.png";
    return std::filesystem::exists(development) ? development : installed;
}

void apply_triangulated_map_calibration(MapBackground& map, const std::filesystem::path& image_path) {
    map.image_path = image_path;
    map.offset_x = 0.0F;
    map.offset_y = 0.0F;
    map.scale_x = 1.0F;
    map.scale_y = 1.0F;
    map.opacity = 0.74F;
    map.locked = false;
    map.georeferenced = true;
    map.offset_east_m = 0.0F;
    map.offset_north_m = 0.0F;
    map.reference_latitude = 49.18407486000555;
    map.reference_longitude = -123.14511168306714;
    map.reference_pixel_x = 494.5F;
    map.reference_pixel_y = 256.5F;
    map.metres_per_pixel = 0.09748007033810534F;
    map.rotation_degrees = 0.09155874851718505F;
    // Review pins 1-4 define the useful Richmond track area. Keep the original
    // image and its calibration coordinates unchanged; crop only its display UVs.
    map.source_crop_left_px = 138.0F;
    map.source_crop_top_px = 94.0F;
    map.source_crop_right_px = 837.0F;
    map.source_crop_bottom_px = 409.0F;
}

std::string lap_time(Timestamp value) {
    const auto total_ms = value / 1000;
    const auto minutes = total_ms / 60'000;
    const auto seconds = (total_ms % 60'000) / 1000;
    const auto milliseconds = total_ms % 1000;
    return std::format("{}:{:02}.{:03}", minutes, seconds, milliseconds);
}

bool contains_sanwa_header(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    for (int index = 0; index < 12 && std::getline(file, line); ++index) {
        if (line.find("REC TIME") != std::string::npos && line.find("ST(%)") != std::string::npos) return true;
    }
    return false;
}

ImU32 speed_color(float speed, float maximum) {
    const auto ratio = std::clamp(speed / std::max(1.0F, maximum), 0.0F, 1.0F);
    const auto hue = (1.0F - ratio) * 0.42F;
    ImVec4 color;
    ImGui::ColorConvertHSVtoRGB(hue, 0.9F, 0.95F, color.x, color.y, color.z);
    color.w = 1.0F;
    return ImGui::ColorConvertFloat4ToU32(color);
}

std::size_t nearest_time_index(const std::vector<Timestamp>& times, Timestamp target) {
    if (times.empty()) return 0;
    const auto iterator = std::lower_bound(times.begin(), times.end(), target);
    if (iterator == times.begin()) return 0;
    if (iterator == times.end()) return times.size() - 1;
    const auto upper = static_cast<std::size_t>(std::distance(times.begin(), iterator));
    const auto lower = upper - 1;
    return target - times[lower] <= times[upper] - target ? lower : upper;
}

template <typename Value>
double interpolate_time_channel(const std::vector<Timestamp>& times, const std::vector<Value>& values, Timestamp target) {
    const auto count = std::min(times.size(), values.size());
    if (count == 0) return 0.0;
    if (target <= times.front()) return static_cast<double>(values.front());
    if (target >= times[count - 1]) return static_cast<double>(values[count - 1]);
    const auto upper_iterator = std::lower_bound(times.begin(), times.begin() + static_cast<std::ptrdiff_t>(count), target);
    const auto upper = static_cast<std::size_t>(std::distance(times.begin(), upper_iterator));
    const auto lower = upper - 1;
    const auto span = static_cast<double>(times[upper] - times[lower]);
    if (span <= 0.0) return static_cast<double>(values[upper]);
    const auto ratio = static_cast<double>(target - times[lower]) / span;
    return static_cast<double>(values[lower]) +
        (static_cast<double>(values[upper]) - static_cast<double>(values[lower])) * ratio;
}

double interpolate_curve(const std::vector<double>& x, const std::vector<double>& y, double target) {
    if (x.empty() || y.empty()) return 0.0;
    const auto count = std::min(x.size(), y.size());
    if (target <= x.front()) return y.front();
    if (target >= x[count - 1]) return y[count - 1];
    const auto upper_iterator = std::lower_bound(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(count), target);
    const auto upper = static_cast<std::size_t>(std::distance(x.begin(), upper_iterator));
    const auto lower = upper - 1;
    const auto span = x[upper] - x[lower];
    if (std::abs(span) < 1e-9) return y[upper];
    const auto ratio = (target - x[lower]) / span;
    return y[lower] + (y[upper] - y[lower]) * ratio;
}

struct DistanceTimeProfile {
    std::vector<double> progress;
    std::vector<double> elapsed;
};

struct AverageTrackProfile {
    std::vector<double> latitude;
    std::vector<double> longitude;
};

DistanceTimeProfile build_distance_time_profile(const Session& session, const LapInfo& lap) {
    DistanceTimeProfile result;
    const auto count = lap.end_index - lap.begin_index + 1;
    result.progress.resize(count);
    result.elapsed.resize(count);
    std::vector<double> distance(count);
    const auto base_time = session.telemetry.time_us[lap.begin_index];
    for (std::size_t offset = 1; offset < count; ++offset) {
        const auto previous = lap.begin_index + offset - 1;
        const auto current = lap.begin_index + offset;
        const auto average_latitude = (session.telemetry.latitude[previous] + session.telemetry.latitude[current]) * 0.5;
        const auto lon_scale = 111'320.0 * std::cos(average_latitude * 3.14159265358979323846 / 180.0);
        const auto dx = (session.telemetry.longitude[current] - session.telemetry.longitude[previous]) * lon_scale;
        const auto dy = (session.telemetry.latitude[current] - session.telemetry.latitude[previous]) * 110'540.0;
        const auto step = std::hypot(dx, dy);
        distance[offset] = distance[offset - 1] + (step < 20.0 ? step : 0.0);
    }
    const auto total_distance = std::max(0.001, distance.back());
    for (std::size_t offset = 0; offset < count; ++offset) {
        result.progress[offset] = distance[offset] / total_distance * 100.0;
        result.elapsed[offset] = static_cast<double>(session.telemetry.time_us[lap.begin_index + offset] - base_time) / kSecond;
    }
    return result;
}

AverageTrackProfile build_average_track_profile(const Session& session) {
    struct LapPath {
        std::vector<double> progress;
        std::vector<double> latitude;
        std::vector<double> longitude;
    };
    std::vector<LapPath> paths;
    for (const auto& lap : session.laps) {
        if (lap.phase != LapPhase::Complete || lap.end_index <= lap.begin_index + 10) continue;
        auto distance = build_distance_time_profile(session, lap);
        LapPath path;
        path.progress = std::move(distance.progress);
        path.latitude.assign(session.telemetry.latitude.begin() + static_cast<std::ptrdiff_t>(lap.begin_index),
                             session.telemetry.latitude.begin() + static_cast<std::ptrdiff_t>(lap.end_index + 1));
        path.longitude.assign(session.telemetry.longitude.begin() + static_cast<std::ptrdiff_t>(lap.begin_index),
                              session.telemetry.longitude.begin() + static_cast<std::ptrdiff_t>(lap.end_index + 1));
        paths.push_back(std::move(path));
    }
    AverageTrackProfile result;
    if (paths.empty()) return result;

    constexpr int sample_count = 401;
    result.latitude.reserve(sample_count);
    result.longitude.reserve(sample_count);
    const auto trimmed_mean = [](std::vector<double> values) {
        std::sort(values.begin(), values.end());
        const auto trim = values.size() >= 5 ? values.size() / 5 : 0;
        const auto first = values.begin() + static_cast<std::ptrdiff_t>(trim);
        const auto last = values.end() - static_cast<std::ptrdiff_t>(trim);
        return std::accumulate(first, last, 0.0) / std::max<std::ptrdiff_t>(1, std::distance(first, last));
    };
    for (int sample = 0; sample < sample_count; ++sample) {
        const auto progress = static_cast<double>(sample) / (sample_count - 1) * 100.0;
        std::vector<double> latitudes;
        std::vector<double> longitudes;
        latitudes.reserve(paths.size());
        longitudes.reserve(paths.size());
        for (const auto& path : paths) {
            latitudes.push_back(interpolate_curve(path.progress, path.latitude, progress));
            longitudes.push_back(interpolate_curve(path.progress, path.longitude, progress));
        }
        result.latitude.push_back(trimmed_mean(std::move(latitudes)));
        result.longitude.push_back(trimmed_mean(std::move(longitudes)));
    }
    return result;
}

std::size_t fastest_complete_lap(const std::vector<LapInfo>& laps) {
    std::size_t best = laps.size();
    for (std::size_t index = 0; index < laps.size(); ++index) {
        if (laps[index].phase != LapPhase::Complete) continue;
        if (best == laps.size() || laps[index].duration_us < laps[best].duration_us) best = index;
    }
    return best;
}

}  // namespace

NativeApp::NativeApp(HWND window, ID3D11Device* device, bool software_renderer)
    : window_(window), device_(device), software_renderer_(software_renderer) {
    const auto loaded = load_ui_preferences(settings_directory() / L"preferences.json");
    const auto& preferences = loaded.preferences;
    light_theme_ = preferences.light_theme;
    metric_units_ = preferences.metric_units;
    show_speed_color_ = preferences.speed_coloured_map;
    show_average_track_guide_ = preferences.average_track_guide;
    show_turn_numbers_ = preferences.show_turn_numbers;
    show_map_grid_ = preferences.show_map_grid;
    map_grid_spacing_m_ = preferences.map_grid_spacing_m;
    separate_compare_maps_ = preferences.separate_compare_maps;
    telemetry_plot_order_ = preferences.telemetry_plot_order;
    compact_telemetry_ = preferences.workspace.telemetry_density == TelemetryDensity::Compact;
    show_radio_panel_ = preferences.workspace.utility_panels.radio_alignment;
    show_analysis_panel_ = preferences.workspace.utility_panels.theoretical_analysis;
    show_diagnostics_panel_ = preferences.workspace.utility_panels.diagnostics;
    show_gg_panel_ = preferences.workspace.utility_panels.gg_plot;
    show_altitude_panel_ = preferences.workspace.utility_panels.altitude;
    workspace_section_ = static_cast<WorkspaceSection>(preferences.workspace.active);
    loaded_layout_version_ = loaded.source_layout_version;
    build_default_layout_ = !std::filesystem::exists(settings_directory() / L"layout.ini") || loaded.layout_rebuild_required;
    if (loaded.layout_rebuild_required) {
        const auto migration = prepare_layout_v3_migration(settings_directory(), loaded.source_layout_version);
        if (!migration.safe_to_rebuild) {
            build_default_layout_ = false;
            write_log(migration.error);
        } else if (migration.backup_created) {
            write_log("Legacy dock layout backed up before the v3 Overview migration");
        }
    }
    if (!loaded.warning.empty()) write_log(loaded.warning);
    ui::apply_theme(light_theme_ ? ui::ThemeMode::Light : ui::ThemeMode::Dark, ui::dpi_scale_for_window(window_));
    write_log(software_renderer ? "Native UI started with WARP software renderer" : "Native UI started with DirectX 11 hardware renderer");
}

NativeApp::~NativeApp() {
    background_.reset();
    UiPreferences preferences;
    preferences.light_theme = light_theme_;
    preferences.metric_units = metric_units_;
    preferences.speed_coloured_map = show_speed_color_;
    preferences.average_track_guide = show_average_track_guide_;
    preferences.show_turn_numbers = show_turn_numbers_;
    preferences.show_map_grid = show_map_grid_;
    preferences.map_grid_spacing_m = map_grid_spacing_m_;
    preferences.separate_compare_maps = separate_compare_maps_;
    preferences.layout_version = loaded_layout_version_;
    preferences.telemetry_plot_order = telemetry_plot_order_;
    preferences.workspace.active = static_cast<UiWorkspace>(workspace_section_);
    preferences.workspace.telemetry_density = compact_telemetry_ ? TelemetryDensity::Compact : TelemetryDensity::Detailed;
    preferences.workspace.utility_panels.radio_alignment = show_radio_panel_;
    preferences.workspace.utility_panels.theoretical_analysis = show_analysis_panel_;
    preferences.workspace.utility_panels.diagnostics = show_diagnostics_panel_;
    preferences.workspace.utility_panels.gg_plot = show_gg_panel_;
    preferences.workspace.utility_panels.altitude = show_altitude_panel_;
    std::string save_error;
    if (!save_ui_preferences_atomic(settings_directory() / L"preferences.json", preferences, &save_error)) {
        write_log(save_error);
    }
}

void NativeApp::open_files(const std::vector<std::filesystem::path>& files) { begin_load(files); }
void NativeApp::enable_soak_mode() { soak_mode_ = true; }

const char* NativeApp::phase_name(LapPhase phase) {
    switch (phase) {
        case LapPhase::Complete: return "complete";
        case LapPhase::OutLap: return "outlap";
        case LapPhase::InLap: return "inlap";
        default: return "invalid";
    }
}

const char* view_mode_name(ViewMode mode) {
    switch (mode) {
        case ViewMode::Continuous: return "Continuous";
        case ViewMode::Compare: return "Compare";
        default: return "Single Lap";
    }
}

const LapInfo* NativeApp::active_lap() const {
    if (!session_ || session_->laps.empty()) return nullptr;
    const auto index = std::clamp(active_lap_index_, 0, static_cast<int>(session_->laps.size() - 1));
    return &session_->laps[index];
}

const LapInfo* NativeApp::compare_lap() const {
    if (!session_ || session_->laps.empty()) return nullptr;
    const auto index = std::clamp(compare_lap_index_, 0, static_cast<int>(session_->laps.size() - 1));
    return &session_->laps[index];
}

const LapInfo* NativeApp::compare_b_lap() const {
    if (!session_ || session_->laps.empty()) return nullptr;
    const auto index = std::clamp(compare_b_lap_index_, 0, static_cast<int>(session_->laps.size() - 1));
    return &session_->laps[index];
}

const LapInfo* NativeApp::playback_lap() const {
    if (!session_ || session_->laps.empty()) return nullptr;
    const auto index = view_mode_ == ViewMode::Compare
        ? std::clamp(playback_lap_index_, 0, static_cast<int>(session_->laps.size() - 1))
        : std::clamp(active_lap_index_, 0, static_cast<int>(session_->laps.size() - 1));
    return &session_->laps[index];
}

bool NativeApp::ensure_unique_complete_comparison_roles() {
    if (!session_ || session_->laps.empty()) return false;

    std::vector<int> complete_laps;
    complete_laps.reserve(session_->laps.size());
    for (std::size_t index = 0; index < session_->laps.size(); ++index) {
        if (session_->laps[index].phase == LapPhase::Complete) complete_laps.push_back(static_cast<int>(index));
    }
    if (complete_laps.size() < 3) return false;
    std::stable_sort(complete_laps.begin(), complete_laps.end(), [&](int left, int right) {
        return session_->laps[static_cast<std::size_t>(left)].duration_us <
               session_->laps[static_cast<std::size_t>(right)].duration_us;
    });

    const auto is_complete = [&](int index) {
        return index >= 0 && index < static_cast<int>(session_->laps.size()) &&
               session_->laps[static_cast<std::size_t>(index)].phase == LapPhase::Complete;
    };
    std::vector<int> used;
    const auto choose = [&](int preferred) {
        if (is_complete(preferred) && std::find(used.begin(), used.end(), preferred) == used.end()) {
            used.push_back(preferred);
            return preferred;
        }
        const auto replacement = std::find_if(complete_laps.begin(), complete_laps.end(), [&](int candidate) {
            return std::find(used.begin(), used.end(), candidate) == used.end();
        });
        used.push_back(*replacement);
        return *replacement;
    };

    const auto reference = choose(fastest_reference_ ? complete_laps.front() : active_lap_index_);
    const auto compare_a = choose(compare_lap_index_);
    const auto compare_b = choose(compare_b_lap_index_);
    const auto previous_reference = active_lap_index_;
    const auto changed = reference != active_lap_index_ || compare_a != compare_lap_index_ ||
                         compare_b != compare_b_lap_index_;
    active_lap_index_ = reference;
    compare_lap_index_ = compare_a;
    compare_b_lap_index_ = compare_b;
    playback_lap_index_ = std::clamp(playback_lap_index_, 0, static_cast<int>(session_->laps.size()) - 1);

    if (changed) {
        plot_cache_active_ = -1;
        plot_cache_compare_ = -1;
        plot_cache_compare_b_ = -1;
        plot_cache_mode_ = static_cast<ViewMode>(-1);
        plot_fit_pending_ = true;
        driver_analysis_dirty_ = true;
        if (reference != previous_reference) {
            cursor_us_ = session_->telemetry.time_us[session_->laps[static_cast<std::size_t>(reference)].begin_index];
        }
    }
    return true;
}

void NativeApp::reset_workspace_state() {
    analysis_rules_ = driver_analysis::default_analysis_rules();
    corner_zones_.clear();
    driver_analysis_ = {};
    driver_analysis_dirty_ = true;
    selected_insight_index_ = -1;
    selected_corner_index_ = -1;
    session_notes_.fill('\0');
    annotations_.clear();
    next_annotation_id_ = 1;
    selected_annotation_id_ = 0;
}

std::string NativeApp::serialize_workspace_state() const {
    nlohmann::json state{
        {"format", "racebox-native-workspace"}, {"version", 2},
        {"formula_version", std::string(driver_analysis::kFormulaVersion)},
        {"fastest_reference", fastest_reference_}, {"reference_index", active_lap_index_},
        {"compare_a_index", compare_lap_index_}, {"compare_b_index", compare_b_lap_index_},
        {"playback_index", playback_lap_index_}, {"view_mode", static_cast<int>(view_mode_)},
        {"session_notes", std::string(session_notes_.data())},
        {"rules", nlohmann::json::array()}, {"corners", nlohmann::json::array()},
        {"annotations", nlohmann::json::array()},
        {"detectors", {
            {"brake_begin_dwell_us", analysis_rules_.brake_begin_dwell_us},
            {"brake_release_dwell_us", analysis_rules_.brake_release_dwell_us},
            {"turn_in_dwell_us", analysis_rules_.turn_in_dwell_us},
            {"first_throttle_dwell_us", analysis_rules_.first_throttle_dwell_us},
            {"full_throttle_dwell_us", analysis_rules_.full_throttle_dwell_us},
            {"brake_begin_percent", analysis_rules_.brake_begin_percent},
            {"brake_release_percent", analysis_rules_.brake_release_percent},
            {"turn_in_steering_percent", analysis_rules_.turn_in_steering_percent},
            {"first_throttle_percent", analysis_rules_.first_throttle_percent},
            {"full_throttle_percent", analysis_rules_.full_throttle_percent},
            {"steering_correction_percent", analysis_rules_.steering_correction_percent},
            {"turn_in_curvature_per_m", analysis_rules_.turn_in_curvature_per_m}}}
    };
    for (const auto& rule : analysis_rules_.metrics) {
        state["rules"].push_back({{"id", static_cast<int>(rule.id)}, {"enabled", rule.enabled}, {"threshold", rule.threshold}});
    }
    for (const auto& corner : corner_zones_) {
        state["corners"].push_back({{"id", corner.id}, {"name", corner.name}, {"start", corner.start_progress},
            {"turn_in", corner.turn_in_progress}, {"apex", corner.apex_progress},
            {"exit", corner.exit_progress}, {"end", corner.end_progress}});
    }
    for (const auto& annotation : annotations_) {
        state["annotations"].push_back({
            {"id", annotation.id}, {"surface", annotation.surface}, {"anchor_window", annotation.anchor_window},
            {"plot_id", annotation.plot_id}, {"plot_index", annotation.plot_index},
            {"x", annotation.normalized_x}, {"y", annotation.normalized_y}, {"cursor_us", annotation.cursor_us},
            {"reference_index", annotation.active_lap_index}, {"compare_a_index", annotation.compare_lap_index},
            {"compare_b_index", annotation.compare_b_lap_index}, {"playback_index", annotation.playback_lap_index},
            {"view_mode", static_cast<int>(annotation.view_mode)}, {"note", std::string(annotation.note.data())}
        });
    }
    return state.dump();
}

void NativeApp::restore_workspace_state() {
    if (!session_ || session_->workspace_state_json.empty()) return;
    try {
        const auto state = nlohmann::json::parse(session_->workspace_state_json);
        if (state.value("format", "") != "racebox-native-workspace") return;
        const auto maximum = std::max(0, static_cast<int>(session_->laps.size()) - 1);
        fastest_reference_ = state.value("fastest_reference", fastest_reference_);
        active_lap_index_ = std::clamp(state.value("reference_index", active_lap_index_), 0, maximum);
        compare_lap_index_ = std::clamp(state.value("compare_a_index", compare_lap_index_), 0, maximum);
        compare_b_lap_index_ = std::clamp(state.value("compare_b_index", compare_b_lap_index_), 0, maximum);
        playback_lap_index_ = std::clamp(state.value("playback_index", playback_lap_index_), 0, maximum);
        view_mode_ = static_cast<ViewMode>(std::clamp(state.value("view_mode", static_cast<int>(view_mode_)), 0, 2));
        const auto notes = state.value("session_notes", std::string{});
        std::copy_n(notes.begin(), std::min(notes.size(), session_notes_.size() - 1), session_notes_.begin());
        for (const auto& value : state.value("rules", nlohmann::json::array())) {
            const auto id = static_cast<driver_analysis::RuleId>(value.value("id", -1));
            for (auto& rule : analysis_rules_.metrics) {
                if (rule.id != id) continue;
                rule.enabled = value.value("enabled", rule.enabled);
                rule.threshold = value.value("threshold", rule.threshold);
            }
        }
        if (const auto detectors = state.find("detectors"); detectors != state.end() && detectors->is_object()) {
            analysis_rules_.brake_begin_dwell_us = detectors->value("brake_begin_dwell_us", analysis_rules_.brake_begin_dwell_us);
            analysis_rules_.brake_release_dwell_us = detectors->value("brake_release_dwell_us", analysis_rules_.brake_release_dwell_us);
            analysis_rules_.turn_in_dwell_us = detectors->value("turn_in_dwell_us", analysis_rules_.turn_in_dwell_us);
            analysis_rules_.first_throttle_dwell_us = detectors->value("first_throttle_dwell_us", analysis_rules_.first_throttle_dwell_us);
            analysis_rules_.full_throttle_dwell_us = detectors->value("full_throttle_dwell_us", analysis_rules_.full_throttle_dwell_us);
            analysis_rules_.brake_begin_percent = detectors->value("brake_begin_percent", analysis_rules_.brake_begin_percent);
            analysis_rules_.brake_release_percent = detectors->value("brake_release_percent", analysis_rules_.brake_release_percent);
            analysis_rules_.turn_in_steering_percent = detectors->value("turn_in_steering_percent", analysis_rules_.turn_in_steering_percent);
            analysis_rules_.first_throttle_percent = detectors->value("first_throttle_percent", analysis_rules_.first_throttle_percent);
            analysis_rules_.full_throttle_percent = detectors->value("full_throttle_percent", analysis_rules_.full_throttle_percent);
            analysis_rules_.steering_correction_percent = detectors->value("steering_correction_percent", analysis_rules_.steering_correction_percent);
            analysis_rules_.turn_in_curvature_per_m = detectors->value("turn_in_curvature_per_m", analysis_rules_.turn_in_curvature_per_m);
        }
        for (const auto& value : state.value("corners", nlohmann::json::array())) {
            driver_analysis::CornerZone corner;
            corner.id = value.value("id", std::string{}); corner.name = value.value("name", std::string{});
            corner.start_progress = value.value("start", 0.0); corner.turn_in_progress = value.value("turn_in", 0.0);
            corner.apex_progress = value.value("apex", 0.0); corner.exit_progress = value.value("exit", 0.0);
            corner.end_progress = value.value("end", 0.0);
            if (!corner.id.empty()) corner_zones_.push_back(std::move(corner));
        }
        for (const auto& value : state.value("annotations", nlohmann::json::array())) {
            Annotation annotation;
            annotation.id = value.value("id", next_annotation_id_++);
            annotation.surface = value.value("surface", std::string{"Workspace"});
            annotation.anchor_window = value.value("anchor_window", std::string{});
            annotation.plot_id = value.value("plot_id", std::string{});
            annotation.plot_index = value.value("plot_index", -3);
            annotation.normalized_x = value.value("x", 0.5F); annotation.normalized_y = value.value("y", 0.5F);
            annotation.cursor_us = value.value("cursor_us", Timestamp{});
            annotation.active_lap_index = value.value("reference_index", active_lap_index_);
            annotation.compare_lap_index = value.value("compare_a_index", compare_lap_index_);
            annotation.compare_b_lap_index = value.value("compare_b_index", compare_b_lap_index_);
            annotation.playback_lap_index = value.value("playback_index", playback_lap_index_);
            annotation.view_mode = static_cast<ViewMode>(std::clamp(value.value("view_mode", 0), 0, 2));
            const auto note = value.value("note", std::string{});
            std::copy_n(note.begin(), std::min(note.size(), annotation.note.size() - 1), annotation.note.begin());
            next_annotation_id_ = std::max(next_annotation_id_, annotation.id + 1);
            annotations_.push_back(std::move(annotation));
        }
        selected_annotation_id_ = annotations_.empty() ? 0 : annotations_.front().id;
        driver_analysis_dirty_ = true;
    } catch (const std::exception& exception) {
        write_log(std::string("Workspace state could not be restored: ") + exception.what());
    }
}

void NativeApp::refresh_driver_analysis(bool force) {
    if (!session_ || !active_lap()) return;
    if (!force && !driver_analysis_dirty_) return;
    if (fastest_reference_) {
        const auto fastest = fastest_complete_lap(session_->laps);
        if (fastest != session_->laps.size()) active_lap_index_ = static_cast<int>(fastest);
    }
    if (view_mode_ == ViewMode::Compare && !ensure_unique_complete_comparison_roles()) return;
    if (corner_zones_.empty()) {
        corner_zones_ = driver_analysis::suggest_corner_zones(
            *session_, *active_lap(), automatic_corner_limit(*session_));
    }
    driver_analysis_ = driver_analysis::analyze_driver_performance(
        *session_, *active_lap(), compare_lap(), compare_b_lap(), corner_zones_, analysis_rules_);
    driver_analysis_dirty_ = false;
    selected_insight_index_ = std::min(selected_insight_index_, static_cast<int>(driver_analysis_.insights.size()) - 1);
}

void NativeApp::auto_number_corners() {
    if (!session_ || !active_lap()) {
        error_ = "A complete reference lap is required to detect turns";
        return;
    }
    corner_zones_ = driver_analysis::suggest_corner_zones(
        *session_, *active_lap(), automatic_corner_limit(*session_));
    for (std::size_t index = 0; index < corner_zones_.size(); ++index) {
        corner_zones_[index].id = "corner-" + std::to_string(index + 1);
        corner_zones_[index].name = "Turn " + std::to_string(index + 1);
    }
    selected_corner_index_ = -1;
    selected_insight_index_ = -1;
    driver_analysis_dirty_ = true;
    error_.clear();
    status_ = std::format("Detected and numbered {} turns from the reference lap", corner_zones_.size());
}

bool NativeApp::place_start_finish_at_index(std::size_t telemetry_index) {
    if (!session_ || session_->telemetry.empty() || telemetry_index >= session_->telemetry.size()) return false;
    const auto containing_lap = std::find_if(session_->laps.begin(), session_->laps.end(), [&](const LapInfo& lap) {
        return telemetry_index >= lap.begin_index && telemetry_index <= lap.end_index;
    });
    const auto range_begin = containing_lap == session_->laps.end() ? std::size_t{} : containing_lap->begin_index;
    const auto range_end = containing_lap == session_->laps.end() ? session_->telemetry.size() - 1 : containing_lap->end_index;
    const auto before = telemetry_index > range_begin + 6 ? telemetry_index - 6 : range_begin;
    const auto after = std::min(range_end, telemetry_index + 6);
    if (after <= before) {
        error_ = "Could not determine track direction at that point";
        return false;
    }

    const auto centre_latitude = session_->telemetry.latitude[telemetry_index];
    const auto centre_longitude = session_->telemetry.longitude[telemetry_index];
    const auto longitude_metres = 111'320.0 * std::cos(centre_latitude * 3.14159265358979323846 / 180.0);
    const auto tangent_east = (session_->telemetry.longitude[after] - session_->telemetry.longitude[before]) * longitude_metres;
    const auto tangent_north = (session_->telemetry.latitude[after] - session_->telemetry.latitude[before]) * 110'540.0;
    const auto tangent_length = std::hypot(tangent_east, tangent_north);
    if (!std::isfinite(tangent_length) || tangent_length < 0.05 || std::abs(longitude_metres) < 1.0) {
        error_ = "Could not determine track direction at that point";
        return false;
    }

    constexpr double half_line_length_m = 4.0;
    const auto perpendicular_east = -tangent_north / tangent_length;
    const auto perpendicular_north = tangent_east / tangent_length;
    const auto endpoint = [&](double direction) {
        return PhysicalMarker{
            centre_latitude + direction * perpendicular_north * half_line_length_m / 110'540.0,
            centre_longitude + direction * perpendicular_east * half_line_length_m / longitude_metres,
            0.0};
    };
    const auto line_a = endpoint(-1.0);
    const auto line_b = endpoint(1.0);
    std::vector<std::string> diagnostics;
    if (!apply_start_finish_line(*session_, line_a, line_b, diagnostics)) {
        error_ = diagnostics.empty() ? "Start/finish placement did not produce valid laps" : diagnostics.back();
        return false;
    }

    const auto fastest = fastest_complete_lap(session_->laps);
    active_lap_index_ = fastest == session_->laps.size() ? 0 : static_cast<int>(fastest);
    fastest_reference_ = true;
    std::vector<int> comparison_candidates;
    for (std::size_t index = 0; index < session_->laps.size(); ++index) {
        if (static_cast<int>(index) != active_lap_index_ && session_->laps[index].phase == LapPhase::Complete) {
            comparison_candidates.push_back(static_cast<int>(index));
        }
    }
    std::sort(comparison_candidates.begin(), comparison_candidates.end(), [&](int left, int right) {
        return session_->laps[static_cast<std::size_t>(left)].duration_us <
               session_->laps[static_cast<std::size_t>(right)].duration_us;
    });
    compare_lap_index_ = comparison_candidates.empty() ? active_lap_index_ : comparison_candidates.front();
    compare_b_lap_index_ = comparison_candidates.size() < 2 ? compare_lap_index_ : comparison_candidates[1];
    playback_lap_index_ = active_lap_index_;
    if (view_mode_ == ViewMode::Compare && comparison_candidates.size() < 2) view_mode_ = ViewMode::SingleLap;
    continuous_begin_ = 0;
    continuous_end_ = std::max(0, static_cast<int>(session_->laps.size()) - 1);
    continuous_drilldown_ = false;
    playing_ = false;
    cursor_us_ = active_lap() ? session_->telemetry.time_us[active_lap()->begin_index] : 0;

    const auto maximum_lap_index = std::max(0, static_cast<int>(session_->laps.size()) - 1);
    for (auto& annotation : annotations_) {
        annotation.active_lap_index = std::clamp(annotation.active_lap_index, 0, maximum_lap_index);
        annotation.compare_lap_index = std::clamp(annotation.compare_lap_index, 0, maximum_lap_index);
        annotation.compare_b_lap_index = std::clamp(annotation.compare_b_lap_index, 0, maximum_lap_index);
        annotation.playback_lap_index = std::clamp(annotation.playback_lap_index, 0, maximum_lap_index);
    }

    auto average_track = build_average_track_profile(*session_);
    average_track_latitude_ = std::move(average_track.latitude);
    average_track_longitude_ = std::move(average_track.longitude);
    plot_cache_active_ = plot_cache_compare_ = plot_cache_compare_b_ = -1;
    plot_cache_continuous_begin_ = plot_cache_continuous_end_ = -1;
    plot_cache_mode_ = static_cast<ViewMode>(-1);
    plot_fit_pending_ = true;
    auto_number_corners();
    start_finish_placement_mode_ = false;
    error_.clear();
    status_ = std::format("Start/finish placed: {} lap segments, {} turns detected and numbered",
                          session_->laps.size(), corner_zones_.size());
    write_log(status_);
    return true;
}

void NativeApp::navigate_to_analysis_target(const driver_analysis::NavigationTarget& target) {
    if (!session_ || !active_lap()) return;
    playing_ = false;
    view_mode_ = ViewMode::Compare;
    driver_analysis_dirty_ = true;
    cursor_us_ = std::clamp(target.reference_timestamp_us,
        session_->telemetry.time_us[active_lap()->begin_index], session_->telemetry.time_us[active_lap()->end_index]);
    plot_fit_pending_ = true;
}

void NativeApp::navigate_to_progress(double progress) {
    if (!session_ || !active_lap()) return;
    const auto profile = build_distance_time_profile(*session_, *active_lap());
    const auto elapsed = interpolate_curve(profile.progress, profile.elapsed, std::clamp(progress, 0.0, 1.0) * 100.0);
    driver_analysis::NavigationTarget target;
    target.reference_timestamp_us = session_->telemetry.time_us[active_lap()->begin_index] + static_cast<Timestamp>(elapsed * kSecond);
    navigate_to_analysis_target(target);
}

std::pair<std::size_t, std::size_t> NativeApp::active_range() const {
    if (!session_ || session_->telemetry.empty()) return {0, 0};
    if (view_mode_ == ViewMode::SingleLap || view_mode_ == ViewMode::Compare) {
        if (const auto* lap = active_lap()) return {lap->begin_index, lap->end_index};
    }
    if (view_mode_ == ViewMode::Continuous && !session_->laps.empty()) {
        const auto begin = std::clamp(continuous_begin_, 0, static_cast<int>(session_->laps.size() - 1));
        const auto end = std::clamp(continuous_end_, begin, static_cast<int>(session_->laps.size() - 1));
        return {session_->laps[begin].begin_index, session_->laps[end].end_index};
    }
    return {0, session_->telemetry.size() - 1};
}

void NativeApp::begin_load(const std::vector<std::filesystem::path>& files) {
    if (files.empty() || loading_) return;
    if (files.size() == 1 && (files.front().extension() == L".rbxsession" || files.front().extension() == L".rbxlap")) {
        pending_vbo_.clear(); pending_racebox_csv_.clear(); pending_sanwa_csv_.clear();
        loading_ = true;
        status_ = "Loading saved session...";
        const auto archive = files.front();
        load_future_ = std::async(std::launch::async, [archive] {
            LoadResult result;
            std::string error;
            if (!load_session_archive(archive, result.session, error)) throw std::runtime_error(error);
            result.diagnostics.emplace_back("Loaded native session archive");
            return result;
        });
        return;
    }
    std::filesystem::path selected_gpx;
    for (const auto& file : files) {
        auto extension = file.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (extension == L".vbo") pending_vbo_ = file;
        else if (extension == L".csv" && contains_sanwa_header(file)) pending_sanwa_csv_ = file;
        else if (extension == L".csv") pending_racebox_csv_ = file;
        else if (extension == L".gpx") selected_gpx = file;
    }
    if (!selected_gpx.empty()) {
        pending_vbo_.clear(); pending_racebox_csv_.clear(); pending_sanwa_csv_.clear();
        loading_ = true;
        error_.clear();
        status_ = "Parsing GPX track on a worker thread...";
        load_future_ = std::async(std::launch::async, [selected_gpx] {
            LoadResult result;
            result.session.name = selected_gpx.stem().string();
            result.session.telemetry = parse_gpx(selected_gpx, result.diagnostics);
            if (result.session.telemetry.empty()) throw std::runtime_error("GPX contains no track points");
            result.session.laps.push_back({1, 0, 0, result.session.telemetry.size() - 1,
                result.session.telemetry.time_us.back(), LapPhase::InLap});
            return result;
        });
        return;
    }
    LoadRequest request{pending_vbo_, pending_racebox_csv_, pending_sanwa_csv_, {}};
    if (request.vbo.empty() && request.racebox_csv.empty()) {
        error_.clear();
        status_ = request.sanwa_csv.empty() ? "No supported telemetry file was selected" :
            "Sanwa file remembered. Open a VBO or RaceBox CSV to display the session.";
        return;
    }
    loading_ = true;
    error_.clear();
    if (!request.vbo.empty() && !request.racebox_csv.empty()) {
        status_ = request.sanwa_csv.empty() ? "Loading RaceBox sources; Sanwa can be added later..." :
            "Parsing and correlating all telemetry sources...";
        load_future_ = std::async(std::launch::async, [request] { return load_session(request); });
    } else {
        status_ = request.vbo.empty() ? "Loading RaceBox CSV; VBO and Sanwa can be added later..." :
            "Loading VBO; RaceBox CSV and Sanwa can be added later...";
        load_future_ = std::async(std::launch::async, [request] {
            LoadResult result;
            if (!request.racebox_csv.empty()) {
                result.session.name = request.racebox_csv.stem().string();
                result.session.csv_path = request.racebox_csv;
                result.session.telemetry = parse_racebox_csv(request.racebox_csv, result.diagnostics);
            } else {
                result.session.name = request.vbo.stem().string();
                result.session.vbo_path = request.vbo;
                result.session.telemetry = parse_vbo(request.vbo, result.diagnostics);
            }
            if (!request.sanwa_csv.empty()) {
                result.session.sanwa_path = request.sanwa_csv;
                result.session.radio = parse_sanwa_csv(request.sanwa_csv, result.diagnostics);
                result.session.alignment = align_radio(result.session.telemetry, result.session.radio);
            }
            result.session.laps = build_lap_index(result.session.telemetry);
            if (result.session.laps.empty() && !result.session.telemetry.empty()) {
                result.session.laps.push_back({1, 0, 0, result.session.telemetry.size() - 1,
                    result.session.telemetry.time_us.back() - result.session.telemetry.time_us.front(), LapPhase::InLap});
            }
            result.session.sector_markers = default_sector_markers(result.session);
            result.session.theoretical_best = calculate_theoretical_best(result.session);
            return result;
        });
    }
}

void NativeApp::poll_loader() {
    if (!loading_ || !load_future_.valid() || load_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    loading_ = false;
    try {
        auto loaded = load_future_.get();
        const auto warning = std::find_if(loaded.diagnostics.begin(), loaded.diagnostics.end(), [](const std::string& value) {
            return value.rfind("WARNING:", 0) == 0;
        });
        session_ = std::move(loaded.session);
        auto average_track = build_average_track_profile(*session_);
        average_track_latitude_ = std::move(average_track.latitude);
        average_track_longitude_ = std::move(average_track.longitude);
        background_.reset();
        if (!session_->map_background.image_path.empty()) {
            std::string texture_error;
            if (!load_texture(device_, session_->map_background.image_path, background_, texture_error)) {
                error_ = "Saved map image could not be restored: " + texture_error;
            }
        } else if (uses_parking_track_background(*session_)) {
            load_triangulated_background();
        }
        plot_cache_active_ = -1;
        plot_cache_compare_ = -1;
        plot_cache_compare_b_ = -1;
        plot_cache_continuous_begin_ = -1;
        plot_cache_continuous_end_ = -1;
        plot_cache_mode_ = static_cast<ViewMode>(-1);
        const auto fastest = fastest_complete_lap(session_->laps);
        active_lap_index_ = fastest == session_->laps.size() ? 0 : static_cast<int>(fastest);
        fastest_reference_ = true;
        std::vector<int> comparison_candidates;
        for (std::size_t index = 0; index < session_->laps.size(); ++index) {
            if (static_cast<int>(index) != active_lap_index_ && session_->laps[index].phase == LapPhase::Complete) {
                comparison_candidates.push_back(static_cast<int>(index));
            }
        }
        std::sort(comparison_candidates.begin(), comparison_candidates.end(), [&](int left, int right) {
            return session_->laps[static_cast<std::size_t>(left)].duration_us < session_->laps[static_cast<std::size_t>(right)].duration_us;
        });
        compare_lap_index_ = comparison_candidates.empty() ? active_lap_index_ : comparison_candidates.front();
        compare_b_lap_index_ = comparison_candidates.size() < 2 ? compare_lap_index_ : comparison_candidates[1];
        if (compare_b_lap_index_ == active_lap_index_) compare_b_lap_index_ = compare_lap_index_;
        playback_lap_index_ = active_lap_index_;
        continuous_begin_ = 0;
        continuous_end_ = static_cast<int>(session_->laps.size()) - 1;
        continuous_drilldown_ = false;
        continuous_resume_cursor_us_ = 0;
        continuous_resume_playing_ = false;
        annotations_.clear();
        next_annotation_id_ = 1;
        selected_annotation_id_ = 0;
        reset_workspace_state();
        restore_workspace_state();
        if (!ensure_unique_complete_comparison_roles() && view_mode_ == ViewMode::Compare) {
            view_mode_ = ViewMode::SingleLap;
            error_ = "Compare mode needs three different complete laps";
        }
        if (view_mode_ == ViewMode::Compare) {
            workspace_section_ = WorkspaceSection::Compare;
        } else if (workspace_section_ == WorkspaceSection::Compare) {
            workspace_section_ = WorkspaceSection::Overview;
        }
        if (workspace_section_ == WorkspaceSection::Analysis) {
            requested_telemetry_tab_ = TelemetryTab::Events;
            telemetry_tab_request_pending_ = true;
        } else if (workspace_section_ == WorkspaceSection::Sectors) {
            requested_telemetry_tab_ = TelemetryTab::Sectors;
            telemetry_tab_request_pending_ = true;
        }
        if (corner_zones_.empty() && active_lap()) {
            corner_zones_ = driver_analysis::suggest_corner_zones(
                *session_, *active_lap(), automatic_corner_limit(*session_));
        }
        driver_analysis_dirty_ = true;
        cursor_us_ = active_lap() ? session_->telemetry.time_us[active_lap()->begin_index] : 0;
        if (soak_mode_) {
            view_mode_ = ViewMode::Continuous;
            continuous_begin_ = 0;
            for (std::size_t index = 0; index < session_->laps.size(); ++index) {
                if (session_->laps[index].phase == LapPhase::Complete) { continuous_begin_ = static_cast<int>(index); break; }
            }
            continuous_end_ = static_cast<int>(session_->laps.size()) - 1;
            cursor_us_ = session_->telemetry.time_us[session_->laps[static_cast<std::size_t>(continuous_begin_)].begin_index];
            repeat_ = true;
            playing_ = true;
        }
        status_ = std::format("Loaded {} GPS rows, {} radio samples, {} laps", session_->telemetry.size(), session_->radio.size(), session_->laps.size());
        if (warning != loaded.diagnostics.end()) error_ = *warning;
        write_log(status_);
    } catch (const std::exception& exception) {
        error_ = exception.what();
        status_ = "Load failed";
        write_log("Load failed: " + error_);
    }
}

bool NativeApp::render() {
    poll_loader();
    hover_seen_this_frame_ = false;
    annotation_click_consumed_ = false;
    annotation_editor_hovered_ = false;
    if (annotation_mode_) start_finish_placement_mode_ = false;
    if (annotation_mode_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        annotation_mode_ = false;
        status_ = "Annotation mode off";
    }
    if (start_finish_placement_mode_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        start_finish_placement_mode_ = false;
        status_ = "Start/finish placement cancelled";
    }
    if (!annotation_mode_ && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        begin_load(open_telemetry_files(window_));
    }
    frame_times_ms_.push_back(ImGui::GetIO().DeltaTime * 1000.0F);
    if (frame_times_ms_.size() > 600) frame_times_ms_.erase(frame_times_ms_.begin(), frame_times_ms_.begin() + 60);
    if (playing_ && session_) {
        const auto [begin, end] = active_range();
        cursor_us_ += static_cast<Timestamp>(ImGui::GetIO().DeltaTime * playback_speed_ * kSecond);
        if (cursor_us_ > session_->telemetry.time_us[end]) {
            if (repeat_) cursor_us_ = session_->telemetry.time_us[begin];
            else { cursor_us_ = session_->telemetry.time_us[end]; playing_ = false; }
        }
    }
    sync_continuous_lap();

    const auto draw_review_locked = [&](auto&& draw) {
        const auto locked = annotation_mode_;
        if (locked) ImGui::BeginDisabled();
        draw();
        if (locked) ImGui::EndDisabled();
    };
    draw_review_locked([&] { draw_app_header(); });

    const auto* viewport = ImGui::GetMainViewport();
    const auto header_height = application_header_height();
    const auto host_origin = viewport->WorkPos + ImVec2(0.0F, header_height);
    const auto host_size = ImVec2(viewport->WorkSize.x, std::max(1.0F, viewport->WorkSize.y - header_height));
    ImGui::SetNextWindowPos(host_origin, ImGuiCond_Always);
    ImGui::SetNextWindowSize(host_size, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    constexpr auto host_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground;
    ImGui::Begin("##RaceBoxDockHost", nullptr, host_flags);
    const auto dockspace = ImHashStr("RaceBoxViewerDockspace");
    if (build_default_layout_) {
        const auto layout_built = build_overview_layout(dockspace, ImGui::GetWindowPos(), ImGui::GetWindowSize());
        build_default_layout_ = false;
        if (layout_built) loaded_layout_version_ = kCurrentUiLayoutVersion;
        else error_ = "The Overview dock layout could not be completed; it will be retried after restart";
    }
    ImGui::DockSpace(dockspace, ImVec2(0.0F, 0.0F), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();
    ImGui::PopStyleVar(2);

    draw_review_locked([&] { draw_playback(); });
    draw_map();
    draw_telemetry();
    draw_review_locked([&] { draw_laps(); });
    if (show_radio_panel_) draw_review_locked([&] { draw_radio(); });
    if (show_analysis_panel_) draw_review_locked([&] { draw_analysis(); });
    if (show_gg_panel_) draw_review_locked([&] { draw_gg_plot(); });
    if (show_altitude_panel_) draw_review_locked([&] { draw_altitude(); });
    if (show_diagnostics_panel_) draw_review_locked([&] { draw_diagnostics(); });
    draw_annotations();
    handle_annotation_workspace();
    if (!hover_seen_this_frame_) hover_cursor_us_.reset();
    return !exit_requested_;
}

bool NativeApp::build_overview_layout(ImGuiID dockspace, ImVec2 origin, ImVec2 size) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodePos(dockspace, origin);
    ImGui::DockBuilderSetNodeSize(dockspace, size);
    ImGuiID left = 0;
    ImGuiID main = dockspace;
    ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, 0.16F, &left, &main);
    ImGuiID right = 0;
    ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.25F, &right, &main);
    ImGuiID telemetry = 0;
    ImGuiID map = 0;
    ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.50F, &telemetry, &map);
    ImGuiID playback = 0;
    ImGuiID insights = 0;
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.23F, &playback, &insights);

    ImGui::DockBuilderDockWindow("Laps and Sectors", left);
    ImGui::DockBuilderDockWindow("Track Map", map);
    ImGui::DockBuilderDockWindow("Telemetry", telemetry);
    ImGui::DockBuilderDockWindow("Playback", playback);
    ImGui::DockBuilderDockWindow("Insights", insights);
    ImGui::DockBuilderDockWindow("Radio Alignment", telemetry);
    ImGui::DockBuilderDockWindow("Analysis", telemetry);
    ImGui::DockBuilderDockWindow("G-G Plot", telemetry);
    ImGui::DockBuilderDockWindow("Altitude", telemetry);
    ImGui::DockBuilderDockWindow("Diagnostics", insights);
    ImGui::DockBuilderFinish(dockspace);
    return ImGui::DockBuilderGetNode(dockspace) && ImGui::DockBuilderGetNode(left) &&
        ImGui::DockBuilderGetNode(map) && ImGui::DockBuilderGetNode(telemetry) &&
        ImGui::DockBuilderGetNode(right) && ImGui::DockBuilderGetNode(playback) && ImGui::DockBuilderGetNode(insights);
}

void NativeApp::draw_app_header() {
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, application_header_height()), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0F, 7.0F));
    constexpr auto header_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (!ImGui::Begin("##RaceBoxAppHeader", nullptr, header_flags)) {
        ImGui::End();
        ImGui::PopStyleVar(3);
        return;
    }

    if (ImGui::BeginMenuBar()) {
        ImGui::TextColored(ImVec4(0.95F, 0.10F, 0.28F, 1.0F), "R");
        ImGui::SameLine(0.0F, 4.0F);
        ImGui::TextUnformatted("RACEBOX");
        ImGui::SameLine(0.0F, 8.0F);
        ImGui::TextDisabled("Telemetry Analysis");
        ImGui::Separator();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open telemetry...", "Ctrl+O")) begin_load(open_telemetry_files(window_));
        if (ImGui::MenuItem("Save processed session...", nullptr, false, session_.has_value())) save_session();
        if (ImGui::MenuItem("Save active lap archive...", nullptr, false, active_lap() != nullptr)) save_active_lap();
        if (ImGui::MenuItem("Export active lap CSV...", nullptr, false, active_lap() != nullptr)) export_active_lap();
        const auto triangulated_available = session_ && uses_parking_track_background(*session_);
        if (ImGui::MenuItem("Import triangulated Google map", nullptr, false, triangulated_available)) {
            load_triangulated_background();
        }
        if (ImGui::MenuItem("Load uncalibrated background (advanced)...", nullptr, false, session_.has_value())) {
            load_background();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear session and remembered sources", nullptr, false, session_.has_value() ||
            !pending_vbo_.empty() || !pending_racebox_csv_.empty() || !pending_sanwa_csv_.empty())) {
            playing_ = false;
            session_.reset();
            background_.reset();
            average_track_latitude_.clear();
            average_track_longitude_.clear();
            annotations_.clear();
            next_annotation_id_ = 1;
            selected_annotation_id_ = 0;
            pending_vbo_.clear(); pending_racebox_csv_.clear(); pending_sanwa_csv_.clear();
            error_.clear();
            status_ = "Open any VBO, RaceBox CSV, GPX, session archive, or Sanwa file.";
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) exit_requested_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Light theme", nullptr, light_theme_)) {
            light_theme_ = !light_theme_;
            ui::apply_theme(light_theme_ ? ui::ThemeMode::Light : ui::ThemeMode::Dark,
                            ui::dpi_scale_for_window(window_));
        }
        ImGui::MenuItem("Speed-coloured map", nullptr, &show_speed_color_);
        ImGui::MenuItem("Metric units", nullptr, &metric_units_);
        ImGui::MenuItem("Annotation mode", nullptr, &annotation_mode_);
        ImGui::MenuItem("Compact telemetry", nullptr, &compact_telemetry_);
        if (ImGui::BeginMenu("Panels")) {
            ImGui::MenuItem("Radio Alignment", nullptr, &show_radio_panel_);
            ImGui::MenuItem("Theoretical Analysis", nullptr, &show_analysis_panel_);
            ImGui::MenuItem("G-G Plot", nullptr, &show_gg_panel_);
            ImGui::MenuItem("Altitude", nullptr, &show_altitude_panel_);
            ImGui::MenuItem("Diagnostics", nullptr, &show_diagnostics_panel_);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reset to Overview layout")) build_default_layout_ = true;
        ImGui::EndMenu();
    }

        const auto workspace_button = [&](const char* label, WorkspaceSection section) {
            ImGui::SameLine();
            ImGui::PushID(label);
            const auto selected = workspace_section_ == section;
            ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.28F, 0.05F, 0.10F, 0.85F) : ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20F, 0.10F, 0.14F, 0.95F));
            if (ImGui::SmallButton(label)) {
                if (section == WorkspaceSection::Compare) {
                    if (ensure_unique_complete_comparison_roles()) {
                        workspace_section_ = section;
                        view_mode_ = ViewMode::Compare;
                        plot_cache_mode_ = static_cast<ViewMode>(-1);
                        plot_fit_pending_ = true;
                        driver_analysis_dirty_ = true;
                    } else {
                        error_ = "Compare mode needs three different complete laps";
                    }
                } else {
                    workspace_section_ = section;
                    if (section == WorkspaceSection::Overview) requested_telemetry_tab_ = TelemetryTab::Telemetry;
                    if (section == WorkspaceSection::Analysis) requested_telemetry_tab_ = TelemetryTab::Events;
                    if (section == WorkspaceSection::Sectors) requested_telemetry_tab_ = TelemetryTab::Sectors;
                    telemetry_tab_request_pending_ = true;
                }
            }
            ImGui::PopStyleColor(2);
            if (selected) {
                const auto minimum = ImGui::GetItemRectMin();
                const auto maximum = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddLine(ImVec2(minimum.x, maximum.y + 1.0F),
                    ImVec2(maximum.x, maximum.y + 1.0F), IM_COL32(242, 38, 78, 255), 2.0F);
            }
            ImGui::PopID();
        };
        workspace_button("OVERVIEW", WorkspaceSection::Overview);
        workspace_button("ANALYSIS", WorkspaceSection::Analysis);
        workspace_button("COMPARE", WorkspaceSection::Compare);
        workspace_button("SECTORS", WorkspaceSection::Sectors);

        ImGui::SameLine(0.0F, 12.0F);
        ImGui::PushStyleColor(ImGuiCol_Button, annotation_mode_
            ? ImVec4(0.62F, 0.08F, 0.16F, 0.95F) : ImVec4(0.10F, 0.13F, 0.18F, 0.92F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75F, 0.10F, 0.20F, 1.0F));
        if (ImGui::SmallButton(annotation_mode_ ? "ANNOTATION ON (ESC)" : "ANNOTATE")) {
            annotation_mode_ = true;
            show_annotation_pins_ = true;
            start_finish_placement_mode_ = false;
            annotation_click_consumed_ = true;
            status_ = "Annotation mode on: click anywhere to place a review pin; Esc finishes";
        }
        ImGui::PopStyleColor(2);
        if (!annotation_mode_ && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Place numbered review pins anywhere in the interface. Press Esc when finished.");
        }

        const auto renderer = software_renderer_ ? "WARP" : "DX11";
        const auto renderer_size = ImGui::CalcTextSize(renderer).x;
        const auto status_size = ImGui::CalcTextSize(status_.c_str()).x;
        const auto desired_x = ImGui::GetWindowWidth() - renderer_size - std::min(status_size, 360.0F) - 34.0F;
        if (desired_x > ImGui::GetCursorPosX() + 20.0F) {
            ImGui::SetCursorPosX(desired_x);
            ImGui::TextDisabled("%s", status_.c_str());
            ImGui::SameLine();
            ImGui::TextColored(software_renderer_ ? ImVec4(1.0F, 0.55F, 0.2F, 1.0F) :
                ImVec4(0.25F, 0.8F, 0.45F, 1.0F), "%s", renderer);
        }
        ImGui::EndMenuBar();
    }

    const auto context_field = [&](const char* label, const char* value) {
        ImGui::TextDisabled("%s", label);
        ImGui::TextUnformatted(value);
    };
    if (ImGui::BeginTable("##session-context", 5, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Session", ImGuiTableColumnFlags_WidthStretch, 1.25F);
        ImGui::TableSetupColumn("Recorded", ImGuiTableColumnFlags_WidthStretch, 1.05F);
        ImGui::TableSetupColumn("Sources", ImGuiTableColumnFlags_WidthStretch, 1.05F);
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthStretch, 0.75F);
        ImGui::TableSetupColumn("Active comparison", ImGuiTableColumnFlags_WidthStretch, 1.9F);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        context_field("SESSION", session_ ? session_->name.c_str() : "No session loaded");
        ImGui::TableNextColumn();
        const auto recorded = recorded_time_label(session_ ? &*session_ : nullptr);
        context_field("RECORDED", recorded.c_str());
        ImGui::TableNextColumn();
        std::string sources = "No sources";
        if (session_) {
            const auto vbo = !session_->vbo_path.empty();
            const auto racebox = !session_->csv_path.empty();
            const auto sanwa = !session_->radio.empty();
            if (!vbo && !racebox && !session_->telemetry.empty()) {
                sources = std::format("Archive telemetry OK  Sanwa {}", sanwa ? "OK" : "-");
            } else {
                sources = std::format("VBO {}  RaceBox {}  Sanwa {}", vbo ? "OK" : "-",
                    racebox ? "OK" : "-", sanwa ? "OK" : "-");
            }
        }
        context_field("SOURCES", sources.c_str());
        ImGui::TableNextColumn();
        context_field("MODE", view_mode_name(view_mode_));
        ImGui::TableNextColumn();
        ImGui::TextDisabled("ACTIVE COMPARISON");
        if (session_ && active_lap()) {
            const auto lap_short = [](const LapInfo* lap) {
                if (!lap) return std::string("-");
                return lap->race_lap > 0 ? std::format("R{}", lap->race_lap) : std::format("L{}", lap->raw_lap);
            };
            ImGui::TextColored(ImVec4(0.20F, 0.78F, 0.40F, 1.0F), "REF %s", lap_short(active_lap()).c_str());
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0F, 0.68F, 0.12F, 1.0F), "A %s", lap_short(compare_lap()).c_str());
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95F, 0.25F, 0.28F, 1.0F), "B %s", lap_short(compare_b_lap()).c_str());
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.10F, 0.85F, 0.95F, 1.0F), "PLAY %s", lap_short(playback_lap()).c_str());
        } else {
            ImGui::TextDisabled("No active laps");
        }
        ImGui::EndTable();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void NativeApp::draw_playback() {
    if (!ImGui::Begin("Playback")) { ImGui::End(); return; }
    if (continuous_drilldown_) {
        if (ImGui::Button("Back to continuous")) return_to_continuous();
        ImGui::SameLine();
        if (const auto* lap = active_lap()) {
            ImGui::Text("Viewing %s%d", lap->race_lap > 0 ? "R" : "L", lap->race_lap > 0 ? lap->race_lap : lap->raw_lap);
        }
    }
    const char* modes[] = {"Single Lap", "Continuous", "Compare"};
    int mode = static_cast<int>(view_mode_);
    ImGui::SetNextItemWidth(130.0F);
    if (ImGui::Combo("##playback-mode", &mode, modes, 3)) {
        const auto previous_mode = view_mode_;
        view_mode_ = static_cast<ViewMode>(mode);
        if (view_mode_ == ViewMode::Compare && !ensure_unique_complete_comparison_roles()) {
            view_mode_ = previous_mode;
            error_ = "Compare mode needs three different complete laps";
        } else {
            continuous_drilldown_ = false;
            plot_cache_active_ = -1;
            plot_cache_compare_ = -1;
            plot_cache_compare_b_ = -1;
            plot_cache_mode_ = static_cast<ViewMode>(-1);
            plot_fit_pending_ = true;
            driver_analysis_dirty_ = true;
        }
    }
    ImGui::SameLine(); ImGui::TextDisabled("PLAYBACK");
    const char* speeds[] = {"0.25x", "0.5x", "1x", "2x"};
    const double values[] = {0.25, 0.5, 1.0, 2.0};
    int selected_speed = playback_speed_ == 0.25 ? 0 : playback_speed_ == 0.5 ? 1 : playback_speed_ == 2.0 ? 3 : 2;
    ImGui::SameLine(); ImGui::SetNextItemWidth(70.0F);
    if (ImGui::Combo("##playback-speed", &selected_speed, speeds, 4)) playback_speed_ = values[selected_speed];
    if (session_) {
        const auto [begin, end] = active_range();
        const auto begin_time = session_->telemetry.time_us[begin];
        const auto end_time = session_->telemetry.time_us[end];
        const auto elapsed_time = std::clamp(cursor_us_ - begin_time, Timestamp{}, end_time - begin_time);
        ImGui::SetWindowFontScale(1.18F);
        ImGui::TextUnformatted(lap_time(elapsed_time).c_str());
        ImGui::SetWindowFontScale(1.0F);
        ImGui::SameLine(); ImGui::TextDisabled("/ %s", lap_time(end_time - begin_time).c_str());
        if (const auto* lap = playback_lap()) {
            ImGui::SameLine();
            ImGui::TextColored(ui::color(ui::ColorToken::Playback), "%s%d", lap->race_lap > 0 ? "R" : "L",
                lap->race_lap > 0 ? lap->race_lap : lap->raw_lap);
        }
        const auto transport_width = 38.0F;
        if (ImGui::Button("|<", ImVec2(transport_width, 0))) cursor_us_ = begin_time;
        ImGui::SameLine();
        if (ImGui::Button(playing_ ? "||" : ">", ImVec2(transport_width, 0))) playing_ = !playing_;
        ImGui::SameLine(); ImGui::Checkbox("Repeat", &repeat_);

        auto cursor_seconds = static_cast<double>(cursor_us_) / kSecond;
        const auto begin_seconds = static_cast<double>(begin_time) / kSecond;
        const auto end_seconds = static_cast<double>(end_time) / kSecond;
        if (ImGui::SliderScalar("##playback-timeline", ImGuiDataType_Double, &cursor_seconds, &begin_seconds, &end_seconds, "")) {
            cursor_us_ = static_cast<Timestamp>(cursor_seconds * kSecond);
        }
        const auto inspection_cursor = hover_cursor_us_.value_or(cursor_us_);
        if (hover_cursor_us_) {
            ImGui::TextColored(ui::color(ui::ColorToken::Inspection), "Mouse inspection active");
        }
        auto inspected_progress = 0.0;
        if (const auto* reference = active_lap()) {
            const auto reference_profile = build_distance_time_profile(*session_, *reference);
            const auto elapsed = static_cast<double>(inspection_cursor - session_->telemetry.time_us[reference->begin_index]) / kSecond;
            inspected_progress = view_mode_ == ViewMode::Compare && !reference_profile.elapsed.empty()
                ? interpolate_curve(reference_profile.elapsed, reference_profile.progress, elapsed)
                : std::clamp(elapsed / std::max(0.001, static_cast<double>(reference->duration_us) / kSecond) * 100.0, 0.0, 100.0);
        }
        const auto time_at_progress = [&](const LapInfo* lap, double progress) {
            if (!lap) return inspection_cursor;
            const auto profile = build_distance_time_profile(*session_, *lap);
            const auto elapsed = interpolate_curve(profile.progress, profile.elapsed, std::clamp(progress, 0.0, 100.0));
            return session_->telemetry.time_us[lap->begin_index] + static_cast<Timestamp>(elapsed * kSecond);
        };
        const auto inspected_data_time = view_mode_ == ViewMode::Compare
            ? time_at_progress(playback_lap(), inspected_progress) : inspection_cursor;
        if (ImGui::CollapsingHeader(hover_cursor_us_ ? "Inspection data" : "Live telemetry")) {
            const auto radio = sample_radio(*session_, inspected_data_time);
            const auto speed = interpolate_time_channel(session_->telemetry.time_us, session_->telemetry.speed_kmh,
                inspected_data_time) * (metric_units_ ? 1.0 : 0.621371);
            const auto lateral = interpolate_time_channel(session_->telemetry.time_us, session_->telemetry.lateral_g, inspected_data_time);
            const auto longitudinal = interpolate_time_channel(session_->telemetry.time_us, session_->telemetry.longitudinal_g, inspected_data_time);
            const auto altitude = interpolate_time_channel(session_->telemetry.time_us, session_->telemetry.altitude_m, inspected_data_time);
            ImGui::Text("%.1f %s   Lat %+.2f g   Long %+.2f g", speed, metric_units_ ? "km/h" : "mph", lateral, longitudinal);
            ImGui::Text("Altitude %.1f m   Distance %.1f%%", altitude, inspected_progress);
            if (radio.valid) ImGui::Text("Throttle %.0f%%   Brake %.0f%%   Steering %+.0f%%", radio.throttle, radio.brake, radio.steering);
            else ImGui::TextDisabled("Sanwa controls: not recorded at this point");
            if (view_mode_ == ViewMode::Compare && active_lap() && compare_lap() && compare_b_lap()) {
                refresh_plot_cache();
                const auto role_row = [&](const char* role, const LapInfo* lap, ui::ColorToken token) {
                    const auto timestamp = time_at_progress(lap, inspected_progress);
                    const auto lap_speed = interpolate_time_channel(session_->telemetry.time_us, session_->telemetry.speed_kmh,
                        timestamp) * (metric_units_ ? 1.0 : 0.621371);
                    const auto lat = interpolate_time_channel(session_->telemetry.time_us, session_->telemetry.lateral_g, timestamp);
                    const auto lon = interpolate_time_channel(session_->telemetry.time_us, session_->telemetry.longitudinal_g, timestamp);
                    const auto controls = sample_radio(*session_, timestamp);
                    ImGui::TextColored(ui::color(token), "%s  %.1f %s | Lat %+.2f  Long %+.2f", role, lap_speed,
                        metric_units_ ? "km/h" : "mph", lat, lon);
                    ImGui::SameLine();
                    if (controls.valid) ImGui::TextDisabled("T %.0f  B %.0f  S %+.0f", controls.throttle, controls.brake, controls.steering);
                    else ImGui::TextDisabled("T -  B -  S -");
                };
                role_row("REF", active_lap(), ui::ColorToken::Reference);
                role_row("A", compare_lap(), ui::ColorToken::CompareA);
                role_row("B", compare_b_lap(), ui::ColorToken::CompareB);
                if (!relative_delta_progress_.empty()) {
                    const auto delta_a = interpolate_curve(relative_delta_progress_, relative_delta_seconds_, inspected_progress);
                    const auto delta_b = interpolate_curve(relative_delta_progress_, relative_delta_b_seconds_, inspected_progress);
                    ImGui::Text("Relative time  A %+.3f s   B %+.3f s", delta_a, delta_b);
                }
            }
        }
    }
    if (!error_.empty()) ImGui::TextColored(ui::color(ui::ColorToken::Danger), "%s", error_.c_str());
    handle_annotation_window("Playback");
    ImGui::End();
}

NativeApp::PlotData NativeApp::build_plot_data(const LapInfo* lap, std::size_t max_points, bool normalized_progress) const {
    PlotData output;
    if (!session_) return output;
    std::size_t begin = 0, end = session_->telemetry.size() - 1;
    if (lap) { begin = lap->begin_index; end = lap->end_index; }
    else { const auto range = active_range(); begin = range.first; end = range.second; }
    const auto count = end - begin + 1;
    const auto stride = std::max<std::size_t>(1, count / max_points);
    const auto base = session_->telemetry.time_us[begin];
    const auto distance_profile = normalized_progress && lap
        ? build_distance_time_profile(*session_, *lap)
        : DistanceTimeProfile{};
    output.time.reserve(count / stride + 1); output.elapsed.reserve(count / stride + 1);
    output.speed.reserve(count / stride + 1); output.speed_mph.reserve(count / stride + 1);
    output.lateral.reserve(count / stride + 1); output.longitudinal.reserve(count / stride + 1); output.altitude.reserve(count / stride + 1);
    output.controls.reserve(count / stride + 1); output.steering.reserve(count / stride + 1);
    for (auto index = begin; index <= end; index += stride) {
        const auto radio = sample_radio(*session_, session_->telemetry.time_us[index]);
        output.time.push_back(normalized_progress && !distance_profile.progress.empty()
            ? distance_profile.progress[index - begin]
            : static_cast<double>(session_->telemetry.time_us[index] - base) / kSecond);
        output.elapsed.push_back(static_cast<double>(session_->telemetry.time_us[index] - base) / kSecond);
        output.speed.push_back(session_->telemetry.speed_kmh[index]);
        output.speed_mph.push_back(session_->telemetry.speed_kmh[index] * 0.621371);
        output.lateral.push_back(session_->telemetry.lateral_g[index]);
        output.longitudinal.push_back(session_->telemetry.longitudinal_g[index]);
        output.altitude.push_back(session_->telemetry.altitude_m[index]);
        output.controls.push_back(radio.valid ? radio.throttle - radio.brake : std::numeric_limits<double>::quiet_NaN());
        output.steering.push_back(radio.valid ? radio.steering : std::numeric_limits<double>::quiet_NaN());
    }
    return output;
}

void NativeApp::refresh_plot_cache() {
    if (!session_) return;
    const auto active_cache_key = view_mode_ == ViewMode::Continuous ? -1 : active_lap_index_;
    if (plot_cache_active_ == active_cache_key && plot_cache_compare_ == compare_lap_index_ &&
        plot_cache_compare_b_ == compare_b_lap_index_ &&
        plot_cache_continuous_begin_ == continuous_begin_ && plot_cache_continuous_end_ == continuous_end_ &&
        plot_cache_mode_ == view_mode_) return;
    const auto normalized = view_mode_ == ViewMode::Compare;
    primary_plot_cache_ = build_plot_data(view_mode_ == ViewMode::Continuous ? nullptr : active_lap(), 2200, normalized);
    compare_plot_cache_ = normalized ? build_plot_data(compare_lap(), 2200, true) : PlotData{};
    compare_b_plot_cache_ = normalized ? build_plot_data(compare_b_lap(), 2200, true) : PlotData{};
    relative_delta_progress_.clear();
    relative_delta_seconds_.clear();
    relative_delta_b_seconds_.clear();
    primary_distance_progress_.clear();
    primary_distance_elapsed_.clear();
    if (normalized && active_lap()) {
        auto primary_profile = build_distance_time_profile(*session_, *active_lap());
        primary_distance_progress_ = primary_profile.progress;
        primary_distance_elapsed_ = primary_profile.elapsed;
        constexpr int sample_count = 401;
        relative_delta_progress_.reserve(sample_count);
        relative_delta_seconds_.reserve(sample_count);
        relative_delta_b_seconds_.reserve(sample_count);
        const auto compare_a_profile = compare_lap()
            ? build_distance_time_profile(*session_, *compare_lap()) : DistanceTimeProfile{};
        const auto compare_b_profile = compare_b_lap()
            ? build_distance_time_profile(*session_, *compare_b_lap()) : DistanceTimeProfile{};
        for (int sample = 0; sample < sample_count; ++sample) {
            const auto progress = static_cast<double>(sample) / (sample_count - 1) * 100.0;
            const auto primary_elapsed = interpolate_curve(primary_profile.progress, primary_profile.elapsed, progress);
            relative_delta_progress_.push_back(progress);
            relative_delta_seconds_.push_back(compare_a_profile.progress.empty() ? 0.0 :
                interpolate_curve(compare_a_profile.progress, compare_a_profile.elapsed, progress) - primary_elapsed);
            relative_delta_b_seconds_.push_back(compare_b_profile.progress.empty() ? 0.0 :
                interpolate_curve(compare_b_profile.progress, compare_b_profile.elapsed, progress) - primary_elapsed);
        }
    }
    plot_cache_active_ = active_cache_key;
    plot_cache_compare_ = compare_lap_index_;
    plot_cache_compare_b_ = compare_b_lap_index_;
    plot_cache_continuous_begin_ = continuous_begin_;
    plot_cache_continuous_end_ = continuous_end_;
    plot_cache_mode_ = view_mode_;
    if (!primary_plot_cache_.time.empty()) {
        linked_plot_x_min_ = primary_plot_cache_.time.front();
        linked_plot_x_max_ = primary_plot_cache_.time.back();
    }
    plot_fit_pending_ = true;
}

void NativeApp::sync_continuous_lap() {
    if (!session_ || view_mode_ != ViewMode::Continuous || session_->laps.empty()) return;
    const auto begin = std::clamp(continuous_begin_, 0, static_cast<int>(session_->laps.size()) - 1);
    const auto end = std::clamp(continuous_end_, begin, static_cast<int>(session_->laps.size()) - 1);
    int selected = end;
    for (auto index = begin; index <= end; ++index) {
        const auto& lap = session_->laps[static_cast<std::size_t>(index)];
        const auto next_begin = index < end
            ? session_->telemetry.time_us[session_->laps[static_cast<std::size_t>(index + 1)].begin_index]
            : session_->telemetry.time_us[lap.end_index] + 1;
        if (cursor_us_ < next_begin) { selected = index; break; }
    }
    if (selected != active_lap_index_) {
        active_lap_index_ = selected;
        driver_analysis_dirty_ = true;
        const auto& lap = session_->laps[static_cast<std::size_t>(selected)];
        write_log(std::format("Continuous playback entered {}{}", lap.race_lap > 0 ? "R" : "L",
            lap.race_lap > 0 ? lap.race_lap : lap.raw_lap));
    }
}

void NativeApp::open_continuous_lap(int lap_index) {
    if (!session_ || view_mode_ != ViewMode::Continuous || lap_index < continuous_begin_ || lap_index > continuous_end_) return;
    continuous_resume_cursor_us_ = cursor_us_;
    continuous_resume_playing_ = playing_;
    continuous_drilldown_ = true;
    playing_ = false;
    active_lap_index_ = lap_index;
    view_mode_ = ViewMode::SingleLap;
    driver_analysis_dirty_ = true;
    cursor_us_ = session_->telemetry.time_us[session_->laps[static_cast<std::size_t>(lap_index)].begin_index];
    refresh_plot_cache();
}

void NativeApp::return_to_continuous() {
    if (!session_ || !continuous_drilldown_) return;
    continuous_drilldown_ = false;
    view_mode_ = ViewMode::Continuous;
    driver_analysis_dirty_ = true;
    const auto [begin, end] = active_range();
    cursor_us_ = std::clamp(continuous_resume_cursor_us_, session_->telemetry.time_us[begin], session_->telemetry.time_us[end]);
    playing_ = continuous_resume_playing_;
    sync_continuous_lap();
    refresh_plot_cache();
}

void NativeApp::draw_telemetry() {
    if (!ImGui::Begin("Telemetry")) { ImGui::End(); return; }
    if (!session_) { ImGui::TextDisabled("No session loaded"); ImGui::End(); return; }
    refresh_plot_cache();
    const auto& primary = primary_plot_cache_;
    const auto& secondary = compare_plot_cache_;
    const auto& tertiary = compare_b_plot_cache_;
    if (!ImGui::BeginTabBar("telemetry-tabs")) { ImGui::End(); return; }
    const auto tab_flags = [&](TelemetryTab tab) {
        return telemetry_tab_request_pending_ && requested_telemetry_tab_ == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    if (ImGui::BeginTabItem("Telemetry", nullptr, tab_flags(TelemetryTab::Telemetry))) {
    if (telemetry_tab_request_pending_ && requested_telemetry_tab_ == TelemetryTab::Telemetry) telemetry_tab_request_pending_ = false;
    if (annotation_mode_) ImGui::BeginDisabled();
    if (continuous_drilldown_) {
        if (ImGui::Button("Back to continuous")) return_to_continuous();
        ImGui::SameLine();
    }
    if (ImGui::Button("Reset zoom")) {
        if (!primary.time.empty()) {
            linked_plot_x_min_ = primary.time.front();
            linked_plot_x_max_ = primary.time.back();
        }
        plot_fit_pending_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset graph order")) telemetry_plot_order_ = default_telemetry_plot_order();
    ImGui::SameLine();
    ImGui::TextDisabled("Mouse wheel: zoom  |  drag: pan  |  linked across traces");
    if (annotation_mode_) ImGui::EndDisabled();
    std::vector<double> continuous_boundaries;
    std::vector<int> continuous_boundary_laps;
    std::vector<int> continuous_boundary_indices;
    if (view_mode_ == ViewMode::Continuous) {
        const auto [range_begin, range_end] = active_range();
        const auto base_time = session_->telemetry.time_us[range_begin];
        for (std::size_t lap_index = 0; lap_index < session_->laps.size(); ++lap_index) {
            const auto& lap = session_->laps[lap_index];
            if (lap.begin_index >= range_begin && lap.begin_index <= range_end) {
                continuous_boundaries.push_back(static_cast<double>(session_->telemetry.time_us[lap.begin_index] - base_time) / kSecond);
                continuous_boundary_laps.push_back(lap.race_lap > 0 ? lap.race_lap : -lap.raw_lap);
                continuous_boundary_indices.push_back(static_cast<int>(lap_index));
            }
        }
    }
    int requested_lap = -1;
    int rendered_plot_count = 0;
    const float telemetry_plot_height = compact_telemetry_ ? ImGui::GetFontSize() * 5.8F : ImGui::GetFontSize() * 11.0F;
    auto plot_is_last = false;
    const auto item_spacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(item_spacing.x, 1.0F));
    const auto draw_header_progress_badge = [](double inspection_x, double progress_percent) {
        const auto label = std::format("{:.0f}% lap", progress_percent);
        const auto text_size = ImGui::CalcTextSize(label.c_str());
        const auto plot_origin = ImPlot::GetPlotPos();
        const auto plot_size = ImPlot::GetPlotSize();
        const auto cursor_pixel = ImPlot::PlotToPixels(inspection_x, ImPlot::GetPlotLimits().Y.Max);
        const auto badge_width = text_size.x + 12.0F;
        const auto badge_height = text_size.y + 6.0F;
        const auto badge_x = std::clamp(cursor_pixel.x - badge_width * 0.5F,
            plot_origin.x + 2.0F, plot_origin.x + plot_size.x - badge_width - 2.0F);
        const auto badge_min = ImVec2(badge_x, plot_origin.y - badge_height - 3.0F);
        const auto badge_max = badge_min + ImVec2(badge_width, badge_height);
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(badge_min, badge_max, ui::color_u32(ui::ColorToken::Inspection), 4.0F);
        draw->AddText(badge_min + ImVec2(6.0F, 3.0F), IM_COL32(4, 18, 24, 255), label.c_str());
    };
    const auto plot = [&](TelemetryPlotId plot_id, const char* title, const std::vector<double>& values,
                          const std::vector<double>& other, const std::vector<double>& third,
                          double minimum, double maximum, bool percent_axis = false) {
        auto visible_minimum = minimum;
        auto visible_maximum = maximum;
        auto expanded_below = false;
        auto expanded_above = false;
        const auto include_visible_values = [&](const std::vector<double>& series) {
            for (const auto value : series) {
                if (!std::isfinite(value)) continue;
                if (value < visible_minimum) {
                    visible_minimum = value;
                    expanded_below = true;
                }
                if (value > visible_maximum) {
                    visible_maximum = value;
                    expanded_above = true;
                }
            }
        };
        include_visible_values(values);
        include_visible_values(other);
        include_visible_values(third);
        const auto visible_span = std::max(0.001, visible_maximum - visible_minimum);
        visible_minimum -= visible_span * (expanded_below ? 0.08 : 0.04);
        visible_maximum += visible_span * (expanded_above ? 0.12 : 0.10);

        const auto stable_id = telemetry_plot_key(plot_id);
        const auto implot_title = std::format("{}###telemetry-{}", title, stable_id);
        if (plot_fit_pending_) ImPlot::SetNextAxesLimits(
            linked_plot_x_min_, linked_plot_x_max_, visible_minimum, visible_maximum, ImPlotCond_Always);
        auto plot_flags = ImPlotFlags_NoLegend | ImPlotFlags_NoTitle;
        if (annotation_mode_) plot_flags |= ImPlotFlags_NoInputs;
        if (ImPlot::BeginPlot(implot_title.c_str(), ImVec2(-1, telemetry_plot_height), plot_flags)) {
            const auto x_flags = ImPlotAxisFlags_NoHighlight | (plot_is_last ? ImPlotAxisFlags_None : ImPlotAxisFlags_NoTickLabels);
            ImPlot::SetupAxes(nullptr, nullptr,
                              x_flags, ImPlotAxisFlags_NoHighlight);
            ImPlot::SetupAxisLinks(ImAxis_X1, &linked_plot_x_min_, &linked_plot_x_max_);
            ImPlot::SetupAxisLimits(ImAxis_Y1, visible_minimum, visible_maximum, ImGuiCond_Always);
            if (percent_axis) {
                static constexpr double ticks[] = {-120.0, -100.0, 0.0, 100.0, 120.0};
                ImPlot::SetupAxisTicks(ImAxis_Y1, ticks, 5);
            }
            const ImPlotSpec primary_style{ImPlotProp_LineColor, ImVec4(0.20F, 0.78F, 0.40F, 1.0F), ImPlotProp_LineWeight, 1.9F};
            ImPlot::PlotLine("Reference", primary.time.data(), values.data(), static_cast<int>(std::min(primary.time.size(), values.size())), primary_style);
            if (!other.empty()) {
                const ImPlotSpec compare_style{ImPlotProp_LineColor, ImVec4(1.0F, 0.68F, 0.12F, 1.0F), ImPlotProp_LineWeight, 1.6F};
                ImPlot::PlotLine("Compare A", secondary.time.data(), other.data(), static_cast<int>(std::min(secondary.time.size(), other.size())), compare_style);
            }
            if (!third.empty()) {
                const ImPlotSpec compare_b_style{ImPlotProp_LineColor, ImVec4(0.95F, 0.25F, 0.28F, 1.0F), ImPlotProp_LineWeight, 1.5F};
                ImPlot::PlotLine("Compare B", tertiary.time.data(), third.data(), static_cast<int>(std::min(tertiary.time.size(), third.size())), compare_b_style);
            }
            if (view_mode_ == ViewMode::Continuous) {
                if (!continuous_boundaries.empty()) {
                    const ImPlotSpec boundary_style{ImPlotProp_LineColor, ImVec4(0.3F, 0.75F, 0.45F, 0.65F), ImPlotProp_LineWeight, 1.0F};
                    ImPlot::PlotInfLines("Lap boundaries", continuous_boundaries.data(), static_cast<int>(continuous_boundaries.size()), boundary_style);
                    for (std::size_t index = 0; index < continuous_boundaries.size(); ++index) {
                        if (continuous_boundary_laps[index] > 0) ImPlot::TagX(continuous_boundaries[index], ImVec4(0.3F, 0.75F, 0.45F, 1.0F), "R%d", continuous_boundary_laps[index]);
                        else ImPlot::TagX(continuous_boundaries[index], ImVec4(0.3F, 0.75F, 0.45F, 1.0F), "L%d", -continuous_boundary_laps[index]);
                    }
                }
            }
            const auto* lap = active_lap();
            const auto lap_progress_at = [&](Timestamp timestamp) {
                if (!lap) return 0.0;
                const auto elapsed = static_cast<double>(timestamp - session_->telemetry.time_us[lap->begin_index]) / kSecond;
                if (view_mode_ == ViewMode::Compare && !primary_distance_elapsed_.empty()) {
                    return std::clamp(interpolate_curve(primary_distance_elapsed_, primary_distance_progress_, elapsed) / 100.0, 0.0, 1.0);
                }
                return std::clamp(static_cast<double>(timestamp - session_->telemetry.time_us[lap->begin_index]) /
                    std::max(1.0, static_cast<double>(lap->duration_us)), 0.0, 1.0);
            };
            const auto plot_base = view_mode_ == ViewMode::Continuous
                ? session_->telemetry.time_us[active_range().first]
                : (lap ? session_->telemetry.time_us[lap->begin_index] : session_->telemetry.time_us.front());
            const auto cursor_x_at = [&](Timestamp timestamp) {
                return view_mode_ == ViewMode::Compare
                    ? lap_progress_at(timestamp) * 100.0
                    : static_cast<double>(timestamp - plot_base) / kSecond;
            };
            const auto playback_x = cursor_x_at(cursor_us_);
            const auto playback_progress = lap_progress_at(cursor_us_);
            const ImPlotSpec playback_style{ImPlotProp_LineColor, ImVec4(1.0F, 0.73F, 0.15F, 0.95F), ImPlotProp_LineWeight, 1.7F};
            ImPlot::PlotInfLines("Playback cursor", &playback_x, 1, playback_style);
            if (rendered_plot_count == 0) {
                if (view_mode_ == ViewMode::Continuous && lap) {
                    ImPlot::TagX(playback_x, ImVec4(1.0F, 0.73F, 0.15F, 1.0F), "PLAY %s%d %.0f%%",
                        lap->race_lap > 0 ? "R" : "L", lap->race_lap > 0 ? lap->race_lap : lap->raw_lap, playback_progress * 100.0);
                } else {
                    ImPlot::TagX(playback_x, ImVec4(1.0F, 0.73F, 0.15F, 1.0F), "PLAY %.0f%%", playback_progress * 100.0);
                }
            }
            const auto plot_hovered = ImPlot::IsPlotHovered();
            if (hover_cursor_us_) {
                const auto inspection_x = cursor_x_at(*hover_cursor_us_);
                const auto inspection_progress = lap_progress_at(*hover_cursor_us_) * 100.0;
                const ImPlotSpec inspection_style{ImPlotProp_LineColor, ImVec4(0.10F, 0.85F, 0.95F, 0.95F), ImPlotProp_LineWeight, 1.3F};
                ImPlot::PlotInfLines("Inspection cursor", &inspection_x, 1, inspection_style);
                if (plot_hovered) {
                    const auto inspection_color = ImVec4(0.10F, 0.85F, 0.95F, 1.0F);
                    draw_header_progress_badge(inspection_x, inspection_progress);
                    const auto reading_text = [&](double value) {
                        switch (plot_id) {
                            case TelemetryPlotId::Speed:
                                return std::format("{:.1f} {}", value, metric_units_ ? "km/h" : "mph");
                            case TelemetryPlotId::LateralG:
                            case TelemetryPlotId::LongitudinalG:
                                return std::format("{:+.2f} g", value);
                            case TelemetryPlotId::Controls:
                                return value < 0.0 ? std::format("Brake {:.0f}%", -value)
                                                   : std::format("Throttle {:.0f}%", value);
                            case TelemetryPlotId::Steering:
                                return std::format("{:+.0f}% {}", value,
                                    value < -0.5 ? "left" : value > 0.5 ? "right" : "centre");
                            case TelemetryPlotId::RelativeTime:
                                break;
                        }
                        return std::format("{:.2f}", value);
                    };
                    const auto annotate_reading = [&](const PlotData& data, const std::vector<double>& series,
                                                       const char* role, const ImVec4& color, float offset_y) {
                        if (data.time.empty() || series.empty()) return;
                        const auto value = interpolate_curve(data.time, series, inspection_x);
                        if (!std::isfinite(value)) return;
                        const auto reading = role && *role
                            ? std::format("{} {}", role, reading_text(value)) : reading_text(value);
                        const auto vertical_ratio = (value - visible_minimum) /
                            std::max(0.001, visible_maximum - visible_minimum);
                        if (vertical_ratio > 0.72) offset_y += 28.0F;
                        else if (vertical_ratio < 0.28) offset_y -= 28.0F;
                        ImPlot::Annotation(inspection_x, value, color, ImVec2(10.0F, offset_y), true,
                                           "%s", reading.c_str());
                    };
                    if (view_mode_ == ViewMode::Compare) {
                        annotate_reading(primary, values, "R", ui::color(ui::ColorToken::Reference), -20.0F);
                        annotate_reading(secondary, other, "A", ui::color(ui::ColorToken::CompareA), 0.0F);
                        annotate_reading(tertiary, third, "B", ui::color(ui::ColorToken::CompareB), 20.0F);
                    } else {
                        annotate_reading(primary, values, "", inspection_color, -10.0F);
                    }
                }
            }
            if (plot_hovered) {
                const auto mouse = ImPlot::GetPlotMousePos();
                hover_seen_this_frame_ = true;
                if (!annotation_mode_ && view_mode_ == ViewMode::Continuous && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    const auto pointer_x = ImGui::GetMousePos().x;
                    for (std::size_t index = 0; index < continuous_boundaries.size(); ++index) {
                        const auto boundary_pixel = ImPlot::PlotToPixels(continuous_boundaries[index], mouse.y);
                        if (std::abs(pointer_x - boundary_pixel.x) <= 9.0F) {
                            requested_lap = continuous_boundary_indices[index];
                            break;
                        }
                    }
                }
                const auto base = view_mode_ == ViewMode::Continuous ? session_->telemetry.time_us[active_range().first] : session_->telemetry.time_us[active_lap()->begin_index];
                const auto offset = view_mode_ == ViewMode::Compare
                    ? static_cast<Timestamp>(interpolate_curve(primary_distance_progress_, primary_distance_elapsed_,
                        std::clamp(mouse.x, 0.0, 100.0)) * kSecond)
                    : static_cast<Timestamp>(mouse.x * kSecond);
                hover_cursor_us_ = std::clamp(base + offset, session_->telemetry.time_us.front(), session_->telemetry.time_us.back());
            }
            const auto plot_origin = ImPlot::GetPlotPos();
            const auto plot_size = ImPlot::GetPlotSize();
            ImGui::GetWindowDrawList()->AddText(plot_origin + ImVec2(6.0F, 4.0F), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                view_mode_ == ViewMode::Compare ? "Lap %" : "Time s");
            handle_annotation_surface(title, static_cast<int>(plot_id), plot_origin.x, plot_origin.y, plot_size.x,
                                      plot_size.y, plot_hovered, stable_id.data());
            ImPlot::EndPlot();
        }
        ++rendered_plot_count;
    };
    const auto relative_available = view_mode_ == ViewMode::Compare && !relative_delta_progress_.empty() &&
        !relative_delta_seconds_.empty() && !primary_distance_progress_.empty() &&
        !primary_distance_elapsed_.empty() && active_lap();
    const auto relative_plot = [&] {
        if (!relative_available) return;
        auto maximum_delta = 0.0;
        for (const auto value : relative_delta_seconds_) maximum_delta = std::max(maximum_delta, std::abs(value));
        for (const auto value : relative_delta_b_seconds_) maximum_delta = std::max(maximum_delta, std::abs(value));
        const auto delta_limit = std::max(0.10, maximum_delta * 1.15);
        if (plot_fit_pending_) ImPlot::SetNextAxesLimits(linked_plot_x_min_, linked_plot_x_max_, -delta_limit, delta_limit, ImPlotCond_Always);
        auto plot_flags = ImPlotFlags_NoLegend | ImPlotFlags_NoTitle;
        if (annotation_mode_) plot_flags |= ImPlotFlags_NoInputs;
        if (ImPlot::BeginPlot("Relative time vs Reference###telemetry-relative_time",
                              ImVec2(-1, telemetry_plot_height), plot_flags)) {
            const auto x_flags = ImPlotAxisFlags_NoHighlight | (plot_is_last ? ImPlotAxisFlags_None : ImPlotAxisFlags_NoTickLabels);
            ImPlot::SetupAxes(nullptr, "Delta (s)", x_flags, ImPlotAxisFlags_NoHighlight);
            ImPlot::SetupAxisLinks(ImAxis_X1, &linked_plot_x_min_, &linked_plot_x_max_);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -delta_limit, delta_limit, ImGuiCond_Always);
            const ImPlotSpec delta_style{ImPlotProp_LineColor, ImVec4(1.0F, 0.68F, 0.12F, 1.0F), ImPlotProp_LineWeight, 2.0F};
            ImPlot::PlotLine("Compare A - Reference", relative_delta_progress_.data(), relative_delta_seconds_.data(),
                static_cast<int>(std::min(relative_delta_progress_.size(), relative_delta_seconds_.size())), delta_style);
            const ImPlotSpec delta_b_style{ImPlotProp_LineColor, ImVec4(0.95F, 0.25F, 0.28F, 1.0F), ImPlotProp_LineWeight, 1.8F};
            ImPlot::PlotLine("Compare B - Reference", relative_delta_progress_.data(), relative_delta_b_seconds_.data(),
                static_cast<int>(std::min(relative_delta_progress_.size(), relative_delta_b_seconds_.size())), delta_b_style);
            const double zero = 0.0;
            const ImPlotSpec zero_style{ImPlotProp_LineColor, ImVec4(0.55F, 0.60F, 0.66F, 0.8F),
                ImPlotProp_LineWeight, 1.0F, ImPlotProp_Flags, ImPlotInfLinesFlags_Horizontal};
            ImPlot::PlotInfLines("Equal time", &zero, 1, zero_style);

            const auto cursor_value = [&](Timestamp timestamp) {
                const auto elapsed = std::clamp(
                    static_cast<double>(timestamp - session_->telemetry.time_us[active_lap()->begin_index]) / kSecond,
                    primary_distance_elapsed_.front(), primary_distance_elapsed_.back());
                const auto progress = interpolate_curve(primary_distance_elapsed_, primary_distance_progress_, elapsed);
                return std::array{progress,
                    interpolate_curve(relative_delta_progress_, relative_delta_seconds_, progress),
                    interpolate_curve(relative_delta_progress_, relative_delta_b_seconds_, progress)};
            };
            const auto playback_values = cursor_value(cursor_us_);
            const auto playback_progress = playback_values[0];
            const auto playback_delta = playback_values[1];
            const auto playback_color = playback_delta <= 0.0 ? ImVec4(0.20F, 0.72F, 0.40F, 1.0F) : ImVec4(0.95F, 0.28F, 0.24F, 1.0F);
            const ImPlotSpec playback_style{ImPlotProp_LineColor, ImVec4(1.0F, 0.73F, 0.15F, 1.0F), ImPlotProp_LineWeight, 1.7F};
            ImPlot::PlotInfLines("Playback cursor", &playback_progress, 1, playback_style);
            if (rendered_plot_count == 0) ImPlot::TagX(playback_progress, playback_color, "PLAY %+.3f s", playback_delta);
            const auto relative_hovered = ImPlot::IsPlotHovered();
            if (hover_cursor_us_) {
                const auto [inspection_progress, inspection_delta, inspection_delta_b] = cursor_value(*hover_cursor_us_);
                const ImPlotSpec inspection_style{ImPlotProp_LineColor, ImVec4(0.10F, 0.85F, 0.95F, 1.0F), ImPlotProp_LineWeight, 1.3F};
                ImPlot::PlotInfLines("Inspection cursor", &inspection_progress, 1, inspection_style);
                if (relative_hovered) {
                    draw_header_progress_badge(inspection_progress, inspection_progress);
                    ImPlot::Annotation(inspection_progress, inspection_delta, ui::color(ui::ColorToken::CompareA),
                        ImVec2(10.0F, -12.0F), true, "A %+.3f s", inspection_delta);
                    ImPlot::Annotation(inspection_progress, inspection_delta_b, ui::color(ui::ColorToken::CompareB),
                        ImVec2(10.0F, 12.0F), true, "B %+.3f s", inspection_delta_b);
                }
            }

            if (relative_hovered) {
                const auto mouse = ImPlot::GetPlotMousePos();
                const auto progress = std::clamp(mouse.x, 0.0, 100.0);
                const auto elapsed = interpolate_curve(primary_distance_progress_, primary_distance_elapsed_, progress);
                hover_seen_this_frame_ = true;
                hover_cursor_us_ = session_->telemetry.time_us[active_lap()->begin_index] + static_cast<Timestamp>(elapsed * kSecond);
            }
            const auto plot_origin = ImPlot::GetPlotPos();
            const auto plot_size = ImPlot::GetPlotSize();
            ImGui::GetWindowDrawList()->AddText(plot_origin + ImVec2(6.0F, 4.0F), ImGui::GetColorU32(ImGuiCol_TextDisabled), "Distance %");
            handle_annotation_surface("Relative time vs Reference", static_cast<int>(TelemetryPlotId::RelativeTime),
                                      plot_origin.x, plot_origin.y, plot_size.x, plot_size.y, relative_hovered,
                                      telemetry_plot_key(TelemetryPlotId::RelativeTime).data());
            ImPlot::EndPlot();
        }
        ++rendered_plot_count;
    };
    const auto& primary_speed = metric_units_ ? primary.speed : primary.speed_mph;
    const auto& secondary_speed = metric_units_ ? secondary.speed : secondary.speed_mph;
    const auto& tertiary_speed = metric_units_ ? tertiary.speed : tertiary.speed_mph;
    auto max_speed = primary_speed.empty() ? 0.0 : *std::max_element(primary_speed.begin(), primary_speed.end());
    if (!secondary_speed.empty()) max_speed = std::max(max_speed, *std::max_element(secondary_speed.begin(), secondary_speed.end()));
    if (!tertiary_speed.empty()) max_speed = std::max(max_speed, *std::max_element(tertiary_speed.begin(), tertiary_speed.end()));
    std::vector<TelemetryPlotId> visible_plot_order;
    visible_plot_order.reserve(telemetry_plot_order_.size());
    for (const auto id : telemetry_plot_order_) {
        if (id != TelemetryPlotId::RelativeTime || relative_available) visible_plot_order.push_back(id);
    }

    const auto display_cursor_us = hover_cursor_us_.value_or(cursor_us_);
    auto plot_cursor_x = primary.time.empty() ? 0.0 : primary.time.front();
    if (view_mode_ == ViewMode::Compare && active_lap() && !primary_distance_elapsed_.empty()) {
        const auto elapsed = std::clamp(static_cast<double>(display_cursor_us - session_->telemetry.time_us[active_lap()->begin_index]) / kSecond,
            primary_distance_elapsed_.front(), primary_distance_elapsed_.back());
        plot_cursor_x = interpolate_curve(primary_distance_elapsed_, primary_distance_progress_, elapsed);
    } else if (!primary.time.empty()) {
        const auto base = view_mode_ == ViewMode::Continuous ? session_->telemetry.time_us[active_range().first]
            : active_lap() ? session_->telemetry.time_us[active_lap()->begin_index] : session_->telemetry.time_us.front();
        plot_cursor_x = std::clamp(static_cast<double>(display_cursor_us - base) / kSecond, primary.time.front(), primary.time.back());
    }

    std::optional<std::pair<TelemetryPlotId, std::size_t>> requested_plot_move;
    constexpr auto plot_payload = "RACEBOX_TELEMETRY_PLOT";
    const auto draw_drop_target = [&](std::size_t target_gap) {
        ImGui::PushID(static_cast<int>(target_gap));
        const auto origin = ImGui::GetCursorScreenPos();
        const auto width = std::max(1.0F, ImGui::GetContentRegionAvail().x);
        ImGui::InvisibleButton("##graph-drop-gap", ImVec2(width, 6.0F));
        auto accepting = false;
        if (!annotation_mode_ && ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload(
                    plot_payload, ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                accepting = true;
                if (payload->IsDelivery() && payload->DataSize == sizeof(TelemetryPlotId)) {
                    requested_plot_move = {*static_cast<const TelemetryPlotId*>(payload->Data), target_gap};
                }
            }
            ImGui::EndDragDropTarget();
        }
        const auto color = accepting ? ImGui::GetColorU32(ImGuiCol_DragDropTarget)
                                     : ImGui::GetColorU32(ImVec4(0.45F, 0.55F, 0.68F, 0.20F));
        ImGui::GetWindowDrawList()->AddLine(origin + ImVec2(0.0F, 3.0F), origin + ImVec2(width, 3.0F), color,
                                           accepting ? 3.0F : 1.0F);
        ImGui::PopID();
    };
    const auto draw_drag_handle = [&](TelemetryPlotId id) {
        const auto key = telemetry_plot_key(id);
        ImGui::PushID(key.data());
        if (annotation_mode_) ImGui::BeginDisabled();
        ImGui::Button("::###graph-drag-handle", ImVec2(30.0F, 0.0F));
        if (!annotation_mode_ && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload(plot_payload, &id, sizeof(id));
            ImGui::Text("Move %s", telemetry_plot_name(id).data());
            ImGui::EndDragDropSource();
        }
        if (!annotation_mode_ && ImGui::IsItemHovered()) ImGui::SetTooltip("Drag this handle to move the graph up or down");
        if (annotation_mode_) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted(telemetry_plot_name(id).data());
        const auto series_value = [&](const PlotData& data, const std::vector<double>& series) {
            return data.time.empty() || series.empty() ? std::numeric_limits<double>::quiet_NaN()
                : interpolate_curve(data.time, series, plot_cursor_x);
        };
        auto values_for = [&](const PlotData& data) {
            switch (id) {
                case TelemetryPlotId::Speed: return series_value(data, metric_units_ ? data.speed : data.speed_mph);
                case TelemetryPlotId::LateralG: return series_value(data, data.lateral);
                case TelemetryPlotId::LongitudinalG: return series_value(data, data.longitudinal);
                case TelemetryPlotId::Controls: return series_value(data, data.controls);
                case TelemetryPlotId::Steering: return series_value(data, data.steering);
                case TelemetryPlotId::RelativeTime: return 0.0;
            }
            return std::numeric_limits<double>::quiet_NaN();
        };
        const auto value_text = [](double value) { return std::isfinite(value) ? std::format("{:.1f}", value) : std::string("-"); };
        ImGui::SameLine();
        if (hover_cursor_us_) {
            ImGui::TextColored(ui::color(ui::ColorToken::Inspection), "INSPECT");
            ImGui::SameLine();
        }
        if (id == TelemetryPlotId::RelativeTime) {
            const auto a = interpolate_curve(relative_delta_progress_, relative_delta_seconds_, plot_cursor_x);
            const auto b = interpolate_curve(relative_delta_progress_, relative_delta_b_seconds_, plot_cursor_x);
            ImGui::TextColored(ui::color(ui::ColorToken::CompareA), "A %+.3f", a);
            ImGui::SameLine(); ImGui::TextColored(ui::color(ui::ColorToken::CompareB), "B %+.3f", b);
        } else {
            ImGui::TextColored(ui::color(ui::ColorToken::Reference), "R %s", value_text(values_for(primary)).c_str());
            if (view_mode_ == ViewMode::Compare) {
                ImGui::SameLine(); ImGui::TextColored(ui::color(ui::ColorToken::CompareA), "A %s", value_text(values_for(secondary)).c_str());
                ImGui::SameLine(); ImGui::TextColored(ui::color(ui::ColorToken::CompareB), "B %s", value_text(values_for(tertiary)).c_str());
            }
        }
        ImGui::PopID();
    };

    for (std::size_t visible_index = 0; visible_index < visible_plot_order.size(); ++visible_index) {
        const auto id = visible_plot_order[visible_index];
        plot_is_last = visible_index + 1 == visible_plot_order.size();
        draw_drop_target(visible_index);
        draw_drag_handle(id);
        switch (id) {
            case TelemetryPlotId::RelativeTime:
                relative_plot();
                break;
            case TelemetryPlotId::Speed:
                plot(id, metric_units_ ? "Speed - km/h" : "Speed - mph", primary_speed, secondary_speed, tertiary_speed,
                     0.0, std::max(20.0, max_speed * 1.15));
                break;
            case TelemetryPlotId::LateralG:
                plot(id, "Lateral G", primary.lateral, secondary.lateral, tertiary.lateral, -4.0, 4.0);
                break;
            case TelemetryPlotId::LongitudinalG:
                plot(id, "Longitudinal G", primary.longitudinal, secondary.longitudinal, tertiary.longitudinal, -4.0, 4.0);
                break;
            case TelemetryPlotId::Controls:
                plot(id, "Throttle + / Brake - %", primary.controls, secondary.controls, tertiary.controls, -120.0, 120.0, true);
                break;
            case TelemetryPlotId::Steering:
                plot(id, "Steering - Left / + Right %", primary.steering, secondary.steering, tertiary.steering, -120.0, 120.0, true);
                break;
        }
    }
    draw_drop_target(visible_plot_order.size());
    if (requested_plot_move && move_telemetry_plot(telemetry_plot_order_, requested_plot_move->first,
                                                    requested_plot_move->second, relative_available)) {
        status_ = std::format("Moved {} graph", telemetry_plot_name(requested_plot_move->first));
    }
    ImGui::PopStyleVar();
    plot_fit_pending_ = false;
    if (requested_lap >= 0) open_continuous_lap(requested_lap);
    ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Events", nullptr, tab_flags(TelemetryTab::Events))) {
        if (telemetry_tab_request_pending_ && requested_telemetry_tab_ == TelemetryTab::Events) telemetry_tab_request_pending_ = false;
        draw_events_tab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Sectors", nullptr, tab_flags(TelemetryTab::Sectors))) {
        if (telemetry_tab_request_pending_ && requested_telemetry_tab_ == TelemetryTab::Sectors) telemetry_tab_request_pending_ = false;
        draw_sectors_tab();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    ImGui::End();
}

void NativeApp::draw_events_tab() {
    refresh_driver_analysis();
    ImGui::TextWrapped("Detected events use sustained thresholds; selecting a row synchronizes every trace and map dot by track distance.");
    if (ImGui::BeginTable("detected-events", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY, ImVec2(-1, -1))) {
        ImGui::TableSetupColumn("Role"); ImGui::TableSetupColumn("Lap"); ImGui::TableSetupColumn("Corner");
        ImGui::TableSetupColumn("Event"); ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Distance");
        ImGui::TableSetupColumn("Progress"); ImGui::TableHeadersRow();
        int row_id = 0;
        const auto render_events = [&](const char* role, const std::vector<driver_analysis::DetectedEvent>& events,
                                       const ImVec4& color) {
            for (const auto& event : events) {
                ImGui::PushID(row_id++);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextColored(color, "%s", role);
                ImGui::TableNextColumn(); ImGui::Text("Raw %d", event.raw_lap);
                const auto configured = std::find_if(corner_zones_.begin(), corner_zones_.end(), [&](const auto& corner) {
                    return corner.id == event.corner_id;
                });
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(configured == corner_zones_.end() ? event.corner_id.c_str() : configured->name.c_str());
                ImGui::TableNextColumn();
                if (ImGui::Selectable(driver_analysis::event_name(event.type).data(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                    navigate_to_progress(event.progress);
                }
                ImGui::TableNextColumn(); ImGui::Text("%.3f s", event.elapsed_s);
                ImGui::TableNextColumn(); ImGui::Text("%.2f m", event.distance_m);
                ImGui::TableNextColumn(); ImGui::Text("%.1f%%", event.progress * 100.0);
                ImGui::PopID();
            }
        };
        render_events("REF", driver_analysis_.reference.events, ImVec4(0.20F, 0.78F, 0.40F, 1.0F));
        if (driver_analysis_.comparisons[0]) render_events("A", driver_analysis_.comparisons[0]->events, ImVec4(1.0F, 0.68F, 0.12F, 1.0F));
        if (driver_analysis_.comparisons[1]) render_events("B", driver_analysis_.comparisons[1]->events, ImVec4(0.95F, 0.25F, 0.28F, 1.0F));
        ImGui::EndTable();
    }
}

void NativeApp::draw_sectors_tab() {
    refresh_driver_analysis();
    ImGui::SeparatorText("PHYSICAL SECTORS / THEORETICAL BEST");
    ImGui::Text("Theoretical best: %s", lap_time(session_->theoretical_best.duration_us).c_str());
    if (ImGui::BeginTable("physical-sector-summary", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Sector"); ImGui::TableSetupColumn("Best time"); ImGui::TableSetupColumn("Source lap"); ImGui::TableHeadersRow();
        for (const auto& sector : session_->theoretical_best.sectors) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("S%d", sector.sector);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(lap_time(sector.duration_us).c_str());
            ImGui::TableNextColumn(); ImGui::Text("L%d", sector.source_raw_lap);
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("CORNER / PHASE METRICS");
    if (ImGui::BeginTable("corner-sector-summary", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY, ImVec2(-1, -1))) {
        ImGui::TableSetupColumn("Lap"); ImGui::TableSetupColumn("Corner"); ImGui::TableSetupColumn("Zone");
        ImGui::TableSetupColumn("Time delta"); ImGui::TableSetupColumn("Min speed"); ImGui::TableSetupColumn("Exit speed");
        ImGui::TableSetupColumn("Throttle pickup"); ImGui::TableSetupColumn("Line outward"); ImGui::TableHeadersRow();
        int metric_row_id = 0;
        for (std::size_t slot = 0; slot < driver_analysis_.comparisons.size(); ++slot) {
            if (!driver_analysis_.comparisons[slot]) continue;
            const auto& comparison = *driver_analysis_.comparisons[slot];
            for (const auto& corner : comparison.corners) {
                ImGui::PushID(metric_row_id++);
                const auto value_for = [&](driver_analysis::MetricKind kind) -> std::optional<double> {
                    const auto found = std::find_if(corner.metrics.begin(), corner.metrics.end(), [&](const auto& metric) { return metric.kind == kind; });
                    return found == corner.metrics.end() ? std::nullopt : found->value;
                };
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%c", slot == 0 ? 'A' : 'B');
                ImGui::TableNextColumn();
                if (ImGui::Selectable(corner.corner_name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                    const auto configured = std::find_if(corner_zones_.begin(), corner_zones_.end(), [&](const auto& value) { return value.id == corner.corner_id; });
                    if (configured != corner_zones_.end()) {
                        selected_corner_index_ = static_cast<int>(std::distance(corner_zones_.begin(), configured));
                        navigate_to_progress(configured->apex_progress);
                    }
                }
                ImGui::TableNextColumn(); ImGui::Text("%.1f-%.1f m", corner.start_distance_m, corner.end_distance_m);
                const auto metric_cell = [&](driver_analysis::MetricKind kind, const char* suffix) {
                    ImGui::TableNextColumn(); const auto value = value_for(kind);
                    if (value) ImGui::Text("%+.3f%s", *value, suffix); else ImGui::TextDisabled("n/a");
                };
                metric_cell(driver_analysis::MetricKind::RelativeTimeChange, " s");
                metric_cell(driver_analysis::MetricKind::MinimumSpeedDelta, " km/h");
                metric_cell(driver_analysis::MetricKind::ExitSpeedDelta, " km/h");
                metric_cell(driver_analysis::MetricKind::ThrottlePickupDelta, " s");
                metric_cell(driver_analysis::MetricKind::EntryLineDeviation, " m");
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void NativeApp::draw_map() {
    if (!ImGui::Begin("Track Map")) { ImGui::End(); return; }
    if (!session_) { ImGui::TextDisabled("No session loaded"); ImGui::End(); return; }
    if (annotation_mode_) ImGui::BeginDisabled();
    const auto triangulated_available = uses_parking_track_background(*session_);
    ImGui::BeginDisabled(!triangulated_available);
    if (ImGui::Button("Import triangulated map")) load_triangulated_background();
    ImGui::EndDisabled();
    if (!triangulated_available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("This calibration is only for the verified Richmond parking track.");
    }
    ImGui::SameLine();
    if (start_finish_placement_mode_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ui::color(ui::ColorToken::Warning));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::color(ui::ColorToken::Warning));
    }
    if (ImGui::Button(start_finish_placement_mode_ ? "Cancel S/F" : "Place S/F")) {
        start_finish_placement_mode_ = !start_finish_placement_mode_;
        if (start_finish_placement_mode_) {
            annotation_mode_ = false;
            playing_ = false;
            status_ = "Click the track centre where the start/finish line belongs; Esc cancels";
        } else {
            status_ = "Start/finish placement cancelled";
        }
    }
    if (start_finish_placement_mode_) ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu turns", corner_zones_.size());
    if (background_.view) {
        ImGui::SameLine();
        const auto lock_label = session_->map_background.locked ? "Unlock" : "Lock";
        if (ImGui::Button(lock_label)) session_->map_background.locked = !session_->map_background.locked;
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            session_->map_background.offset_x = 0.0F;
            session_->map_background.offset_y = 0.0F;
            session_->map_background.offset_east_m = 0.0F;
            session_->map_background.offset_north_m = 0.0F;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Map settings")) ImGui::OpenPopup("map-settings-popup");
    if (ImGui::BeginPopup("map-settings-popup")) {
        ImGui::Checkbox("Speed-coloured trace", &show_speed_color_);
        ImGui::Checkbox("Average complete-lap GPS", &show_average_track_guide_);
        ImGui::Checkbox("Show numbered turns", &show_turn_numbers_);
        if (ImGui::Button("Auto-detect and number turns")) auto_number_corners();
        ImGui::Checkbox("Metric grid", &show_map_grid_);
        if (show_map_grid_) {
            ImGui::SetNextItemWidth(160.0F);
            ImGui::SliderFloat("Grid spacing", &map_grid_spacing_m_, 5.0F, 25.0F, "%.0f m");
        }
        if (view_mode_ == ViewMode::Compare) ImGui::Checkbox("Three separate lap maps", &separate_compare_maps_);
        ImGui::Separator();
        if (ImGui::Button("Load uncalibrated image (advanced)...")) load_background();
        if (background_.view) {
            ImGui::SetNextItemWidth(160.0F);
            ImGui::SliderFloat("Background opacity", &session_->map_background.opacity, 0.0F, 1.0F);
            if (!session_->map_background.georeferenced) {
                ImGui::SetNextItemWidth(160.0F);
                if (ImGui::DragFloat("Uniform image zoom", &session_->map_background.scale_x, 0.002F, 0.1F, 5.0F)) {
                    session_->map_background.scale_y = session_->map_background.scale_x;
                }
                ImGui::TextDisabled("Not GPS calibrated; aspect ratio remains locked.");
            } else {
                ImGui::TextColored(ui::color(ui::ColorToken::Positive), "GPS calibrated: fixed 1:1 scale");
                ImGui::TextDisabled("%.9f m/px | %.6f deg", session_->map_background.metres_per_pixel,
                    session_->map_background.rotation_degrees);
                ImGui::TextDisabled("Offset E %.2f m, N %.2f m", session_->map_background.offset_east_m,
                    session_->map_background.offset_north_m);
                const auto crop = resolve_map_image_crop(session_->map_background,
                    static_cast<double>(background_.width), static_cast<double>(background_.height));
                ImGui::TextDisabled("Review crop %.0f x %.0f px (pins 1-4)",
                    crop.right - crop.left, crop.bottom - crop.top);
            }
        }
        ImGui::EndPopup();
    }
    if (background_.view && session_->map_background.georeferenced) {
        ImGui::SameLine(); ImGui::TextColored(ui::color(ui::ColorToken::Positive), "1:1");
        if (!session_->map_background.locked) { ImGui::SameLine(); ImGui::TextDisabled("drag background to align"); }
    }
    if (start_finish_placement_mode_) {
        ImGui::TextColored(ui::color(ui::ColorToken::Warning),
            "PLACEMENT: click the track centre; the line will be perpendicular to travel.");
    }
    if (annotation_mode_) ImGui::EndDisabled();
    auto size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 1.0F);
    size.y = std::max(size.y, 1.0F);
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("map-canvas", size, ImGuiButtonFlags_MouseButtonLeft);
    const auto map_hovered = ImGui::IsItemHovered();
    const auto map_active = ImGui::IsItemActive();
    auto* draw = ImGui::GetWindowDrawList();
    struct Projection {
        ImVec2 panel_origin;
        ImVec2 panel_size;
        double center_lat{};
        double center_lon{};
        double lon_metres{};
        double min_x{};
        double max_x{};
        double min_y{};
        double max_y{};
        double scale{};
    };
    const auto make_projection = [&](const std::vector<const LapInfo*>& laps, ImVec2 panel_origin, ImVec2 panel_size) {
        Projection result{};
        result.panel_origin = panel_origin;
        result.panel_size = panel_size;
        auto min_lat = std::numeric_limits<double>::max(), max_lat = -min_lat, min_lon = min_lat, max_lon = -min_lat;
        for (const auto* lap : laps) {
            if (!lap) continue;
            for (auto index = lap->begin_index; index <= lap->end_index; ++index) {
                min_lat = std::min(min_lat, session_->telemetry.latitude[index]); max_lat = std::max(max_lat, session_->telemetry.latitude[index]);
                min_lon = std::min(min_lon, session_->telemetry.longitude[index]); max_lon = std::max(max_lon, session_->telemetry.longitude[index]);
            }
        }
        for (std::size_t index = 0; index < std::min(average_track_latitude_.size(), average_track_longitude_.size()); ++index) {
            min_lat = std::min(min_lat, average_track_latitude_[index]); max_lat = std::max(max_lat, average_track_latitude_[index]);
            min_lon = std::min(min_lon, average_track_longitude_[index]); max_lon = std::max(max_lon, average_track_longitude_[index]);
        }
        if (background_.view && session_->map_background.georeferenced) {
            const auto crop = resolve_map_image_crop(session_->map_background,
                static_cast<double>(background_.width), static_cast<double>(background_.height));
            for (const auto pixel_x : {crop.left, crop.right}) {
                for (const auto pixel_y : {crop.top, crop.bottom}) {
                    const auto coordinate = map_pixel_to_coordinate(session_->map_background, pixel_x, pixel_y);
                    min_lat = std::min(min_lat, coordinate.latitude); max_lat = std::max(max_lat, coordinate.latitude);
                    min_lon = std::min(min_lon, coordinate.longitude); max_lon = std::max(max_lon, coordinate.longitude);
                }
            }
        }
        if (!std::isfinite(min_lat) || !std::isfinite(max_lat) || min_lat > max_lat || min_lon > max_lon) {
            result.center_lat = session_->map_background.reference_latitude;
            result.center_lon = session_->map_background.reference_longitude;
            result.lon_metres = 111'320.0 * std::cos(result.center_lat * 3.14159265358979323846 / 180.0);
            result.min_x = result.min_y = -1.0;
            result.max_x = result.max_y = 1.0;
            result.scale = 1.0;
            return result;
        }
        result.center_lat = (min_lat + max_lat) * 0.5;
        result.center_lon = (min_lon + max_lon) * 0.5;
        result.lon_metres = 111'320.0 * std::cos(result.center_lat * 3.14159265358979323846 / 180.0);
        result.min_x = (min_lon - result.center_lon) * result.lon_metres;
        result.max_x = (max_lon - result.center_lon) * result.lon_metres;
        result.min_y = (min_lat - result.center_lat) * 110'540.0;
        result.max_y = (max_lat - result.center_lat) * 110'540.0;
        result.scale = std::min((panel_size.x - 24.0F) / std::max(0.1, result.max_x - result.min_x),
                                (panel_size.y - 24.0F) / std::max(0.1, result.max_y - result.min_y));
        return result;
    };
    const auto project_coordinate = [&](const Projection& projection, double latitude, double longitude) {
        const auto x = (longitude - projection.center_lon) * projection.lon_metres;
        const auto y = (latitude - projection.center_lat) * 110'540.0;
        return projection.panel_origin + projection.panel_size * 0.5F +
            ImVec2(static_cast<float>(x * projection.scale), static_cast<float>(-y * projection.scale));
    };
    const auto convert = [&](const Projection& projection, std::size_t index) {
        return project_coordinate(projection, session_->telemetry.latitude[index], session_->telemetry.longitude[index]);
    };
    const auto convert_coordinate = [&](const Projection& projection, double latitude, double longitude) {
        return project_coordinate(projection, latitude, longitude);
    };
    const auto update_background_from_input = [&](const Projection& projection, bool panel_interacting) {
        if (!background_.view || session_->map_background.locked || annotation_mode_ ||
            start_finish_placement_mode_ || !panel_interacting) return;
        const auto& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (session_->map_background.georeferenced) {
                const auto pixels_per_metre = std::max(0.001F, static_cast<float>(projection.scale));
                session_->map_background.offset_east_m += io.MouseDelta.x / pixels_per_metre;
                session_->map_background.offset_north_m -= io.MouseDelta.y / pixels_per_metre;
            } else {
                session_->map_background.offset_x += io.MouseDelta.x / std::max(1.0F, projection.panel_size.x);
                session_->map_background.offset_y += io.MouseDelta.y / std::max(1.0F, projection.panel_size.y);
            }
        }
        if (io.MouseWheel != 0.0F && !session_->map_background.georeferenced) {
            const auto factor = std::pow(1.025F, io.MouseWheel);
            session_->map_background.scale_x = std::clamp(session_->map_background.scale_x * factor, 0.1F, 5.0F);
            session_->map_background.scale_y = session_->map_background.scale_x;
        }
    };
    const auto draw_panel = [&](const Projection& projection) {
        draw->AddRectFilled(projection.panel_origin, projection.panel_origin + projection.panel_size, IM_COL32(8, 13, 20, 255));
        if (background_.view) {
            const auto tint = IM_COL32(255, 255, 255, static_cast<int>(session_->map_background.opacity * 255));
            if (session_->map_background.georeferenced) {
                constexpr auto degrees_to_radians = 3.14159265358979323846 / 180.0;
                const auto offset = ImVec2(session_->map_background.offset_east_m * static_cast<float>(projection.scale),
                                           -session_->map_background.offset_north_m * static_cast<float>(projection.scale));
                const auto reference = project_coordinate(projection, session_->map_background.reference_latitude,
                                                          session_->map_background.reference_longitude) + offset;
                const auto angle = session_->map_background.rotation_degrees * degrees_to_radians;
                const auto pixel_scale = session_->map_background.metres_per_pixel * projection.scale;
                const auto axis_x = ImVec2(static_cast<float>(std::cos(angle) * pixel_scale),
                                           static_cast<float>(-std::sin(angle) * pixel_scale));
                const auto axis_y = ImVec2(static_cast<float>(std::sin(angle) * pixel_scale),
                                           static_cast<float>(std::cos(angle) * pixel_scale));
                const auto top_left = reference - axis_x * session_->map_background.reference_pixel_x -
                    axis_y * session_->map_background.reference_pixel_y;
                const auto crop = resolve_map_image_crop(session_->map_background,
                    static_cast<double>(background_.width), static_cast<double>(background_.height));
                const auto crop_top_left = top_left + axis_x * static_cast<float>(crop.left) +
                    axis_y * static_cast<float>(crop.top);
                const auto crop_top_right = top_left + axis_x * static_cast<float>(crop.right) +
                    axis_y * static_cast<float>(crop.top);
                const auto crop_bottom_left = top_left + axis_x * static_cast<float>(crop.left) +
                    axis_y * static_cast<float>(crop.bottom);
                const auto crop_bottom_right = top_left + axis_x * static_cast<float>(crop.right) +
                    axis_y * static_cast<float>(crop.bottom);
                const auto inverse_width = 1.0F / std::max(1, background_.width);
                const auto inverse_height = 1.0F / std::max(1, background_.height);
                draw->AddImageQuad(background_.view, crop_top_left, crop_top_right, crop_bottom_right, crop_bottom_left,
                    ImVec2(static_cast<float>(crop.left) * inverse_width, static_cast<float>(crop.top) * inverse_height),
                    ImVec2(static_cast<float>(crop.right) * inverse_width, static_cast<float>(crop.top) * inverse_height),
                    ImVec2(static_cast<float>(crop.right) * inverse_width, static_cast<float>(crop.bottom) * inverse_height),
                    ImVec2(static_cast<float>(crop.left) * inverse_width, static_cast<float>(crop.bottom) * inverse_height), tint);
            } else {
                const auto offset = ImVec2(session_->map_background.offset_x * projection.panel_size.x,
                                           session_->map_background.offset_y * projection.panel_size.y);
                const auto image_aspect = static_cast<float>(background_.width) / std::max(1, background_.height);
                const auto panel_aspect = projection.panel_size.x / std::max(1.0F, projection.panel_size.y);
                auto image_size = panel_aspect > image_aspect
                    ? ImVec2(projection.panel_size.y * image_aspect, projection.panel_size.y)
                    : ImVec2(projection.panel_size.x, projection.panel_size.x / image_aspect);
                image_size = image_size * session_->map_background.scale_x;
                const auto center = projection.panel_origin + projection.panel_size * 0.5F + offset;
                draw->AddImage(background_.view, center - image_size * 0.5F, center + image_size * 0.5F,
                    ImVec2(0, 0), ImVec2(1, 1), tint);
            }
        }
        if (show_map_grid_) {
            const auto spacing = std::max(1.0, static_cast<double>(map_grid_spacing_m_));
            for (auto x = std::ceil(projection.min_x / spacing) * spacing; x <= projection.max_x; x += spacing) {
                const auto screen_x = projection.panel_origin.x + projection.panel_size.x * 0.5F + static_cast<float>(x * projection.scale);
                draw->AddLine(ImVec2(screen_x, projection.panel_origin.y), ImVec2(screen_x, projection.panel_origin.y + projection.panel_size.y), IM_COL32(190, 215, 230, 55));
            }
            for (auto y = std::ceil(projection.min_y / spacing) * spacing; y <= projection.max_y; y += spacing) {
                const auto screen_y = projection.panel_origin.y + projection.panel_size.y * 0.5F - static_cast<float>(y * projection.scale);
                draw->AddLine(ImVec2(projection.panel_origin.x, screen_y), ImVec2(projection.panel_origin.x + projection.panel_size.x, screen_y), IM_COL32(190, 215, 230, 55));
            }
            draw->AddText(projection.panel_origin + ImVec2(8, projection.panel_size.y - 22), IM_COL32(215, 230, 240, 190),
                std::format("{:.0f} m grid", spacing).c_str());
        }
    };
    const auto draw_average_track = [&](const Projection& projection) {
        const auto count = std::min(average_track_latitude_.size(), average_track_longitude_.size());
        if (!show_average_track_guide_ || count < 2) return;
        auto previous = convert_coordinate(projection, average_track_latitude_.front(), average_track_longitude_.front());
        for (std::size_t index = 1; index < count; ++index) {
            const auto point = convert_coordinate(projection, average_track_latitude_[index], average_track_longitude_[index]);
            draw->AddLine(previous, point, IM_COL32(7, 16, 18, 190), 7.0F);
            draw->AddLine(previous, point, IM_COL32(39, 206, 137, 150), 2.0F);
            previous = point;
        }
        draw->AddText(projection.panel_origin + ImVec2(8, projection.panel_size.y - 42), IM_COL32(39, 206, 137, 210),
                      "Average complete-lap GPS reference");
    };
    const auto draw_start_finish = [&](const Projection& projection) {
        if (!session_->start_finish_line) return;
        const auto a = convert_coordinate(projection, session_->start_finish_line->a.latitude,
                                           session_->start_finish_line->a.longitude);
        const auto b = convert_coordinate(projection, session_->start_finish_line->b.latitude,
                                           session_->start_finish_line->b.longitude);
        draw->AddLine(a, b, IM_COL32(5, 8, 12, 245), 8.0F);
        constexpr int segment_count = 8;
        for (int segment = 0; segment < segment_count; ++segment) {
            const auto start_ratio = static_cast<float>(segment) / segment_count;
            const auto end_ratio = static_cast<float>(segment + 1) / segment_count;
            const auto start = a + (b - a) * start_ratio;
            const auto end = a + (b - a) * end_ratio;
            draw->AddLine(start, end, segment % 2 == 0 ? IM_COL32(245, 248, 252, 255) : IM_COL32(238, 50, 75, 255), 4.0F);
        }
        draw->AddCircleFilled(a, 4.0F, IM_COL32(245, 248, 252, 255));
        draw->AddCircleFilled(b, 4.0F, IM_COL32(238, 50, 75, 255));
        const auto centre = (a + b) * 0.5F;
        draw->AddText(centre + ImVec2(7.0F, -17.0F), IM_COL32(5, 8, 12, 255), "S/F");
        draw->AddText(centre + ImVec2(6.0F, -18.0F), IM_COL32(250, 252, 255, 255), "S/F");
    };
    const auto draw_turn_numbers = [&](const LapInfo* lap, const Projection& projection) {
        if (!show_turn_numbers_ || !lap || corner_zones_.empty()) return;
        const auto profile = build_distance_time_profile(*session_, *lap);
        if (profile.progress.empty()) return;
        for (std::size_t corner_index = 0; corner_index < corner_zones_.size(); ++corner_index) {
            const auto target = corner_zones_[corner_index].apex_progress * 100.0;
            const auto found = std::lower_bound(profile.progress.begin(), profile.progress.end(), target);
            const auto offset = std::min(static_cast<std::size_t>(std::distance(profile.progress.begin(), found)),
                                         profile.progress.size() - 1);
            const auto point = convert(projection, lap->begin_index + offset);
            const auto label = std::format("T{}", corner_index + 1);
            const auto text_size = ImGui::CalcTextSize(label.c_str());
            const auto label_min = point + ImVec2(7.0F, -text_size.y - 7.0F);
            const auto label_max = label_min + text_size + ImVec2(8.0F, 5.0F);
            draw->AddCircleFilled(point, 4.5F, ui::color_u32(ui::ColorToken::Warning));
            draw->AddRectFilled(label_min, label_max, IM_COL32(7, 12, 19, 225), 4.0F);
            draw->AddRect(label_min, label_max, ui::color_u32(ui::ColorToken::Warning), 4.0F, 0, 1.2F);
            draw->AddText(label_min + ImVec2(4.0F, 2.0F), IM_COL32(248, 250, 253, 255), label.c_str());
        }
    };
    const auto place_start_finish_from_click = [&](const LapInfo* lap, const Projection& projection, bool panel_hovered) {
        if (!start_finish_placement_mode_ || annotation_mode_ || !lap || !panel_hovered ||
            !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
        const auto mouse = ImGui::GetIO().MousePos;
        const auto count = lap->end_index - lap->begin_index + 1;
        const auto stride = std::max<std::size_t>(1, count / 2500);
        auto nearest = lap->begin_index;
        auto nearest_distance = std::numeric_limits<float>::max();
        for (auto index = lap->begin_index; index <= lap->end_index; index += stride) {
            const auto point = convert(projection, index);
            const auto distance = (point.x - mouse.x) * (point.x - mouse.x) +
                                  (point.y - mouse.y) * (point.y - mouse.y);
            if (distance < nearest_distance) {
                nearest_distance = distance;
                nearest = index;
            }
        }
        place_start_finish_at_index(nearest);
    };
    const auto reference_distance = view_mode_ == ViewMode::Compare && active_lap()
        ? build_distance_time_profile(*session_, *active_lap()) : DistanceTimeProfile{};
    const auto draw_lap = [&](const LapInfo* lap, const Projection& projection, ImU32 fixed_color, float width, bool panel_hovered, const char* label, float label_y) {
        if (!lap) return;
        const auto lap_distance = view_mode_ == ViewMode::Compare
            ? build_distance_time_profile(*session_, *lap) : DistanceTimeProfile{};
        const auto count = lap->end_index - lap->begin_index + 1;
        const auto stride = std::max<std::size_t>(1, count / 2200);
        const auto maximum = *std::max_element(session_->telemetry.speed_kmh.begin() + lap->begin_index, session_->telemetry.speed_kmh.begin() + lap->end_index + 1);
        auto previous = convert(projection, lap->begin_index);
        for (auto index = lap->begin_index + stride; index <= lap->end_index; index += stride) {
            const auto point = convert(projection, index);
            draw->AddLine(previous, point, show_speed_color_ && view_mode_ != ViewMode::Compare ? speed_color(session_->telemetry.speed_kmh[index], maximum) : fixed_color, width);
            previous = point;
        }
        if (view_mode_ == ViewMode::Compare && lap == active_lap() && selected_corner_index_ >= 0 &&
            selected_corner_index_ < static_cast<int>(corner_zones_.size()) && !lap_distance.progress.empty()) {
            const auto& corner = corner_zones_[static_cast<std::size_t>(selected_corner_index_)];
            const auto phase_color = [&](double progress) {
                if (progress < corner.turn_in_progress * 100.0) return IM_COL32(80, 170, 255, 245);
                if (progress < corner.apex_progress * 100.0) return IM_COL32(255, 176, 35, 245);
                if (progress < corner.exit_progress * 100.0) return IM_COL32(230, 95, 235, 245);
                return IM_COL32(45, 225, 135, 245);
            };
            auto previous_zone = convert(projection, lap->begin_index);
            for (std::size_t offset = 1; offset < lap_distance.progress.size(); ++offset) {
                const auto progress = lap_distance.progress[offset];
                const auto point = convert(projection, lap->begin_index + offset);
                if (progress >= corner.start_progress * 100.0 && progress <= corner.end_progress * 100.0) {
                    draw->AddLine(previous_zone, point, IM_COL32(6, 12, 18, 230), 8.0F);
                    draw->AddLine(previous_zone, point, phase_color(progress), 4.5F);
                }
                previous_zone = point;
            }
            const std::array phase_points{
                std::pair{corner.start_progress, "S"}, std::pair{corner.turn_in_progress, "T"},
                std::pair{corner.apex_progress, "A"}, std::pair{corner.exit_progress, "X"},
                std::pair{corner.end_progress, "E"}};
            for (const auto& [normalized, marker_label] : phase_points) {
                const auto target = normalized * 100.0;
                const auto found = std::lower_bound(lap_distance.progress.begin(), lap_distance.progress.end(), target);
                const auto offset = static_cast<std::size_t>(std::distance(lap_distance.progress.begin(), found));
                const auto point = convert(projection, lap->begin_index + std::min(offset, lap_distance.progress.size() - 1));
                draw->AddCircleFilled(point, marker_label[0] == 'A' ? 6.0F : 4.5F, IM_COL32(245, 248, 252, 255));
                draw->AddText(point + ImVec2(5.0F, -13.0F), IM_COL32(245, 248, 252, 255), marker_label);
            }
        }
        if (panel_hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const auto mouse = ImGui::GetIO().MousePos;
            auto nearest = lap->begin_index;
            auto nearest_distance = std::numeric_limits<float>::max();
            for (auto index = lap->begin_index; index <= lap->end_index; index += stride) {
                const auto point = convert(projection, index);
                const auto distance = (point.x - mouse.x) * (point.x - mouse.x) + (point.y - mouse.y) * (point.y - mouse.y);
                if (distance < nearest_distance) { nearest_distance = distance; nearest = index; }
            }
            hover_seen_this_frame_ = true;
            if (view_mode_ == ViewMode::Compare && active_lap() && !reference_distance.progress.empty() &&
                !lap_distance.progress.empty()) {
                const auto progress = lap_distance.progress[nearest - lap->begin_index];
                const auto reference_elapsed = interpolate_curve(reference_distance.progress, reference_distance.elapsed, progress);
                hover_cursor_us_ = session_->telemetry.time_us[active_lap()->begin_index] +
                    static_cast<Timestamp>(reference_elapsed * kSecond);
            } else {
                hover_cursor_us_ = session_->telemetry.time_us[nearest];
            }
        }
        const auto cursor_index_at = [&](Timestamp timestamp) {
            if (view_mode_ == ViewMode::Compare && active_lap() && !reference_distance.elapsed.empty() &&
                !lap_distance.elapsed.empty()) {
                const auto reference_elapsed = std::clamp(
                    static_cast<double>(timestamp - session_->telemetry.time_us[active_lap()->begin_index]) / kSecond,
                    reference_distance.elapsed.front(), reference_distance.elapsed.back());
                const auto progress = interpolate_curve(reference_distance.elapsed, reference_distance.progress, reference_elapsed);
                const auto elapsed = interpolate_curve(lap_distance.progress, lap_distance.elapsed, progress);
                const auto lap_time = session_->telemetry.time_us[lap->begin_index] + static_cast<Timestamp>(elapsed * kSecond);
                return std::clamp(nearest_time_index(session_->telemetry.time_us, lap_time), lap->begin_index, lap->end_index);
            }
            return std::clamp(nearest_time_index(session_->telemetry.time_us, timestamp), lap->begin_index, lap->end_index);
        };
        const auto playback_index = cursor_index_at(cursor_us_);
        const auto chosen_playback = view_mode_ != ViewMode::Compare || lap == playback_lap();
        draw->AddCircleFilled(convert(projection, playback_index), chosen_playback ? 5.5F : 4.5F, fixed_color);
        draw->AddCircle(convert(projection, playback_index), chosen_playback ? 7.5F : 6.0F,
            chosen_playback ? IM_COL32(25, 220, 245, 255) : IM_COL32(255, 190, 42, 230), 0, chosen_playback ? 2.3F : 1.5F);
        if (hover_cursor_us_) {
            const auto inspection_index = cursor_index_at(*hover_cursor_us_);
            const auto inspection_point = convert(projection, inspection_index);
            draw->AddCircle(inspection_point, 7.0F, IM_COL32(25, 220, 245, 255), 0, 2.0F);
            draw->AddCircleFilled(inspection_point, 2.5F, IM_COL32(25, 220, 245, 255));
        }
        const auto speed = metric_units_ ? session_->telemetry.speed_kmh[playback_index] : session_->telemetry.speed_kmh[playback_index] * 0.621371F;
        draw->AddText(projection.panel_origin + ImVec2(8, label_y), fixed_color,
            std::format("{}  {:.1f} {}", label, speed, metric_units_ ? "km/h" : "mph").c_str());
        if (view_mode_ == ViewMode::Compare && label_y == 8.0F) {
            draw->AddText(projection.panel_origin + ImVec2(8, projection.panel_size.y - 62), IM_COL32(225, 235, 240, 210),
                "Dots: same track distance");
        }
        for (const auto& marker : session_->sector_markers) {
            auto nearest = lap->begin_index;
            auto best = std::numeric_limits<double>::max();
            const auto marker_lon_scale = std::cos(marker.latitude * 3.14159265358979323846 / 180.0);
            for (auto index = lap->begin_index; index <= lap->end_index; ++index) {
                const auto lat = session_->telemetry.latitude[index] - marker.latitude;
                const auto lon = (session_->telemetry.longitude[index] - marker.longitude) * marker_lon_scale;
                const auto distance = lat * lat + lon * lon;
                if (distance < best) { best = distance; nearest = index; }
            }
            draw->AddCircle(convert(projection, nearest), 6.0F, IM_COL32(70, 230, 135, 255), 0, 2.0F);
        }
    };
    const auto draw_independent_playback_marker = [&](const Projection& projection) {
        const auto* lap = playback_lap();
        if (view_mode_ != ViewMode::Compare || !lap || lap == active_lap() || lap == compare_lap() || lap == compare_b_lap() ||
            !active_lap() || reference_distance.elapsed.empty()) return;
        const auto lap_distance = build_distance_time_profile(*session_, *lap);
        if (lap_distance.elapsed.empty()) return;
        const auto reference_elapsed = std::clamp(
            static_cast<double>(cursor_us_ - session_->telemetry.time_us[active_lap()->begin_index]) / kSecond,
            reference_distance.elapsed.front(), reference_distance.elapsed.back());
        const auto progress = interpolate_curve(reference_distance.elapsed, reference_distance.progress, reference_elapsed);
        const auto elapsed = interpolate_curve(lap_distance.progress, lap_distance.elapsed, progress);
        const auto timestamp = session_->telemetry.time_us[lap->begin_index] + static_cast<Timestamp>(elapsed * kSecond);
        const auto index = std::clamp(nearest_time_index(session_->telemetry.time_us, timestamp), lap->begin_index, lap->end_index);
        const auto point = convert(projection, index);
        draw->AddCircleFilled(point, 4.5F, ui::color_u32(ui::ColorToken::Playback));
        draw->AddCircle(point, 8.0F, IM_COL32(235, 250, 255, 255), 0, 1.8F);
        draw->AddText(point + ImVec2(7.0F, -16.0F), ui::color_u32(ui::ColorToken::Playback), "Playback");
    };
    const auto pointer = ImGui::GetIO().MousePos;
    if (start_finish_placement_mode_ && map_hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const auto map_laps = [&](const LapInfo* first, const LapInfo* second = nullptr, const LapInfo* third = nullptr) {
        std::vector<const LapInfo*> laps;
        const auto append = [&](const LapInfo* lap) {
            if (lap && std::find(laps.begin(), laps.end(), lap) == laps.end()) laps.push_back(lap);
        };
        append(first); append(second); append(third);
        return laps;
    };
    if (view_mode_ == ViewMode::Compare && separate_compare_maps_ && compare_lap() && compare_b_lap() && size.x >= 256.0F) {
        const auto gap = 8.0F;
        const auto panel_size = ImVec2((size.x - gap * 2.0F) / 3.0F, size.y);
        const auto left_origin = origin;
        const auto middle_origin = origin + ImVec2(panel_size.x + gap, 0);
        const auto right_origin = origin + ImVec2((panel_size.x + gap) * 2.0F, 0);
        const auto shared_projection = make_projection(map_laps(active_lap(), compare_lap(), compare_b_lap()), left_origin, panel_size);
        auto primary_projection = shared_projection;
        auto compare_projection = shared_projection;
        auto compare_b_projection = shared_projection;
        compare_projection.panel_origin = middle_origin;
        compare_b_projection.panel_origin = right_origin;
        const auto left_contains = ImRect(left_origin, left_origin + panel_size).Contains(pointer);
        const auto middle_contains = ImRect(middle_origin, middle_origin + panel_size).Contains(pointer);
        const auto right_contains = ImRect(right_origin, right_origin + panel_size).Contains(pointer);
        const auto left_hovered = map_hovered && left_contains;
        const auto middle_hovered = map_hovered && middle_contains;
        const auto right_hovered = map_hovered && right_contains;
        update_background_from_input(primary_projection, left_contains && (map_hovered || map_active));
        update_background_from_input(compare_projection, middle_contains && (map_hovered || map_active));
        update_background_from_input(compare_b_projection, right_contains && (map_hovered || map_active));
        place_start_finish_from_click(active_lap(), primary_projection, left_hovered);
        draw->PushClipRect(left_origin, left_origin + panel_size, true);
        draw_panel(primary_projection); draw_average_track(primary_projection);
        draw_lap(active_lap(), primary_projection, ui::color_u32(ui::ColorToken::Reference), 2.8F, left_hovered, "Reference", 8.0F);
        draw_start_finish(primary_projection);
        draw_turn_numbers(active_lap(), primary_projection);
        draw_independent_playback_marker(primary_projection);
        draw->PopClipRect();
        draw->PushClipRect(middle_origin, middle_origin + panel_size, true);
        draw_panel(compare_projection); draw_average_track(compare_projection);
        draw_lap(compare_lap(), compare_projection, ui::color_u32(ui::ColorToken::CompareA), 2.8F, middle_hovered, "Compare A", 8.0F);
        draw_start_finish(compare_projection);
        draw->PopClipRect();
        draw->PushClipRect(right_origin, right_origin + panel_size, true);
        draw_panel(compare_b_projection); draw_average_track(compare_b_projection);
        draw_lap(compare_b_lap(), compare_b_projection, ui::color_u32(ui::ColorToken::CompareB), 2.8F, right_hovered, "Compare B", 8.0F);
        draw_start_finish(compare_b_projection);
        draw->PopClipRect();
        handle_annotation_surface("Track Map - Reference", -1, left_origin.x, left_origin.y, panel_size.x, panel_size.y, left_hovered);
        handle_annotation_surface("Track Map - Compare A", -1, middle_origin.x, middle_origin.y, panel_size.x, panel_size.y, middle_hovered);
        handle_annotation_surface("Track Map - Compare B", -1, right_origin.x, right_origin.y, panel_size.x, panel_size.y, right_hovered);
    } else {
        const auto projection = make_projection(view_mode_ == ViewMode::Compare
            ? map_laps(active_lap(), compare_lap(), compare_b_lap())
            : map_laps(active_lap()), origin, size);
        update_background_from_input(projection, map_hovered || map_active);
        place_start_finish_from_click(active_lap(), projection, map_hovered);
        draw->PushClipRect(origin, origin + size, true);
        draw_panel(projection);
        draw_average_track(projection);
        draw_lap(active_lap(), projection, view_mode_ == ViewMode::Compare ? ui::color_u32(ui::ColorToken::Reference) : IM_COL32(74, 158, 207, 255),
                 2.8F, map_hovered, view_mode_ == ViewMode::Compare ? "Reference" : "Lap", 8.0F);
        if (view_mode_ == ViewMode::Compare) {
            draw_lap(compare_lap(), projection, ui::color_u32(ui::ColorToken::CompareA), 2.3F, false, "Compare A", 26.0F);
            draw_lap(compare_b_lap(), projection, ui::color_u32(ui::ColorToken::CompareB), 2.1F, false, "Compare B", 44.0F);
            draw_independent_playback_marker(projection);
        }
        draw_start_finish(projection);
        draw_turn_numbers(active_lap(), projection);
        draw->PopClipRect();
        handle_annotation_surface("Track Map", -1, origin.x, origin.y, size.x, size.y, map_hovered);
    }
    ImGui::End();
}

void NativeApp::draw_laps() {
    if (!ImGui::Begin("Laps and Sectors")) { ImGui::End(); return; }
    if (!session_) { ImGui::TextDisabled("No session loaded"); ImGui::End(); return; }
    if (view_mode_ == ViewMode::Continuous) {
        if (ImGui::Button("Race laps")) {
            for (std::size_t index = 0; index < session_->laps.size(); ++index) {
                if (session_->laps[index].phase == LapPhase::Complete) { continuous_begin_ = static_cast<int>(index); break; }
            }
            for (std::size_t index = session_->laps.size(); index-- > 0;) {
                if (session_->laps[index].phase == LapPhase::Complete) { continuous_end_ = static_cast<int>(index); break; }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Entire session")) { continuous_begin_ = 0; continuous_end_ = static_cast<int>(session_->laps.size()) - 1; }
        ImGui::SameLine();
        if (ImGui::Button("Current + next 5")) {
            continuous_begin_ = active_lap_index_;
            continuous_end_ = std::min(static_cast<int>(session_->laps.size()) - 1, active_lap_index_ + 5);
        }
        ImGui::SliderInt("Start lap", &continuous_begin_, 0, static_cast<int>(session_->laps.size()) - 1);
        continuous_end_ = std::max(continuous_end_, continuous_begin_);
        ImGui::SliderInt("End lap", &continuous_end_, continuous_begin_, static_cast<int>(session_->laps.size()) - 1);
        ImGui::Checkbox("Include outlap", &include_out_lap_); ImGui::SameLine(); ImGui::Checkbox("Include inlap", &include_in_lap_);
    }
    const auto lap_label = [&](int lap_index) {
        if (lap_index < 0 || lap_index >= static_cast<int>(session_->laps.size())) return std::string("None");
        const auto& lap = session_->laps[static_cast<std::size_t>(lap_index)];
        const auto name = lap.race_lap > 0 ? std::format("R{}", lap.race_lap) : std::format("L{}", lap.raw_lap);
        return std::format("{} - {} - {}", name, lap_time(lap.duration_us), phase_name(lap.phase));
    };
    const auto invalidate_comparison = [&] {
        plot_cache_active_ = -1;
        plot_cache_compare_ = -1;
        plot_cache_compare_b_ = -1;
        driver_analysis_dirty_ = true;
    };
    ImGui::SeparatorText("LAP SELECTION");
    if (ImGui::Checkbox("Auto fastest complete reference", &fastest_reference_) && fastest_reference_) {
        const auto fastest = fastest_complete_lap(session_->laps);
        if (fastest != session_->laps.size()) {
            active_lap_index_ = static_cast<int>(fastest);
            cursor_us_ = session_->telemetry.time_us[session_->laps[fastest].begin_index];
            ensure_unique_complete_comparison_roles();
            invalidate_comparison();
        }
    }
    const auto combo = [&](const char* heading, const char* id, int& selected_index, const ImVec4& color,
                           auto disabled_for, auto on_change) {
        ImGui::TextColored(color, "%s", heading);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::BeginCombo(id, lap_label(selected_index).c_str())) {
            for (std::size_t index = 0; index < session_->laps.size(); ++index) {
                const auto value = static_cast<int>(index);
                const auto disabled = disabled_for(value);
                ImGui::BeginDisabled(disabled);
                const auto selected = selected_index == value;
                if (ImGui::Selectable(lap_label(value).c_str(), selected) && !disabled) {
                    selected_index = value;
                    on_change(index);
                    invalidate_comparison();
                }
                ImGui::EndDisabled();
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor();
    };
    combo("REFERENCE", "##reference-lap", active_lap_index_, ImVec4(0.20F, 0.78F, 0.40F, 1.0F),
        [&](int value) { return session_->laps[static_cast<std::size_t>(value)].phase != LapPhase::Complete ||
            value == compare_lap_index_ || value == compare_b_lap_index_; }, [&](std::size_t index) {
            fastest_reference_ = false;
            cursor_us_ = session_->telemetry.time_us[session_->laps[index].begin_index];
        });
    combo("COMPARE A", "##compare-a-lap", compare_lap_index_, ImVec4(1.0F, 0.68F, 0.12F, 1.0F),
        [&](int value) { return session_->laps[static_cast<std::size_t>(value)].phase != LapPhase::Complete ||
            value == active_lap_index_ || value == compare_b_lap_index_; }, [](std::size_t) {});
    combo("COMPARE B", "##compare-b-lap", compare_b_lap_index_, ImVec4(0.95F, 0.25F, 0.28F, 1.0F),
        [&](int value) { return session_->laps[static_cast<std::size_t>(value)].phase != LapPhase::Complete ||
            value == active_lap_index_ || value == compare_lap_index_; }, [](std::size_t) {});
    combo("PLAYBACK LAP", "##playback-lap", playback_lap_index_, ImVec4(0.10F, 0.85F, 0.95F, 1.0F),
        [](int) { return false; }, [](std::size_t) {});
    const auto complete_role = [&](int value) { return value >= 0 && value < static_cast<int>(session_->laps.size()) &&
        session_->laps[static_cast<std::size_t>(value)].phase == LapPhase::Complete; };
    const auto valid_roles = active_lap_index_ != compare_lap_index_ && active_lap_index_ != compare_b_lap_index_ &&
        compare_lap_index_ != compare_b_lap_index_ && complete_role(active_lap_index_) &&
        complete_role(compare_lap_index_) && complete_role(compare_b_lap_index_);
    ImGui::BeginDisabled(!valid_roles);
    if (ImGui::Button("Compare selected laps")) { view_mode_ = ViewMode::Compare; invalidate_comparison(); }
    ImGui::EndDisabled();
    if (!valid_roles) { ImGui::SameLine(); ImGui::TextDisabled("Reference, A and B need different complete laps"); }

    ImGui::SeparatorText("ALL LAPS");
    if (ImGui::BeginTable("laps", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Lap", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 1.25F);
        ImGui::TableSetupColumn("Delta", ImGuiTableColumnFlags_WidthStretch, 0.85F);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 32.0F);
        ImGui::TableHeadersRow();
        const auto reference_duration = active_lap() ? active_lap()->duration_us : Timestamp{};
        for (std::size_t index = 0; index < session_->laps.size(); ++index) {
            const auto& lap = session_->laps[index];
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const auto lap_name = lap.race_lap > 0 ? std::format("R{}", lap.race_lap) : std::format("L{}", lap.raw_lap);
            std::string roles;
            if (active_lap_index_ == static_cast<int>(index)) roles += "R";
            if (compare_lap_index_ == static_cast<int>(index)) roles += roles.empty() ? "A" : "/A";
            if (compare_b_lap_index_ == static_cast<int>(index)) roles += roles.empty() ? "B" : "/B";
            if (playback_lap_index_ == static_cast<int>(index)) roles += roles.empty() ? "P" : "/P";
            const auto label = roles.empty() ? lap_name : std::format("{}  {}", lap_name, roles);
            const auto reference_disabled = lap.phase != LapPhase::Complete ||
                compare_lap_index_ == static_cast<int>(index) || compare_b_lap_index_ == static_cast<int>(index);
            ImGui::BeginDisabled(reference_disabled);
            if (ImGui::Selectable(label.c_str(), active_lap_index_ == static_cast<int>(index), ImGuiSelectableFlags_SpanAllColumns) && !reference_disabled) {
                active_lap_index_ = static_cast<int>(index); fastest_reference_ = false;
                cursor_us_ = session_->telemetry.time_us[lap.begin_index]; invalidate_comparison();
            }
            ImGui::EndDisabled();
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(lap_time(lap.duration_us).c_str());
            ImGui::TableSetColumnIndex(2);
            if (lap.phase == LapPhase::Complete && reference_duration > 0) {
                const auto delta = static_cast<double>(lap.duration_us - reference_duration) / kSecond;
                ImGui::TextColored(delta <= 0.0 ? ui::color(ui::ColorToken::Positive) : ui::color(ui::ColorToken::TextMuted), "%+.3f", delta);
            } else ImGui::TextDisabled("%s", phase_name(lap.phase));
            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton("...")) ImGui::OpenPopup("lap-role-popup");
            if (ImGui::BeginPopup("lap-role-popup")) {
                ImGui::TextDisabled("Assign %s", lap_name.c_str());
                ImGui::Separator();
                ImGui::BeginDisabled(reference_disabled);
                if (ImGui::MenuItem("Set as Reference")) {
                    active_lap_index_ = static_cast<int>(index); fastest_reference_ = false;
                    cursor_us_ = session_->telemetry.time_us[lap.begin_index]; invalidate_comparison();
                }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(lap.phase != LapPhase::Complete || active_lap_index_ == static_cast<int>(index) ||
                    compare_b_lap_index_ == static_cast<int>(index));
                if (ImGui::MenuItem("Set as Compare A")) { compare_lap_index_ = static_cast<int>(index); invalidate_comparison(); }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(lap.phase != LapPhase::Complete || active_lap_index_ == static_cast<int>(index) ||
                    compare_lap_index_ == static_cast<int>(index));
                if (ImGui::MenuItem("Set as Compare B")) { compare_b_lap_index_ = static_cast<int>(index); invalidate_comparison(); }
                ImGui::EndDisabled();
                if (ImGui::MenuItem("Set as Playback")) playback_lap_index_ = static_cast<int>(index);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    const auto stats = [&](const char* label, const LapInfo* lap, const ImVec4& color) {
        if (!lap) return;
        float maximum_speed = 0.0F, lateral = 0.0F;
        double speed_sum = 0.0;
        for (auto index = lap->begin_index; index <= lap->end_index; ++index) {
            maximum_speed = std::max(maximum_speed, session_->telemetry.speed_kmh[index]);
            lateral = std::max(lateral, std::abs(session_->telemetry.lateral_g[index]));
            speed_sum += session_->telemetry.speed_kmh[index];
        }
        const auto count = lap->end_index - lap->begin_index + 1;
        ImGui::TextColored(color, "%s", label);
        ImGui::SameLine();
        const auto conversion = metric_units_ ? 1.0 : 0.621371;
        ImGui::Text("max %.1f %s  avg %.1f %s  max lat %.2f g", maximum_speed * conversion,
            metric_units_ ? "km/h" : "mph", speed_sum / count * conversion, metric_units_ ? "km/h" : "mph", lateral);
    };
    if (ImGui::CollapsingHeader("Lap statistics")) {
        stats("Reference", active_lap(), ui::color(ui::ColorToken::Reference));
        if (view_mode_ == ViewMode::Compare) {
            stats("Compare A", compare_lap(), ui::color(ui::ColorToken::CompareA));
            stats("Compare B", compare_b_lap(), ui::color(ui::ColorToken::CompareB));
        }
    }
    handle_annotation_window("Laps and Sectors");
    ImGui::End();
}

void NativeApp::draw_radio() {
    if (!ImGui::Begin("Radio Alignment")) { ImGui::End(); return; }
    if (!session_) { ImGui::TextDisabled("No session loaded"); ImGui::End(); return; }
    if (session_->radio.empty()) {
        ImGui::TextDisabled("Sanwa controls were not recorded for this session.");
        ImGui::TextWrapped("Open a compatible Sanwa CSV with the GPS sources to calculate control alignment and correlation quality.");
        handle_annotation_window("Radio Alignment");
        ImGui::End();
        return;
    }
    const auto& alignment = session_->alignment;
    ImGui::Text("Sanwa samples: %zu at native 100 Hz", session_->radio.size());
    ImGui::Text("Anchor: %+.3f s", static_cast<double>(alignment.radio_anchor_us) / kSecond);
    ImGui::Text("Fine correction: %+.0f ms", static_cast<double>(alignment.fine_correction_us) / 1000.0);
    ImGui::Text("Throttle response: %.0f ms", static_cast<double>(alignment.throttle_response_us) / 1000.0);
    ImGui::Text("Steering response: %.0f ms", static_cast<double>(alignment.steering_response_us) / 1000.0);
    ImGui::SeparatorText("GPS / SANWA CORRELATION");
    ImGui::Text("Input / acceleration r: %.3f", alignment.trigger_correlation);
    ImGui::Text("Acceleration direction: %.0f%%", alignment.direction_agreement * 100.0);
    ImGui::Text("Steering / yaw r: %.3f", alignment.steering_yaw_correlation);
    ImGui::Text("Median lap steering r: %.3f", alignment.lap_steering_correlation);
    ImGui::TextColored(alignment.confidence == "high" ? ImVec4(0.25F, 0.8F, 0.45F, 1) : ImVec4(1, 0.65F, 0.2F, 1),
                       "Confidence: %s", alignment.confidence.c_str());
    ImGui::TextWrapped("%s", alignment.reason.c_str());
    handle_annotation_window("Radio Alignment");
    ImGui::End();
}

void NativeApp::draw_analysis() {
    if (!ImGui::Begin("Analysis")) { ImGui::End(); return; }
    if (!session_) { ImGui::TextDisabled("No session loaded"); ImGui::End(); return; }
    const auto fastest = fastest_complete_lap(session_->laps);
    if (fastest != session_->laps.size()) {
        const auto& lap = session_->laps[fastest];
        ImGui::Text("Fastest complete lap: R%d  %s", lap.race_lap, lap_time(lap.duration_us).c_str());
    }
    ImGui::Text("Physical sectors: %zu", session_->sector_markers.size() + 1);
    for (std::size_t index = 0; index < session_->sector_markers.size(); ++index) {
        auto percent = static_cast<float>(session_->sector_markers[index].reference_fraction * 100.0);
        ImGui::SetNextItemWidth(210.0F);
        const auto label = std::format("S{} boundary##sector{}", index + 1, index);
        if (ImGui::SliderFloat(label.c_str(), &percent, 2.0F, 98.0F, "%.1f%%")) {
            move_sector_marker(*session_, index, percent / 100.0);
        }
    }
    ImGui::Text("Theoretical best: %s", lap_time(session_->theoretical_best.duration_us).c_str());
    if (ImGui::BeginTable("theory", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Sector"); ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Source lap"); ImGui::TableHeadersRow();
        for (const auto& sector : session_->theoretical_best.sectors) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("S%d", sector.sector);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(lap_time(sector.duration_us).c_str());
            ImGui::TableNextColumn(); ImGui::Text("L%d", sector.source_raw_lap);
        }
        ImGui::EndTable();
    }
    handle_annotation_window("Analysis");
    ImGui::End();
}

void NativeApp::draw_diagnostics() {
    if (!ImGui::Begin("Diagnostics")) { ImGui::End(); return; }
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    auto sorted = frame_times_ms_;
    std::sort(sorted.begin(), sorted.end());
    const auto p95 = sorted.empty() ? 0.0F : sorted[static_cast<std::size_t>((sorted.size() - 1) * 0.95)];
    const auto worst = sorted.empty() ? 0.0F : sorted.back();
    ImGui::Text("Renderer: %s", software_renderer_ ? "Microsoft WARP" : "DirectX 11 hardware");
    ImGui::Text("Frame: %.2f ms  p95 %.2f ms  worst %.2f ms", ImGui::GetIO().DeltaTime * 1000.0F, p95, worst);
    ImGui::Text("Rate: %.1f FPS", ImGui::GetIO().Framerate);
    ImGui::Text("Working memory: %.1f MB", static_cast<double>(memory.WorkingSetSize) / (1024.0 * 1024.0));
    if (session_) {
        const auto telemetry_bytes = session_->telemetry.size() *
            (sizeof(Timestamp) + sizeof(std::int64_t) + 2 * sizeof(double) + 5 * sizeof(float) + sizeof(std::uint8_t) + sizeof(std::int32_t));
        const auto radio_bytes = session_->radio.size() * (sizeof(Timestamp) + 3 * sizeof(float));
        ImGui::Text("Canonical buffers: %.1f MB", static_cast<double>(telemetry_bytes + radio_bytes) / (1024.0 * 1024.0));
        ImGui::Text("Plot cache: %zu reference + %zu A + %zu B points", primary_plot_cache_.time.size(),
            compare_plot_cache_.time.size(), compare_b_plot_cache_.time.size());
    }
    ImGui::TextDisabled("Diagnostics remain local; nothing is uploaded.");
    handle_annotation_window("Diagnostics");
    ImGui::End();
}

void NativeApp::draw_gg_plot() {
    if (!ImGui::Begin("G-G Plot")) { ImGui::End(); return; }
    if (!session_) { ImGui::TextDisabled("No session loaded"); ImGui::End(); return; }
    refresh_plot_cache();
    if (ImPlot::BeginPlot("Lateral / Longitudinal G", ImVec2(-1, -1), ImPlotFlags_Equal)) {
        ImPlot::SetupAxes("Lateral G: left - / right +", "Longitudinal G: brake - / accel +");
        auto g_limit = 4.0;
        const auto include_g = [&](const PlotData& values) {
            for (const auto value : values.lateral) if (std::isfinite(value)) g_limit = std::max(g_limit, std::abs(value) * 1.15);
            for (const auto value : values.longitudinal) if (std::isfinite(value)) g_limit = std::max(g_limit, std::abs(value) * 1.15);
        };
        include_g(primary_plot_cache_); include_g(compare_plot_cache_); include_g(compare_b_plot_cache_);
        ImPlot::SetupAxisLimits(ImAxis_X1, -g_limit, g_limit, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -g_limit, g_limit, ImGuiCond_Always);
        const ImPlotSpec primary_style{ImPlotProp_LineColor, ImVec4(0.20F, 0.78F, 0.40F, 0.8F),
            ImPlotProp_Marker, ImPlotMarker_Circle, ImPlotProp_MarkerSize, 2.0F, ImPlotProp_LineWeight, 0.0F};
        ImPlot::PlotScatter("Reference", primary_plot_cache_.lateral.data(), primary_plot_cache_.longitudinal.data(),
            static_cast<int>(std::min(primary_plot_cache_.lateral.size(), primary_plot_cache_.longitudinal.size())), primary_style);
        if (view_mode_ == ViewMode::Compare && !compare_plot_cache_.lateral.empty()) {
            const ImPlotSpec compare_style{ImPlotProp_LineColor, ImVec4(1.0F, 0.68F, 0.12F, 0.65F),
                ImPlotProp_Marker, ImPlotMarker_Circle, ImPlotProp_MarkerSize, 2.0F, ImPlotProp_LineWeight, 0.0F};
            ImPlot::PlotScatter("Compare A", compare_plot_cache_.lateral.data(), compare_plot_cache_.longitudinal.data(),
                static_cast<int>(std::min(compare_plot_cache_.lateral.size(), compare_plot_cache_.longitudinal.size())), compare_style);
        }
        if (view_mode_ == ViewMode::Compare && !compare_b_plot_cache_.lateral.empty()) {
            const ImPlotSpec compare_b_style{ImPlotProp_LineColor, ImVec4(0.95F, 0.25F, 0.28F, 0.60F),
                ImPlotProp_Marker, ImPlotMarker_Circle, ImPlotProp_MarkerSize, 2.0F, ImPlotProp_LineWeight, 0.0F};
            ImPlot::PlotScatter("Compare B", compare_b_plot_cache_.lateral.data(), compare_b_plot_cache_.longitudinal.data(),
                static_cast<int>(std::min(compare_b_plot_cache_.lateral.size(), compare_b_plot_cache_.longitudinal.size())), compare_b_style);
        }
        ImPlot::EndPlot();
    }
    ImGui::TextWrapped("The envelope shows combined tire load. Points near the outside are the hardest acceleration, braking, and cornering events.");
    handle_annotation_window("G-G Plot");
    ImGui::End();
}

void NativeApp::draw_altitude() {
    if (!ImGui::Begin("Altitude")) { ImGui::End(); return; }
    if (!session_) { ImGui::TextDisabled("No session loaded"); ImGui::End(); return; }
    refresh_plot_cache();
    if (ImPlot::BeginPlot("GPS altitude", ImVec2(-1, -1), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes(view_mode_ == ViewMode::Compare ? "Lap progress (%)" : "Time (s)", "Metres");
        if (!primary_plot_cache_.time.empty()) ImPlot::SetupAxisLimits(ImAxis_X1, primary_plot_cache_.time.front(), primary_plot_cache_.time.back(), ImGuiCond_Always);
        auto altitude_minimum = std::numeric_limits<double>::max();
        auto altitude_maximum = -std::numeric_limits<double>::max();
        const auto include_altitude = [&](const PlotData& values) {
            for (const auto value : values.altitude) if (std::isfinite(value)) {
                altitude_minimum = std::min(altitude_minimum, value); altitude_maximum = std::max(altitude_maximum, value);
            }
        };
        include_altitude(primary_plot_cache_); include_altitude(compare_plot_cache_); include_altitude(compare_b_plot_cache_);
        if (altitude_minimum <= altitude_maximum) {
            const auto span = std::max(0.5, altitude_maximum - altitude_minimum);
            ImPlot::SetupAxisLimits(ImAxis_Y1, altitude_minimum - span * 0.08, altitude_maximum + span * 0.12, ImGuiCond_Always);
        }
        const ImPlotSpec primary_style{ImPlotProp_LineColor, ImVec4(0.20F, 0.78F, 0.40F, 1.0F), ImPlotProp_LineWeight, 1.7F};
        ImPlot::PlotLine("Reference", primary_plot_cache_.time.data(), primary_plot_cache_.altitude.data(),
            static_cast<int>(std::min(primary_plot_cache_.time.size(), primary_plot_cache_.altitude.size())), primary_style);
        if (view_mode_ == ViewMode::Compare && !compare_plot_cache_.altitude.empty()) {
            const ImPlotSpec compare_style{ImPlotProp_LineColor, ImVec4(1.0F, 0.68F, 0.12F, 1.0F), ImPlotProp_LineWeight, 1.4F};
            ImPlot::PlotLine("Compare A", compare_plot_cache_.time.data(), compare_plot_cache_.altitude.data(),
                static_cast<int>(std::min(compare_plot_cache_.time.size(), compare_plot_cache_.altitude.size())), compare_style);
        }
        if (view_mode_ == ViewMode::Compare && !compare_b_plot_cache_.altitude.empty()) {
            const ImPlotSpec compare_b_style{ImPlotProp_LineColor, ImVec4(0.95F, 0.25F, 0.28F, 1.0F), ImPlotProp_LineWeight, 1.4F};
            ImPlot::PlotLine("Compare B", compare_b_plot_cache_.time.data(), compare_b_plot_cache_.altitude.data(),
                static_cast<int>(std::min(compare_b_plot_cache_.time.size(), compare_b_plot_cache_.altitude.size())), compare_b_style);
        }
        ImPlot::EndPlot();
    }
    ImGui::TextDisabled("GPS altitude is useful for trend context; small changes on flat ground can be receiver noise.");
    handle_annotation_window("Altitude");
    ImGui::End();
}

void NativeApp::handle_annotation_surface(const char* surface, int plot_index, float origin_x, float origin_y,
                                          float width, float height, bool hovered, const char* stable_plot_id) {
    if (width <= 1.0F || height <= 1.0F) return;
    auto* draw = ImGui::GetWindowDrawList();
    const auto mouse = ImGui::GetIO().MousePos;
    auto clicked_existing = false;
    for (auto& annotation : annotations_) {
        if (stable_plot_id && *stable_plot_id) {
            const auto stable_match = annotation.plot_id == stable_plot_id ||
                (annotation.plot_id.empty() && annotation.surface == surface);
            if (!stable_match) continue;
        } else if (annotation.surface != surface || annotation.plot_index != plot_index) {
            continue;
        }
        const auto position = ImVec2(origin_x + annotation.normalized_x * width, origin_y + annotation.normalized_y * height);
        const auto selected = annotation.id == selected_annotation_id_;
        const auto radius = selected ? 11.0F : 9.0F;
        if (show_annotation_pins_) {
            draw->AddCircleFilled(position, radius, selected ? IM_COL32(255, 176, 32, 255) : IM_COL32(23, 126, 214, 245));
            draw->AddCircle(position, radius, IM_COL32(255, 255, 255, 255), 0, 2.0F);
            const auto number = std::to_string(annotation.id);
            const auto text_size = ImGui::CalcTextSize(number.c_str());
            draw->AddText(position - text_size * 0.5F, IM_COL32(255, 255, 255, 255), number.c_str());
        }
        const auto delta_x = mouse.x - position.x;
        const auto delta_y = mouse.y - position.y;
        if (show_annotation_pins_ && hovered && delta_x * delta_x + delta_y * delta_y <= (radius + 3.0F) * (radius + 3.0F)) {
            const auto note = annotation.note.data()[0] == '\0' ? "No comment yet" : annotation.note.data();
            ImGui::SetTooltip("#%d  %s\n%s", annotation.id, annotation.surface.c_str(), note);
            if (annotation_mode_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                selected_annotation_id_ = annotation.id;
                clicked_existing = true;
                annotation_click_consumed_ = true;
            }
        }
    }
    if (!annotation_mode_ || !hovered || clicked_existing || annotation_click_consumed_ ||
        !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;

    Annotation annotation;
    annotation.id = next_annotation_id_++;
    annotation.surface = surface;
    if (stable_plot_id) annotation.plot_id = stable_plot_id;
    annotation.plot_index = plot_index;
    annotation.normalized_x = std::clamp((mouse.x - origin_x) / width, 0.0F, 1.0F);
    annotation.normalized_y = std::clamp((mouse.y - origin_y) / height, 0.0F, 1.0F);
    annotation.cursor_us = hover_cursor_us_.value_or(cursor_us_);
    annotation.active_lap_index = active_lap_index_;
    annotation.compare_lap_index = compare_lap_index_;
    annotation.compare_b_lap_index = compare_b_lap_index_;
    annotation.playback_lap_index = playback_lap_index_;
    annotation.view_mode = view_mode_;
    selected_annotation_id_ = annotation.id;
    annotations_.push_back(std::move(annotation));
    annotation_click_consumed_ = true;
    status_ = std::format("Annotation #{} added on {}", selected_annotation_id_, surface);
}

void NativeApp::handle_annotation_window(const char* surface) {
    const auto window_position = ImGui::GetWindowPos();
    const auto content_min = ImGui::GetWindowContentRegionMin();
    const auto content_max = ImGui::GetWindowContentRegionMax();
    const auto origin = window_position + content_min;
    const auto size = content_max - content_min;
    const auto hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    handle_annotation_surface(surface, -2, origin.x, origin.y, size.x, size.y, hovered);
}

void NativeApp::handle_annotation_workspace() {
    constexpr int workspace_plot_index = -3;
    const auto mouse = ImGui::GetMousePos();
    const auto* annotation_editor = ImGui::FindWindowByName("Insights");
    const auto over_annotation_editor = annotation_editor_hovered_ || (annotation_editor &&
        (!annotation_editor->DockIsActive || annotation_editor->DockTabIsVisible) &&
        ImGui::IsMouseHoveringRect(annotation_editor->Pos, annotation_editor->Pos + annotation_editor->Size, false));
    const auto pin_near_mouse = [&](ImVec2 position, float radius) {
        const auto dx = mouse.x - position.x;
        const auto dy = mouse.y - position.y;
        return dx * dx + dy * dy <= (radius + 3.0F) * (radius + 3.0F);
    };
    const auto bounds_for = [](const Annotation& annotation) {
        struct Bounds { ImVec2 origin; ImVec2 size; ImGuiViewport* viewport; };
        if (!annotation.anchor_window.empty()) {
            if (const auto* window = ImGui::FindWindowByName(annotation.anchor_window.c_str())) {
                const auto visible = window->Active && (!window->DockIsActive || window->DockTabIsVisible) && !window->Hidden;
                if (visible) return Bounds{window->Pos, window->Size, window->Viewport};
                return Bounds{{}, {}, nullptr};
            }
            return Bounds{{}, {}, nullptr};
        }
        auto* viewport = ImGui::GetMainViewport();
        return Bounds{viewport->Pos, viewport->Size, viewport};
    };

    for (auto& annotation : annotations_) {
        if (annotation.plot_index != workspace_plot_index) continue;
        const auto bounds = bounds_for(annotation);
        if (!bounds.viewport || bounds.size.x <= 1.0F || bounds.size.y <= 1.0F) continue;
        const auto position = ImVec2(bounds.origin.x + annotation.normalized_x * bounds.size.x,
                                     bounds.origin.y + annotation.normalized_y * bounds.size.y);
        const auto selected = annotation.id == selected_annotation_id_;
        const auto radius = selected ? 11.0F : 9.0F;
        if (show_annotation_pins_) {
            auto* draw = ImGui::GetForegroundDrawList(bounds.viewport);
            draw->AddCircleFilled(position, radius, selected ? IM_COL32(255, 176, 32, 255) : IM_COL32(23, 126, 214, 245));
            draw->AddCircle(position, radius, IM_COL32(255, 255, 255, 255), 0, 2.0F);
            const auto number = std::to_string(annotation.id);
            const auto text_size = ImGui::CalcTextSize(number.c_str());
            draw->AddText(position - text_size * 0.5F, IM_COL32(255, 255, 255, 255), number.c_str());
        }
        if (!over_annotation_editor && show_annotation_pins_ && pin_near_mouse(position, radius)) {
            const auto note = annotation.note.data()[0] == '\0' ? "No comment yet" : annotation.note.data();
            ImGui::SetTooltip("#%d  %s\n%s", annotation.id, annotation.surface.c_str(), note);
            if (annotation_mode_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                selected_annotation_id_ = annotation.id;
                annotation_click_consumed_ = true;
            }
        }
    }

    if (!annotation_mode_ || annotation_click_consumed_ || over_annotation_editor || ImGui::IsAnyItemActive() ||
        !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;

    auto* hovered = GImGui ? GImGui->HoveredWindow : nullptr;
    if (hovered && (hovered->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) != 0) return;
    auto* anchor = hovered;
    while (anchor && anchor->ParentWindow) anchor = anchor->ParentWindow;
    if (anchor && std::string_view(anchor->Name) == "Insights") return;

    const auto* viewport = ImGui::GetMainViewport();
    auto origin = viewport->Pos;
    auto size = viewport->Size;
    std::string surface = "Workspace";
    std::string anchor_name;
    if (anchor) {
        origin = anchor->Pos;
        size = anchor->Size;
        anchor_name = anchor->Name;
        const auto name = std::string_view(anchor->Name);
        if (name == "##MainMenuBar") surface = "Main menu";
        else if (name.starts_with("##") || name.starts_with("WindowOverViewport_")) surface = "Dock tabs / workspace";
        else surface = std::string(name.substr(0, name.find("###")));
    }
    if (size.x <= 1.0F || size.y <= 1.0F) return;

    Annotation annotation;
    annotation.id = next_annotation_id_++;
    annotation.surface = std::move(surface);
    annotation.anchor_window = std::move(anchor_name);
    annotation.plot_index = workspace_plot_index;
    annotation.normalized_x = std::clamp((mouse.x - origin.x) / size.x, 0.0F, 1.0F);
    annotation.normalized_y = std::clamp((mouse.y - origin.y) / size.y, 0.0F, 1.0F);
    annotation.cursor_us = hover_cursor_us_.value_or(cursor_us_);
    annotation.active_lap_index = active_lap_index_;
    annotation.compare_lap_index = compare_lap_index_;
    annotation.compare_b_lap_index = compare_b_lap_index_;
    annotation.playback_lap_index = playback_lap_index_;
    annotation.view_mode = view_mode_;
    selected_annotation_id_ = annotation.id;
    annotations_.push_back(std::move(annotation));
    annotation_click_consumed_ = true;
    status_ = std::format("Annotation #{} added on {}", selected_annotation_id_, annotations_.back().surface);
}

void NativeApp::go_to_annotation(const Annotation& annotation) {
    if (!session_ || session_->telemetry.empty()) return;
    playing_ = false;
    view_mode_ = annotation.view_mode;
    continuous_drilldown_ = false;
    active_lap_index_ = std::clamp(annotation.active_lap_index, 0, static_cast<int>(session_->laps.size()) - 1);
    compare_lap_index_ = std::clamp(annotation.compare_lap_index, 0, static_cast<int>(session_->laps.size()) - 1);
    compare_b_lap_index_ = std::clamp(annotation.compare_b_lap_index, 0, static_cast<int>(session_->laps.size()) - 1);
    playback_lap_index_ = std::clamp(annotation.playback_lap_index, 0, static_cast<int>(session_->laps.size()) - 1);
    fastest_reference_ = false;
    if (view_mode_ == ViewMode::Compare && !ensure_unique_complete_comparison_roles()) {
        view_mode_ = ViewMode::SingleLap;
        error_ = "Saved compare view needs three different complete laps";
    }
    cursor_us_ = std::clamp(annotation.cursor_us, session_->telemetry.time_us.front(), session_->telemetry.time_us.back());
    plot_cache_active_ = -1;
    plot_cache_compare_ = -1;
    plot_cache_compare_b_ = -1;
    plot_cache_mode_ = static_cast<ViewMode>(-1);
    driver_analysis_dirty_ = true;
    sync_continuous_lap();
    plot_fit_pending_ = true;
}

void NativeApp::draw_annotations() {
    const auto visible = ImGui::Begin("Insights");
    annotation_editor_hovered_ = ImGui::IsWindowHovered(
        ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (!visible) { ImGui::End(); return; }
    if (!session_) { ImGui::TextDisabled("No session loaded"); ImGui::End(); return; }
    refresh_driver_analysis();
    if (ImGui::BeginTabBar("insights-tabs")) {
        if (ImGui::BeginTabItem("Insights")) {
            ImGui::TextDisabled("Deterministic formula set %s; raw GPS is never rewritten.", driver_analysis_.formula_version.c_str());
            if (ImGui::Button("Export analysis JSON...")) export_driver_analysis();
            for (std::size_t slot = 0; slot < driver_analysis_.comparisons.size(); ++slot) {
                if (!driver_analysis_.comparisons[slot]) continue;
                const auto& comparison = *driver_analysis_.comparisons[slot];
                ImGui::TextDisabled("Compare %c GPS analysis-only correction %.2f m (residual %.2f m) | line metrics %s",
                    slot == 0 ? 'A' : 'B', comparison.line_translation.magnitude_m,
                    comparison.line_translation.residual_rms_m,
                    comparison.confidence.line_metrics_enabled ? "enabled" : "disabled");
            }
            if (driver_analysis_.insights.empty()) {
                ImGui::TextWrapped("No verified insights passed the enabled confidence and rule gates for these laps.");
            }
            for (std::size_t index = 0; index < driver_analysis_.insights.size(); ++index) {
                const auto& insight = driver_analysis_.insights[index];
                ImGui::PushID(static_cast<int>(index));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0F);
                ImGui::BeginChild("insight-card", ImVec2(-1.0F, 132.0F), ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_NoScrollbar);
                const auto severity_color = insight.positive ? ImVec4(0.20F, 0.82F, 0.42F, 1.0F) :
                    insight.severity == driver_analysis::Severity::High ? ImVec4(0.96F, 0.25F, 0.27F, 1.0F) :
                    insight.severity == driver_analysis::Severity::Medium ? ImVec4(1.0F, 0.65F, 0.12F, 1.0F) :
                    ImVec4(0.72F, 0.76F, 0.82F, 1.0F);
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(severity_color.x, severity_color.y, severity_color.z, 0.23F));
                const auto role = insight.comparison == driver_analysis::ComparisonSlot::CompareA ? "A" : "B";
                const auto heading = std::format("{} | {} | Compare {}##insight", insight.title, insight.corner_name, role);
                if (ImGui::Selectable(heading.c_str(), selected_insight_index_ == static_cast<int>(index))) {
                    selected_insight_index_ = static_cast<int>(index);
                    const auto corner = std::find_if(corner_zones_.begin(), corner_zones_.end(), [&](const auto& value) {
                        return value.id == insight.corner_id;
                    });
                    selected_corner_index_ = corner == corner_zones_.end() ? -1 : static_cast<int>(std::distance(corner_zones_.begin(), corner));
                    navigate_to_analysis_target(insight.navigation);
                }
                ImGui::PopStyleColor();
                ImGui::TextWrapped("%s", insight.detail.c_str());
                const auto confidence_label = insight.confidence_band == driver_analysis::ConfidenceBand::WeakSignal
                    ? "WEAK SIGNAL" : "ACTIONABLE";
                ImGui::TextColored(severity_color, "%s | %s | confidence %d%% | time effect %+.3f s",
                    insight.positive ? "POSITIVE" : insight.severity == driver_analysis::Severity::High ? "HIGH" :
                    insight.severity == driver_analysis::Severity::Medium ? "MEDIUM" : "LOW",
                    confidence_label, insight.confidence, insight.estimated_time_effect_s);
                ImGui::TextDisabled("Rule %s | threshold %.3f", insight.rule_stable_id.c_str(), insight.threshold);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::Dummy(ImVec2(0.0F, 3.0F));
                ImGui::PopID();
            }
            if (ImGui::CollapsingHeader("Derived metrics")) {
            if (ImGui::BeginTable("derived-metrics", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY, ImVec2(-1, 270.0F))) {
                ImGui::TableSetupColumn("Lap"); ImGui::TableSetupColumn("Corner"); ImGui::TableSetupColumn("Metric");
                ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Threshold");
                ImGui::TableSetupColumn("Status"); ImGui::TableSetupColumn("Confidence"); ImGui::TableHeadersRow();
                for (std::size_t slot = 0; slot < driver_analysis_.comparisons.size(); ++slot) {
                    if (!driver_analysis_.comparisons[slot]) continue;
                    const auto& comparison = *driver_analysis_.comparisons[slot];
                    for (const auto& corner : comparison.corners) for (const auto& metric : corner.metrics) {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%c", slot == 0 ? 'A' : 'B');
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(corner.corner_name.c_str());
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(driver_analysis::metric_name(metric.kind).data());
                        ImGui::TableNextColumn();
                        if (metric.value) ImGui::Text("%+.3f", *metric.value); else ImGui::TextDisabled("n/a");
                        ImGui::TableNextColumn();
                        if (metric.rule_id) ImGui::Text("%.3f", metric.threshold); else ImGui::TextDisabled("context");
                        ImGui::TableNextColumn();
                        if (!metric.value) ImGui::TextDisabled("N/A");
                        else if (metric.positive && metric.triggered) ImGui::TextColored(ImVec4(0.2F, 0.82F, 0.42F, 1), "GAIN");
                        else if (metric.triggered) ImGui::TextColored(ImVec4(1.0F, 0.52F, 0.16F, 1), "FLAG");
                        else ImGui::TextColored(ImVec4(0.38F, 0.76F, 0.52F, 1), "PASS");
                        ImGui::TableNextColumn(); ImGui::Text("%d%%", metric.confidence);
                    }
                }
                ImGui::EndTable();
            }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Rules / Formula")) {
            auto rules_changed = false;
            if (ImGui::Button("Reset all rules")) { analysis_rules_ = driver_analysis::default_analysis_rules(); rules_changed = true; }
            ImGui::SameLine(); ImGui::TextDisabled("%s", driver_analysis::kFormulaVersion.data());
            for (const auto& descriptor : driver_analysis::rule_descriptors()) {
                auto setting = std::find_if(analysis_rules_.metrics.begin(), analysis_rules_.metrics.end(), [&](const auto& value) {
                    return value.id == descriptor.id;
                });
                if (setting == analysis_rules_.metrics.end()) continue;
                ImGui::PushID(descriptor.stable_id.data());
                rules_changed |= ImGui::Checkbox("##enabled", &setting->enabled);
                ImGui::SameLine(); ImGui::TextUnformatted(descriptor.name.data());
                ImGui::TextDisabled("Threshold"); ImGui::SameLine(); ImGui::SetNextItemWidth(92.0F);
                rules_changed |= ImGui::InputDouble("##threshold", &setting->threshold, 0.01, 0.05, "%.3f");
                ImGui::SameLine(); ImGui::TextDisabled("%s", descriptor.unit.data());
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset")) { setting->enabled = true; setting->threshold = descriptor.default_threshold; rules_changed = true; }
                ImGui::TextWrapped("Formula: %s", descriptor.formula.data());
                ImGui::Separator();
                ImGui::PopID();
            }
            if (ImGui::CollapsingHeader("Event detector thresholds")) {
                rules_changed |= ImGui::InputFloat("Brake begins (%)", &analysis_rules_.brake_begin_percent, 1.0F, 5.0F, "%.1f");
                rules_changed |= ImGui::InputFloat("Brake released below (%)", &analysis_rules_.brake_release_percent, 1.0F, 5.0F, "%.1f");
                rules_changed |= ImGui::InputFloat("Turn-in steering (%)", &analysis_rules_.turn_in_steering_percent, 1.0F, 5.0F, "%.1f");
                rules_changed |= ImGui::InputFloat("First throttle (%)", &analysis_rules_.first_throttle_percent, 1.0F, 5.0F, "%.1f");
                rules_changed |= ImGui::InputFloat("Full throttle (%)", &analysis_rules_.full_throttle_percent, 1.0F, 5.0F, "%.1f");
                rules_changed |= ImGui::InputFloat("Steering correction (%)", &analysis_rules_.steering_correction_percent, 1.0F, 5.0F, "%.1f");
                rules_changed |= ImGui::InputDouble("Turn-in curvature (1/m)", &analysis_rules_.turn_in_curvature_per_m, 0.001, 0.005, "%.4f");
                const auto dwell_input = [&](const char* label, Timestamp& dwell) {
                    auto milliseconds = static_cast<double>(dwell) / 1000.0;
                    if (ImGui::InputDouble(label, &milliseconds, 5.0, 20.0, "%.0f ms")) {
                        dwell = static_cast<Timestamp>(std::clamp(milliseconds, 20.0, 500.0) * 1000.0);
                        rules_changed = true;
                    }
                };
                dwell_input("Brake-begin dwell", analysis_rules_.brake_begin_dwell_us);
                dwell_input("Brake-release dwell", analysis_rules_.brake_release_dwell_us);
                dwell_input("Turn-in dwell", analysis_rules_.turn_in_dwell_us);
                dwell_input("First-throttle dwell", analysis_rules_.first_throttle_dwell_us);
                dwell_input("Full-throttle dwell", analysis_rules_.full_throttle_dwell_us);
            }
            ImGui::SeparatorText("EDITABLE CORNERS / DISTANCE TIMELINE");
            if (ImGui::Button("Auto-suggest corners from reference")) {
                auto_number_corners();
                rules_changed = true;
            }
            ImGui::SameLine(); ImGui::TextDisabled("Drag each phase boundary; values are lap-distance percentages.");
            for (std::size_t index = 0; index < corner_zones_.size(); ++index) {
                auto& corner = corner_zones_[index];
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::TreeNodeEx(corner.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    std::array<char, 96> name{};
                    std::copy_n(corner.name.begin(), std::min(corner.name.size(), name.size() - 1), name.begin());
                    if (ImGui::InputText("Name", name.data(), name.size())) { corner.name = name.data(); rules_changed = true; }
                    auto phase_drag = [&](const char* label, double& value) {
                        auto percent = value * 100.0;
                        const double minimum = 0.0, maximum = 100.0;
                        if (ImGui::SliderScalar(label, ImGuiDataType_Double, &percent, &minimum,
                                &maximum, "%.2f%%")) { value = percent / 100.0; rules_changed = true; }
                    };
                    phase_drag("Start", corner.start_progress); phase_drag("Turn-in", corner.turn_in_progress);
                    phase_drag("Apex", corner.apex_progress); phase_drag("Exit", corner.exit_progress); phase_drag("End", corner.end_progress);
                    constexpr double epsilon = 0.0001;
                    corner.start_progress = std::clamp(corner.start_progress, 0.0, 1.0 - 4 * epsilon);
                    corner.turn_in_progress = std::clamp(corner.turn_in_progress, corner.start_progress + epsilon, 1.0 - 3 * epsilon);
                    corner.apex_progress = std::clamp(corner.apex_progress, corner.turn_in_progress + epsilon, 1.0 - 2 * epsilon);
                    corner.exit_progress = std::clamp(corner.exit_progress, corner.apex_progress + epsilon, 1.0 - epsilon);
                    corner.end_progress = std::clamp(corner.end_progress, corner.exit_progress + epsilon, 1.0);
                    if (ImGui::Button("Go to apex")) { selected_corner_index_ = static_cast<int>(index); navigate_to_progress(corner.apex_progress); }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (rules_changed) { driver_analysis_dirty_ = true; refresh_driver_analysis(); }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Dev Notes")) {
            draw_dev_notes();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void NativeApp::draw_dev_notes() {
    const auto linked_annotation = std::find_if(annotations_.begin(), annotations_.end(), [&](const Annotation& annotation) {
        return annotation.id == selected_annotation_id_;
    });
    const auto lap_label = [&](int index) {
        if (!session_ || session_->laps.empty()) return std::string("not loaded");
        index = std::clamp(index, 0, static_cast<int>(session_->laps.size()) - 1);
        const auto& lap = session_->laps[static_cast<std::size_t>(index)];
        if (lap.race_lap > 0) return std::format("R{} (raw {})", lap.race_lap, lap.raw_lap);
        return std::format("raw {} ({})", lap.raw_lap, phase_name(lap.phase));
    };

    ImGui::SeparatorText("NOTES FOR CODEX");
    ImGui::TextWrapped("Write what you want Codex to investigate or change. Select a numbered pin first when the note refers to an exact place in the interface or telemetry.");
    if (linked_annotation != annotations_.end()) {
        ImGui::TextColored(ImVec4(0.20F, 0.78F, 0.95F, 1.0F), "Linked pin #%d | %s | %.3fs",
            linked_annotation->id, linked_annotation->surface.c_str(),
            static_cast<double>(linked_annotation->cursor_us) / kSecond);
    } else {
        ImGui::TextDisabled("Linked pin: none (this note applies to the whole session)");
    }
    ImGui::InputTextMultiline("Notes for Codex##session-notes", session_notes_.data(), session_notes_.size(), ImVec2(-1, 150));
    if (ImGui::Button("Copy note for Codex")) {
        const auto* annotation = linked_annotation == annotations_.end() ? nullptr : &*linked_annotation;
        const auto reference_index = annotation ? annotation->active_lap_index : active_lap_index_;
        const auto compare_a_index = annotation ? annotation->compare_lap_index : compare_lap_index_;
        const auto compare_b_index = annotation ? annotation->compare_b_lap_index : compare_b_lap_index_;
        const auto playback_index = annotation ? annotation->playback_lap_index : playback_lap_index_;
        const auto context_view = annotation ? annotation->view_mode : view_mode_;
        const auto context_cursor = annotation ? annotation->cursor_us : cursor_us_;

        std::string payload = "RaceBox Viewer note for Codex\n";
        payload += std::format("Session: {}\n", session_ ? session_->name : "not loaded");
        if (annotation) {
            payload += std::format("Linked annotation: #{} | surface: {} | telemetry: {:.3f} s\n",
                annotation->id, annotation->surface, static_cast<double>(annotation->cursor_us) / kSecond);
        } else {
            payload += "Linked annotation: none (whole-session note)\n";
        }
        payload += std::format("View: {} | cursor: {:.3f} s\n", view_mode_name(context_view),
            static_cast<double>(context_cursor) / kSecond);
        payload += std::format("Reference: {}\nCompare A: {}\nCompare B: {}\nPlayback: {}\n",
            lap_label(reference_index), lap_label(compare_a_index), lap_label(compare_b_index), lap_label(playback_index));
        payload += "\nNotes:\n";
        payload += session_notes_.front() == '\0' ? "(no written note)" : session_notes_.data();
        if (annotation && annotation->note.front() != '\0') {
            payload += "\n\nExisting pin comment:\n";
            payload += annotation->note.data();
        }
        ImGui::SetClipboardText(payload.c_str());
        status_ = annotation
            ? std::format("Copied Notes for Codex with linked pin #{}", annotation->id)
            : "Copied Notes for Codex with current telemetry context";
    }
    ImGui::TextWrapped("Paste the copied text into your Codex task. The app does not send anything automatically.");

    ImGui::SeparatorText("LOCATION PINS");
    ImGui::Checkbox("Annotation mode", &annotation_mode_);
    ImGui::SameLine();
    ImGui::Checkbox("Show pins", &show_annotation_pins_);
    ImGui::TextWrapped(annotation_mode_
        ? "Click anywhere in the interface to place a numbered location link without operating controls underneath. Press Esc to finish."
        : "Turn on Annotation mode to place or select location links for your Codex note. Normal controls remain active while it is off.");
    ImGui::Separator();

    if (annotations_.empty()) {
        ImGui::TextDisabled("No annotations in this session.");
    } else if (ImGui::BeginTable("annotation-list", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                                 ImVec2(-1, 180))) {
        ImGui::TableSetupColumn("Pin", ImGuiTableColumnFlags_WidthFixed, 42.0F);
        ImGui::TableSetupColumn("Surface");
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 72.0F);
        ImGui::TableHeadersRow();
        for (const auto& annotation : annotations_) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const auto label = std::format("#{}##annotation{}", annotation.id, annotation.id);
            if (ImGui::Selectable(label.c_str(), selected_annotation_id_ == annotation.id, ImGuiSelectableFlags_SpanAllColumns)) {
                selected_annotation_id_ = annotation.id;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(annotation.surface.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.3fs", static_cast<double>(annotation.cursor_us) / kSecond);
        }
        ImGui::EndTable();
    }

    auto selected = std::find_if(annotations_.begin(), annotations_.end(), [&](const Annotation& annotation) {
        return annotation.id == selected_annotation_id_;
    });
    if (selected != annotations_.end()) {
        ImGui::SeparatorText(std::format("LINKED PIN #{}", selected->id).c_str());
        ImGui::Text("%s | %s", selected->surface.c_str(), view_mode_name(selected->view_mode));
        ImGui::Text("Telemetry %.3fs | ref %d | A %d | B %d | play %d",
            static_cast<double>(selected->cursor_us) / kSecond, selected->active_lap_index,
            selected->compare_lap_index, selected->compare_b_lap_index, selected->playback_lap_index);
        if (ImGui::Button("Go to pin")) go_to_annotation(*selected);
        ImGui::SameLine();
        if (ImGui::Button("Delete pin")) {
            annotations_.erase(selected);
            selected_annotation_id_ = annotations_.empty() ? 0 : annotations_.back().id;
        }
    }
    if (!annotations_.empty()) {
        ImGui::Separator();
        if (ImGui::Button("Export review JSON...")) export_annotations();
        ImGui::SameLine();
        if (ImGui::Button("Clear all")) {
            annotations_.clear();
            selected_annotation_id_ = 0;
        }
    }
}

void NativeApp::export_annotations() {
    if (annotations_.empty()) return;
    const auto path = save_file(window_, L"RaceBox Annotation Review", L"json", L"*.json");
    if (!path) return;
    try {
        nlohmann::json output{{"format", "racebox-annotation-review"}, {"version", 1},
                              {"session", session_ ? session_->name : std::string{}}, {"annotations", nlohmann::json::array()}};
        for (const auto& annotation : annotations_) {
            const auto* lap = session_ && annotation.active_lap_index >= 0 &&
                annotation.active_lap_index < static_cast<int>(session_->laps.size())
                ? &session_->laps[static_cast<std::size_t>(annotation.active_lap_index)] : nullptr;
            output["annotations"].push_back({
                {"id", annotation.id}, {"surface", annotation.surface}, {"anchor_window", annotation.anchor_window},
                {"plot_id", annotation.plot_id}, {"plot_index", annotation.plot_index},
                {"normalized_x", annotation.normalized_x}, {"normalized_y", annotation.normalized_y},
                {"telemetry_time_us", annotation.cursor_us}, {"telemetry_time_seconds", static_cast<double>(annotation.cursor_us) / kSecond},
                {"view_mode", view_mode_name(annotation.view_mode)}, {"active_lap_index", annotation.active_lap_index},
                {"active_lap", lap ? (lap->race_lap > 0 ? std::format("R{}", lap->race_lap) : std::format("L{}", lap->raw_lap)) : ""},
                {"compare_lap_index", annotation.compare_lap_index}, {"compare_b_lap_index", annotation.compare_b_lap_index},
                {"playback_lap_index", annotation.playback_lap_index}, {"comment", std::string(annotation.note.data())}
            });
        }
        std::ofstream file(*path);
        if (!file) throw std::runtime_error("Could not create annotation review file");
        file << output.dump(2);
        if (!file) throw std::runtime_error("Could not finish writing annotation review file");
        status_ = std::format("Exported {} annotations", annotations_.size());
    } catch (const std::exception& exception) {
        error_ = exception.what();
    }
}

void NativeApp::export_driver_analysis() {
    if (!session_ || !active_lap()) return;
    refresh_driver_analysis();
    const auto path = save_file(window_, L"RaceBox Driver Analysis", L"json", L"*.json");
    if (!path) return;
    try {
        nlohmann::json output{
            {"format", "racebox-driver-analysis"}, {"version", 1},
            {"formula_version", driver_analysis_.formula_version}, {"session", session_->name},
            {"reference_raw_lap", driver_analysis_.reference.raw_lap},
            {"session_notes", std::string(session_notes_.data())},
            {"rules", nlohmann::json::array()}, {"corners", nlohmann::json::array()},
            {"comparisons", nlohmann::json::array()}, {"insights", nlohmann::json::array()},
            {"diagnostics", driver_analysis_.diagnostics}
        };
        for (const auto& descriptor : driver_analysis::rule_descriptors()) {
            const auto* setting = driver_analysis::find_rule(analysis_rules_, descriptor.id);
            output["rules"].push_back({{"id", descriptor.stable_id}, {"name", descriptor.name},
                {"enabled", setting ? setting->enabled : false},
                {"threshold", setting ? setting->threshold : descriptor.default_threshold},
                {"unit", descriptor.unit}, {"formula", descriptor.formula}});
        }
        for (const auto& corner : corner_zones_) {
            output["corners"].push_back({{"id", corner.id}, {"name", corner.name},
                {"start_progress", corner.start_progress}, {"turn_in_progress", corner.turn_in_progress},
                {"apex_progress", corner.apex_progress}, {"exit_progress", corner.exit_progress},
                {"end_progress", corner.end_progress}});
        }
        for (std::size_t slot = 0; slot < driver_analysis_.comparisons.size(); ++slot) {
            if (!driver_analysis_.comparisons[slot]) continue;
            const auto& comparison = *driver_analysis_.comparisons[slot];
            nlohmann::json value{
                {"slot", slot == 0 ? "compare_a" : "compare_b"}, {"raw_lap", comparison.raw_lap},
                {"analysis_only_translation", {{"east_m", comparison.line_translation.east_m},
                    {"north_m", comparison.line_translation.north_m}, {"magnitude_m", comparison.line_translation.magnitude_m},
                    {"residual_rms_m", comparison.line_translation.residual_rms_m}}},
                {"confidence", {{"overall", comparison.confidence.overall},
                    {"band", static_cast<int>(comparison.confidence.band)},
                    {"line_metrics_enabled", comparison.confidence.line_metrics_enabled},
                    {"satellite_score", comparison.confidence.satellite_score},
                    {"sampling_score", comparison.confidence.sampling_score},
                    {"repeatability_score", comparison.confidence.repeatability_score},
                    {"event_clarity_score", comparison.confidence.event_clarity_score},
                    {"translation_score", comparison.confidence.translation_score}}},
                {"corners", nlohmann::json::array()}
            };
            for (const auto& corner : comparison.corners) {
                nlohmann::json corner_value{{"id", corner.corner_id}, {"name", corner.corner_name},
                    {"start_distance_m", corner.start_distance_m}, {"end_distance_m", corner.end_distance_m},
                    {"metrics", nlohmann::json::array()}};
                for (const auto& metric : corner.metrics) {
                    corner_value["metrics"].push_back({{"name", driver_analysis::metric_name(metric.kind)},
                        {"value", metric.value ? nlohmann::json(*metric.value) : nlohmann::json(nullptr)},
                        {"threshold", metric.threshold}, {"enabled", metric.rule_enabled},
                        {"triggered", metric.triggered}, {"positive", metric.positive},
                        {"severity", static_cast<int>(metric.severity)}, {"confidence", metric.confidence},
                        {"reference_progress", metric.navigation.reference_progress},
                        {"reference_timestamp_us", metric.navigation.reference_timestamp_us}});
                }
                value["corners"].push_back(std::move(corner_value));
            }
            output["comparisons"].push_back(std::move(value));
        }
        for (const auto& insight : driver_analysis_.insights) {
            output["insights"].push_back({{"id", insight.id}, {"title", insight.title}, {"detail", insight.detail},
                {"corner_id", insight.corner_id}, {"corner_name", insight.corner_name},
                {"slot", insight.comparison == driver_analysis::ComparisonSlot::CompareA ? "compare_a" : "compare_b"},
                {"comparison_raw_lap", insight.comparison_raw_lap}, {"metric", driver_analysis::metric_name(insight.metric)},
                {"rule", insight.rule_stable_id}, {"measured_value", insight.measured_value},
                {"threshold", insight.threshold}, {"estimated_time_effect_s", insight.estimated_time_effect_s},
                {"positive", insight.positive}, {"severity", static_cast<int>(insight.severity)},
                {"confidence", insight.confidence}, {"confidence_band", static_cast<int>(insight.confidence_band)},
                {"reference_progress", insight.navigation.reference_progress},
                {"reference_timestamp_us", insight.navigation.reference_timestamp_us}});
        }
        std::ofstream file(*path);
        if (!file) throw std::runtime_error("Could not create driver-analysis JSON");
        file << output.dump(2);
        if (!file) throw std::runtime_error("Could not finish driver-analysis JSON");
        status_ = std::format("Exported {} deterministic insights", driver_analysis_.insights.size());
    } catch (const std::exception& exception) {
        error_ = exception.what();
    }
}

void NativeApp::save_session() {
    if (!session_) return;
    session_->workspace_state_json = serialize_workspace_state();
    if (const auto path = save_file(window_, L"RaceBox Session", L"rbxsession", L"*.rbxsession")) {
        std::string error;
        if (save_session_archive(*session_, *path, error)) status_ = "Session saved"; else error_ = error;
    }
}

void NativeApp::export_active_lap() {
    if (!session_ || !active_lap()) return;
    if (const auto path = save_file(window_, L"RaceBox Lap CSV", L"csv", L"*.csv")) {
        std::string error;
        if (export_lap_csv(*session_, *active_lap(), *path, error)) status_ = "Lap exported"; else error_ = error;
    }
}

void NativeApp::save_active_lap() {
    if (!session_ || !active_lap()) return;
    session_->workspace_state_json = serialize_workspace_state();
    if (const auto path = save_file(window_, L"RaceBox Lap Archive", L"rbxlap", L"*.rbxlap")) {
        std::string error;
        if (save_lap_archive(*session_, *active_lap(), *path, error)) status_ = "Lap archive saved"; else error_ = error;
    }
}

void NativeApp::load_background() {
    if (!session_) return;
    if (const auto path = open_image_file(window_)) {
        std::string error;
        if (load_texture(device_, *path, background_, error)) {
            session_->map_background.image_path = *path;
            session_->map_background.offset_x = 0.0F;
            session_->map_background.offset_y = 0.0F;
            session_->map_background.scale_x = 1.0F;
            session_->map_background.scale_y = 1.0F;
            session_->map_background.locked = false;
            session_->map_background.georeferenced = false;
            session_->map_background.offset_east_m = 0.0F;
            session_->map_background.offset_north_m = 0.0F;
            session_->map_background.reference_latitude = 0.0;
            session_->map_background.reference_longitude = 0.0;
            session_->map_background.reference_pixel_x = 0.0F;
            session_->map_background.reference_pixel_y = 0.0F;
            session_->map_background.metres_per_pixel = 0.0F;
            session_->map_background.rotation_degrees = 0.0F;
            session_->map_background.source_crop_left_px = 0.0F;
            session_->map_background.source_crop_top_px = 0.0F;
            session_->map_background.source_crop_right_px = 0.0F;
            session_->map_background.source_crop_bottom_px = 0.0F;
            status_ = "Uncalibrated map loaded unlocked; drag to place it and use the wheel for uniform zoom";
        } else error_ = error;
    }
}

void NativeApp::load_triangulated_background() {
    if (!session_) return;
    if (!uses_parking_track_background(*session_)) {
        error_ = "The bundled triangulated map is calibrated only for the Richmond parking track";
        return;
    }
    const auto path = triangulated_map_path();
    std::string texture_error;
    if (!load_texture(device_, path, background_, texture_error)) {
        error_ = "Triangulated map could not be loaded from " + path.string() + ": " + texture_error;
        write_log(error_);
        return;
    }
    apply_triangulated_map_calibration(session_->map_background, path);
    error_.clear();
    status_ = "Triangulated Google map loaded unlocked; drag the image, then lock it";
}

}  // namespace racebox::app
