#include "mac_app.hpp"

#include "racebox_version.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace racebox::app::mac {
namespace {

constexpr double kEarthRadiusM = 6'378'137.0;
constexpr double kPi = 3.14159265358979323846;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string first_text(const std::filesystem::path& path, std::size_t limit = 8192) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::string result(limit, '\0');
    stream.read(result.data(), static_cast<std::streamsize>(result.size()));
    result.resize(static_cast<std::size_t>(stream.gcount()));
    return lower(std::move(result));
}

bool looks_like_sanwa(const std::filesystem::path& path) {
    const auto text = first_text(path);
    return text.find("steering") != std::string::npos ||
           text.find("trigger") != std::string::npos ||
           (text.find("throttle") != std::string::npos && text.find("voltage") != std::string::npos);
}

const char* phase_name(LapPhase phase) {
    switch (phase) {
        case LapPhase::Complete: return "Complete";
        case LapPhase::OutLap: return "Out lap";
        case LapPhase::InLap: return "In lap";
        default: return "Invalid";
    }
}

std::string duration_text(Timestamp value) {
    const auto total_ms = value / 1000;
    const auto minutes = total_ms / 60'000;
    const auto seconds = (total_ms / 1000) % 60;
    const auto millis = total_ms % 1000;
    std::ostringstream out;
    out << minutes << ':' << std::setfill('0') << std::setw(2) << seconds << '.' << std::setw(3) << millis;
    return out.str();
}

void plot_series(const char* title, const char* y_label, const std::vector<double>& x,
                 const std::vector<double>& y, ImVec2 size = ImVec2(-1.0F, 170.0F)) {
    if (x.empty() || y.empty()) return;
    if (ImPlot::BeginPlot(title, size)) {
        ImPlot::SetupAxes("Time (s)", y_label, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::PlotLine(title, x.data(), y.data(), static_cast<int>(std::min(x.size(), y.size())));
        ImPlot::EndPlot();
    }
}

}  // namespace

MacApp::MacApp(std::filesystem::path resource_root) : resource_root_(std::move(resource_root)) {
    load_demo();
}

void MacApp::begin_drop() {
    drop_paths_.clear();
}

void MacApp::add_dropped_file(std::filesystem::path path) {
    drop_paths_.push_back(std::move(path));
}

void MacApp::finish_drop() {
    if (!drop_paths_.empty()) load_paths(drop_paths_);
}

void MacApp::load_demo() {
    const auto demo = resource_root_ / "demo";
    const auto vbo = demo / "session.vbo";
    const auto csv = demo / "session.csv";
    if (!std::filesystem::exists(vbo) || !std::filesystem::exists(csv)) {
        status_ = "Drop one VBO and one RaceBox CSV here. A Sanwa CSV is optional.";
        return;
    }
    load_paths({vbo, csv, demo / "sanwa.csv"});
}

void MacApp::load_paths(const std::vector<std::filesystem::path>& paths) {
    LoadRequest request;
    for (const auto& path : paths) {
        if (!std::filesystem::exists(path)) continue;
        const auto extension = lower(path.extension().string());
        if (extension == ".vbo") request.vbo = path;
        else if (extension == ".gpx") request.gpx = path;
        else if (extension == ".csv" && looks_like_sanwa(path)) request.sanwa_csv = path;
        else if (extension == ".csv") request.racebox_csv = path;
    }

    if (request.vbo.empty() || request.racebox_csv.empty()) {
        status_ = "Import needs a VBO and RaceBox CSV together; add a Sanwa CSV for controls.";
        return;
    }

    try {
        loaded_ = load_session(request);
        if (loaded_->session.laps.empty()) {
            throw std::runtime_error("the session did not contain a usable lap");
        }
        selected_lap_ = 0;
        for (std::size_t i = 0; i < loaded_->session.laps.size(); ++i) {
            const auto& candidate = loaded_->session.laps[i];
            const auto& current = loaded_->session.laps[selected_lap_];
            if (candidate.phase == LapPhase::Complete &&
                (current.phase != LapPhase::Complete || candidate.duration_us < current.duration_us)) {
                selected_lap_ = i;
            }
        }
        rebuild_plot_cache();
        status_ = "Loaded " + request.vbo.filename().string() + " + " + request.racebox_csv.filename().string();
        if (!request.sanwa_csv.empty()) status_ += " + " + request.sanwa_csv.filename().string();
    } catch (const std::exception& error) {
        loaded_.reset();
        status_ = std::string("Import failed: ") + error.what();
    }
}

void MacApp::select_lap(std::size_t index) {
    if (!loaded_ || index >= loaded_->session.laps.size() || index == selected_lap_) return;
    selected_lap_ = index;
    rebuild_plot_cache();
}

void MacApp::rebuild_plot_cache() {
    time_s_.clear();
    east_m_.clear();
    north_m_.clear();
    speed_kmh_.clear();
    lateral_g_.clear();
    longitudinal_g_.clear();
    vertical_g_.clear();
    yaw_rate_dps_.clear();
    throttle_.clear();
    brake_.clear();
    steering_.clear();
    if (!loaded_ || loaded_->session.telemetry.empty() || loaded_->session.laps.empty()) return;

    const auto& session = loaded_->session;
    const auto& telemetry = session.telemetry;
    const auto& lap = session.laps[std::min(selected_lap_, session.laps.size() - 1)];
    const auto begin = std::min(lap.begin_index, telemetry.size() - 1);
    const auto end = std::min(lap.end_index, telemetry.size() - 1);
    const auto latitude0 = telemetry.latitude[begin] * kPi / 180.0;
    const auto longitude0 = telemetry.longitude[begin] * kPi / 180.0;
    const auto time0 = telemetry.time_us[begin];
    const auto count = end >= begin ? end - begin + 1 : 0;
    time_s_.reserve(count);
    east_m_.reserve(count);
    north_m_.reserve(count);
    throttle_.reserve(count);
    brake_.reserve(count);
    steering_.reserve(count);

    for (std::size_t i = begin; i <= end; ++i) {
        const auto latitude = telemetry.latitude[i] * kPi / 180.0;
        const auto longitude = telemetry.longitude[i] * kPi / 180.0;
        time_s_.push_back(static_cast<double>(telemetry.time_us[i] - time0) / 1'000'000.0);
        east_m_.push_back((longitude - longitude0) * std::cos(latitude0) * kEarthRadiusM);
        north_m_.push_back((latitude - latitude0) * kEarthRadiusM);
        const auto radio = sample_radio(session, telemetry.time_us[i]);
        throttle_.push_back(radio.valid ? radio.throttle : 0.0);
        brake_.push_back(radio.valid ? -radio.brake : 0.0);
        steering_.push_back(radio.valid ? radio.steering : 0.0);
    }

    const auto copy_range = [begin, end](const auto& input) {
        return std::vector<double>(input.begin() + static_cast<std::ptrdiff_t>(begin),
                                   input.begin() + static_cast<std::ptrdiff_t>(end + 1));
    };
    speed_kmh_ = copy_range(telemetry.speed_kmh);
    lateral_g_ = copy_range(telemetry.lateral_g);
    longitudinal_g_ = copy_range(telemetry.longitudinal_g);
    vertical_g_ = copy_range(telemetry.vertical_g);
    if (loaded_->imu_analysis.vehicle_yaw_rate_dps.size() == telemetry.size()) {
        yaw_rate_dps_ = copy_range(loaded_->imu_analysis.vehicle_yaw_rate_dps);
    }
}

void MacApp::draw() {
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("RaceBox Mac", nullptr, flags);

    ImGui::TextColored(ImVec4(0.96F, 0.05F, 0.25F, 1.0F), "R");
    ImGui::SameLine();
    ImGui::Text("RACEBOX  Telemetry Analysis");
    ImGui::SameLine();
    ImGui::TextDisabled("v%s  |  macOS preview", RACEBOX_VERSION_STRING);
    ImGui::SameLine(ImGui::GetWindowWidth() - 145.0F);
    if (ImGui::Checkbox("Light mode", &light_mode_)) {
        if (light_mode_) ImGui::StyleColorsLight();
        else ImGui::StyleColorsDark();
    }
    ImGui::Separator();
    ImGui::TextWrapped("%s", status_.c_str());

    if (!loaded_) {
        ImGui::Spacing();
        ImGui::TextWrapped("Drag the session files from Finder onto this window. The first Mac milestone reads the same portable telemetry core as Windows.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("MacWorkspace")) {
        if (ImGui::BeginTabItem("Overview")) { draw_overview(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Track Map")) { draw_track_map(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Telemetry")) { draw_telemetry(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Laps")) { draw_laps(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("IMU")) { draw_imu(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void MacApp::draw_overview() {
    const auto& result = *loaded_;
    const auto& session = result.session;
    ImGui::Text("Session: %s", session.name.c_str());
    ImGui::Text("Telemetry samples: %zu", session.telemetry.size());
    ImGui::Text("Laps: %zu", session.laps.size());
    ImGui::Text("Sanwa control samples: %zu", session.radio.size());
    ImGui::Text("Control alignment: %s (%.0f%%)", session.alignment.confidence.c_str(),
                session.alignment.merge_confidence * 100.0);
    ImGui::Text("IMU: %s", result.imu_analysis.available ? "available" : "not available");
    ImGui::Separator();
    ImGui::TextWrapped("This preview intentionally starts with read-only telemetry, lap selection, map, controls and IMU. Race Day storage, annotations and Crew Chief need native macOS service adapters before they are enabled.");
    if (!result.diagnostics.empty() && ImGui::CollapsingHeader("Import diagnostics")) {
        for (const auto& item : result.diagnostics) ImGui::BulletText("%s", item.c_str());
    }
}

void MacApp::draw_track_map() {
    if (east_m_.empty()) return;
    if (ImPlot::BeginPlot("Selected lap GPS trace", ImVec2(-1.0F, -1.0F), ImPlotFlags_Equal)) {
        ImPlot::SetupAxes("East (m)", "North (m)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        const ImPlotSpec style{ImPlotProp_LineColor, ImVec4(0.10F, 0.85F, 0.35F, 1.0F),
                               ImPlotProp_LineWeight, 2.5F};
        ImPlot::PlotLine("Raw GPS", east_m_.data(), north_m_.data(), static_cast<int>(east_m_.size()), style);
        ImPlot::EndPlot();
    }
}

void MacApp::draw_telemetry() {
    if (!loaded_ || loaded_->session.laps.empty()) return;
    const auto& lap = loaded_->session.laps[selected_lap_];
    ImGui::Text("Lap %d  |  %s  |  %s", lap.race_lap, duration_text(lap.duration_us).c_str(), phase_name(lap.phase));
    plot_series("Speed", "km/h", time_s_, speed_kmh_);
    plot_series("Lateral G", "G", time_s_, lateral_g_);
    plot_series("Longitudinal G", "G", time_s_, longitudinal_g_);
    if (ImPlot::BeginPlot("Throttle / Brake", ImVec2(-1.0F, 170.0F))) {
        ImPlot::SetupAxes("Time (s)", "%", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        const ImPlotSpec throttle_style{ImPlotProp_LineColor, ImVec4(0.15F, 0.85F, 0.35F, 1.0F)};
        const ImPlotSpec brake_style{ImPlotProp_LineColor, ImVec4(0.95F, 0.15F, 0.20F, 1.0F)};
        ImPlot::PlotLine("Throttle", time_s_.data(), throttle_.data(), static_cast<int>(time_s_.size()), throttle_style);
        ImPlot::PlotLine("Brake", time_s_.data(), brake_.data(), static_cast<int>(time_s_.size()), brake_style);
        ImPlot::EndPlot();
    }
    plot_series("Steering", "%", time_s_, steering_);
}

void MacApp::draw_laps() {
    const auto& laps = loaded_->session.laps;
    if (ImGui::BeginTable("Laps", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Selected");
        ImGui::TableSetupColumn("Race lap");
        ImGui::TableSetupColumn("Source lap");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Time");
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < laps.size(); ++i) {
            const auto& lap = laps[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const auto label = std::string(i == selected_lap_ ? "Viewing##" : "View##") + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), i == selected_lap_, ImGuiSelectableFlags_SpanAllColumns)) select_lap(i);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", lap.race_lap);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%d", lap.raw_lap);
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(phase_name(lap.phase));
            ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(duration_text(lap.duration_us).c_str());
        }
        ImGui::EndTable();
    }
}

void MacApp::draw_imu() {
    const auto& analysis = loaded_->imu_analysis;
    if (!analysis.available) {
        ImGui::TextWrapped("This session has no usable accelerometer/gyro channels.");
        return;
    }
    ImGui::Text("Stationary zero calibration: %s", analysis.initial_stationary_zero_used ? "used" : "not found");
    ImGui::Text("Yaw calibration matches: %zu  |  correlation %.2f", analysis.calibration.matched_samples,
                analysis.calibration.heading_correlation);
    plot_series("Vertical G", "G", time_s_, vertical_g_, ImVec2(-1.0F, 190.0F));
    plot_series("Vehicle yaw rate", "degrees/s", time_s_, yaw_rate_dps_, ImVec2(-1.0F, 190.0F));
    if (ImGui::BeginTable("Motion events", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Event");
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Magnitude");
        ImGui::TableSetupColumn("Confidence");
        ImGui::TableHeadersRow();
        for (const auto& event : analysis.events) {
            ImGui::TableNextRow();
            const auto name = imu::event_name(event.type);
            ImGui::TableSetColumnIndex(0); ImGui::Text("%.*s", static_cast<int>(name.size()), name.data());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f s", event.time_us / 1'000'000.0);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f", event.magnitude);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%d%%", event.confidence);
        }
        ImGui::EndTable();
    }
}

}  // namespace racebox::app::mac
