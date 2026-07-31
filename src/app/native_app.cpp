#include "native_app.hpp"
#include "racebox/plot_decimation.hpp"
#include "racebox_version.h"
#include "ui_preferences.hpp"
#include "ui_theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <nlohmann/json.hpp>
#include <psapi.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <format>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace racebox::app {
namespace {

constexpr Timestamp kSecond = 1'000'000;

std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(encoded.data()),
        encoded.size(),
    };
}

std::string environment_variable(const char* name) {
    const auto length = GetEnvironmentVariableA(name, nullptr, 0);
    if (length == 0) return {};
    std::string value(length, '\0');
    const auto copied = GetEnvironmentVariableA(name, value.data(), length);
    if (copied == 0 || copied >= length) return {};
    value.resize(copied);
    return value;
}

template <std::size_t Size>
void set_text_buffer(std::array<char, Size>& buffer, std::string_view text) {
    buffer.fill('\0');
    std::copy_n(text.begin(), std::min(text.size(), Size - 1), buffer.begin());
}

struct StringInputUserData {
    std::string* value{};
    ImGuiInputTextCallback chained_callback{};
    void* chained_user_data{};
};

int string_input_callback(ImGuiInputTextCallbackData* data) {
    auto* user_data = static_cast<StringInputUserData*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* value = user_data->value;
        value->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = value->data();
    } else if (user_data->chained_callback) {
        data->UserData = user_data->chained_user_data;
        return user_data->chained_callback(data);
    }
    return 0;
}

bool input_text_string(const char* label, std::string& value,
                       ImGuiInputTextFlags flags = 0,
                       ImGuiInputTextCallback callback = nullptr,
                       void* user_data = nullptr) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    StringInputUserData data{&value, callback, user_data};
    return ImGui::InputText(label, value.data(), value.capacity() + 1, flags, string_input_callback, &data);
}

bool input_text_multiline_string(const char* label, std::string& value, ImVec2 size,
                                 ImGuiInputTextFlags flags = 0) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    StringInputUserData data{&value, nullptr, nullptr};
    return ImGui::InputTextMultiline(
        label, value.data(), value.capacity() + 1, size, flags, string_input_callback, &data);
}

std::string local_date_label() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    return std::format("{:04}-{:02}-{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

std::string utc_timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
        utc.tm_hour, utc.tm_min, utc.tm_sec);
}

std::string setup_knowledge_id(
    std::string_view previous_run_id,
    std::string_view current_run_id,
    std::string_view question,
    std::string_view setup_change,
    std::string_view driver_result) {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    const auto add = [&](std::string_view value) {
        for (const auto character : value) {
            hash ^= static_cast<unsigned char>(character);
            hash *= 1'099'511'628'211ULL;
        }
        hash ^= 0xFFU;
        hash *= 1'099'511'628'211ULL;
    };
    add(previous_run_id);
    add(current_run_id);
    add(question);
    add(setup_change);
    add(driver_result);
    return std::format("setup-result-{:016X}", hash);
}

void apply_crew_chief_report(
    race_day::SetupKnowledgeRecord& record,
    const crew_chief::Report& report) {
    record.verdict = report.verdict;
    record.confidence = report.confidence;
    record.summary = report.summary;
    record.observations.clear();
    for (const auto& observation : report.observations) {
        record.observations.push_back({
            observation.area, observation.change, observation.meaning, observation.evidence_ids});
    }
    record.confounds = report.confounds;
    record.next_test = report.next_test;
    record.causality_note = report.causality_note;
    record.route = report.route;
    record.model = report.model;
    record.thinking = report.thinking;
    record.selected_lane = report.selected_lane;
    record.evidence = {
        report.evidence.analytics_contract,
        report.evidence.formula_version,
        report.evidence.quality_confidence,
        report.evidence.track_status,
        report.evidence.track_translation_m,
        report.evidence.track_residual_rms_m,
        report.evidence.lap_time_delta_s,
        report.evidence.lateral_status,
        report.evidence.lateral_response_delta_g,
        report.evidence.forward_status,
        report.evidence.forward_bite_delta_g,
        report.evidence.previous_brake_indicators,
        report.evidence.current_brake_indicators,
        report.evidence.top_speed_status,
        report.evidence.straight_top_speed_delta_kmh,
        report.evidence.straight_entry_speed_delta_kmh,
        report.evidence.straight_acceleration_delta_g,
        report.evidence.straight_speed_attribution,
        report.evidence.brake_response_status,
        report.evidence.brake_decel_delta_g,
        report.evidence.brake_response_delay_delta_s,
        report.evidence.corner_balance_status,
        report.evidence.steering_for_lateral_g_delta_percent,
        report.evidence.yaw_per_steering_delta_dps,
        report.evidence.overdriving_status,
        report.evidence.overdriving_index_delta,
        report.evidence.overdriving_risk_sample_delta_percent,
        report.evidence.current_late_overdriving_delta_score,
        report.evidence.chassis_roll_status,
        report.evidence.surface_tilt_delta_deg,
        report.evidence.chassis_roll_delta_deg,
        report.evidence.roll_per_lateral_g_delta_deg,
        report.evidence.roll_rate_status,
        report.evidence.previous_roll_rate_p90_dps,
        report.evidence.current_roll_rate_p90_dps,
        report.evidence.roll_rate_delta_dps,
        report.evidence.roll_rate_per_lateral_g_delta_dps,
        report.evidence.current_late_roll_rate_delta_dps,
        report.evidence.previous_roll_rate_samples,
        report.evidence.current_roll_rate_samples,
        report.evidence.current_surface_tilt_source,
        report.evidence.current_surface_tilt_samples,
    };
}

void fill_missing_setup_evidence(
    crew_chief::Report& report,
    const race_day::SetupAnalyticsSummary& summary) {
    report.evidence.analytics_contract = summary.analytics_contract;
    report.evidence.formula_version = summary.formula_version;
    if (report.evidence.quality_confidence == 0) report.evidence.quality_confidence = summary.quality_confidence;
    if (report.evidence.track_status.empty()) report.evidence.track_status = summary.track_status;
    if (!report.evidence.lap_time_delta_s) report.evidence.lap_time_delta_s = summary.lap_time_delta_s;
    if (report.evidence.lateral_status.empty()) report.evidence.lateral_status = summary.lateral_status;
    if (!report.evidence.lateral_response_delta_g) report.evidence.lateral_response_delta_g = summary.lateral_response_delta_g;
    if (report.evidence.forward_status.empty()) report.evidence.forward_status = summary.forward_status;
    if (!report.evidence.forward_bite_delta_g) report.evidence.forward_bite_delta_g = summary.forward_bite_delta_g;
    if (report.evidence.previous_brake_indicators == 0) {
        report.evidence.previous_brake_indicators = summary.previous_brake_indicators;
    }
    if (report.evidence.current_brake_indicators == 0) {
        report.evidence.current_brake_indicators = summary.current_brake_indicators;
    }
    if (report.evidence.top_speed_status.empty()) report.evidence.top_speed_status = summary.top_speed_status;
    if (!report.evidence.straight_top_speed_delta_kmh) {
        report.evidence.straight_top_speed_delta_kmh = summary.straight_top_speed_delta_kmh;
    }
    if (!report.evidence.straight_entry_speed_delta_kmh) {
        report.evidence.straight_entry_speed_delta_kmh = summary.straight_entry_speed_delta_kmh;
    }
    if (!report.evidence.straight_acceleration_delta_g) {
        report.evidence.straight_acceleration_delta_g = summary.straight_acceleration_delta_g;
    }
    if (report.evidence.straight_speed_attribution.empty()) {
        report.evidence.straight_speed_attribution = summary.straight_speed_attribution;
    }
    if (report.evidence.brake_response_status.empty()) {
        report.evidence.brake_response_status = summary.brake_response_status;
    }
    if (!report.evidence.brake_decel_delta_g) report.evidence.brake_decel_delta_g = summary.brake_decel_delta_g;
    if (!report.evidence.brake_response_delay_delta_s) {
        report.evidence.brake_response_delay_delta_s = summary.brake_response_delay_delta_s;
    }
    if (report.evidence.corner_balance_status.empty()) {
        report.evidence.corner_balance_status = summary.corner_balance_status;
    }
    if (!report.evidence.steering_for_lateral_g_delta_percent) {
        report.evidence.steering_for_lateral_g_delta_percent = summary.steering_for_lateral_g_delta_percent;
    }
    if (!report.evidence.yaw_per_steering_delta_dps) {
        report.evidence.yaw_per_steering_delta_dps = summary.yaw_per_steering_delta_dps;
    }
    if (report.evidence.overdriving_status.empty()) {
        report.evidence.overdriving_status = summary.overdriving_status;
    }
    if (!report.evidence.overdriving_index_delta) {
        report.evidence.overdriving_index_delta = summary.overdriving_index_delta;
    }
    if (!report.evidence.overdriving_risk_sample_delta_percent) {
        report.evidence.overdriving_risk_sample_delta_percent = summary.overdriving_risk_sample_delta_percent;
    }
    if (!report.evidence.current_late_overdriving_delta_score) {
        report.evidence.current_late_overdriving_delta_score = summary.current_late_overdriving_delta_score;
    }
    if (report.evidence.chassis_roll_status.empty()) {
        report.evidence.chassis_roll_status = summary.chassis_roll_status;
    }
    if (!report.evidence.surface_tilt_delta_deg) {
        report.evidence.surface_tilt_delta_deg = summary.surface_tilt_delta_deg;
    }
    if (!report.evidence.chassis_roll_delta_deg) {
        report.evidence.chassis_roll_delta_deg = summary.chassis_roll_delta_deg;
    }
    if (!report.evidence.roll_per_lateral_g_delta_deg) {
        report.evidence.roll_per_lateral_g_delta_deg = summary.roll_per_lateral_g_delta_deg;
    }
    if (report.evidence.roll_rate_status.empty()) {
        report.evidence.roll_rate_status = summary.roll_rate_status;
    }
    if (!report.evidence.previous_roll_rate_p90_dps) {
        report.evidence.previous_roll_rate_p90_dps = summary.previous_roll_rate_p90_dps;
    }
    if (!report.evidence.current_roll_rate_p90_dps) {
        report.evidence.current_roll_rate_p90_dps = summary.current_roll_rate_p90_dps;
    }
    if (!report.evidence.roll_rate_delta_dps) {
        report.evidence.roll_rate_delta_dps = summary.roll_rate_delta_dps;
    }
    if (!report.evidence.roll_rate_per_lateral_g_delta_dps) {
        report.evidence.roll_rate_per_lateral_g_delta_dps = summary.roll_rate_per_lateral_g_delta_dps;
    }
    if (!report.evidence.current_late_roll_rate_delta_dps) {
        report.evidence.current_late_roll_rate_delta_dps = summary.current_late_roll_rate_delta_dps;
    }
    if (report.evidence.previous_roll_rate_samples == 0) {
        report.evidence.previous_roll_rate_samples = summary.previous_roll_rate_samples;
    }
    if (report.evidence.current_roll_rate_samples == 0) {
        report.evidence.current_roll_rate_samples = summary.current_roll_rate_samples;
    }
    if (report.evidence.current_surface_tilt_source.empty()) {
        report.evidence.current_surface_tilt_source = summary.current_surface_tilt_source;
    }
    if (report.evidence.current_surface_tilt_samples == 0) {
        report.evidence.current_surface_tilt_samples = summary.current_surface_tilt_samples;
    }
}

nlohmann::json race_day_run_context(const race_day::Day& day, const race_day::Run& run) {
    auto checklist = nlohmann::json::array();
    int checklist_complete = 0;
    for (const auto& item : run.checklist) {
        checklist.push_back({
            {"id", item.id},
            {"label", item.label},
            {"checked", item.checked},
            {"note", item.note},
        });
        if (item.checked) ++checklist_complete;
    }
    const auto number_or_null = [](const std::optional<double>& value) {
        return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
    };
    return {
        {"event", {
            {"name", day.event_name},
            {"track", day.track_name},
            {"date", day.date},
        }},
        {"run", {
            {"id", run.id},
            {"label", run.label},
            {"kind", race_day::kind_name(run.kind)},
            {"ordinal", run.ordinal},
            {"main_group", std::string(1, run.main_group)},
            {"main_leg", run.main_leg},
            {"recorded_at_utc", run.recorded_at_utc},
        }},
        {"notes", {
            {"pre_run", run.pre_run_notes},
            {"setup_changes", run.setup_changes},
            {"post_run_driver_feel", run.post_run_notes},
        }},
        {"conditions", {
            {"ambient_temperature_c", number_or_null(run.conditions.ambient_temperature_c)},
            {"track_temperature_c", number_or_null(run.conditions.track_temperature_c)},
            {"track_condition", run.conditions.track_condition},
            {"tire_set_id", run.conditions.tire_set_id},
            {"tire_compound", run.conditions.tire_compound},
            {"tire_runs_before", run.conditions.tire_runs_before >= 0
                ? nlohmann::json(run.conditions.tire_runs_before) : nlohmann::json(nullptr)},
            {"sauce_compound", run.conditions.sauce_compound},
            {"sauce_minutes_before", run.conditions.sauce_minutes_before >= 0
                ? nlohmann::json(run.conditions.sauce_minutes_before) : nlohmann::json(nullptr)},
            {"tire_warmer_minutes", run.conditions.tire_warmer_minutes >= 0
                ? nlohmann::json(run.conditions.tire_warmer_minutes) : nlohmann::json(nullptr)},
            {"tire_warmer_temperature_c", number_or_null(run.conditions.tire_warmer_temperature_c)},
            {"battery_pack", run.conditions.battery_pack},
            {"battery_voltage", number_or_null(run.conditions.battery_voltage)},
        }},
        {"pre_run_checklist", {
            {"completed", checklist_complete},
            {"total", run.checklist.size()},
            {"items", std::move(checklist)},
        }},
        {"attached_sources", {
            {"file_count", run.telemetry_files.size()},
            {"original_paths_shared", false},
        }},
    };
}

float application_header_height(float text_scale) {
    const auto* viewport = ImGui::GetMainViewport();
    const auto dpi_scale = std::clamp(viewport ? viewport->DpiScale : 1.0F, 1.0F, 2.0F);
    const auto normalized_text_scale = std::clamp(text_scale, 1.0F, 1.5F);
    return (136.0F + (normalized_text_scale - 1.0F) * 96.0F) * dpi_scale;
}

void set_dock_tree_customizable(ImGuiDockNode* node, bool customizable) {
    if (!node) return;
    constexpr auto lock_flags = static_cast<ImGuiDockNodeFlags>(
        static_cast<int>(ImGuiDockNodeFlags_NoResize) |
        static_cast<int>(ImGuiDockNodeFlags_NoDocking));
    if (customizable) {
        node->LocalFlags = static_cast<ImGuiDockNodeFlags>(node->LocalFlags & ~lock_flags);
    } else {
        node->LocalFlags = static_cast<ImGuiDockNodeFlags>(node->LocalFlags | lock_flags);
    }
    set_dock_tree_customizable(node->ChildNodes[0], customizable);
    set_dock_tree_customizable(node->ChildNodes[1], customizable);
}

std::string recorded_time_iso8601(const Session* session) {
    if (!session || session->telemetry.absolute_time_us.empty() || session->telemetry.absolute_time_us.front() <= 0) {
        return "Not recorded";
    }
    constexpr std::uint64_t unix_to_filetime_ticks = 116'444'736'000'000'000ULL;
    const auto unix_microseconds = static_cast<std::uint64_t>(session->telemetry.absolute_time_us.front());
    const auto filetime_ticks = unix_microseconds * 10ULL + unix_to_filetime_ticks;
    FILETIME file_time{static_cast<DWORD>(filetime_ticks & 0xFFFFFFFFULL), static_cast<DWORD>(filetime_ticks >> 32U)};
    SYSTEMTIME system_time{};
    if (!FileTimeToSystemTime(&file_time, &system_time)) return "Not recorded";
    return std::format(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
        system_time.wYear, system_time.wMonth, system_time.wDay,
        system_time.wHour, system_time.wMinute, system_time.wSecond);
}

std::string friendly_recorded_time_label(std::string_view recorded_at_utc) {
    if (recorded_at_utc.empty() || recorded_at_utc == "Not recorded") {
        return "Not recorded";
    }
    if (recorded_at_utc.size() >= 20 &&
        recorded_at_utc[4] == '-' && recorded_at_utc[7] == '-' &&
        recorded_at_utc[10] == 'T' && recorded_at_utc[13] == ':' &&
        recorded_at_utc[16] == ':') {
        return std::format(
            "{}-{}-{} {}:{}:{} UTC",
            recorded_at_utc.substr(0, 4), recorded_at_utc.substr(5, 2),
            recorded_at_utc.substr(8, 2), recorded_at_utc.substr(11, 2),
            recorded_at_utc.substr(14, 2), recorded_at_utc.substr(17, 2));
    }
    return std::string(recorded_at_utc);
}

std::string recorded_local_date(const Session* session) {
    if (!session || session->telemetry.absolute_time_us.empty() ||
        session->telemetry.absolute_time_us.front() <= 0) {
        return local_date_label();
    }
    constexpr std::uint64_t unix_to_filetime_ticks =
        116'444'736'000'000'000ULL;
    const auto unix_microseconds = static_cast<std::uint64_t>(
        session->telemetry.absolute_time_us.front());
    const auto filetime_ticks =
        unix_microseconds * 10ULL + unix_to_filetime_ticks;
    const FILETIME utc_file_time{
        static_cast<DWORD>(filetime_ticks & 0xFFFFFFFFULL),
        static_cast<DWORD>(filetime_ticks >> 32U),
    };
    FILETIME local_file_time{};
    SYSTEMTIME local_system_time{};
    if (!FileTimeToLocalFileTime(&utc_file_time, &local_file_time) ||
        !FileTimeToSystemTime(&local_file_time, &local_system_time)) {
        return local_date_label();
    }
    return std::format(
        "{:04}-{:02}-{:02}",
        local_system_time.wYear,
        local_system_time.wMonth,
        local_system_time.wDay);
}

std::size_t invalidate_setup_knowledge_for_run(
    race_day::Day& day, std::string_view run_id) {
    const auto previous_size = day.setup_knowledge.size();
    std::erase_if(
        day.setup_knowledge,
        [&](const race_day::SetupKnowledgeRecord& record) {
            return record.previous_run_id == run_id ||
                record.current_run_id == run_id;
        });
    return previous_size - day.setup_knowledge.size();
}

bool replace_telemetry_source_slot(
    race_day::Run& run,
    std::size_t source_index,
    const std::filesystem::path& replacement,
    const source_identity::SourceIdentity& identity,
    std::string& error) {
    if (source_index >= run.telemetry_files.size()) {
        error = "The telemetry source changed before it could be repaired.";
        return false;
    }
    auto staged = run;
    race_day::refresh_telemetry_source_identities(staged);
    staged.telemetry_files[source_index] = replacement;
    staged.telemetry_source_identities[source_index] = identity;
    const auto validation =
        race_day::validate_telemetry_source_composition(staged);
    if (!validation.ok) {
        error = validation.error;
        return false;
    }
    run = std::move(staged);
    error.clear();
    return true;
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

const char* insight_outcome_label(insight_evidence::Outcome outcome) noexcept {
    switch (outcome) {
    case insight_evidence::Outcome::DataLimited: return "NOT ENOUGH DATA";
    case insight_evidence::Outcome::Inconclusive: return "NO CLEAR TIME EFFECT";
    case insight_evidence::Outcome::NetLoss: return "TIME LOST";
    case insight_evidence::Outcome::Compensation: return "RECOVERY ONLY";
    case insight_evidence::Outcome::RetainedGain: return "TIME GAIN HELD";
    case insight_evidence::Outcome::TradeoffGain: return "WHOLE-CORNER GAIN";
    }
    return "RESULT UNKNOWN";
}

const char* insight_reliability_label(insight_evidence::Reliability reliability) noexcept {
    switch (reliability) {
    case insight_evidence::Reliability::Unproven: return "NOT REPEATED YET";
    case insight_evidence::Reliability::Likely: return "PROMISING PATTERN";
    case insight_evidence::Reliability::ReliableAssociation: return "REPEATED AND CONSISTENT";
    }
    return "RELIABILITY UNKNOWN";
}

const char* insight_recommendation_label(insight_evidence::Recommendation recommendation) noexcept {
    switch (recommendation) {
    case insight_evidence::Recommendation::None: return "NO DRIVING CHANGE";
    case insight_evidence::Recommendation::Observe: return "WATCH ONLY";
    case insight_evidence::Recommendation::Validate: return "TEST AGAIN";
    case insight_evidence::Recommendation::RecommendTechnique: return "TRY THIS TECHNIQUE";
    case insight_evidence::Recommendation::RecommendSequence: return "COPY THE WHOLE SEQUENCE";
    }
    return "NO RECOMMENDATION";
}

const char* insight_reason_label(insight_evidence::Reason reason) noexcept {
    switch (reason) {
    case insight_evidence::Reason::InvalidInput: return "invalid analysis input";
    case insight_evidence::Reason::LowDataConfidence: return "recording quality is too low";
    case insight_evidence::Reason::IncompleteLap: return "lap is incomplete";
    case insight_evidence::Reason::TelemetryGap: return "recording has a data gap";
    case insight_evidence::Reason::ContextChanged: return "setup or conditions changed";
    case insight_evidence::Reason::LineMetricDisabled: return "GPS line comparison is unavailable";
    case insight_evidence::Reason::BelowNoiseFloor: return "difference is smaller than normal timing variation";
    case insight_evidence::Reason::PriorPhaseLoss: return "time was lost earlier in the corner";
    case insight_evidence::Reason::DownstreamPayback: return "most of the local gain was lost afterward";
    case insight_evidence::Reason::TrackLimitViolation: return "track limits were exceeded";
    case insight_evidence::Reason::ExtraSteeringCorrection: return "an extra steering correction was needed";
    case insight_evidence::Reason::PossibleInstability: return "possible instability was detected";
    case insight_evidence::Reason::AggregateEvidenceMissing: return "more matching laps are needed";
    case insight_evidence::Reason::InsufficientComparableLaps: return "too few matching laps";
    case insight_evidence::Reason::InsufficientSupportingLaps: return "too few laps showed the same result";
    case insight_evidence::Reason::LowSupportRate: return "the result did not repeat often enough";
    case insight_evidence::Reason::ReliabilityIntervalCrossesNoise: return "normal variation could explain the result";
    }
    return "evidence unavailable";
}

std::string timing_position_label(double effect_s) {
    constexpr double epsilon = 0.0005;
    if (effect_s < -epsilon) return std::format("{:.3f} s ahead", -effect_s);
    if (effect_s > epsilon) return std::format("{:.3f} s behind", effect_s);
    return "even with reference";
}

std::string timing_change_label(double effect_s) {
    constexpr double epsilon = 0.0005;
    if (effect_s < -epsilon) return std::format("gained {:.3f} s", -effect_s);
    if (effect_s > epsilon) return std::format("lost {:.3f} s", effect_s);
    return "no measurable change";
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
    auto crew_endpoint = environment_variable("RACEBOX_CREW_CHIEF_URL");
    if (crew_endpoint.empty()) {
        crew_endpoint = "http://100.73.60.87:18804/v1/crew-chief/chat";
    }
    set_text_buffer(crew_chief_endpoint_, crew_endpoint);
    set_text_buffer(crew_chief_question_,
        "What does the telemetry say changed, did the change produce a reliable gain, and what should I test next?");
    crew_chief_bearer_token_ = environment_variable("RACEBOX_CREW_CHIEF_TOKEN");
    race_day_ = race_day::standard_day(false, false);
    race_day_.date = local_date_label();
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
    compare_b_enabled_ = preferences.workspace.compare_b_enabled;
    show_analysis_aligned_traces_ = preferences.analysis_aligned_map_traces;
    telemetry_watch_folder_ = preferences.telemetry_import_folder;
    if (telemetry_watch_folder_.empty()) {
        telemetry_watch_folder_ = default_telemetry_download_folder();
    }
    auto_detect_sanwa_usb_ = preferences.auto_detect_sanwa_usb;
    text_scale_ = preferences.text_scale;
    customize_layout_ = preferences.workspace.customize_layout;
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
        const auto migration = prepare_layout_v4_migration(settings_directory(), loaded.source_layout_version);
        if (!migration.safe_to_rebuild) {
            build_default_layout_ = false;
            write_log(migration.error);
        } else if (migration.backup_created) {
            write_log("Legacy dock layout backed up before the v4 guided-workspace migration");
        }
    }
    if (!loaded.warning.empty()) write_log(loaded.warning);
    ui::apply_theme(light_theme_ ? ui::ThemeMode::Light : ui::ThemeMode::Dark, ui::dpi_scale_for_window(window_));
    ImGui::GetStyle().FontScaleMain = text_scale_;
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
    preferences.analysis_aligned_map_traces = show_analysis_aligned_traces_;
    preferences.telemetry_import_folder = telemetry_watch_folder_;
    preferences.auto_detect_sanwa_usb = auto_detect_sanwa_usb_;
    preferences.text_scale = text_scale_;
    preferences.layout_version = loaded_layout_version_;
    preferences.telemetry_plot_order = telemetry_plot_order_;
    preferences.workspace.active = static_cast<UiWorkspace>(workspace_section_);
    preferences.workspace.telemetry_density = compact_telemetry_ ? TelemetryDensity::Compact : TelemetryDensity::Detailed;
    preferences.workspace.compare_b_enabled = compare_b_enabled_;
    preferences.workspace.customize_layout = customize_layout_;
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

void NativeApp::open_files(const std::vector<std::filesystem::path>& files) {
    session_import_requested_ = false;
    session_import_files_.clear();
    begin_load(files);
}
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
    if (!compare_b_enabled_ || !session_ || session_->laps.empty()) return nullptr;
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
    const auto required_laps = compare_b_enabled_ ? std::size_t{3} : std::size_t{2};
    if (complete_laps.size() < required_laps) return false;
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
    const auto compare_b = compare_b_enabled_ ? choose(compare_b_lap_index_) : compare_b_lap_index_;
    const auto previous_reference = active_lap_index_;
    const auto changed = reference != active_lap_index_ || compare_a != compare_lap_index_ ||
                         (compare_b_enabled_ && compare_b != compare_b_lap_index_);
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
    crew_chief_setup_change_.fill('\0');
    set_text_buffer(crew_chief_question_,
        "What does the telemetry say changed, did the change produce a reliable gain, and what should I test next?");
    crew_chief_after_slot_ = driver_analysis::ComparisonSlot::CompareA;
    crew_chief_history_.clear();
    crew_chief_report_.reset();
    crew_chief_error_.clear();
    annotations_.clear();
    next_annotation_id_ = 1;
    selected_annotation_id_ = 0;
}

std::string NativeApp::serialize_workspace_state() const {
    nlohmann::json state{
        {"format", "racebox-native-workspace"}, {"version", 4},
        {"formula_version", std::string(driver_analysis::kFormulaVersion)},
        {"fastest_reference", fastest_reference_}, {"reference_index", active_lap_index_},
        {"compare_a_index", compare_lap_index_}, {"compare_b_index", compare_b_lap_index_},
        {"compare_b_enabled", compare_b_enabled_},
        {"playback_index", playback_lap_index_}, {"view_mode", static_cast<int>(view_mode_)},
        {"session_notes", std::string(session_notes_.data())},
        {"crew_chief", {
            {"setup_change", std::string(crew_chief_setup_change_.data())},
            {"question", std::string(crew_chief_question_.data())},
            {"after_slot", crew_chief_after_slot_ == driver_analysis::ComparisonSlot::CompareA ? "compare_a" : "compare_b"},
            {"history", nlohmann::json::array()}}},
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
            {"turn_in_curvature_per_m", analysis_rules_.turn_in_curvature_per_m}}},
        {"evidence", {
            {"minimum_time_floor_s", analysis_rules_.evidence.minimum_time_floor_s},
            {"sample_period_multiplier", analysis_rules_.evidence.sample_period_multiplier},
            {"repeatability_sigma_multiplier", analysis_rules_.evidence.repeatability_sigma_multiplier},
            {"maximum_payback_fraction", analysis_rules_.evidence.maximum_payback_fraction},
            {"minimum_data_confidence", analysis_rules_.evidence.minimum_data_confidence},
            {"reliable_data_confidence", analysis_rules_.evidence.reliable_data_confidence},
            {"likely_comparable_laps", analysis_rules_.evidence.likely_comparable_laps},
            {"likely_supporting_laps", analysis_rules_.evidence.likely_supporting_laps},
            {"likely_support_rate", analysis_rules_.evidence.likely_support_rate},
            {"reliable_comparable_laps", analysis_rules_.evidence.reliable_comparable_laps},
            {"reliable_supporting_laps", analysis_rules_.evidence.reliable_supporting_laps},
            {"reliable_support_rate", analysis_rules_.evidence.reliable_support_rate}}}
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
            {"compare_b_index", annotation.compare_b_lap_index},
            {"compare_b_enabled", annotation.compare_b_enabled},
            {"playback_index", annotation.playback_lap_index},
            {"view_mode", static_cast<int>(annotation.view_mode)}, {"note", std::string(annotation.note.data())}
        });
    }
    for (const auto& turn : crew_chief_history_) {
        state["crew_chief"]["history"].push_back({
            {"role", turn.role == "assistant" ? "assistant" : "user"},
            {"content", turn.content.substr(0, 4000)}
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
        compare_b_enabled_ = state.value("compare_b_enabled", compare_b_enabled_);
        playback_lap_index_ = std::clamp(state.value("playback_index", playback_lap_index_), 0, maximum);
        view_mode_ = static_cast<ViewMode>(std::clamp(state.value("view_mode", static_cast<int>(view_mode_)), 0, 2));
        const auto notes = state.value("session_notes", std::string{});
        std::copy_n(notes.begin(), std::min(notes.size(), session_notes_.size() - 1), session_notes_.begin());
        if (const auto crew = state.find("crew_chief"); crew != state.end() && crew->is_object()) {
            set_text_buffer(crew_chief_setup_change_, crew->value("setup_change", std::string{}));
            set_text_buffer(crew_chief_question_, crew->value("question",
                std::string{"What does the telemetry say changed, did the change produce a reliable gain, and what should I test next?"}));
            crew_chief_after_slot_ = crew->value("after_slot", std::string{"compare_a"}) == "compare_b"
                ? driver_analysis::ComparisonSlot::CompareB
                : driver_analysis::ComparisonSlot::CompareA;
            if (!compare_b_enabled_) crew_chief_after_slot_ = driver_analysis::ComparisonSlot::CompareA;
            crew_chief_history_.clear();
            for (const auto& turn : crew->value("history", nlohmann::json::array())) {
                if (!turn.is_object() || crew_chief_history_.size() >= 20) break;
                crew_chief_history_.push_back({
                    turn.value("role", std::string{"user"}) == "assistant" ? "assistant" : "user",
                    turn.value("content", std::string{}).substr(0, 4000)
                });
            }
        }
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
        if (const auto evidence = state.find("evidence"); evidence != state.end() && evidence->is_object()) {
            auto& parameters = analysis_rules_.evidence;
            parameters.minimum_time_floor_s = std::clamp(evidence->value("minimum_time_floor_s", parameters.minimum_time_floor_s), 0.001, 2.0);
            parameters.sample_period_multiplier = std::clamp(evidence->value("sample_period_multiplier", parameters.sample_period_multiplier), 0.01, 20.0);
            parameters.repeatability_sigma_multiplier = std::clamp(evidence->value("repeatability_sigma_multiplier", parameters.repeatability_sigma_multiplier), 0.0, 20.0);
            parameters.maximum_payback_fraction = std::clamp(evidence->value("maximum_payback_fraction", parameters.maximum_payback_fraction), 0.0, 1.0);
            parameters.minimum_data_confidence = std::clamp(evidence->value("minimum_data_confidence", parameters.minimum_data_confidence), 0, 100);
            parameters.reliable_data_confidence = std::clamp(evidence->value("reliable_data_confidence", parameters.reliable_data_confidence), parameters.minimum_data_confidence, 100);
            parameters.likely_comparable_laps = static_cast<std::size_t>(std::clamp(
                evidence->value("likely_comparable_laps", static_cast<int>(parameters.likely_comparable_laps)), 1, 1000));
            parameters.likely_supporting_laps = static_cast<std::size_t>(std::clamp(
                evidence->value("likely_supporting_laps", static_cast<int>(parameters.likely_supporting_laps)),
                1, static_cast<int>(parameters.likely_comparable_laps)));
            parameters.likely_support_rate = std::clamp(evidence->value("likely_support_rate", parameters.likely_support_rate), 0.0, 1.0);
            parameters.reliable_comparable_laps = static_cast<std::size_t>(std::clamp(
                evidence->value("reliable_comparable_laps", static_cast<int>(parameters.reliable_comparable_laps)),
                static_cast<int>(parameters.likely_comparable_laps), 1000));
            parameters.reliable_supporting_laps = static_cast<std::size_t>(std::clamp(
                evidence->value("reliable_supporting_laps", static_cast<int>(parameters.reliable_supporting_laps)),
                static_cast<int>(parameters.likely_supporting_laps), static_cast<int>(parameters.reliable_comparable_laps)));
            parameters.reliable_support_rate = std::clamp(evidence->value("reliable_support_rate", parameters.reliable_support_rate),
                parameters.likely_support_rate, 1.0);
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
            annotation.compare_b_enabled = value.value("compare_b_enabled", compare_b_enabled_);
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

void NativeApp::begin_load(
    const std::vector<std::filesystem::path>& files,
    std::string race_day_run_id) {
    if (files.empty() || loading_) return;
    pending_displayed_race_day_run_id_ =
        std::move(race_day_run_id);
    active_load_files_.clear();
    pending_vbo_.clear();
    pending_racebox_csv_.clear();
    pending_sanwa_csv_.clear();
    auto single_extension = files.size() == 1
        ? files.front().extension().wstring()
        : std::wstring{};
    std::transform(
        single_extension.begin(), single_extension.end(),
        single_extension.begin(), ::towlower);
    if (files.size() == 1 &&
        (single_extension == L".rbxsession" ||
         single_extension == L".rbxlap")) {
        pending_vbo_.clear(); pending_racebox_csv_.clear(); pending_sanwa_csv_.clear();
        active_load_files_ = files;
        loading_ = true;
        status_ = "Loading saved session...";
        const auto archive = files.front();
        load_future_ = std::async(std::launch::async, [archive] {
            LoadResult result;
            std::string error;
            if (!load_session_archive(archive, result.session, error)) throw std::runtime_error(error);
            result.imu_analysis = imu::analyze(result.session.telemetry);
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
        const auto selected_sanwa = pending_sanwa_csv_;
        pending_vbo_.clear();
        pending_racebox_csv_.clear();
        active_load_files_ = {selected_gpx};
        if (!selected_sanwa.empty()) {
            active_load_files_.push_back(selected_sanwa);
        }
        loading_ = true;
        error_.clear();
        status_ = selected_sanwa.empty()
            ? "Parsing GPX track on a worker thread..."
            : "Parsing GPX track and aligning Sanwa controls...";
        load_future_ = std::async(std::launch::async, [selected_gpx, selected_sanwa] {
            LoadResult result;
            result.session.name = selected_gpx.stem().string();
            result.session.telemetry = parse_gpx(selected_gpx, result.diagnostics);
            if (result.session.telemetry.empty()) throw std::runtime_error("GPX contains no track points");
            if (!selected_sanwa.empty()) {
                result.session.sanwa_path = selected_sanwa;
                result.session.radio =
                    parse_sanwa_csv(selected_sanwa, result.diagnostics);
                result.session.alignment =
                    align_radio(result.session.telemetry, result.session.radio);
            }
            result.session.laps.push_back({1, 0, 0, result.session.telemetry.size() - 1,
                result.session.telemetry.time_us.back(), LapPhase::InLap});
            result.imu_analysis = imu::analyze(result.session.telemetry);
            return result;
        });
        return;
    }
    LoadRequest request{pending_vbo_, pending_racebox_csv_, pending_sanwa_csv_, {}};
    if (request.vbo.empty() && request.racebox_csv.empty()) {
        active_load_files_.clear();
        pending_displayed_race_day_run_id_.clear();
        error_.clear();
        status_ = request.sanwa_csv.empty() ? "No supported telemetry file was selected" :
            "Sanwa controls need a RaceBox CSV, VBO, or GPX recording in the same selection.";
        return;
    }
    if (!request.vbo.empty()) active_load_files_.push_back(request.vbo);
    if (!request.racebox_csv.empty()) active_load_files_.push_back(request.racebox_csv);
    if (!request.sanwa_csv.empty()) active_load_files_.push_back(request.sanwa_csv);
    loading_ = true;
    error_.clear();
    if (!request.vbo.empty() && !request.racebox_csv.empty()) {
        status_ = request.sanwa_csv.empty() ? "Loading RaceBox and VBO telemetry without radio controls..." :
            "Parsing and correlating all telemetry sources...";
        load_future_ = std::async(std::launch::async, [request] { return load_session(request); });
    } else {
        status_ = request.vbo.empty()
            ? "Loading RaceBox CSV telemetry..."
            : "Loading VBO telemetry...";
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
            result.imu_analysis = imu::analyze(result.session.telemetry);
            return result;
        });
    }
}

void NativeApp::start_session_import(int preferred_run) {
    if (loading_) {
        session_import_error_ =
            "Finish loading the current session before importing another run.";
        status_ = session_import_error_;
        return;
    }
    if (race_day_busy_ && preferred_run >= 0) {
        session_import_error_ =
            "Wait for the current run comparison to finish before changing its data.";
        status_ = session_import_error_;
        return;
    }
    workspace_section_ = WorkspaceSection::RaceDay;
    const auto files = open_telemetry_files(
        window_, telemetry_watch_folder_);
    if (files.empty()) return;
    start_session_import_files(files, preferred_run);
}

void NativeApp::start_session_import_files(
    const std::vector<std::filesystem::path>& files,
    int preferred_run) {
    if (loading_) {
        session_import_error_ =
            "Finish loading the current session before importing another run.";
        status_ = session_import_error_;
        return;
    }
    if (race_day_busy_ && preferred_run >= 0) {
        session_import_error_ =
            "Wait for the current run comparison to finish before changing its data.";
        status_ = session_import_error_;
        return;
    }
    workspace_section_ = WorkspaceSection::RaceDay;
    session_import_error_.clear();
    if (files.empty()) return;
    std::vector<race_day::TelemetrySourceKind> selected_kinds;
    selected_kinds.reserve(files.size());
    for (const auto& file : files) {
        const auto kind =
            race_day::telemetry_source_kind(file);
        if (kind ==
            race_day::TelemetrySourceKind::Unsupported) {
            session_import_error_ = "Unsupported session file: " +
                path_utf8(file.filename());
            error_ = session_import_error_;
            return;
        }
        if (std::find(
                selected_kinds.begin(),
                selected_kinds.end(), kind) !=
            selected_kinds.end()) {
            session_import_error_ = std::string("Select only one ") +
                race_day::telemetry_source_kind_name(kind) +
                " file for a run.";
            error_ = session_import_error_;
            return;
        }
        selected_kinds.push_back(kind);
    }
    const auto selected_has =
        [&](race_day::TelemetrySourceKind kind) {
            return std::find(
                selected_kinds.begin(),
                selected_kinds.end(), kind) !=
                selected_kinds.end();
        };
    if (selected_has(
            race_day::TelemetrySourceKind::NativeArchive) &&
        files.size() != 1) {
        session_import_error_ =
            "A saved session archive must be selected by itself.";
        error_ = session_import_error_;
        return;
    }
    if (selected_has(race_day::TelemetrySourceKind::Gpx) &&
        (selected_has(race_day::TelemetrySourceKind::Vbo) ||
         selected_has(
             race_day::TelemetrySourceKind::RaceBoxCsv))) {
        session_import_error_ =
            "GPX cannot be mixed with VBO or RaceBox CSV files.";
        error_ = session_import_error_;
        return;
    }
    const auto selection_has_primary =
        selected_has(race_day::TelemetrySourceKind::Vbo) ||
        selected_has(
            race_day::TelemetrySourceKind::RaceBoxCsv) ||
        selected_has(race_day::TelemetrySourceKind::Gpx) ||
        selected_has(
            race_day::TelemetrySourceKind::NativeArchive);
    if (!selection_has_primary && preferred_run >= 0 &&
        preferred_run <
            static_cast<int>(race_day_.runs.size()) &&
        race_day::has_primary_telemetry(
            race_day_.runs[
                static_cast<std::size_t>(preferred_run)])) {
        auto& run = race_day_.runs[
            static_cast<std::size_t>(preferred_run)];
        const auto attached =
            race_day::attach_or_replace_telemetry_sources(
                run, files);
        if (!attached.ok) {
            session_import_error_ = attached.error;
            error_ = session_import_error_;
            return;
        }
        const auto invalidated =
            invalidate_setup_knowledge_for_run(race_day_, run.id);
        if (displayed_race_day_run_id_ == run.id) {
            displayed_race_day_run_id_.clear();
        }
        race_day_dirty_ = true;
        race_day_report_.reset();
        race_day_pending_knowledge_.reset();
        selected_setup_knowledge_record_ = -1;
        session_import_error_.clear();
        status_ = std::format(
            "Updated {} with {} new and {} replaced source(s){}",
            run.label, attached.added, attached.replaced,
            invalidated == 0
                ? ""
                : std::format(
                      "; removed {} outdated comparison result(s)",
                      invalidated));
        return;
    }
    session_import_files_ = files;
    session_import_requested_ = true;
    session_import_preferred_run_ = preferred_run;
    begin_load(files);
    if (!loading_) {
        session_import_requested_ = false;
        session_import_preferred_run_ = -1;
        session_import_files_.clear();
        session_import_error_ =
            "A run needs a RaceBox CSV, VBO, GPX, or session archive. "
            "Select the Sanwa file together with one of those recordings.";
        error_ = session_import_error_;
    }
}

void NativeApp::start_folder_import(int preferred_run) {
    if (folder_scan_busy_) {
        status_ = "The import folder is already being scanned.";
        return;
    }
    workspace_section_ = WorkspaceSection::RaceDay;
    if (telemetry_watch_folder_.empty()) {
        const auto selected = choose_telemetry_folder(window_);
        if (!selected) return;
        telemetry_watch_folder_ = *selected;
    }
    folder_import_preferred_run_ = preferred_run;
    discovered_files_.clear();
    discovered_file_selected_.clear();
    folder_scan_busy_ = true;
    const auto folder = telemetry_watch_folder_;
    status_ = "Scanning the saved import folder for telemetry...";
    folder_scan_future_ = std::async(
        std::launch::async,
        [folder] {
            return scan_telemetry_folder(folder);
        });
}

void NativeApp::open_racebox_cloud_export() {
    constexpr auto url =
        L"https://www.racebox.pro/webapp/login";
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteW(
            window_, L"open", url, nullptr, nullptr,
            SW_SHOWNORMAL));
    if (result <= 32) {
        error_ =
            "Windows could not open the official RaceBox sign-in page.";
        return;
    }
    status_ =
        "RaceBox sign-in opened in your browser. Export CSV or VBO, "
        "then scan the saved import folder here.";
}

void NativeApp::poll_import_discovery() {
    using namespace std::chrono_literals;
    if (folder_scan_busy_ && folder_scan_future_.valid() &&
        folder_scan_future_.wait_for(0ms) ==
            std::future_status::ready) {
        folder_scan_busy_ = false;
        auto scan = folder_scan_future_.get();
        if (!scan.error.empty()) {
            error_ = std::move(scan.error);
        } else {
            discovered_files_ = std::move(scan.files);
            discovered_file_selected_.assign(
                discovered_files_.size(), false);
            const auto primary = std::find_if(
                discovered_files_.begin(),
                discovered_files_.end(),
                [](const auto& file) {
                    return file.kind !=
                        race_day::TelemetrySourceKind::SanwaCsv;
                });
            if (primary != discovered_files_.end()) {
                discovered_file_selected_[
                    static_cast<std::size_t>(
                        std::distance(
                            discovered_files_.begin(), primary))] =
                    true;
            }
            discovered_files_popup_open_ = true;
            status_ = discovered_files_.empty()
                ? "No supported RaceBox, VBO, GPX, archive, or Sanwa files were found."
                : std::format(
                    "Found {} supported telemetry file(s) in the import folder{}.",
                    discovered_files_.size(),
                    scan.truncated
                        ? " (bounded scan; choose a narrower folder for more)"
                        : "");
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (auto_detect_sanwa_usb_ && now >= next_usb_poll_) {
        next_usb_poll_ = now + 3s;
        const auto roots = removable_drive_roots();
        std::unordered_set<std::wstring> current;
        for (const auto& root : roots) {
            auto key = root.wstring();
            std::transform(
                key.begin(), key.end(), key.begin(), ::towlower);
            current.insert(key);
            if (!known_removable_roots_.contains(key)) {
                pending_usb_scan_roots_.push_back(root);
            }
        }
        known_removable_roots_ = std::move(current);
    }

    if (!usb_scan_busy_ && !pending_usb_scan_roots_.empty()) {
        const auto root = pending_usb_scan_roots_.front();
        pending_usb_scan_roots_.pop_front();
        usb_scan_busy_ = true;
        usb_scan_future_ = std::async(
            std::launch::async,
            [root] {
                return scan_telemetry_folder(
                    root, true, 8'000, 6);
            });
    }
    if (usb_scan_busy_ && usb_scan_future_.valid() &&
        usb_scan_future_.wait_for(0ms) ==
            std::future_status::ready) {
        usb_scan_busy_ = false;
        auto scan = usb_scan_future_.get();
        if (!scan.error.empty()) {
            write_log(scan.error);
        } else if (!scan.files.empty()) {
            detected_usb_sanwa_ = std::move(scan.files.front());
            detected_usb_popup_open_ = true;
            status_ = std::format(
                "Detected Sanwa telemetry on USB: {}",
                path_utf8(
                    detected_usb_sanwa_->path.filename()));
        }
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
        imu_analysis_ = std::move(loaded.imu_analysis);
        current_session_source_files_ = active_load_files_;
        displayed_race_day_run_id_ =
            std::move(pending_displayed_race_day_run_id_);
        pending_displayed_race_day_run_id_.clear();
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
            error_ = compare_b_enabled_
                ? "Compare mode needs three different complete laps when Compare B is enabled"
                : "Compare mode needs two different complete laps";
        }
        if (view_mode_ == ViewMode::Compare) {
            workspace_section_ = WorkspaceSection::Compare;
        } else if (workspace_section_ == WorkspaceSection::Compare) {
            workspace_section_ = WorkspaceSection::Session;
        }
        if (workspace_section_ == WorkspaceSection::CrewChief) {
            requested_insights_tab_ = InsightsTab::Insights;
            insights_tab_request_pending_ = true;
        } else if (workspace_section_ == WorkspaceSection::Reports) {
            requested_telemetry_tab_ = TelemetryTab::Sectors;
            telemetry_tab_request_pending_ = true;
        } else if (workspace_section_ == WorkspaceSection::Session) {
            requested_telemetry_tab_ = TelemetryTab::Telemetry;
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
        if (session_import_requested_) {
            PendingSessionImport pending;
            pending.files = current_session_source_files_;
            pending.recorded_at_utc =
                recorded_time_iso8601(&*session_);
            pending.recorded_date =
                recorded_local_date(&*session_);
            if (session_import_preferred_run_ >= 0 &&
                session_import_preferred_run_ <
                    static_cast<int>(race_day_.runs.size())) {
                const auto& preferred = race_day_.runs[
                    static_cast<std::size_t>(session_import_preferred_run_)];
                pending.kind = preferred.kind;
                pending.kind_locked =
                    preferred.kind == race_day::RunKind::Custom;
                pending.destination_run = session_import_preferred_run_;
                pending.run_label = preferred.label;
            }
            pending_session_import_ = std::move(pending);
            session_import_error_.clear();
            if (pending_session_import_->destination_run < 0) {
                reset_session_import_destination();
            }
            session_import_requested_ = false;
            session_import_preferred_run_ = -1;
            session_import_files_.clear();
            status_ =
                "Session loaded. Confirm where it belongs in Race Day.";
        }
        write_log(status_);
    } catch (const std::exception& exception) {
        const auto import_failed = session_import_requested_;
        session_import_requested_ = false;
        session_import_preferred_run_ = -1;
        session_import_files_.clear();
        pending_displayed_race_day_run_id_.clear();
        error_ = exception.what();
        if (import_failed) session_import_error_ = error_;
        status_ = "Load failed";
        write_log("Load failed: " + error_);
    }
}

bool NativeApp::render() {
    poll_loader();
    poll_import_discovery();
    ImGui::GetStyle().FontScaleMain = text_scale_;
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
        start_session_import();
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
    const auto header_height = application_header_height(text_scale_);
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
        const auto layout_built = build_guided_layout(dockspace, ImGui::GetWindowPos(), ImGui::GetWindowSize());
        build_default_layout_ = false;
        if (layout_built) loaded_layout_version_ = kCurrentUiLayoutVersion;
        else error_ = "The guided dock layout could not be completed; it will be retried after restart";
    }
    ImGui::DockSpace(dockspace, ImVec2(0.0F, 0.0F), ImGuiDockNodeFlags_PassthruCentralNode);
    set_dock_tree_customizable(ImGui::DockBuilderGetNode(dockspace), customize_layout_);
    ImGui::End();
    ImGui::PopStyleVar(2);

    if (workspace_section_ == WorkspaceSection::RaceDay) {
        draw_review_locked([&] { draw_race_day(); });
        draw_annotations();
        handle_annotation_workspace();
        if (!hover_seen_this_frame_) hover_cursor_us_.reset();
        return !exit_requested_;
    }
    if (workspace_section_ == WorkspaceSection::Reports) {
        draw_review_locked([&] { draw_reports(); });
        draw_annotations();
        handle_annotation_workspace();
        if (!hover_seen_this_frame_) hover_cursor_us_.reset();
        return !exit_requested_;
    }

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

bool NativeApp::build_guided_layout(ImGuiID dockspace, ImVec2 origin, ImVec2 size) {
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

void NativeApp::draw_session_run_selector() {
    const race_day::Run* displayed_run = nullptr;
    if (!displayed_race_day_run_id_.empty()) {
        const auto found = std::find_if(
            race_day_.runs.begin(), race_day_.runs.end(),
            [&](const race_day::Run& run) {
                return run.id == displayed_race_day_run_id_;
            });
        if (found != race_day_.runs.end()) displayed_run = &*found;
    }

    const auto preview = displayed_run
        ? displayed_run->label
        : session_
        ? std::string{"Current files (not linked to Race Day)"}
        : std::string{"Choose a Race Day run..."};

    ImGui::TextDisabled("RACE DAY RUN");
    ImGui::SetNextItemWidth(-1.0F);
    if (!ImGui::BeginCombo(
            "##session-race-day-run", preview.c_str())) {
        return;
    }

    if (race_day_.runs.empty()) {
        ImGui::TextDisabled(
            "No Race Day runs yet. Add a recording in Race Day first.");
    }
    for (std::size_t index = 0;
         index < race_day_.runs.size(); ++index) {
        const auto& run = race_day_.runs[index];
        const auto composition =
            race_day::validate_telemetry_source_composition(
                run, true);
        auto sources_available = !run.telemetry_files.empty();
        for (std::size_t source_index = 0;
             source_index < run.telemetry_files.size();
             ++source_index) {
            sources_available =
                sources_available &&
                race_day::telemetry_source_state(
                    run, source_index) ==
                    race_day::TelemetrySourceState::Available;
        }
        const auto ready = composition.ok &&
            composition.has_primary && sources_available;
        const auto state = run.telemetry_files.empty()
            ? "NO DATA"
            : !composition.ok
            ? "NEEDS REPAIR"
            : !sources_available
            ? "NEEDS FILE"
            : composition.has_primary
            ? "READY"
            : "CONTROLS ONLY";
        const auto label = std::format(
            "{}  |  {}##session-run-{}",
            run.label, state, index);
        const auto selected =
            displayed_race_day_run_id_ == run.id;
        ImGui::BeginDisabled(!ready || loading_);
        if (ImGui::Selectable(label.c_str(), selected)) {
            open_race_day_run_in_viewer(index);
        }
        ImGui::EndDisabled();
        if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
}

void NativeApp::draw_app_header() {
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, application_header_height(text_scale_)), ImGuiCond_Always);
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
        const auto compact_header_controls =
            viewport->WorkSize.x < 1450.0F * text_scale_;
        const auto toggle_theme = [&] {
            light_theme_ = !light_theme_;
            ui::apply_theme(light_theme_ ? ui::ThemeMode::Light : ui::ThemeMode::Dark,
                            ui::dpi_scale_for_window(window_));
            ImGui::GetStyle().FontScaleMain = text_scale_;
            status_ = light_theme_ ? "Light mode enabled" : "Dark mode enabled";
        };
        const auto toggle_annotation = [&] {
            annotation_mode_ = !annotation_mode_;
            if (annotation_mode_) {
                show_annotation_pins_ = true;
                start_finish_placement_mode_ = false;
                annotation_click_consumed_ = true;
                status_ = "Annotation mode on: click anywhere to place a review pin; Esc finishes";
            } else {
                status_ = "Annotation mode off";
            }
        };
        ImGui::TextColored(ImVec4(0.95F, 0.10F, 0.28F, 1.0F), "R");
        ImGui::SameLine(0.0F, 4.0F);
        ImGui::TextUnformatted("RACEBOX");
        if (!compact_header_controls) {
            ImGui::SameLine(0.0F, 8.0F);
            ImGui::TextDisabled("Telemetry Analysis");
        }
        ImGui::SameLine(0.0F, 6.0F);
        ImGui::TextColored(ImVec4(0.34F, 0.72F, 0.92F, 1.0F), "v%s", RACEBOX_VERSION_STRING);
        ImGui::Separator();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Add recording to Race Day...", "Ctrl+O")) {
            start_session_import();
        }
        if (ImGui::MenuItem(
                "Scan saved import folder...", nullptr, false,
                !folder_scan_busy_)) {
            start_folder_import();
        }
        if (ImGui::MenuItem("Choose import folder...")) {
            const auto selected = choose_telemetry_folder(
                window_, telemetry_watch_folder_);
            if (selected) {
                telemetry_watch_folder_ = *selected;
                status_ = "Telemetry import folder updated.";
            }
        }
        if (ImGui::MenuItem("Open official RaceBox cloud export...")) {
            open_racebox_cloud_export();
        }
        ImGui::SeparatorText("Race day");
        if (ImGui::MenuItem("New race day")) {
            if (race_day_dirty_) {
                workspace_section_ = WorkspaceSection::RaceDay;
                race_day_error_ =
                    "Save the current Race Day, or use its New button "
                    "to confirm replacing unsaved changes.";
            } else {
                new_race_day();
            }
        }
        if (ImGui::MenuItem("Open race day...")) open_race_day();
        if (ImGui::MenuItem("Save race day", nullptr, false, !race_day_.runs.empty())) save_race_day(false);
        if (ImGui::MenuItem("Save race day as...", nullptr, false, !race_day_.runs.empty())) save_race_day(true);
        ImGui::SeparatorText("Telemetry");
        if (ImGui::MenuItem("Save processed session...", nullptr, false, session_.has_value())) save_session();
        if (ImGui::MenuItem("Save active lap archive...", nullptr, false, active_lap() != nullptr)) save_active_lap();
        if (ImGui::MenuItem("Export active lap CSV...", nullptr, false, active_lap() != nullptr)) export_active_lap();
        ImGui::SeparatorText("Review");
        if (ImGui::MenuItem("Import annotation review...", nullptr, false,
                            session_.has_value())) {
            import_annotations();
        }
        if (ImGui::MenuItem("Export annotation review...", nullptr, false, !annotations_.empty())) {
            export_annotations();
        }
        const auto triangulated_available = session_ && uses_parking_track_background(*session_);
        if (ImGui::MenuItem("Import triangulated Google map", nullptr, false, triangulated_available)) {
            load_triangulated_background();
        }
        if (ImGui::MenuItem("Load uncalibrated background (advanced)...", nullptr, false, session_.has_value())) {
            load_background();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear current session and import state", nullptr, false, session_.has_value() ||
            !pending_vbo_.empty() || !pending_racebox_csv_.empty() || !pending_sanwa_csv_.empty())) {
            playing_ = false;
            session_.reset();
            imu_analysis_ = {};
            background_.reset();
            average_track_latitude_.clear();
            average_track_longitude_.clear();
            annotations_.clear();
            next_annotation_id_ = 1;
            selected_annotation_id_ = 0;
            pending_vbo_.clear(); pending_racebox_csv_.clear(); pending_sanwa_csv_.clear();
            active_load_files_.clear();
            current_session_source_files_.clear();
            displayed_race_day_run_id_.clear();
            pending_displayed_race_day_run_id_.clear();
            session_import_files_.clear();
            session_import_requested_ = false;
            session_import_preferred_run_ = -1;
            pending_session_import_.reset();
            session_import_error_.clear();
            error_.clear();
            status_ = "Import a RaceBox, VBO, GPX, or session archive recording.";
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            if (race_day_dirty_) {
                workspace_section_ = WorkspaceSection::RaceDay;
                race_day_error_ =
                    "Save the Race Day before exiting so run notes "
                    "and telemetry links are not lost.";
            } else {
                exit_requested_ = true;
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Light theme", nullptr, light_theme_)) {
            toggle_theme();
        }
        ImGui::MenuItem("Speed-coloured map", nullptr, &show_speed_color_);
        ImGui::MenuItem("Metric units", nullptr, &metric_units_);
        ImGui::MenuItem("Annotation mode", nullptr, &annotation_mode_);
        ImGui::MenuItem("Compact telemetry", nullptr, &compact_telemetry_);
        if (ImGui::BeginMenu("Text size")) {
            const auto text_size = [&](const char* label, float scale) {
                if (ImGui::MenuItem(label, nullptr, std::abs(text_scale_ - scale) < 0.001F)) {
                    text_scale_ = scale;
                    ImGui::GetStyle().FontScaleMain = text_scale_;
                    status_ = std::format("Text size set to {:.0f}%", text_scale_ * 100.0F);
                }
            };
            text_size("100%", 1.0F);
            text_size("115%", 1.15F);
            text_size("130%", 1.30F);
            text_size("150%", 1.50F);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Panels")) {
            ImGui::MenuItem("Radio Alignment", nullptr, &show_radio_panel_);
            ImGui::MenuItem("Theoretical Analysis", nullptr, &show_analysis_panel_);
            ImGui::MenuItem("G-G Plot", nullptr, &show_gg_panel_);
            ImGui::MenuItem("Altitude", nullptr, &show_altitude_panel_);
            ImGui::MenuItem("Diagnostics", nullptr, &show_diagnostics_panel_);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Customize layout", nullptr, &customize_layout_)) {
            status_ = customize_layout_
                ? "Layout editing enabled: drag tabs and splitters to customize the workspace"
                : "Layout locked: accidental tab moves and splitter drags are disabled";
        }
        if (ImGui::MenuItem("Reset to guided layout")) {
            customize_layout_ = false;
            build_default_layout_ = true;
            status_ = "Guided layout restored and locked";
        }
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
                        error_ = compare_b_enabled_
                            ? "Compare mode needs three different complete laps when Compare B is enabled"
                            : "Compare mode needs two different complete laps";
                    }
                } else {
                    workspace_section_ = section;
                    if (section == WorkspaceSection::Session) {
                        requested_telemetry_tab_ = TelemetryTab::Telemetry;
                        requested_insights_tab_ = InsightsTab::Insights;
                        telemetry_tab_request_pending_ = true;
                        insights_tab_request_pending_ = true;
                    }
                    if (section == WorkspaceSection::CrewChief) {
                        requested_insights_tab_ = InsightsTab::Insights;
                        insights_tab_request_pending_ = true;
                        show_analysis_panel_ = true;
                    }
                    if (section == WorkspaceSection::Reports) {
                        requested_telemetry_tab_ = TelemetryTab::Sectors;
                        requested_insights_tab_ = InsightsTab::Insights;
                        telemetry_tab_request_pending_ = true;
                        insights_tab_request_pending_ = true;
                    }
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
        workspace_button("RACE DAY", WorkspaceSection::RaceDay);
        workspace_button("SESSION", WorkspaceSection::Session);
        workspace_button("COMPARE", WorkspaceSection::Compare);
        workspace_button("ANALYSIS", WorkspaceSection::CrewChief);
        workspace_button("REPORTS", WorkspaceSection::Reports);

        const auto renderer = software_renderer_ ? "WARP" : "DX11";
        if (compact_header_controls) {
            ImGui::SameLine(0.0F, 8.0F);
            if (ImGui::BeginMenu("TOOLS")) {
                if (ImGui::MenuItem(
                        light_theme_ ? "Use dark theme" : "Use light theme")) {
                    toggle_theme();
                }
                if (ImGui::MenuItem(
                        customize_layout_ ? "Lock layout" : "Customize layout")) {
                    customize_layout_ = !customize_layout_;
                    status_ = customize_layout_
                        ? "Layout editing enabled: drag tabs and splitters to customize the workspace"
                        : "Layout locked: accidental tab moves and splitter drags are disabled";
                }
                if (ImGui::MenuItem(
                        annotation_mode_ ? "Finish annotation mode" : "Annotate",
                        nullptr, annotation_mode_)) {
                    toggle_annotation();
                }
                ImGui::Separator();
                ImGui::TextDisabled("Renderer: %s", renderer);
                ImGui::EndMenu();
            }
        } else {
            ImGui::SameLine(0.0F, 12.0F);
            if (ImGui::SmallButton(
                    light_theme_ ? "THEME: LIGHT" : "THEME: DARK")) {
                toggle_theme();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Switch between light and dark graphics.");
            }

            ImGui::SameLine(0.0F, 8.0F);
            if (ImGui::SmallButton(
                    customize_layout_ ? "LAYOUT: CUSTOM" : "LAYOUT: LOCKED")) {
                customize_layout_ = !customize_layout_;
                status_ = customize_layout_
                    ? "Layout editing enabled: drag tabs and splitters to customize the workspace"
                    : "Layout locked: accidental tab moves and splitter drags are disabled";
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(customize_layout_
                    ? "Click to lock the current panel arrangement."
                    : "Click to allow panel dragging and splitter resizing.");
            }

            ImGui::SameLine(0.0F, 12.0F);
            ImGui::PushStyleColor(ImGuiCol_Button, annotation_mode_
                ? ImVec4(0.62F, 0.08F, 0.16F, 0.95F)
                : ImVec4(0.10F, 0.13F, 0.18F, 0.92F));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonHovered,
                ImVec4(0.75F, 0.10F, 0.20F, 1.0F));
            if (ImGui::SmallButton(
                    annotation_mode_ ? "ANNOTATION ON (ESC)" : "ANNOTATE")) {
                if (!annotation_mode_) toggle_annotation();
            }
            ImGui::PopStyleColor(2);
            if (!annotation_mode_ && ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Place numbered review pins anywhere in the interface. Press Esc when finished.");
            }

            ImGui::SameLine(0.0F, 10.0F);
            ImGui::TextColored(
                software_renderer_
                    ? ImVec4(1.0F, 0.55F, 0.2F, 1.0F)
                    : ImVec4(0.25F, 0.8F, 0.45F, 1.0F),
                "%s", renderer);
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
        if (workspace_section_ == WorkspaceSection::Session) {
            draw_session_run_selector();
        } else {
            context_field(
                "SESSION",
                session_
                    ? session_->name.c_str()
                    : "No session loaded");
        }
        ImGui::TableNextColumn();
        const auto recorded = friendly_recorded_time_label(
            recorded_time_iso8601(session_ ? &*session_ : nullptr));
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
            if (compare_b_enabled_) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95F, 0.25F, 0.28F, 1.0F), "B %s", lap_short(compare_b_lap()).c_str());
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.10F, 0.85F, 0.95F, 1.0F), "PLAY %s", lap_short(playback_lap()).c_str());
        } else {
            ImGui::TextDisabled("No active laps");
        }
        ImGui::EndTable();
    }
    draw_session_import_popup();
    draw_import_discovery_popups();
    draw_global_notification();
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void NativeApp::draw_global_notification() {
    ImGui::Separator();
    std::string* attention = nullptr;
    if (!error_.empty()) attention = &error_;
    else if (!race_day_error_.empty()) attention = &race_day_error_;
    else if (!crew_chief_error_.empty()) attention = &crew_chief_error_;

    if (attention) {
        ImGui::TextColored(ui::color(ui::ColorToken::Danger), "NEEDS ATTENTION");
        ImGui::SameLine();
        const auto dismiss_width = ImGui::CalcTextSize("Dismiss").x + ImGui::GetStyle().FramePadding.x * 2.0F;
        ImGui::PushTextWrapPos(std::max(ImGui::GetCursorPosX() + 120.0F,
            ImGui::GetWindowWidth() - dismiss_width - 26.0F));
        ImGui::TextWrapped("%s", attention->c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss##global-notification")) attention->clear();
        return;
    }

    ImGui::TextColored(loading_ ? ui::color(ui::ColorToken::Warning) : ui::color(ui::ColorToken::Playback),
        "%s", loading_ ? "WORKING" : "STATUS");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", status_.c_str());
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
            error_ = compare_b_enabled_
                ? "Compare mode needs three different complete laps when Compare B is enabled"
                : "Compare mode needs two different complete laps";
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
            const auto lateral = interpolate_time_channel(session_->telemetry.time_us, session_->telemetry.lateral_g, inspected_data_time) -
                (imu_analysis_.initial_stationary_zero_used ? imu_analysis_.acceleration_zero_g[1] : 0.0);
            const auto longitudinal = interpolate_time_channel(session_->telemetry.time_us, session_->telemetry.longitudinal_g, inspected_data_time) -
                (imu_analysis_.initial_stationary_zero_used ? imu_analysis_.acceleration_zero_g[0] : 0.0);
            const auto altitude = interpolate_time_channel(session_->telemetry.time_us, session_->telemetry.altitude_m, inspected_data_time);
            ImGui::Text("%.1f %s   Lat %+.2f g   Long %+.2f g", speed, metric_units_ ? "km/h" : "mph", lateral, longitudinal);
            ImGui::Text("Altitude %.1f m   Distance %.1f%%", altitude, inspected_progress);
            if (radio.valid) ImGui::Text("Throttle %.0f%%   Brake %.0f%%   Steering %+.0f%%", radio.throttle, radio.brake, radio.steering);
            else ImGui::TextDisabled("Sanwa controls: not recorded at this point");
            if (view_mode_ == ViewMode::Compare && active_lap() && compare_lap()) {
                refresh_plot_cache();
                const auto role_row = [&](const char* role, const LapInfo* lap, ui::ColorToken token) {
                    const auto timestamp = time_at_progress(lap, inspected_progress);
                    const auto lap_speed = interpolate_time_channel(session_->telemetry.time_us, session_->telemetry.speed_kmh,
                        timestamp) * (metric_units_ ? 1.0 : 0.621371);
                    const auto lat = interpolate_time_channel(session_->telemetry.time_us, session_->telemetry.lateral_g, timestamp) -
                        (imu_analysis_.initial_stationary_zero_used ? imu_analysis_.acceleration_zero_g[1] : 0.0);
                    const auto lon = interpolate_time_channel(session_->telemetry.time_us, session_->telemetry.longitudinal_g, timestamp) -
                        (imu_analysis_.initial_stationary_zero_used ? imu_analysis_.acceleration_zero_g[0] : 0.0);
                    const auto controls = sample_radio(*session_, timestamp);
                    ImGui::TextColored(ui::color(token), "%s  %.1f %s | Lat %+.2f  Long %+.2f", role, lap_speed,
                        metric_units_ ? "km/h" : "mph", lat, lon);
                    ImGui::SameLine();
                    if (controls.valid) ImGui::TextDisabled("T %.0f  B %.0f  S %+.0f", controls.throttle, controls.brake, controls.steering);
                    else ImGui::TextDisabled("T -  B -  S -");
                };
                role_row("REF", active_lap(), ui::ColorToken::Reference);
                role_row("A", compare_lap(), ui::ColorToken::CompareA);
                if (compare_b_enabled_) role_row("B", compare_b_lap(), ui::ColorToken::CompareB);
                if (!relative_delta_progress_.empty()) {
                    const auto delta_a = interpolate_curve(relative_delta_progress_, relative_delta_seconds_, inspected_progress);
                    if (compare_b_enabled_) {
                        const auto delta_b = interpolate_curve(relative_delta_progress_, relative_delta_b_seconds_, inspected_progress);
                        ImGui::Text("Relative time  A %+.3f s   B %+.3f s", delta_a, delta_b);
                    } else {
                        ImGui::Text("Relative time  A %+.3f s", delta_a);
                    }
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
    const auto base = session_->telemetry.time_us[begin];
    const auto distance_profile = normalized_progress && lap
        ? build_distance_time_profile(*session_, *lap)
        : DistanceTimeProfile{};
    std::vector<std::array<double, 2>> radio_channels;
    if (count > max_points && max_points > 0) {
        radio_channels.reserve(count);
        for (auto index = begin; index <= end; ++index) {
            const auto radio = sample_radio(
                *session_, session_->telemetry.time_us[index]);
            radio_channels.push_back({
                radio.valid ? radio.throttle - radio.brake
                            : std::numeric_limits<double>::quiet_NaN(),
                radio.valid ? radio.steering
                            : std::numeric_limits<double>::quiet_NaN(),
            });
        }
    }
    constexpr std::size_t decimation_channels = 10;
    const auto sample_indices = peak_preserving_indices(
        count, max_points, decimation_channels,
        [&](std::size_t offset, std::size_t channel) {
            const auto index = begin + offset;
            switch (channel) {
                case 0: return static_cast<double>(
                    session_->telemetry.speed_kmh[index]);
                case 1: return static_cast<double>(
                    session_->telemetry.lateral_g[index]);
                case 2: return static_cast<double>(
                    session_->telemetry.longitudinal_g[index]);
                case 3: return static_cast<double>(
                    session_->telemetry.vertical_g[index]);
                case 4: return static_cast<double>(
                    session_->telemetry.altitude_m[index]);
                case 5: return static_cast<double>(
                    session_->telemetry.gyro_x_dps[index]);
                case 6: return static_cast<double>(
                    session_->telemetry.gyro_y_dps[index]);
                case 7: return static_cast<double>(
                    session_->telemetry.gyro_z_dps[index]);
                case 8: return radio_channels.empty()
                    ? 0.0 : radio_channels[offset][0];
                case 9: return radio_channels.empty()
                    ? 0.0 : radio_channels[offset][1];
                default: return 0.0;
            }
        });
    const auto reserve = [&](auto& values) {
        values.reserve(sample_indices.size());
    };
    reserve(output.time); reserve(output.elapsed);
    reserve(output.speed); reserve(output.speed_mph);
    reserve(output.lateral); reserve(output.longitudinal);
    reserve(output.altitude); reserve(output.vertical);
    reserve(output.gyro_x); reserve(output.gyro_y);
    reserve(output.gyro_z); reserve(output.vehicle_yaw);
    reserve(output.controls); reserve(output.steering);
    for (const auto offset : sample_indices) {
        const auto index = begin + offset;
        const auto radio = sample_radio(*session_, session_->telemetry.time_us[index]);
        output.time.push_back(normalized_progress && !distance_profile.progress.empty()
            ? distance_profile.progress[offset]
            : static_cast<double>(session_->telemetry.time_us[index] - base) / kSecond);
        output.elapsed.push_back(static_cast<double>(session_->telemetry.time_us[index] - base) / kSecond);
        output.speed.push_back(session_->telemetry.speed_kmh[index]);
        output.speed_mph.push_back(session_->telemetry.speed_kmh[index] * 0.621371);
        output.lateral.push_back(session_->telemetry.lateral_g[index] -
            (imu_analysis_.initial_stationary_zero_used ? imu_analysis_.acceleration_zero_g[1] : 0.0));
        output.longitudinal.push_back(session_->telemetry.longitudinal_g[index] -
            (imu_analysis_.initial_stationary_zero_used ? imu_analysis_.acceleration_zero_g[0] : 0.0));
        output.altitude.push_back(session_->telemetry.altitude_m[index]);
        output.vertical.push_back(session_->telemetry.vertical_g[index] -
            (imu_analysis_.initial_stationary_zero_used ? imu_analysis_.acceleration_zero_g[2] : 0.0));
        output.gyro_x.push_back(session_->telemetry.gyro_x_dps[index] -
            (imu_analysis_.calibration.stationary_bias_used ? imu_analysis_.calibration.gyro_bias_dps[0] : 0.0));
        output.gyro_y.push_back(session_->telemetry.gyro_y_dps[index] -
            (imu_analysis_.calibration.stationary_bias_used ? imu_analysis_.calibration.gyro_bias_dps[1] : 0.0));
        output.gyro_z.push_back(session_->telemetry.gyro_z_dps[index] -
            (imu_analysis_.calibration.stationary_bias_used ? imu_analysis_.calibration.gyro_bias_dps[2] : 0.0));
        output.vehicle_yaw.push_back(index < imu_analysis_.vehicle_yaw_rate_dps.size()
            ? imu_analysis_.vehicle_yaw_rate_dps[index] : std::numeric_limits<float>::quiet_NaN());
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
    compare_b_plot_cache_ = normalized && compare_b_enabled_
        ? build_plot_data(compare_b_lap(), 2200, true)
        : PlotData{};
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
        const auto compare_b_profile = compare_b_enabled_ && compare_b_lap()
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
                        if (compare_b_enabled_) {
                            annotate_reading(tertiary, third, "B", ui::color(ui::ColorToken::CompareB), 20.0F);
                        }
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
        if (compare_b_enabled_) {
            for (const auto value : relative_delta_b_seconds_) maximum_delta = std::max(maximum_delta, std::abs(value));
        }
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
            if (compare_b_enabled_) {
                const ImPlotSpec delta_b_style{ImPlotProp_LineColor, ImVec4(0.95F, 0.25F, 0.28F, 1.0F), ImPlotProp_LineWeight, 1.8F};
                ImPlot::PlotLine("Compare B - Reference", relative_delta_progress_.data(), relative_delta_b_seconds_.data(),
                    static_cast<int>(std::min(relative_delta_progress_.size(), relative_delta_b_seconds_.size())), delta_b_style);
            }
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
                    if (compare_b_enabled_) {
                        ImPlot::Annotation(inspection_progress, inspection_delta_b, ui::color(ui::ColorToken::CompareB),
                            ImVec2(10.0F, 12.0F), true, "B %+.3f s", inspection_delta_b);
                    }
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
            ImGui::TextColored(ui::color(ui::ColorToken::CompareA), "A %+.3f", a);
            if (compare_b_enabled_) {
                const auto b = interpolate_curve(relative_delta_progress_, relative_delta_b_seconds_, plot_cursor_x);
                ImGui::SameLine();
                ImGui::TextColored(ui::color(ui::ColorToken::CompareB), "B %+.3f", b);
            }
        } else {
            ImGui::TextColored(ui::color(ui::ColorToken::Reference), "R %s", value_text(values_for(primary)).c_str());
            if (view_mode_ == ViewMode::Compare) {
                ImGui::SameLine(); ImGui::TextColored(ui::color(ui::ColorToken::CompareA), "A %s", value_text(values_for(secondary)).c_str());
                if (compare_b_enabled_) {
                    ImGui::SameLine();
                    ImGui::TextColored(ui::color(ui::ColorToken::CompareB), "B %s", value_text(values_for(tertiary)).c_str());
                }
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
    if (ImGui::BeginTabItem("IMU", nullptr, tab_flags(TelemetryTab::Imu))) {
        if (telemetry_tab_request_pending_ && requested_telemetry_tab_ == TelemetryTab::Imu) telemetry_tab_request_pending_ = false;
        draw_imu_tab();
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

void NativeApp::draw_imu_tab() {
    if (!session_ || !imu_analysis_.available) {
        ImGui::TextWrapped("No recorded IMU channels are available. Older archives and GPX files open with empty compatibility channels.");
        return;
    }
    refresh_plot_cache();
    const auto& data = primary_plot_cache_;
    const auto sample_rate = session_->telemetry.size() > 1 && session_->telemetry.time_us.back() > session_->telemetry.time_us.front()
        ? static_cast<double>(session_->telemetry.size() - 1) * kSecond /
            static_cast<double>(session_->telemetry.time_us.back() - session_->telemetry.time_us.front())
        : 0.0;
    ImGui::Text("Recorded IMU output: %.1f Hz", sample_rate);
    if (imu_analysis_.initial_stationary_zero_used) {
        const auto zero_start = static_cast<double>(session_->telemetry.time_us[imu_analysis_.zero_begin_index]) / kSecond;
        const auto zero_end = static_cast<double>(session_->telemetry.time_us[imu_analysis_.zero_end_index]) / kSecond;
        ImGui::TextColored(ui::color(ui::ColorToken::Positive),
            "Initial stationary zero: %.2f-%.2f s   Long %+.3f  Lat %+.3f  Vertical %+.3f g",
            zero_start, zero_end, imu_analysis_.acceleration_zero_g[0], imu_analysis_.acceleration_zero_g[1],
            imu_analysis_.acceleration_zero_g[2]);
    } else {
        ImGui::TextColored(ui::color(ui::ColorToken::Warning),
            "No two-second stationary block was found; acceleration graphs remain uncalibrated.");
    }
    const auto& calibration = imu_analysis_.calibration;
    if (calibration.valid) {
        ImGui::TextColored(ui::color(ui::ColorToken::Positive),
            "Vehicle yaw estimate calibrated: r=%.3f from %zu GPS/gyro samples",
            calibration.heading_correlation, calibration.matched_samples);
        ImGui::TextDisabled("Axis mix X %+.3f  Y %+.3f  Z %+.3f   Bias %+.2f / %+.2f / %+.2f deg/s (%s)",
            calibration.yaw_projection[0], calibration.yaw_projection[1], calibration.yaw_projection[2],
            calibration.gyro_bias_dps[0], calibration.gyro_bias_dps[1], calibration.gyro_bias_dps[2],
            calibration.stationary_bias_used ? "stationary samples" : "whole-session fallback");
    } else {
        ImGui::TextColored(ui::color(ui::ColorToken::Warning),
            "Vehicle yaw estimate withheld: gyro/GPS correlation %.3f is below the 0.60 reliability gate.",
            calibration.heading_correlation);
    }
    ImGui::TextWrapped("Displayed acceleration and gyro axes are zeroed from the first stationary block. Stored channels remain untouched. X/Y/Z are not assumed roll/pitch/yaw; vehicle yaw is a separate calibrated estimate. Raw GPS is never moved or rewritten.");
    ImGui::Separator();

    const auto [range_begin, range_end] = active_range();
    const auto base_time = session_->telemetry.time_us[range_begin];
    const auto x_label = view_mode_ == ViewMode::Compare ? "Lap progress (%)" : "Time (s)";
    const auto cursor_x = [&] {
        if (view_mode_ == ViewMode::Compare && active_lap() && !primary_distance_elapsed_.empty()) {
            const auto elapsed = std::clamp(static_cast<double>(cursor_us_ - session_->telemetry.time_us[active_lap()->begin_index]) / kSecond,
                primary_distance_elapsed_.front(), primary_distance_elapsed_.back());
            return interpolate_curve(primary_distance_elapsed_, primary_distance_progress_, elapsed);
        }
        return static_cast<double>(cursor_us_ - base_time) / kSecond;
    }();
    const auto update_hover = [&](bool hovered) {
        if (!hovered || annotation_mode_) return;
        const auto mouse = ImPlot::GetPlotMousePos();
        Timestamp timestamp{};
        if (view_mode_ == ViewMode::Compare && active_lap() && !primary_distance_progress_.empty()) {
            const auto elapsed = interpolate_curve(primary_distance_progress_, primary_distance_elapsed_, std::clamp(mouse.x, 0.0, 100.0));
            timestamp = session_->telemetry.time_us[active_lap()->begin_index] + static_cast<Timestamp>(elapsed * kSecond);
        } else {
            timestamp = base_time + static_cast<Timestamp>(mouse.x * kSecond);
        }
        hover_seen_this_frame_ = true;
        hover_cursor_us_ = std::clamp(timestamp, session_->telemetry.time_us[range_begin], session_->telemetry.time_us[range_end]);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) cursor_us_ = *hover_cursor_us_;
    };
    const auto limits = [](std::initializer_list<const std::vector<double>*> series, double fallback) {
        auto maximum = fallback;
        for (const auto* values : series) {
            for (const auto value : *values) if (std::isfinite(value)) maximum = std::max(maximum, std::abs(value));
        }
        return maximum * 1.12;
    };
    const auto cursor_style = ImPlotSpec{ImPlotProp_LineColor, ImVec4(1.0F, 0.73F, 0.15F, 0.95F), ImPlotProp_LineWeight, 1.6F};
    const auto plot_height = std::max(150.0F, ImGui::GetContentRegionAvail().y * 0.31F);

    const auto vertical_limit = limits({&data.vertical}, 1.5);
    if (ImPlot::BeginPlot("Zero-calibrated vertical acceleration###imu-vertical", ImVec2(-1, plot_height),
                          annotation_mode_ ? ImPlotFlags_NoInputs : ImPlotFlags_None)) {
        ImPlot::SetupAxes(x_label, "Vertical G");
        if (!data.time.empty()) ImPlot::SetupAxisLimits(ImAxis_X1, data.time.front(), data.time.back(), ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -vertical_limit, vertical_limit, ImGuiCond_Always);
        const ImPlotSpec vertical_style{ImPlotProp_LineColor, ImVec4(0.20F, 0.78F, 0.40F, 1.0F), ImPlotProp_LineWeight, 1.8F};
        ImPlot::PlotLine("Vertical G", data.time.data(), data.vertical.data(),
            static_cast<int>(std::min(data.time.size(), data.vertical.size())), vertical_style);
        const double reference_lines[] = {0.0, -imu_analysis_.vertical_rest_g * 0.60};
        const ImPlotSpec reference_style{ImPlotProp_LineColor, ImVec4(0.55F, 0.62F, 0.70F, 0.65F),
            ImPlotProp_LineWeight, 1.0F, ImPlotProp_Flags, ImPlotInfLinesFlags_Horizontal};
        ImPlot::PlotInfLines("Zero / possible-airborne threshold", reference_lines, 2, reference_style);
        ImPlot::PlotInfLines("Playback cursor", &cursor_x, 1, cursor_style);
        const auto hovered = ImPlot::IsPlotHovered();
        update_hover(hovered);
        const auto origin = ImPlot::GetPlotPos();
        const auto size = ImPlot::GetPlotSize();
        handle_annotation_surface("Vertical acceleration", -1, origin.x, origin.y, size.x, size.y, hovered, "imu_vertical");
        ImPlot::EndPlot();
    }

    const auto gyro_limit = limits({&data.gyro_x, &data.gyro_y, &data.gyro_z, &data.vehicle_yaw}, 100.0);
    if (ImPlot::BeginPlot("Gyroscope###imu-gyro", ImVec2(-1, plot_height),
                          annotation_mode_ ? ImPlotFlags_NoInputs : ImPlotFlags_None)) {
        ImPlot::SetupAxes(x_label, "Degrees / second");
        if (!data.time.empty()) ImPlot::SetupAxisLimits(ImAxis_X1, data.time.front(), data.time.back(), ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -gyro_limit, gyro_limit, ImGuiCond_Always);
        const ImPlotSpec x_style{ImPlotProp_LineColor, ImVec4(0.95F, 0.30F, 0.30F, 0.8F), ImPlotProp_LineWeight, 1.2F};
        const ImPlotSpec y_style{ImPlotProp_LineColor, ImVec4(0.35F, 0.75F, 1.0F, 0.8F), ImPlotProp_LineWeight, 1.2F};
        const ImPlotSpec z_style{ImPlotProp_LineColor, ImVec4(0.75F, 0.45F, 1.0F, 0.8F), ImPlotProp_LineWeight, 1.2F};
        const ImPlotSpec yaw_style{ImPlotProp_LineColor, ImVec4(1.0F, 0.78F, 0.18F, 1.0F), ImPlotProp_LineWeight, 2.0F};
        ImPlot::PlotLine("Zeroed X", data.time.data(), data.gyro_x.data(), static_cast<int>(std::min(data.time.size(), data.gyro_x.size())), x_style);
        ImPlot::PlotLine("Zeroed Y", data.time.data(), data.gyro_y.data(), static_cast<int>(std::min(data.time.size(), data.gyro_y.size())), y_style);
        ImPlot::PlotLine("Zeroed Z", data.time.data(), data.gyro_z.data(), static_cast<int>(std::min(data.time.size(), data.gyro_z.size())), z_style);
        if (calibration.valid) ImPlot::PlotLine("Vehicle yaw estimate", data.time.data(), data.vehicle_yaw.data(),
            static_cast<int>(std::min(data.time.size(), data.vehicle_yaw.size())), yaw_style);
        ImPlot::PlotInfLines("Playback cursor", &cursor_x, 1, cursor_style);
        const auto hovered = ImPlot::IsPlotHovered();
        update_hover(hovered);
        const auto origin = ImPlot::GetPlotPos();
        const auto size = ImPlot::GetPlotSize();
        handle_annotation_surface("Gyroscope", -1, origin.x, origin.y, size.x, size.y, hovered, "imu_gyro");
        ImPlot::EndPlot();
    }

    ImGui::Text("Detected IMU indicators: %zu", imu_analysis_.events.size());
    ImGui::TextDisabled("Possible airborne requires low vertical load for 80 ms; high load (>3.5 g) and rapid rotation (>220 deg/s) are indicators, not automatic crash claims.");
    if (ImGui::BeginTable("imu-events", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(-1, 150.0F))) {
        ImGui::TableSetupColumn("Indicator"); ImGui::TableSetupColumn("Lap"); ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Reading");
        ImGui::TableSetupColumn("Confidence"); ImGui::TableSetupColumn("Action"); ImGui::TableHeadersRow();
        for (std::size_t index = 0; index < imu_analysis_.events.size(); ++index) {
            const auto& event = imu_analysis_.events[index];
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(imu::event_name(event.type).data());
            const auto event_lap = std::find_if(session_->laps.begin(), session_->laps.end(), [&](const LapInfo& lap) {
                return event.peak_index >= lap.begin_index && event.peak_index <= lap.end_index;
            });
            ImGui::TableNextColumn();
            if (event_lap == session_->laps.end()) ImGui::TextUnformatted("-");
            else if (event_lap->race_lap > 0) ImGui::Text("R%d", event_lap->race_lap);
            else ImGui::Text("Raw %d", event_lap->raw_lap);
            ImGui::TableNextColumn(); ImGui::Text("%.3f s", static_cast<double>(event.time_us) / kSecond);
            ImGui::TableNextColumn(); ImGui::Text(event.type == imu::EventType::RapidRotation ? "%+.1f deg/s" : "%+.2f g", event.magnitude);
            ImGui::TableNextColumn(); ImGui::Text("%d%%", event.confidence);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Go")) {
                for (std::size_t lap_index = 0; lap_index < session_->laps.size(); ++lap_index) {
                    const auto& lap = session_->laps[lap_index];
                    if (event.peak_index >= lap.begin_index && event.peak_index <= lap.end_index) {
                        active_lap_index_ = static_cast<int>(lap_index);
                        view_mode_ = ViewMode::SingleLap;
                        plot_cache_mode_ = static_cast<ViewMode>(-1);
                        break;
                    }
                }
                cursor_us_ = event.time_us;
                playing_ = false;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
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
    if (view_mode_ == ViewMode::Compare && show_analysis_aligned_traces_) refresh_driver_analysis();
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
        if (view_mode_ == ViewMode::Compare) {
            ImGui::Checkbox("Separate lap maps", &separate_compare_maps_);
        }
        if (view_mode_ == ViewMode::Compare) {
            ImGui::Checkbox("Analysis-aligned comparison traces", &show_analysis_aligned_traces_);
            ImGui::TextDisabled("Display-only whole-lap correction; raw GPS remains stored unchanged.");
        }
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
    const auto display_translation_for = [&](const LapInfo* lap) -> const driver_analysis::LineTranslation* {
        if (!show_analysis_aligned_traces_ || view_mode_ != ViewMode::Compare || !lap) return nullptr;
        const auto slot = lap == compare_lap() ? std::size_t{0} : lap == compare_b_lap() ? std::size_t{1} : std::size_t{2};
        if (slot >= driver_analysis_.comparisons.size() || !driver_analysis_.comparisons[slot] ||
            !driver_analysis_.comparisons[slot]->confidence.line_metrics_enabled) return nullptr;
        return &driver_analysis_.comparisons[slot]->line_translation;
    };
    if (view_mode_ == ViewMode::Compare && show_analysis_aligned_traces_) {
        auto displayed = false;
        for (std::size_t slot = 0; slot < driver_analysis_.comparisons.size(); ++slot) {
            if (!driver_analysis_.comparisons[slot] || !driver_analysis_.comparisons[slot]->confidence.line_metrics_enabled) continue;
            if (!displayed) {
                ImGui::TextColored(ui::color(ui::ColorToken::Inspection), "DISPLAY: analysis-aligned comparison GPS");
                displayed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%c %.2f m", slot == 0 ? 'A' : 'B',
                driver_analysis_.comparisons[slot]->line_translation.magnitude_m);
        }
        if (displayed) {
            ImGui::SameLine();
            ImGui::TextDisabled("(raw recording preserved)");
        }
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
            const auto* translation = display_translation_for(lap);
            for (auto index = lap->begin_index; index <= lap->end_index; ++index) {
                const auto coordinate = translation
                    ? driver_analysis::apply_line_translation(session_->telemetry.latitude[index], session_->telemetry.longitude[index], *translation)
                    : driver_analysis::TranslatedCoordinate{session_->telemetry.latitude[index], session_->telemetry.longitude[index]};
                min_lat = std::min(min_lat, coordinate.latitude); max_lat = std::max(max_lat, coordinate.latitude);
                min_lon = std::min(min_lon, coordinate.longitude); max_lon = std::max(max_lon, coordinate.longitude);
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
    const auto convert_lap = [&](const Projection& projection, const LapInfo* lap, std::size_t index) {
        if (const auto* translation = display_translation_for(lap)) {
            const auto coordinate = driver_analysis::apply_line_translation(
                session_->telemetry.latitude[index], session_->telemetry.longitude[index], *translation);
            return project_coordinate(projection, coordinate.latitude, coordinate.longitude);
        }
        return convert(projection, index);
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
        auto previous = convert_lap(projection, lap, lap->begin_index);
        for (auto index = lap->begin_index + stride; index <= lap->end_index; index += stride) {
            const auto point = convert_lap(projection, lap, index);
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
            auto previous_zone = convert_lap(projection, lap, lap->begin_index);
            for (std::size_t offset = 1; offset < lap_distance.progress.size(); ++offset) {
                const auto progress = lap_distance.progress[offset];
                const auto point = convert_lap(projection, lap, lap->begin_index + offset);
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
                const auto point = convert_lap(projection, lap, lap->begin_index + std::min(offset, lap_distance.progress.size() - 1));
                draw->AddCircleFilled(point, marker_label[0] == 'A' ? 6.0F : 4.5F, IM_COL32(245, 248, 252, 255));
                draw->AddText(point + ImVec2(5.0F, -13.0F), IM_COL32(245, 248, 252, 255), marker_label);
            }
        }
        if (panel_hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const auto mouse = ImGui::GetIO().MousePos;
            auto nearest = lap->begin_index;
            auto nearest_distance = std::numeric_limits<float>::max();
            for (auto index = lap->begin_index; index <= lap->end_index; index += stride) {
                const auto point = convert_lap(projection, lap, index);
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
        draw->AddCircleFilled(convert_lap(projection, lap, playback_index), chosen_playback ? 5.5F : 4.5F, fixed_color);
        draw->AddCircle(convert_lap(projection, lap, playback_index), chosen_playback ? 7.5F : 6.0F,
            chosen_playback ? IM_COL32(25, 220, 245, 255) : IM_COL32(255, 190, 42, 230), 0, chosen_playback ? 2.3F : 1.5F);
        if (hover_cursor_us_) {
            const auto inspection_index = cursor_index_at(*hover_cursor_us_);
            const auto inspection_point = convert_lap(projection, lap, inspection_index);
            draw->AddCircle(inspection_point, 7.0F, IM_COL32(25, 220, 245, 255), 0, 2.0F);
            draw->AddCircleFilled(inspection_point, 2.5F, IM_COL32(25, 220, 245, 255));
        }
        const auto speed = metric_units_ ? session_->telemetry.speed_kmh[playback_index] : session_->telemetry.speed_kmh[playback_index] * 0.621371F;
        const auto* displayed_translation = display_translation_for(lap);
        draw->AddText(projection.panel_origin + ImVec2(8, label_y), fixed_color,
            (displayed_translation
                ? std::format("{}  {:.1f} {}  aligned {:.2f} m", label, speed, metric_units_ ? "km/h" : "mph",
                              displayed_translation->magnitude_m)
                : std::format("{}  {:.1f} {}", label, speed, metric_units_ ? "km/h" : "mph")).c_str());
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
            draw->AddCircle(convert_lap(projection, lap, nearest), 6.0F, IM_COL32(70, 230, 135, 255), 0, 2.0F);
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
    if (view_mode_ == ViewMode::Compare && separate_compare_maps_ &&
        compare_lap() &&
        size.x >= (compare_b_enabled_ ? 480.0F : 320.0F)) {
        const auto show_third_panel =
            compare_b_enabled_ && compare_b_lap();
        const auto panel_count = show_third_panel ? 3.0F : 2.0F;
        const auto gap = 8.0F;
        const auto panel_size = ImVec2(
            (size.x - gap * (panel_count - 1.0F)) / panel_count,
            size.y);
        const auto left_origin = origin;
        const auto middle_origin = origin + ImVec2(panel_size.x + gap, 0);
        const auto right_origin = origin + ImVec2((panel_size.x + gap) * 2.0F, 0);
        const auto shared_projection = make_projection(
            map_laps(active_lap(), compare_lap(),
                     show_third_panel ? compare_b_lap() : nullptr),
            left_origin, panel_size);
        auto primary_projection = shared_projection;
        auto compare_projection = shared_projection;
        auto compare_b_projection = shared_projection;
        compare_projection.panel_origin = middle_origin;
        compare_b_projection.panel_origin = right_origin;
        const auto left_contains = ImRect(left_origin, left_origin + panel_size).Contains(pointer);
        const auto middle_contains = ImRect(middle_origin, middle_origin + panel_size).Contains(pointer);
        const auto right_contains = show_third_panel &&
            ImRect(right_origin, right_origin + panel_size).Contains(pointer);
        const auto left_hovered = map_hovered && left_contains;
        const auto middle_hovered = map_hovered && middle_contains;
        const auto right_hovered = map_hovered && right_contains;
        update_background_from_input(primary_projection, left_contains && (map_hovered || map_active));
        update_background_from_input(compare_projection, middle_contains && (map_hovered || map_active));
        if (show_third_panel) {
            update_background_from_input(
                compare_b_projection,
                right_contains && (map_hovered || map_active));
        }
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
        if (show_third_panel) {
            draw->PushClipRect(
                right_origin, right_origin + panel_size, true);
            draw_panel(compare_b_projection);
            draw_average_track(compare_b_projection);
            draw_lap(compare_b_lap(), compare_b_projection,
                ui::color_u32(ui::ColorToken::CompareB), 2.8F,
                right_hovered, "Compare B", 8.0F);
            draw_start_finish(compare_b_projection);
            draw->PopClipRect();
        }
        handle_annotation_surface("Track Map - Reference", -1, left_origin.x, left_origin.y, panel_size.x, panel_size.y, left_hovered);
        handle_annotation_surface("Track Map - Compare A", -1, middle_origin.x, middle_origin.y, panel_size.x, panel_size.y, middle_hovered);
        if (show_third_panel) {
            handle_annotation_surface(
                "Track Map - Compare B", -1, right_origin.x,
                right_origin.y, panel_size.x, panel_size.y,
                right_hovered);
        }
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
    if (ImGui::Checkbox("Add Compare B (third trace)", &compare_b_enabled_)) {
        if (compare_b_enabled_ && !ensure_unique_complete_comparison_roles()) {
            compare_b_enabled_ = false;
            error_ = "Compare B needs a third different complete lap";
        } else {
            if (!compare_b_enabled_) crew_chief_after_slot_ = driver_analysis::ComparisonSlot::CompareA;
            error_.clear();
            invalidate_comparison();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reference plus Compare A is the normal comparison. Enable this only when a third lap helps.");
    }
    combo("REFERENCE", "##reference-lap", active_lap_index_, ImVec4(0.20F, 0.78F, 0.40F, 1.0F),
        [&](int value) { return session_->laps[static_cast<std::size_t>(value)].phase != LapPhase::Complete ||
            value == compare_lap_index_ || (compare_b_enabled_ && value == compare_b_lap_index_); }, [&](std::size_t index) {
            fastest_reference_ = false;
            cursor_us_ = session_->telemetry.time_us[session_->laps[index].begin_index];
        });
    combo("COMPARE A", "##compare-a-lap", compare_lap_index_, ImVec4(1.0F, 0.68F, 0.12F, 1.0F),
        [&](int value) { return session_->laps[static_cast<std::size_t>(value)].phase != LapPhase::Complete ||
            value == active_lap_index_ || (compare_b_enabled_ && value == compare_b_lap_index_); }, [](std::size_t) {});
    if (compare_b_enabled_) {
        combo("COMPARE B", "##compare-b-lap", compare_b_lap_index_, ImVec4(0.95F, 0.25F, 0.28F, 1.0F),
            [&](int value) { return session_->laps[static_cast<std::size_t>(value)].phase != LapPhase::Complete ||
                value == active_lap_index_ || value == compare_lap_index_; }, [](std::size_t) {});
    }
    combo("PLAYBACK LAP", "##playback-lap", playback_lap_index_, ImVec4(0.10F, 0.85F, 0.95F, 1.0F),
        [](int) { return false; }, [](std::size_t) {});
    const auto complete_role = [&](int value) { return value >= 0 && value < static_cast<int>(session_->laps.size()) &&
        session_->laps[static_cast<std::size_t>(value)].phase == LapPhase::Complete; };
    const auto valid_roles = active_lap_index_ != compare_lap_index_ &&
        complete_role(active_lap_index_) && complete_role(compare_lap_index_) &&
        (!compare_b_enabled_ || (active_lap_index_ != compare_b_lap_index_ &&
            compare_lap_index_ != compare_b_lap_index_ && complete_role(compare_b_lap_index_)));
    ImGui::BeginDisabled(!valid_roles);
    if (ImGui::Button("Compare selected laps")) { view_mode_ = ViewMode::Compare; invalidate_comparison(); }
    ImGui::EndDisabled();
    if (!valid_roles) {
        ImGui::SameLine();
        ImGui::TextDisabled(compare_b_enabled_
            ? "Reference, A and B need different complete laps"
            : "Reference and A need different complete laps");
    }

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
            if (compare_b_enabled_ && compare_b_lap_index_ == static_cast<int>(index)) {
                roles += roles.empty() ? "B" : "/B";
            }
            if (playback_lap_index_ == static_cast<int>(index)) roles += roles.empty() ? "P" : "/P";
            const auto label = roles.empty() ? lap_name : std::format("{}  {}", lap_name, roles);
            const auto reference_disabled = lap.phase != LapPhase::Complete ||
                compare_lap_index_ == static_cast<int>(index) ||
                (compare_b_enabled_ && compare_b_lap_index_ == static_cast<int>(index));
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
                    (compare_b_enabled_ && compare_b_lap_index_ == static_cast<int>(index)));
                if (ImGui::MenuItem("Set as Compare A")) { compare_lap_index_ = static_cast<int>(index); invalidate_comparison(); }
                ImGui::EndDisabled();
                if (compare_b_enabled_) {
                    ImGui::BeginDisabled(lap.phase != LapPhase::Complete || active_lap_index_ == static_cast<int>(index) ||
                        compare_lap_index_ == static_cast<int>(index));
                    if (ImGui::MenuItem("Set as Compare B")) {
                        compare_b_lap_index_ = static_cast<int>(index);
                        invalidate_comparison();
                    }
                    ImGui::EndDisabled();
                }
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
            lateral = std::max(lateral, static_cast<float>(std::abs(session_->telemetry.lateral_g[index] -
                (imu_analysis_.initial_stationary_zero_used ? imu_analysis_.acceleration_zero_g[1] : 0.0))));
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
            if (compare_b_enabled_) stats("Compare B", compare_b_lap(), ui::color(ui::ColorToken::CompareB));
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
            (sizeof(Timestamp) + sizeof(std::int64_t) + 2 * sizeof(double) + 9 * sizeof(float) + sizeof(std::uint8_t) + sizeof(std::int32_t));
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
    annotation.compare_b_enabled = compare_b_enabled_;
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
    annotation.compare_b_enabled = compare_b_enabled_;
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
    compare_b_enabled_ = annotation.compare_b_enabled;
    playback_lap_index_ = std::clamp(annotation.playback_lap_index, 0, static_cast<int>(session_->laps.size()) - 1);
    fastest_reference_ = false;
    if (view_mode_ == ViewMode::Compare && !ensure_unique_complete_comparison_roles()) {
        view_mode_ = ViewMode::SingleLap;
        error_ = compare_b_enabled_
            ? "Saved compare view needs three different complete laps when Compare B is enabled"
            : "Saved compare view needs two different complete laps";
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
        const auto tab_flags = [&](InsightsTab tab) {
            return insights_tab_request_pending_ && requested_insights_tab_ == tab
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        };
        if (ImGui::BeginTabItem("Insights", nullptr, tab_flags(InsightsTab::Insights))) {
            if (insights_tab_request_pending_ && requested_insights_tab_ == InsightsTab::Insights) {
                insights_tab_request_pending_ = false;
            }
            ImGui::TextDisabled("Deterministic formula set %s; raw GPS is never rewritten.", driver_analysis_.formula_version.c_str());
            if (ImGui::Button("Export analysis JSON...")) export_driver_analysis();
            for (std::size_t slot = 0; slot < driver_analysis_.comparisons.size(); ++slot) {
                if (!driver_analysis_.comparisons[slot]) continue;
                const auto& comparison = *driver_analysis_.comparisons[slot];
                ImGui::TextDisabled("Compare %c GPS correction %.2f m (residual %.2f m) | visible %s | line metrics %s",
                    slot == 0 ? 'A' : 'B', comparison.line_translation.magnitude_m,
                    comparison.line_translation.residual_rms_m,
                    show_analysis_aligned_traces_ && comparison.confidence.line_metrics_enabled ? "aligned" : "raw",
                    comparison.confidence.line_metrics_enabled ? "enabled" : "disabled");
            }
            if (driver_analysis_.insights.empty()) {
                ImGui::TextWrapped("No telemetry changes passed the enabled metric and minimum data-quality gates for these laps.");
            }
            for (std::size_t index = 0; index < driver_analysis_.insights.size(); ++index) {
                const auto& insight = driver_analysis_.insights[index];
                ImGui::PushID(static_cast<int>(index));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0F);
                ImGui::BeginChild("insight-card", ImVec2(-1.0F, 310.0F), ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_NoScrollbar);
                const auto severity_color = [&] {
                    switch (insight.outcome) {
                    case insight_evidence::Outcome::RetainedGain: return ImVec4(0.20F, 0.82F, 0.42F, 1.0F);
                    case insight_evidence::Outcome::TradeoffGain: return ImVec4(0.90F, 0.72F, 0.20F, 1.0F);
                    case insight_evidence::Outcome::Compensation: return ImVec4(0.25F, 0.68F, 0.94F, 1.0F);
                    case insight_evidence::Outcome::NetLoss: return ImVec4(0.96F, 0.25F, 0.27F, 1.0F);
                    case insight_evidence::Outcome::Inconclusive: return ImVec4(0.72F, 0.76F, 0.82F, 1.0F);
                    case insight_evidence::Outcome::DataLimited: return ImVec4(0.58F, 0.62F, 0.68F, 1.0F);
                    }
                    return ImVec4(0.72F, 0.76F, 0.82F, 1.0F);
                }();
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(severity_color.x, severity_color.y, severity_color.z, 0.23F));
                const auto role = insight.comparison == driver_analysis::ComparisonSlot::CompareA ? "A" : "B";
                const auto heading = std::format("{}##insight", insight.title);
                if (ImGui::Selectable(heading.c_str(), selected_insight_index_ == static_cast<int>(index))) {
                    selected_insight_index_ = static_cast<int>(index);
                    const auto corner = std::find_if(corner_zones_.begin(), corner_zones_.end(), [&](const auto& value) {
                        return value.id == insight.corner_id;
                    });
                    selected_corner_index_ = corner == corner_zones_.end() ? -1 : static_cast<int>(std::distance(corner_zones_.begin(), corner));
                    navigate_to_analysis_target(insight.navigation);
                }
                ImGui::PopStyleColor();
                ImGui::TextDisabled("%s | Compare %s", insight.corner_name.c_str(), role);
                ImGui::TextWrapped("%s", insight.detail.c_str());
                ImGui::TextColored(severity_color, "Result: %s", insight_outcome_label(insight.outcome));
                ImGui::Text("Repeatability: %s", insight_reliability_label(insight.reliability));
                ImGui::Text("Driver advice: %s", insight_recommendation_label(insight.recommendation));
                const auto retained_label = timing_position_label(insight.retained_effect_s);
                const auto local_label = timing_change_label(insight.local_effect_s);
                const auto prior_label = timing_change_label(insight.prior_phase_effect_s);
                ImGui::Text("At next decision: %s", retained_label.c_str());
                ImGui::TextWrapped("After this input: %s | before it: %s", local_label.c_str(), prior_label.c_str());
                ImGui::TextWrapped("Normal timing variation %.3f s | same result %zu/%zu laps | recording quality %d%%",
                    insight.time_noise_floor_s, insight.supporting_laps, insight.comparable_laps, insight.confidence);
                if (insight.downstream_payback_fraction) {
                    ImGui::TextDisabled("Downstream payback %.0f%%", *insight.downstream_payback_fraction * 100.0);
                }
                if (!insight.evidence_reasons.empty()) {
                    std::string reasons;
                    for (std::size_t reason_index = 0; reason_index < std::min<std::size_t>(3, insight.evidence_reasons.size()); ++reason_index) {
                        if (!reasons.empty()) reasons += " | ";
                        reasons += insight_reason_label(insight.evidence_reasons[reason_index]);
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextWrapped("Evidence: %s", reasons.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::TextDisabled("Motorsport metric: %s | trigger %.3f",
                    driver_analysis::metric_name(insight.metric).data(), insight.threshold);
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
                        else if (metric.positive && metric.triggered) ImGui::TextColored(ImVec4(0.2F, 0.82F, 0.42F, 1), "FAVORABLE");
                        else if (metric.triggered) ImGui::TextColored(ImVec4(1.0F, 0.52F, 0.16F, 1), "CHANGED");
                        else ImGui::TextColored(ImVec4(0.38F, 0.76F, 0.52F, 1), "WITHIN");
                        ImGui::TableNextColumn(); ImGui::Text("%d%%", metric.confidence);
                    }
                }
                ImGui::EndTable();
            }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Rules / Formula", nullptr, tab_flags(InsightsTab::Rules))) {
            if (insights_tab_request_pending_ && requested_insights_tab_ == InsightsTab::Rules) {
                insights_tab_request_pending_ = false;
            }
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
            if (ImGui::CollapsingHeader("Recommendation evidence gates", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("A favorable single-lap metric is only a candidate. A recommendation must retain time through the next driver decision, clear the timing-noise floor, and repeat on quality-screened matching laps.");
                auto& evidence = analysis_rules_.evidence;
                rules_changed |= ImGui::InputDouble("Minimum timing floor (s)", &evidence.minimum_time_floor_s, 0.005, 0.025, "%.3f");
                rules_changed |= ImGui::InputDouble("Sample-period multiplier", &evidence.sample_period_multiplier, 0.1, 0.5, "%.2f");
                rules_changed |= ImGui::InputDouble("Repeatability sigma multiplier", &evidence.repeatability_sigma_multiplier, 0.1, 0.5, "%.2f");
                auto payback_percent = evidence.maximum_payback_fraction * 100.0;
                if (ImGui::InputDouble("Maximum downstream payback (%)", &payback_percent, 1.0, 5.0, "%.0f")) {
                    evidence.maximum_payback_fraction = std::clamp(payback_percent / 100.0, 0.0, 1.0);
                    rules_changed = true;
                }
                rules_changed |= ImGui::InputInt("Minimum data confidence", &evidence.minimum_data_confidence, 1, 5);
                rules_changed |= ImGui::InputInt("Reliable data confidence", &evidence.reliable_data_confidence, 1, 5);
                const auto edit_count = [&](const char* label, std::size_t& value) {
                    auto temporary = static_cast<int>(value);
                    if (ImGui::InputInt(label, &temporary, 1, 2)) {
                        value = static_cast<std::size_t>(std::max(1, temporary));
                        rules_changed = true;
                    }
                };
                edit_count("Likely: comparable laps", evidence.likely_comparable_laps);
                edit_count("Likely: supporting laps", evidence.likely_supporting_laps);
                auto likely_rate_percent = evidence.likely_support_rate * 100.0;
                if (ImGui::InputDouble("Likely: support rate (%)", &likely_rate_percent, 1.0, 5.0, "%.0f")) {
                    evidence.likely_support_rate = std::clamp(likely_rate_percent / 100.0, 0.0, 1.0);
                    rules_changed = true;
                }
                edit_count("Reliable: comparable laps", evidence.reliable_comparable_laps);
                edit_count("Reliable: supporting laps", evidence.reliable_supporting_laps);
                auto reliable_rate_percent = evidence.reliable_support_rate * 100.0;
                if (ImGui::InputDouble("Reliable: support rate (%)", &reliable_rate_percent, 1.0, 5.0, "%.0f")) {
                    evidence.reliable_support_rate = std::clamp(reliable_rate_percent / 100.0, 0.0, 1.0);
                    rules_changed = true;
                }
                evidence.minimum_time_floor_s = std::clamp(evidence.minimum_time_floor_s, 0.001, 2.0);
                evidence.sample_period_multiplier = std::clamp(evidence.sample_period_multiplier, 0.01, 20.0);
                evidence.repeatability_sigma_multiplier = std::clamp(evidence.repeatability_sigma_multiplier, 0.0, 20.0);
                evidence.minimum_data_confidence = std::clamp(evidence.minimum_data_confidence, 0, 100);
                evidence.reliable_data_confidence = std::clamp(evidence.reliable_data_confidence, evidence.minimum_data_confidence, 100);
                evidence.likely_supporting_laps = std::min(evidence.likely_supporting_laps, evidence.likely_comparable_laps);
                evidence.reliable_comparable_laps = std::max(evidence.reliable_comparable_laps, evidence.likely_comparable_laps);
                evidence.reliable_supporting_laps = std::clamp(evidence.reliable_supporting_laps,
                    evidence.likely_supporting_laps, evidence.reliable_comparable_laps);
                evidence.reliable_support_rate = std::max(evidence.reliable_support_rate, evidence.likely_support_rate);
                ImGui::TextDisabled("Dynamic floor = max(minimum, sample multiplier x sample period, sigma multiplier x repeatability sigma).");
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
        if (ImGui::BeginTabItem("Dev Notes", nullptr, tab_flags(InsightsTab::DevNotes))) {
            if (insights_tab_request_pending_ && requested_insights_tab_ == InsightsTab::DevNotes) {
                insights_tab_request_pending_ = false;
            }
            draw_dev_notes();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void NativeApp::draw_crew_chief() {
    using namespace std::chrono_literals;
    if (crew_chief_busy_ && crew_chief_future_.valid() &&
        crew_chief_future_.wait_for(0ms) == std::future_status::ready) {
        auto response = crew_chief_future_.get();
        crew_chief_busy_ = false;
        if (response.ok) {
            crew_chief_report_ = std::move(response.report);
            crew_chief_error_.clear();
            crew_chief_history_.push_back({
                "user",
                std::format("Setup change: {}\nQuestion: {}",
                    crew_chief_setup_change_.data(), crew_chief_question_.data())
            });
            crew_chief_history_.push_back({"assistant", crew_chief_report_->summary});
            if (crew_chief_history_.size() > 20) {
                crew_chief_history_.erase(
                    crew_chief_history_.begin(),
                    crew_chief_history_.begin() + static_cast<std::ptrdiff_t>(crew_chief_history_.size() - 20));
            }
            status_ = std::format("Crew Chief answered with {}% confidence", crew_chief_report_->confidence);
        } else {
            crew_chief_error_ = std::move(response.error);
            status_ = "Crew Chief is unavailable; deterministic analysis remains active";
        }
    }

    ImGui::TextWrapped(
        "Advanced lap-only check for the run currently displayed in Session. "
        "Use the Race Day Crew Chief comparison above for setup changes between two recorded runs.");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30F, 0.78F, 0.95F, 1.0F));
    ImGui::TextWrapped(
        "The AI receives calculated evidence, not graph screenshots or your telemetry files. "
        "It must describe association separately from proof that the setting caused the result.");
    ImGui::PopStyleColor();
    nlohmann::json crew_prior_setup_results = nlohmann::json::array();
    std::size_t crew_prior_setup_result_count = 0;
    if (!race_day_.runs.empty()) {
        const auto current_index = std::clamp(
            race_day_current_run_, 0, static_cast<int>(race_day_.runs.size()) - 1);
        const auto& current_run = race_day_.runs[static_cast<std::size_t>(current_index)];
        const auto memory_query = std::string(crew_chief_question_.data()) + " " +
            std::string(crew_chief_setup_change_.data());
        const auto relevant = race_day::relevant_setup_knowledge(
            race_day_, memory_query, current_run);
        crew_prior_setup_result_count = relevant.size();
        crew_prior_setup_results = race_day::build_prior_setup_results(
            race_day_, relevant);
        ImGui::TextColored(
            ImVec4(0.30F, 0.78F, 0.95F, 1.0F),
            "Setup Knowledge: %zu matching saved result%s from the Race Day book",
            crew_prior_setup_result_count,
            crew_prior_setup_result_count == 1 ? "" : "s");
    } else {
        ImGui::TextDisabled(
            "Setup Knowledge: create or open a Race Day book to use saved real-world results.");
    }

    const auto* before = active_lap();
    const auto* after_a = compare_lap();
    const auto* after_b = compare_b_lap();
    const auto lap_name = [&](const LapInfo* lap) {
        if (!lap) return std::string("not loaded");
        return lap->race_lap > 0
            ? std::format("R{} (raw {}, {})", lap->race_lap, lap->raw_lap, lap_time(lap->duration_us))
            : std::format("raw {} ({}, {})", lap->raw_lap, phase_name(lap->phase), lap_time(lap->duration_us));
    };
    ImGui::SeparatorText("BEFORE / AFTER");
    ImGui::Text("Before (Reference): %s", lap_name(before).c_str());
    if (!compare_b_enabled_) crew_chief_after_slot_ = driver_analysis::ComparisonSlot::CompareA;
    auto after_slot = crew_chief_after_slot_ == driver_analysis::ComparisonSlot::CompareA ? 0 : 1;
    if (ImGui::RadioButton(std::format("After: Compare A - {}", lap_name(after_a)).c_str(), after_slot == 0)) {
        after_slot = 0;
        crew_chief_after_slot_ = driver_analysis::ComparisonSlot::CompareA;
        crew_chief_report_.reset();
    }
    if (compare_b_enabled_) {
        if (ImGui::RadioButton(std::format("After: Compare B - {}", lap_name(after_b)).c_str(), after_slot == 1)) {
            after_slot = 1;
            crew_chief_after_slot_ = driver_analysis::ComparisonSlot::CompareB;
            crew_chief_report_.reset();
        }
    }

    ImGui::SeparatorText("WHAT CHANGED");
    ImGui::TextWrapped(
        "Include the old and new value when you know it. Example: rear spring 2.6 to 2.8, "
        "front ride height +1 mm, fresh rear tires, or repaired bent steering link.");
    ImGui::InputTextMultiline(
        "Setup / condition change##crew-change",
        crew_chief_setup_change_.data(), crew_chief_setup_change_.size(), ImVec2(-1, 92.0F));
    ImGui::InputTextMultiline(
        "Question##crew-question",
        crew_chief_question_.data(), crew_chief_question_.size(), ImVec2(-1, 82.0F));

    if (ImGui::CollapsingHeader("Connection")) {
        ImGui::InputText("Crew Chief address", crew_chief_endpoint_.data(), crew_chief_endpoint_.size());
        ImGui::TextDisabled(
            "Default: private Tailscale service on the existing OpenClaw/Codex server. "
            "Optional bearer token: RACEBOX_CREW_CHIEF_TOKEN.");
        ImGui::Text("Bearer token: %s", crew_chief_bearer_token_.empty() ? "not required/configured" : "configured");
    }

    const auto* after = crew_chief_after_slot_ == driver_analysis::ComparisonSlot::CompareA ? after_a : after_b;
    const auto can_send = session_ && before && after && before->phase == LapPhase::Complete &&
        after->phase == LapPhase::Complete && crew_chief_setup_change_.front() != '\0' &&
        crew_chief_question_.front() != '\0' && crew_chief_endpoint_.front() != '\0' && !crew_chief_busy_;
    ImGui::BeginDisabled(!can_send);
    if (ImGui::Button(
            "Ask about selected laps",
            ImVec2(-1, 34.0F))) {
        refresh_driver_analysis(true);
        const auto evidence = crew_chief::build_evidence_packet(
            *session_, *before, *after, driver_analysis_, crew_chief_after_slot_, &imu_analysis_);
        const auto endpoint = std::string(crew_chief_endpoint_.data());
        const auto token = crew_chief_bearer_token_;
        const auto setup_change = std::string(crew_chief_setup_change_.data());
        const auto question = std::string(crew_chief_question_.data());
        const auto history = crew_chief_history_;
        const auto prior_setup_results = crew_prior_setup_results;
        crew_chief_report_.reset();
        crew_chief_error_.clear();
        crew_chief_busy_ = true;
        crew_chief_future_ = std::async(std::launch::async,
            [endpoint, token, setup_change, question, evidence, prior_setup_results, history] {
                return crew_chief::request_report(
                    endpoint, token, setup_change, question, evidence,
                    prior_setup_results, history);
            });
        status_ = "Crew Chief is reviewing the measured before/after evidence";
    }
    ImGui::EndDisabled();
    if (!can_send && !crew_chief_busy_) {
        if (!before || !after || before->phase != LapPhase::Complete || after->phase != LapPhase::Complete) {
            ImGui::TextDisabled("Choose complete Reference and Compare laps first.");
        } else if (crew_chief_setup_change_.front() == '\0') {
            ImGui::TextDisabled("Write what changed before asking the Crew Chief.");
        }
    }
    if (crew_chief_busy_) {
        ImGui::TextColored(ImVec4(0.90F, 0.72F, 0.20F, 1.0F),
            "Reviewing speed, G-load, yaw, steering, controls, corners, timing, quality, and repeatability...");
    }
    if (!crew_chief_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96F, 0.32F, 0.30F, 1.0F));
        ImGui::TextWrapped("Crew Chief connection: %s", crew_chief_error_.c_str());
        ImGui::PopStyleColor();
        ImGui::TextWrapped(
            "The local telemetry, graphs, and deterministic Insights still work normally while the private AI service is offline.");
    }

    if (crew_chief_report_) {
        const auto& report = *crew_chief_report_;
        const auto verdict_color =
            report.verdict == "supported" ? ImVec4(0.20F, 0.82F, 0.42F, 1.0F) :
            report.verdict == "mixed" ? ImVec4(0.90F, 0.72F, 0.20F, 1.0F) :
            ImVec4(0.72F, 0.76F, 0.82F, 1.0F);
        ImGui::SeparatorText("CREW CHIEF REPORT");
        ImGui::TextColored(verdict_color, "%s | %d%% confidence",
            report.verdict == "supported" ? "CHANGE SUPPORTED" :
            report.verdict == "mixed" ? "MIXED RESULT" :
            report.verdict == "not_supported" ? "CHANGE NOT SUPPORTED" :
            report.verdict == "data_limited" ? "MORE CLEAN LAPS NEEDED" :
            "NO CLEAR ANSWER",
            report.confidence);
        ImGui::TextWrapped("%s", report.summary.c_str());
        for (std::size_t index = 0; index < report.observations.size(); ++index) {
            const auto& observation = report.observations[index];
            ImGui::PushID(static_cast<int>(index));
            const auto flags = index == 0 ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
            if (ImGui::TreeNodeEx(observation.area.c_str(), flags)) {
                ImGui::TextWrapped("Measured change: %s", observation.change.c_str());
                ImGui::TextWrapped("What that means: %s", observation.meaning.c_str());
                if (!observation.evidence_ids.empty()) {
                    std::string ids;
                    for (const auto& id : observation.evidence_ids) {
                        if (!ids.empty()) ids += " | ";
                        ids += id;
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextWrapped("Evidence: %s", ids.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (!report.confounds.empty() && ImGui::CollapsingHeader("What could be misleading")) {
            for (const auto& confound : report.confounds) ImGui::BulletText("%s", confound.c_str());
        }
        ImGui::SeparatorText("NEXT TEST");
        ImGui::TextWrapped("%s", report.next_test.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Causality: %s", report.causality_note.c_str());
        ImGui::PopStyleColor();
        if (!report.model.empty()) {
            ImGui::TextDisabled(
                "Private lane: %s | %s | %s | saved results used: %zu",
                report.model.c_str(), report.route.c_str(),
                report.thinking.empty() ? "thinking not reported" : report.thinking.c_str(),
                report.prior_setup_result_ids.size());
        }
    }

    if (!crew_chief_history_.empty() && ImGui::CollapsingHeader("Conversation history")) {
        if (ImGui::Button("Clear conversation")) {
            crew_chief_history_.clear();
            crew_chief_report_.reset();
        }
        ImGui::BeginChild("crew-chief-history", ImVec2(-1, 190.0F), ImGuiChildFlags_Borders);
        for (const auto& turn : crew_chief_history_) {
            ImGui::TextColored(
                turn.role == "assistant" ? ImVec4(0.30F, 0.78F, 0.95F, 1.0F) : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                "%s", turn.role == "assistant" ? "Crew Chief" : "You");
            ImGui::TextWrapped("%s", turn.content.c_str());
            ImGui::Separator();
        }
        ImGui::EndChild();
    }
}

void NativeApp::new_race_day() {
    race_day_ = race_day::standard_day(race_day_include_q4_, race_day_triple_a_);
    race_day_.date = local_date_label();
    race_day_path_.clear();
    selected_race_day_run_ = 0;
    race_day_detail_run_seen_ = -1;
    race_day_previous_run_ = 0;
    race_day_current_run_ = std::min(1, static_cast<int>(race_day_.runs.size()) - 1);
    race_day_report_.reset();
    race_day_pending_knowledge_.reset();
    pending_race_day_relink_.reset();
    selected_setup_knowledge_record_ = -1;
    race_day_relevant_history_count_ = 0;
    displayed_race_day_run_id_.clear();
    pending_displayed_race_day_run_id_.clear();
    race_day_error_.clear();
    race_day_dirty_ = true;
    workspace_section_ = WorkspaceSection::RaceDay;
    status_ = "New race day created";
}

void NativeApp::open_race_day() {
    if (race_day_dirty_) {
        workspace_section_ = WorkspaceSection::RaceDay;
        race_day_error_ =
            "Save the current Race Day before opening another one. "
            "This prevents losing run notes or telemetry links.";
        return;
    }
    const auto path = open_race_day_file(window_);
    if (!path) return;
    race_day::Day loaded;
    std::string load_error;
    if (!race_day::load(*path, loaded, load_error)) {
        race_day_error_ = load_error;
        error_ = load_error;
        return;
    }
    race_day_ = std::move(loaded);
    race_day_path_ = *path;
    selected_race_day_run_ = 0;
    race_day_detail_run_seen_ = -1;
    race_day_previous_run_ = 0;
    race_day_current_run_ = std::min(1, static_cast<int>(race_day_.runs.size()) - 1);
    race_day_report_.reset();
    race_day_pending_knowledge_.reset();
    pending_race_day_relink_.reset();
    selected_setup_knowledge_record_ = race_day_.setup_knowledge.empty()
        ? -1 : static_cast<int>(race_day_.setup_knowledge.size()) - 1;
    race_day_relevant_history_count_ = 0;
    displayed_race_day_run_id_.clear();
    pending_displayed_race_day_run_id_.clear();
    race_day_error_.clear();
    race_day_dirty_ = false;
    workspace_section_ = WorkspaceSection::RaceDay;
    status_ = std::format("Opened race day with {} runs", race_day_.runs.size());
}

void NativeApp::save_race_day(bool choose_path) {
    if (race_day_.runs.empty()) return;
    auto destination = race_day_path_;
    if (choose_path || destination.empty()) {
        const auto selected = save_file(window_, L"RaceBox Race Day", L"rbxday", L"*.rbxday");
        if (!selected) return;
        destination = *selected;
    }
    std::string save_error;
    if (!race_day::save(race_day_, destination, save_error)) {
        race_day_error_ = save_error;
        error_ = save_error;
        return;
    }
    race_day_path_ = std::move(destination);
    race_day_dirty_ = false;
    race_day_error_.clear();
    status_ = std::format(
        "Race day saved: {}", path_utf8(race_day_path_.filename()));
}

void NativeApp::attach_race_day_telemetry(std::size_t run_index) {
    if (run_index >= race_day_.runs.size()) return;
    selected_race_day_run_ = static_cast<int>(run_index);
    start_session_import(selected_race_day_run_);
}

void NativeApp::open_race_day_run_in_viewer(
    std::size_t run_index) {
    if (run_index >= race_day_.runs.size() || loading_) return;
    const auto& run = race_day_.runs[run_index];
    const auto composition =
        race_day::validate_telemetry_source_composition(
            run, true);
    if (!composition.ok) {
        race_day_error_ = composition.error;
        error_ = std::format(
            "{} cannot be opened: {}",
            run.label, composition.error);
        return;
    }
    for (std::size_t source_index = 0;
         source_index < run.telemetry_files.size();
         ++source_index) {
        if (race_day::telemetry_source_state(
                run, source_index, true) !=
            race_day::TelemetrySourceState::Available) {
            race_day_error_ =
                "An attached telemetry file no longer matches the "
                "saved recording. Review or replace it in Race Day "
                "before loading this run.";
            error_ = race_day_error_;
            return;
        }
    }
    race_day_error_.clear();
    selected_race_day_run_ = static_cast<int>(run_index);
    workspace_section_ = WorkspaceSection::Session;
    status_ = std::format(
        "Loading {} from Race Day...", run.label);
    begin_load(run.telemetry_files, run.id);
}

void NativeApp::use_open_session_for_race_day_run(std::size_t run_index) {
    if (run_index >= race_day_.runs.size()) return;
    if (race_day_busy_) {
        session_import_error_ =
            "Wait for the current run comparison to finish before changing its data.";
        return;
    }
    if (!session_ || current_session_source_files_.empty()) {
        session_import_error_ =
            "The current viewer session has no reusable source files. "
            "Choose files for this run instead.";
        error_ = session_import_error_;
        return;
    }
    const auto& run = race_day_.runs[run_index];
    PendingSessionImport pending;
    pending.files = current_session_source_files_;
    pending.recorded_at_utc = recorded_time_iso8601(&*session_);
    pending.recorded_date = recorded_local_date(&*session_);
    pending.kind = run.kind;
    pending.kind_locked = run.kind == race_day::RunKind::Custom;
    pending.destination_run = static_cast<int>(run_index);
    pending.run_label = run.label;
    pending_session_import_ = std::move(pending);
    session_import_error_.clear();
    status_ = "Current telemetry is ready to classify and add to Race Day.";
}

void NativeApp::reset_session_import_destination() {
    if (!pending_session_import_) return;
    auto& pending = *pending_session_import_;
    pending.destination_run = -1;
    for (std::size_t index = 0; index < race_day_.runs.size(); ++index) {
        const auto& run = race_day_.runs[index];
        if (run.kind == pending.kind && run.telemetry_files.empty()) {
            pending.destination_run = static_cast<int>(index);
            pending.run_label = run.label;
            return;
        }
    }

    int next_ordinal = 1;
    for (const auto& run : race_day_.runs) {
        if (run.kind == pending.kind) {
            next_ordinal = std::max(next_ordinal, run.ordinal + 1);
        }
    }
    switch (pending.kind) {
        case race_day::RunKind::Practice:
            pending.run_label =
                "Practice " + std::to_string(next_ordinal);
            break;
        case race_day::RunKind::Qualifying:
            pending.run_label = "Q" + std::to_string(next_ordinal);
            break;
        case race_day::RunKind::Main:
            pending.run_label = "Race " + std::to_string(next_ordinal);
            break;
        case race_day::RunKind::Custom:
            pending.run_label = "Run " + std::to_string(next_ordinal);
            break;
    }
}

void NativeApp::commit_session_import() {
    if (!pending_session_import_ ||
        pending_session_import_->files.empty()) {
        return;
    }
    if (race_day_busy_) {
        session_import_error_ =
            "Wait for the current run comparison to finish before changing its data.";
        return;
    }

    const auto pending = *pending_session_import_;
    auto updated = race_day_;
    const auto had_recorded_run = std::any_of(
        updated.runs.begin(), updated.runs.end(),
        [](const auto& run) {
            return race_day::has_primary_telemetry(run);
        });

    std::size_t destination = updated.runs.size();
    if (pending.destination_run >= 0 &&
        pending.destination_run < static_cast<int>(updated.runs.size()) &&
        updated.runs[static_cast<std::size_t>(
            pending.destination_run)].kind == pending.kind) {
        destination = static_cast<std::size_t>(
            pending.destination_run);
    } else {
        switch (pending.kind) {
            case race_day::RunKind::Practice:
                race_day::add_practice(updated);
                break;
            case race_day::RunKind::Qualifying:
                race_day::add_qualifying(updated);
                break;
            case race_day::RunKind::Main:
                race_day::add_race(updated, pending.run_label);
                break;
            case race_day::RunKind::Custom:
                race_day::add_custom(updated, pending.run_label);
                break;
        }
        destination = updated.runs.size() - 1;
    }

    auto& run = updated.runs[destination];
    if (!pending.run_label.empty()) run.label = pending.run_label;
    run.recorded_at_utc =
        pending.recorded_at_utc == "Not recorded"
        ? std::string{}
        : pending.recorded_at_utc;
    const auto attached =
        race_day::attach_or_replace_telemetry_sources(
            run, pending.files);
    if (!attached.ok) {
        session_import_error_ = attached.error;
        return;
    }
    const auto invalidated =
        invalidate_setup_knowledge_for_run(updated, run.id);
    if (!had_recorded_run || updated.date.empty()) {
        updated.date = pending.recorded_date;
    }

    race_day_ = std::move(updated);
    selected_race_day_run_ = static_cast<int>(destination);
    displayed_race_day_run_id_ =
        race_day_.runs[destination].id;
    pending_displayed_race_day_run_id_.clear();
    race_day_current_run_ = selected_race_day_run_;
    if (race_day_previous_run_ == race_day_current_run_ &&
        race_day_.runs.size() > 1) {
        race_day_previous_run_ =
            std::max(0, race_day_current_run_ - 1);
        if (race_day_previous_run_ == race_day_current_run_) {
            race_day_previous_run_ = 1;
        }
    }
    race_day_detail_run_seen_ = -1;
    race_day_dirty_ = true;
    race_day_report_.reset();
    race_day_pending_knowledge_.reset();
    selected_setup_knowledge_record_ = -1;
    session_import_error_.clear();
    workspace_section_ = WorkspaceSection::RaceDay;
    status_ = std::format(
        "{} added to Race Day as {} on {} ({} new, {} replaced)",
        session_ ? session_->name : std::string{"Session"},
        race_day_.runs[destination].label,
        pending.recorded_date,
        attached.added, attached.replaced);
    if (invalidated > 0) {
        status_ += std::format(
            "; removed {} outdated comparison result(s)",
            invalidated);
    }
    pending_session_import_.reset();
    ImGui::CloseCurrentPopup();
}

void NativeApp::draw_session_import_popup() {
    if (!pending_session_import_) return;
    auto& pending = *pending_session_import_;
    if (pending.open_popup) {
        ImGui::OpenPopup("Add imported session to Race Day");
        pending.open_popup = false;
    }
    const auto* viewport = ImGui::GetMainViewport();
    const auto maximum_size = ImVec2(
        std::max(300.0F, viewport->WorkSize.x - 32.0F),
        std::max(240.0F, viewport->WorkSize.y - 32.0F));
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(std::min(390.0F, maximum_size.x), 0.0F),
        maximum_size);
    ImGui::SetNextWindowSize(
        ImVec2(
            std::min(
                620.0F * std::clamp(text_scale_, 1.0F, 1.5F),
                maximum_size.x),
            0.0F),
        ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(
            "Add imported session to Race Day", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const auto compact_import =
        ImGui::GetContentRegionAvail().x <
        430.0F * std::clamp(text_scale_, 1.0F, 1.5F);

    ImGui::TextColored(
        ui::color(ui::ColorToken::Playback),
        "SESSION DATA IS LOADED");
    ImGui::TextWrapped(
        "Tell Race Day what kind of run this was. The recording "
        "time is read from telemetry, this computer's local date is "
        "used for Race Day, and telemetry remains available in the "
        "Session workspace.");
    ImGui::Separator();
    ImGui::TextUnformatted("What session was this?");
    const auto select_kind =
        [&](const char* label, race_day::RunKind kind) {
            const auto selected = pending.kind == kind;
            if (ImGui::RadioButton(label, selected)) {
                pending.kind = kind;
                reset_session_import_destination();
            }
        };
    if (pending.kind_locked) {
        ImGui::TextColored(
            ui::color(ui::ColorToken::Playback),
            "Custom / test run");
        ImGui::TextDisabled(
            "This import will stay attached to the selected custom run.");
    } else {
        if (ImGui::BeginTable(
                "session-kind-options",
                compact_import ? 1 : 3,
                ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            select_kind("Practice", race_day::RunKind::Practice);
            ImGui::TableNextColumn();
            select_kind("Qualifying", race_day::RunKind::Qualifying);
            ImGui::TableNextColumn();
            select_kind("Race", race_day::RunKind::Main);
            ImGui::EndTable();
        }
    }

    ImGui::Spacing();
    const auto recorded_display =
        friendly_recorded_time_label(pending.recorded_at_utc);
    ImGui::Text("Recorded: %s", recorded_display.c_str());
    if (pending.recorded_at_utc == "Not recorded") {
        ImGui::TextColored(
            ui::color(ui::ColorToken::Warning),
            "This file has no absolute timestamp; Race Day will "
            "use today's date.");
    } else {
        ImGui::TextDisabled(
            "Race Day date will use %s.",
            pending.recorded_date.c_str());
    }

    std::string destination_label =
        pending.destination_run >= 0 &&
            pending.destination_run <
                static_cast<int>(race_day_.runs.size())
        ? race_day_.runs[static_cast<std::size_t>(
              pending.destination_run)].label
        : "Create a new run";
    if (ImGui::BeginCombo(
            "Race Day run", destination_label.c_str())) {
        for (std::size_t index = 0;
             index < race_day_.runs.size(); ++index) {
            const auto& run = race_day_.runs[index];
            if (run.kind != pending.kind) continue;
            const auto label = std::format(
                "{} {}##session-slot-{}",
                run.label,
                run.telemetry_files.empty()
                    ? "(empty)"
                    : "(replace matching data)",
                index);
            const auto selected =
                pending.destination_run ==
                static_cast<int>(index);
            if (ImGui::Selectable(label.c_str(), selected)) {
                pending.destination_run =
                    static_cast<int>(index);
                pending.run_label = run.label;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        const auto create_selected =
            pending.destination_run < 0;
        if (ImGui::Selectable(
                "Create a new run##session-slot-new",
                create_selected)) {
            pending.destination_run = -1;
            int next_ordinal = 1;
            for (const auto& run : race_day_.runs) {
                if (run.kind == pending.kind) {
                    next_ordinal = std::max(
                        next_ordinal, run.ordinal + 1);
                }
            }
            switch (pending.kind) {
                case race_day::RunKind::Practice:
                    pending.run_label =
                        "Practice " +
                        std::to_string(next_ordinal);
                    break;
                case race_day::RunKind::Qualifying:
                    pending.run_label =
                        "Q" + std::to_string(next_ordinal);
                    break;
                case race_day::RunKind::Main:
                    pending.run_label =
                        "Race " +
                        std::to_string(next_ordinal);
                    break;
                case race_day::RunKind::Custom:
                    pending.run_label =
                        "Run " +
                        std::to_string(next_ordinal);
                    break;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextUnformatted("Run name");
    ImGui::SetNextItemWidth(-1.0F);
    input_text_string("##session-run-name", pending.run_label);

    if (pending.destination_run >= 0 &&
        pending.destination_run <
            static_cast<int>(race_day_.runs.size()) &&
        !race_day_.runs[static_cast<std::size_t>(
             pending.destination_run)].telemetry_files.empty()) {
        ImGui::TextColored(
            ui::color(ui::ColorToken::Warning),
            "This run already has data. Matching source types "
            "will be replaced; other sources are kept.");
    }
    const auto has_recorded_run = std::any_of(
        race_day_.runs.begin(), race_day_.runs.end(),
        [](const auto& run) {
            return race_day::has_primary_telemetry(run);
        });
    if (has_recorded_run && !race_day_.date.empty() &&
        race_day_.date != pending.recorded_date) {
        ImGui::TextColored(
            ui::color(ui::ColorToken::Warning),
            "This recording is dated %s, while this Race Day is "
            "dated %s. The existing event date will be kept.",
            pending.recorded_date.c_str(),
            race_day_.date.c_str());
    }

    ImGui::SeparatorText("FILES IN THIS SESSION");
    for (const auto& path : pending.files) {
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextWrapped(
            "%s", path_utf8(path.filename()).c_str());
    }
    if (!session_import_error_.empty()) {
        ImGui::TextColored(
            ui::color(ui::ColorToken::Danger),
            "%s", session_import_error_.c_str());
    }
    ImGui::Separator();
    const auto add_action_width = std::max(
        180.0F,
        ImGui::CalcTextSize("ADD TO RACE DAY").x +
            ImGui::GetStyle().FramePadding.x * 2.0F + 12.0F);
    const auto view_action_width = std::max(
        160.0F,
        ImGui::CalcTextSize("View telemetry only").x +
            ImGui::GetStyle().FramePadding.x * 2.0F + 12.0F);
    const auto stack_import_actions =
        compact_import ||
        ImGui::GetContentRegionAvail().x <
            add_action_width + view_action_width +
                ImGui::GetStyle().ItemSpacing.x;
    ImGui::BeginDisabled(
        pending.files.empty() || pending.run_label.empty());
    if (ImGui::Button(
            "ADD TO RACE DAY",
            ImVec2(
                stack_import_actions ? -1.0F : add_action_width,
                38.0F))) {
        commit_session_import();
        ImGui::EndDisabled();
        if (!pending_session_import_) {
            ImGui::EndPopup();
            return;
        }
    } else {
        ImGui::EndDisabled();
    }
    if (!stack_import_actions) ImGui::SameLine();
    if (ImGui::Button(
            "View telemetry only",
            ImVec2(
                stack_import_actions ? -1.0F : view_action_width,
                38.0F))) {
        pending_session_import_.reset();
        session_import_error_.clear();
        workspace_section_ = WorkspaceSection::Session;
        status_ =
            "Session kept in the viewer without adding it to Race Day.";
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::EndPopup();
}

void NativeApp::draw_import_discovery_popups() {
    if (discovered_files_popup_open_) {
        ImGui::OpenPopup("Import from saved folder");
        discovered_files_popup_open_ = false;
    }
    ImGui::SetNextWindowSize(
        ImVec2(860.0F, 620.0F), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(
            "Import from saved folder", nullptr,
            ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped(
            "Choose the files that belong to one recording. The newest "
            "primary file is selected as a starting point, but optional "
            "RaceBox/VBO/Sanwa pairing is never guessed.");
        ImGui::TextDisabled(
            "Folder: %s",
            path_utf8(telemetry_watch_folder_).c_str());
        if (ImGui::Button("Change folder...")) {
            const auto selected = choose_telemetry_folder(
                window_, telemetry_watch_folder_);
            if (selected) {
                telemetry_watch_folder_ = *selected;
                ImGui::CloseCurrentPopup();
                start_folder_import(folder_import_preferred_run_);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Scan again")) {
            ImGui::CloseCurrentPopup();
            start_folder_import(folder_import_preferred_run_);
        }

        ImGui::Separator();
        if (discovered_files_.empty()) {
            ImGui::TextDisabled(
                "No supported telemetry files were found. RaceBox CSV "
                "files must contain Latitude, Longitude, and Speed; Sanwa "
                "CSV files must contain REC TIME, ST(%%), and TH(%%).");
        } else {
            ImGui::BeginChild(
                "folder-discovery-files", ImVec2(0.0F, -80.0F),
                ImGuiChildFlags_Borders);
            ImGuiListClipper clipper;
            clipper.Begin(
                static_cast<int>(discovered_files_.size()),
                58.0F);
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart;
                     row < clipper.DisplayEnd; ++row) {
                    const auto index =
                        static_cast<std::size_t>(row);
                    const auto& file = discovered_files_[index];
                    ImGui::PushID(row);
                    bool selected =
                        discovered_file_selected_[index];
                    const auto label = std::format(
                        "{} | {}",
                        race_day::telemetry_source_kind_name(
                            file.kind),
                        path_utf8(file.path.filename()));
                    if (ImGui::Checkbox(
                            label.c_str(), &selected)) {
                        if (selected) {
                            if (file.kind ==
                                race_day::TelemetrySourceKind::
                                    NativeArchive) {
                                std::fill(
                                    discovered_file_selected_.begin(),
                                    discovered_file_selected_.end(),
                                    false);
                            } else {
                                for (std::size_t other = 0;
                                     other <
                                         discovered_files_.size();
                                     ++other) {
                                    if (other == index) continue;
                                    const auto other_kind =
                                        discovered_files_[other].kind;
                                    const auto duplicate =
                                        other_kind == file.kind;
                                    const auto archive =
                                        other_kind ==
                                        race_day::
                                            TelemetrySourceKind::
                                                NativeArchive;
                                    const auto gpx_conflict =
                                        (file.kind ==
                                             race_day::
                                                 TelemetrySourceKind::
                                                     Gpx &&
                                         (other_kind ==
                                              race_day::
                                                  TelemetrySourceKind::
                                                      Vbo ||
                                          other_kind ==
                                              race_day::
                                                  TelemetrySourceKind::
                                                      RaceBoxCsv)) ||
                                        (other_kind ==
                                             race_day::
                                                 TelemetrySourceKind::
                                                     Gpx &&
                                         (file.kind ==
                                              race_day::
                                                  TelemetrySourceKind::
                                                      Vbo ||
                                          file.kind ==
                                              race_day::
                                                  TelemetrySourceKind::
                                                      RaceBoxCsv));
                                    if (duplicate || archive ||
                                        gpx_conflict) {
                                        discovered_file_selected_[
                                            other] = false;
                                    }
                                }
                            }
                        }
                        discovered_file_selected_[index] =
                            selected;
                    }
                    ImGui::TextDisabled(
                        "%s | %.1f MB | %s",
                        discovered_file_time_label(
                            file.modified_at).c_str(),
                        static_cast<double>(file.size_bytes) /
                            (1024.0 * 1024.0),
                        path_utf8(
                            file.path.parent_path()).c_str());
                    ImGui::Separator();
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
        }

        std::vector<std::filesystem::path> selected_files;
        for (std::size_t index = 0;
             index < discovered_files_.size(); ++index) {
            if (discovered_file_selected_[index]) {
                selected_files.push_back(
                    discovered_files_[index].path);
            }
        }
        ImGui::BeginDisabled(selected_files.empty());
        if (ImGui::Button(
                "IMPORT SELECTED FILES",
                ImVec2(250.0F, 42.0F))) {
            start_session_import_files(
                selected_files,
                folder_import_preferred_run_);
            if (session_import_error_.empty()) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0F, 42.0F))) {
            ImGui::CloseCurrentPopup();
        }
        if (!session_import_error_.empty()) {
            ImGui::TextColored(
                ui::color(ui::ColorToken::Warning),
                "%s", session_import_error_.c_str());
        }
        ImGui::EndPopup();
    }

    if (detected_usb_popup_open_) {
        ImGui::OpenPopup("Sanwa telemetry detected on USB");
        detected_usb_popup_open_ = false;
    }
    if (ImGui::BeginPopupModal(
            "Sanwa telemetry detected on USB", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto available = detected_usb_sanwa_ &&
            std::filesystem::exists(
                detected_usb_sanwa_->path);
        ImGui::TextWrapped(
            "A removable drive contains a Sanwa CSV. Nothing has been "
            "attached automatically, so an older or unrelated run cannot "
            "be mixed in by mistake.");
        if (detected_usb_sanwa_) {
            ImGui::Separator();
            ImGui::Text(
                "%s",
                path_utf8(
                    detected_usb_sanwa_->path.filename()).c_str());
            ImGui::TextDisabled(
                "%s | %.1f MB",
                discovered_file_time_label(
                    detected_usb_sanwa_->modified_at).c_str(),
                static_cast<double>(
                    detected_usb_sanwa_->size_bytes) /
                    (1024.0 * 1024.0));
            ImGui::TextWrapped(
                "%s",
                path_utf8(
                    detected_usb_sanwa_->path.parent_path()).c_str());
        }
        if (!available) {
            ImGui::TextColored(
                ui::color(ui::ColorToken::Warning),
                "The removable drive is no longer available.");
        }

        const auto valid_run =
            !race_day_.runs.empty() &&
            selected_race_day_run_ >= 0 &&
            selected_race_day_run_ <
                static_cast<int>(race_day_.runs.size());
        const auto run_has_primary =
            valid_run &&
            race_day::has_primary_telemetry(
                race_day_.runs[
                    static_cast<std::size_t>(
                        selected_race_day_run_)]);
        ImGui::BeginDisabled(
            !available || !run_has_primary);
        const auto run_label = valid_run
            ? race_day_.runs[
                  static_cast<std::size_t>(
                      selected_race_day_run_)].label
            : std::string("selected run");
        if (ImGui::Button(
                std::format(
                    "ADD SANWA TO {}",
                    run_label).c_str(),
                ImVec2(320.0F, 38.0F))) {
            start_session_import_files(
                {detected_usb_sanwa_->path},
                selected_race_day_run_);
            if (session_import_error_.empty()) {
                detected_usb_sanwa_.reset();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndDisabled();
        if (!run_has_primary) {
            ImGui::TextDisabled(
                "Select a Race Day run that already has RaceBox/VBO "
                "telemetry, or import matching files together.");
        }

        ImGui::BeginDisabled(!available);
        if (ImGui::Button(
                "IMPORT WITH MATCHING FILES...",
                ImVec2(320.0F, 38.0F))) {
            auto files = open_telemetry_files(
                window_, telemetry_watch_folder_);
            const auto has_sanwa = std::any_of(
                files.begin(), files.end(),
                [](const auto& path) {
                    return race_day::telemetry_source_kind(
                        path) ==
                        race_day::TelemetrySourceKind::SanwaCsv;
                });
            if (!files.empty() && !has_sanwa) {
                files.push_back(
                    detected_usb_sanwa_->path);
            }
            if (!files.empty()) {
                start_session_import_files(
                    files,
                    valid_run
                        ? selected_race_day_run_ : -1);
                if (session_import_error_.empty()) {
                    detected_usb_sanwa_.reset();
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndDisabled();

        if (detected_usb_sanwa_ && ImGui::Button(
                "Use this folder for imports")) {
            telemetry_watch_folder_ =
                detected_usb_sanwa_->path.parent_path();
            status_ =
                "Sanwa USB folder saved as the telemetry import folder.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Ignore")) {
            detected_usb_sanwa_.reset();
            ImGui::CloseCurrentPopup();
        }
        if (!session_import_error_.empty()) {
            ImGui::TextColored(
                ui::color(ui::ColorToken::Warning),
                "%s", session_import_error_.c_str());
        }
        ImGui::EndPopup();
    }
}

void NativeApp::begin_race_day_source_relink(
    std::size_t source_index) {
    if (race_day_.runs.empty()) return;
    if (race_day_busy_) {
        race_day_error_ =
            "Wait for the current run comparison to finish before repairing its data.";
        return;
    }
    const auto run_index = static_cast<std::size_t>(std::clamp(
        selected_race_day_run_, 0,
        static_cast<int>(race_day_.runs.size()) - 1));
    auto& run = race_day_.runs[run_index];
    if (source_index >= run.telemetry_files.size()) return;

    race_day::refresh_telemetry_source_identities(run);
    const auto candidates = open_telemetry_files(window_);
    if (candidates.empty()) return;

    race_day::Run candidate_run;
    candidate_run.telemetry_files = candidates;
    race_day::refresh_telemetry_source_identities(candidate_run);

    std::vector<source_identity::Candidate> matcher_candidates;
    matcher_candidates.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        matcher_candidates.push_back({
            std::to_string(index),
            candidate_run.telemetry_source_identities[index],
        });
    }

    PendingRaceDayRelink pending;
    pending.run_index = run_index;
    pending.source_index = source_index;
    pending.candidate_paths = candidates;
    pending.candidate_identities =
        candidate_run.telemetry_source_identities;
    pending.result = source_identity::match_source(
        run.telemetry_source_identities[source_index],
        matcher_candidates);

    const auto candidate_index_for =
        [&](const std::optional<std::string>& id)
            -> std::optional<std::size_t> {
        if (!id) return std::nullopt;
        for (std::size_t index = 0;
             index < matcher_candidates.size(); ++index) {
            if (matcher_candidates[index].id == *id) return index;
        }
        return std::nullopt;
    };
    if (pending.result.status == source_identity::MatchStatus::Exact &&
        !pending.result.requires_user_confirmation) {
        const auto match =
            candidate_index_for(pending.result.recommended_candidate_id);
        if (match) {
            std::string replacement_error;
            if (!replace_telemetry_source_slot(
                    run, source_index,
                    pending.candidate_paths[*match],
                    pending.candidate_identities[*match],
                    replacement_error)) {
                race_day_error_ = std::move(replacement_error);
                return;
            }
            const auto invalidated =
                invalidate_setup_knowledge_for_run(
                    race_day_, run.id);
            race_day_dirty_ = true;
            race_day_report_.reset();
            race_day_pending_knowledge_.reset();
            selected_setup_knowledge_record_ = -1;
            race_day_error_.clear();
            status_ = std::format(
                "Relinked {} using its exact source fingerprint{}",
                path_utf8(run.telemetry_files[source_index].filename()),
                invalidated == 0
                    ? ""
                    : std::format(
                          "; removed {} outdated comparison result(s)",
                          invalidated));
            return;
        }
    }

    pending_race_day_relink_ = std::move(pending);
}

void NativeApp::draw_race_day_source_relink_popup() {
    if (!pending_race_day_relink_) return;
    if (pending_race_day_relink_->open_popup) {
        ImGui::OpenPopup("Confirm telemetry source");
        pending_race_day_relink_->open_popup = false;
    }
    if (!ImGui::BeginPopupModal(
            "Confirm telemetry source", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    auto& pending = *pending_race_day_relink_;
    const auto valid_source =
        pending.run_index < race_day_.runs.size() &&
        pending.source_index <
            race_day_.runs[pending.run_index].telemetry_files.size();
    if (!valid_source) {
        ImGui::TextWrapped(
            "The run changed before the source could be repaired.");
        if (ImGui::Button("Close")) {
            pending_race_day_relink_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    auto& run = race_day_.runs[pending.run_index];
    const auto& expected =
        run.telemetry_source_identities[pending.source_index];
    ImGui::TextWrapped(
        "The original file is missing: %s",
        expected.canonical_filename.empty()
            ? "unknown telemetry source"
            : expected.canonical_filename.c_str());
    switch (pending.result.status) {
        case source_identity::MatchStatus::Probable:
            ImGui::TextColored(
                ui::color(ui::ColorToken::Warning),
                "A likely match was found. Confirm it before replacing the saved path.");
            break;
        case source_identity::MatchStatus::Ambiguous:
            ImGui::TextColored(
                ui::color(ui::ColorToken::Warning),
                "More than one file could match. Choose the recording you recognize.");
            break;
        case source_identity::MatchStatus::Missing:
            ImGui::TextColored(
                ui::color(ui::ColorToken::Warning),
                "No file met the automatic match threshold. Choose manually only if you are sure.");
            break;
        case source_identity::MatchStatus::Exact:
            ImGui::TextUnformatted(
                "Confirm the selected exact match.");
            break;
    }
    ImGui::Separator();

    const auto choose = [&](
        std::string_view candidate_id,
        bool fingerprint_match) {
        if (!fingerprint_match) {
            race_day_error_ =
                "This file does not match the saved recording fingerprint. "
                "Use Add / replace run data if it is a different recording.";
            return;
        }
        for (std::size_t index = 0;
             index < pending.candidate_paths.size(); ++index) {
            if (candidate_id != std::to_string(index)) continue;
            std::string replacement_error;
            if (!replace_telemetry_source_slot(
                    run, pending.source_index,
                    pending.candidate_paths[index],
                    pending.candidate_identities[index],
                    replacement_error)) {
                race_day_error_ = std::move(replacement_error);
                return;
            }
            const auto invalidated =
                invalidate_setup_knowledge_for_run(
                    race_day_, run.id);
            race_day_dirty_ = true;
            race_day_report_.reset();
            race_day_pending_knowledge_.reset();
            selected_setup_knowledge_record_ = -1;
            race_day_error_.clear();
            status_ = std::format(
                "Relinked {} after fingerprint confirmation{}",
                path_utf8(pending.candidate_paths[index].filename()),
                invalidated == 0
                    ? ""
                    : std::format(
                          "; removed {} outdated comparison result(s)",
                          invalidated));
            pending_race_day_relink_.reset();
            ImGui::CloseCurrentPopup();
            return;
        }
    };

    for (const auto& suggestion : pending.result.suggestions) {
        std::size_t candidate_index =
            pending.candidate_paths.size();
        for (std::size_t index = 0;
             index < pending.candidate_paths.size(); ++index) {
            if (suggestion.candidate_id == std::to_string(index)) {
                candidate_index = index;
                break;
            }
        }
        if (candidate_index >= pending.candidate_paths.size()) continue;
        ImGui::PushID(static_cast<int>(candidate_index));
        const auto& path = pending.candidate_paths[candidate_index];
        const auto candidate_name = path_utf8(path.filename());
        ImGui::TextUnformatted(candidate_name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled(
            "score %d | name %s | size %s | time %s | fingerprint %s",
            suggestion.score,
            suggestion.filename_match ? "match" : "-",
            suggestion.size_match ? "match" : "-",
            suggestion.modified_time_match ? "match" : "-",
            suggestion.fingerprint_match ? "match" : "-");
        ImGui::SameLine();
        ImGui::BeginDisabled(!suggestion.fingerprint_match);
        const auto use_candidate =
            ImGui::Button("Use this file");
        ImGui::EndDisabled();
        if (use_candidate) {
            const auto id = suggestion.candidate_id;
            choose(id, suggestion.fingerprint_match);
            ImGui::PopID();
            ImGui::EndPopup();
            return;
        }
        if (!suggestion.fingerprint_match) {
            ImGui::TextDisabled(
                "Different recording: use Add / replace run data.");
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    if (!race_day_error_.empty()) {
        ImGui::TextColored(
            ui::color(ui::ColorToken::Danger),
            "%s", race_day_error_.c_str());
    }
    if (ImGui::Button("Cancel")) {
        pending_race_day_relink_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void NativeApp::analyze_race_day_runs() {
    if (race_day_busy_ || race_day_.runs.size() < 2) return;
    const auto count = static_cast<int>(race_day_.runs.size());
    race_day_previous_run_ = std::clamp(race_day_previous_run_, 0, count - 1);
    race_day_current_run_ = std::clamp(race_day_current_run_, 0, count - 1);
    if (race_day_previous_run_ == race_day_current_run_) {
        race_day_error_ = "Previous and current must be two different runs";
        return;
    }
    const auto previous = race_day_.runs[static_cast<std::size_t>(race_day_previous_run_)];
    const auto current = race_day_.runs[static_cast<std::size_t>(race_day_current_run_)];
    if (!race_day::has_primary_telemetry(previous) ||
        !race_day::has_primary_telemetry(current)) {
        race_day_error_ =
            "Attach RaceBox, VBO, GPX, or session archive data "
            "to both runs first";
        return;
    }

    const auto previous_context = race_day_run_context(race_day_, previous);
    const auto current_context = race_day_run_context(race_day_, current);
    const auto relevant_indices = race_day::relevant_setup_knowledge(
        race_day_, race_day_question_, current);
    const auto prior_setup_results = race_day::build_prior_setup_results(
        race_day_, relevant_indices);
    const auto endpoint = std::string(crew_chief_endpoint_.data());
    const auto token = crew_chief_bearer_token_;
    const auto question = race_day_question_;
    race_day::SetupKnowledgeRecord pending;
    pending.id = setup_knowledge_id(
        previous.id, current.id, question, current.setup_changes, current.post_run_notes);
    pending.created_at_utc = utc_timestamp();
    pending.event_name = race_day_.event_name;
    pending.track_name = race_day_.track_name;
    pending.previous_run_id = previous.id;
    pending.previous_run_label = previous.label;
    pending.current_run_id = current.id;
    pending.current_run_label = current.label;
    pending.handling_question = question;
    pending.setup_change = current.setup_changes;
    pending.driver_result = current.post_run_notes;
    pending.previous_conditions = previous.conditions;
    pending.current_conditions = current.conditions;
    race_day_pending_knowledge_ = std::move(pending);
    race_day_relevant_history_count_ = relevant_indices.size();
    race_day_report_.reset();
    race_day_error_.clear();
    race_day_busy_ = true;
    race_day_future_ = std::async(std::launch::async,
        [previous, current, previous_context, current_context, prior_setup_results,
         endpoint, token, question] {
            try {
                const auto previous_loaded = race_day::load_run_telemetry(previous);
                const auto current_loaded = race_day::load_run_telemetry(current);
                const auto deterministic_summary = race_day::analyze_setup_change(
                    previous_loaded.session, previous_loaded.imu_analysis,
                    current_loaded.session, current_loaded.imu_analysis);
                const auto previous_csv = race_day::build_analytics_csv(
                    previous_loaded.session, previous_loaded.imu_analysis);
                const auto current_csv = race_day::build_analytics_csv(
                    current_loaded.session, current_loaded.imu_analysis);
                auto response = crew_chief::request_race_day_report(
                    endpoint, token, previous_context, previous_csv,
                    current_context, current_csv, question, prior_setup_results);
                if (response.ok) fill_missing_setup_evidence(response.report, deterministic_summary);
                return response;
            } catch (const std::exception& exception) {
                crew_chief::Response response;
                response.error = exception.what();
                return response;
            }
        });
    status_ = std::format("Analyzing {} against {}", current.label, previous.label);
}

void NativeApp::draw_reports() {
    const auto* viewport = ImGui::GetMainViewport();
    const auto header_height = application_header_height(text_scale_);
    const auto origin = viewport->WorkPos + ImVec2(0.0F, header_height);
    const auto size = ImVec2(viewport->WorkSize.x,
        std::max(1.0F, viewport->WorkSize.y - header_height));
    ImGui::SetNextWindowPos(origin, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Reports Workspace", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ui::ColorToken::Playback));
    ImGui::TextUnformatted("SESSION REPORT");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("Measured summary and export tools. No AI interpretation is required.");

    if (!session_) {
        ImGui::Separator();
        ImGui::TextWrapped(
            "Import a session to build a report from laps, sectors, and deterministic insights. "
            "After loading, choose whether it was Practice, Qualifying, or Race.");
        if (ImGui::Button("Import session...", ImVec2(190.0F, 38.0F))) {
            start_session_import();
        }
        ImGui::End();
        return;
    }

    refresh_driver_analysis();
    if (ImGui::Button("Export analysis JSON...")) export_driver_analysis();
    ImGui::SameLine();
    if (ImGui::Button("Export active lap CSV...")) export_active_lap();
    ImGui::SameLine();
    if (ImGui::Button("Save session archive...")) save_session();
    ImGui::SameLine();
    if (ImGui::Button("Save active lap archive...")) save_active_lap();

    const LapInfo* best_lap = nullptr;
    std::size_t complete_laps = 0;
    for (const auto& lap : session_->laps) {
        if (lap.phase != LapPhase::Complete) continue;
        ++complete_laps;
        if (!best_lap || lap.duration_us < best_lap->duration_us) best_lap = &lap;
    }
    const auto session_duration = session_->telemetry.empty()
        ? Timestamp{}
        : session_->telemetry.time_us.back() - session_->telemetry.time_us.front();

    ImGui::SeparatorText("SESSION SUMMARY");
    if (ImGui::BeginTable("report-summary", 4,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
        const auto summary_cell = [](const char* label, const std::string& value) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", label);
            ImGui::TextUnformatted(value.c_str());
        };
        ImGui::TableNextRow();
        summary_cell("SESSION", session_->name.empty() ? std::string{"Unnamed session"} : session_->name);
        summary_cell(
            "RECORDED",
            friendly_recorded_time_label(
                recorded_time_iso8601(&*session_)));
        summary_cell("TELEMETRY", std::format("{} samples | {}", session_->telemetry.size(), lap_time(session_duration)));
        summary_cell("COMPLETE LAPS", std::format("{} of {}", complete_laps, session_->laps.size()));
        ImGui::TableNextRow();
        summary_cell("BEST COMPLETE LAP", best_lap ? lap_time(best_lap->duration_us) : std::string{"Not available"});
        summary_cell("THEORETICAL BEST", session_->theoretical_best.duration_us > 0
            ? lap_time(session_->theoretical_best.duration_us) : std::string{"Not available"});
        summary_cell("SECTORS", std::format("{} calculated", session_->theoretical_best.sectors.size()));
        const auto embedded_archive_telemetry =
            session_->vbo_path.empty() && session_->csv_path.empty() &&
            !session_->telemetry.empty();
        summary_cell("SOURCES", embedded_archive_telemetry
            ? std::format("Archive telemetry OK | Sanwa {}",
                session_->radio.empty() ? "-" : "OK")
            : std::format("VBO {} | RaceBox {} | Sanwa {}",
                session_->vbo_path.empty() ? "-" : "OK",
                session_->csv_path.empty() ? "-" : "OK",
                session_->radio.empty() ? "-" : "OK"));
        ImGui::EndTable();
    }

    const auto role_label = [](const LapInfo* lap) {
        if (!lap) return std::string{"Off"};
        return lap->race_lap > 0 ? std::format("R{}", lap->race_lap) : std::format("L{}", lap->raw_lap);
    };
    ImGui::SeparatorText("CURRENT COMPARISON");
    ImGui::TextColored(ui::color(ui::ColorToken::Reference), "Reference %s", role_label(active_lap()).c_str());
    ImGui::SameLine();
    ImGui::TextColored(ui::color(ui::ColorToken::CompareA), "Compare A %s", role_label(compare_lap()).c_str());
    ImGui::SameLine();
    ImGui::TextColored(compare_b_enabled_ ? ui::color(ui::ColorToken::CompareB) : ui::color(ui::ColorToken::TextMuted),
        "Compare B %s", role_label(compare_b_lap()).c_str());
    ImGui::SameLine();
    ImGui::TextColored(ui::color(ui::ColorToken::Playback), "Playback %s", role_label(playback_lap()).c_str());

    const auto available = ImGui::GetContentRegionAvail();
    const auto lap_width = std::max(420.0F, available.x * 0.56F);
    ImGui::BeginChild("report-laps", ImVec2(lap_width, -1.0F), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("LAP TABLE");
    if (ImGui::BeginTable("report-lap-table", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Lap");
        ImGui::TableSetupColumn("Phase");
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Vs best");
        ImGui::TableSetupColumn("Role");
        ImGui::TableHeadersRow();
        for (std::size_t index = 0; index < session_->laps.size(); ++index) {
            const auto& lap = session_->laps[index];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s%d", lap.race_lap > 0 ? "R" : "L",
                lap.race_lap > 0 ? lap.race_lap : lap.raw_lap);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(phase_name(lap.phase));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(lap_time(lap.duration_us).c_str());
            ImGui::TableNextColumn();
            if (best_lap && lap.phase == LapPhase::Complete) {
                ImGui::Text("%+.3f s",
                    static_cast<double>(lap.duration_us - best_lap->duration_us) / static_cast<double>(kSecond));
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            std::string roles;
            if (active_lap_index_ == static_cast<int>(index)) roles = "Reference";
            if (compare_lap_index_ == static_cast<int>(index)) roles += roles.empty() ? "A" : " / A";
            if (compare_b_enabled_ && compare_b_lap_index_ == static_cast<int>(index)) {
                roles += roles.empty() ? "B" : " / B";
            }
            if (playback_lap_index_ == static_cast<int>(index)) roles += roles.empty() ? "Playback" : " / Playback";
            if (roles.empty()) ImGui::TextDisabled("-");
            else ImGui::TextUnformatted(roles.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("report-findings", ImVec2(0.0F, -1.0F), ImGuiChildFlags_Borders);
    std::size_t metric_count = 0;
    std::size_t changed_metric_count = 0;
    for (const auto& comparison : driver_analysis_.comparisons) {
        if (!comparison) continue;
        for (const auto& corner : comparison->corners) {
            metric_count += corner.metrics.size();
            changed_metric_count += static_cast<std::size_t>(std::count_if(
                corner.metrics.begin(), corner.metrics.end(), [](const auto& metric) {
                    return metric.value.has_value() && metric.triggered;
                }));
        }
    }
    ImGui::SeparatorText("DETERMINISTIC FINDINGS");
    ImGui::Text("%zu insight%s", driver_analysis_.insights.size(),
        driver_analysis_.insights.size() == 1 ? "" : "s");
    ImGui::Text("%zu changed metric%s from %zu calculated",
        changed_metric_count, changed_metric_count == 1 ? "" : "s", metric_count);
    ImGui::TextDisabled("Formula %s", driver_analysis_.formula_version.c_str());
    ImGui::Separator();
    if (driver_analysis_.insights.empty()) {
        ImGui::TextWrapped("No finding passed the enabled evidence and data-quality gates.");
    } else {
        for (std::size_t index = 0;
             index < std::min<std::size_t>(driver_analysis_.insights.size(), 10); ++index) {
            const auto& insight = driver_analysis_.insights[index];
            ImGui::BulletText("%s", insight.title.c_str());
            ImGui::Indent();
            ImGui::TextDisabled("%s | %s", insight.corner_name.c_str(),
                insight_outcome_label(insight.outcome));
            ImGui::Unindent();
        }
        if (driver_analysis_.insights.size() > 10) {
            ImGui::TextDisabled("%zu more finding%s available in Crew Chief > Insights",
                driver_analysis_.insights.size() - 10,
                driver_analysis_.insights.size() - 10 == 1 ? "" : "s");
        }
    }
    ImGui::SeparatorText("THEORETICAL SECTORS");
    for (const auto& sector : session_->theoretical_best.sectors) {
        ImGui::Text("S%d  %s  (raw lap %d)", sector.sector,
            lap_time(sector.duration_us).c_str(), sector.source_raw_lap);
    }
    ImGui::EndChild();
    ImGui::End();
}

void NativeApp::draw_race_day() {
    using namespace std::chrono_literals;
    if (race_day_busy_ && race_day_future_.valid() &&
        race_day_future_.wait_for(0ms) == std::future_status::ready) {
        auto response = race_day_future_.get();
        race_day_busy_ = false;
        if (response.ok) {
            race_day_report_ = std::move(response.report);
            race_day_error_.clear();
            if (race_day_pending_knowledge_) {
                apply_crew_chief_report(*race_day_pending_knowledge_, *race_day_report_);
                const auto stored_id = race_day_pending_knowledge_->id;
                race_day::upsert_setup_knowledge(
                    race_day_, std::move(*race_day_pending_knowledge_));
                race_day_pending_knowledge_.reset();
                const auto stored = std::find_if(
                    race_day_.setup_knowledge.begin(), race_day_.setup_knowledge.end(),
                    [&](const auto& record) { return record.id == stored_id; });
                selected_setup_knowledge_record_ = stored == race_day_.setup_knowledge.end()
                    ? -1 : static_cast<int>(std::distance(race_day_.setup_knowledge.begin(), stored));
                race_day_dirty_ = true;
                if (!race_day_path_.empty()) {
                    std::string save_error;
                    if (race_day::save(race_day_, race_day_path_, save_error)) {
                        race_day_dirty_ = false;
                    } else {
                        race_day_error_ =
                            "Analysis completed, but the setup result could not be saved: " + save_error;
                    }
                }
            }
            status_ = std::format(
                "Race-day analysis answered with {}% confidence; setup result stored",
                race_day_report_->confidence);
        } else {
            race_day_pending_knowledge_.reset();
            race_day_error_ = std::move(response.error);
            status_ = "Race-day AI unavailable; the event book remains saved locally";
        }
    }

    const auto* viewport = ImGui::GetMainViewport();
    const auto origin = viewport->WorkPos + ImVec2(0.0F, application_header_height(text_scale_));
    const auto size = ImVec2(viewport->WorkSize.x,
        std::max(1.0F, viewport->WorkSize.y - application_header_height(text_scale_)));
    ImGui::SetNextWindowPos(origin, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Race Day Workspace", nullptr, flags)) {
        ImGui::End();
        return;
    }

    const auto compact_race_day =
        size.x < 1200.0F * std::clamp(text_scale_, 1.0F, 1.5F);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30F, 0.78F, 0.95F, 1.0F));
    ImGui::TextUnformatted("RACE DAY BOOK");
    ImGui::PopStyleColor();
    if (race_day_dirty_) {
        ImGui::SameLine();
        ImGui::TextColored(
            ImVec4(0.95F, 0.68F, 0.18F, 1.0F), "UNSAVED");
    }
    if (!compact_race_day) ImGui::SameLine();
    ImGui::TextWrapped(
        "Import each recording once, classify it, and keep telemetry, "
        "setup notes, conditions, and results together.");

    if (compact_race_day) {
        ImGui::TextDisabled("EVENT");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0F);
        if (input_text_string(
                "##race-day-event", race_day_.event_name)) {
            race_day_dirty_ = true;
        }
        ImGui::TextDisabled("TRACK");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0F);
        if (input_text_string(
                "##race-day-track", race_day_.track_name)) {
            race_day_dirty_ = true;
        }
        ImGui::TextDisabled("DATE");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0F);
        if (input_text_string(
                "##race-day-date", race_day_.date)) {
            race_day_dirty_ = true;
        }
    } else {
        ImGui::SetNextItemWidth(230.0F);
        if (input_text_string(
                "Event##race-day-event", race_day_.event_name)) {
            race_day_dirty_ = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(210.0F);
        if (input_text_string(
                "Track##race-day-track", race_day_.track_name)) {
            race_day_dirty_ = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0F);
        if (input_text_string(
                "Date##race-day-date", race_day_.date)) {
            race_day_dirty_ = true;
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("New")) ImGui::OpenPopup("Replace race day?");
    ImGui::SameLine();
    if (ImGui::Button("Open...")) open_race_day();
    ImGui::SameLine();
    if (ImGui::Button("Save")) save_race_day(false);
    ImGui::SameLine();
    if (ImGui::Button("Save as...")) save_race_day(true);

    if (ImGui::BeginPopupModal("Replace race day?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Create a new race day and replace the unsaved event currently in memory?");
        ImGui::Checkbox("Include Q4 (trophy race)", &race_day_include_q4_);
        ImGui::Checkbox("Triple A Main", &race_day_triple_a_);
        if (ImGui::Button("Create new day")) {
            new_race_day();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Separator();
    ImGui::PushStyleColor(
        ImGuiCol_Button, ImVec4(0.68F, 0.06F, 0.14F, 0.95F));
    ImGui::PushStyleColor(
        ImGuiCol_ButtonHovered, ImVec4(0.82F, 0.08F, 0.18F, 1.0F));
    if (ImGui::Button(
            "IMPORT FILES...", ImVec2(190.0F, 42.0F))) {
        start_session_import();
    }
    ImGui::PopStyleColor(2);
    if (!compact_race_day) ImGui::SameLine();
    ImGui::BeginDisabled(folder_scan_busy_);
    if (ImGui::Button(
            folder_scan_busy_
                ? "SCANNING FOLDER..."
                : "SCAN SAVED FOLDER",
            ImVec2(205.0F, 42.0F))) {
        start_folder_import();
    }
    ImGui::EndDisabled();
    if (!compact_race_day) ImGui::SameLine();
    if (ImGui::Button(
            "RACEBOX CLOUD EXPORT...",
            ImVec2(230.0F, 42.0F))) {
        open_racebox_cloud_export();
    }
    ImGui::TextDisabled("IMPORT FOLDER");
    ImGui::SameLine();
    ImGui::TextWrapped(
        "%s",
        telemetry_watch_folder_.empty()
            ? "Not selected"
            : path_utf8(telemetry_watch_folder_).c_str());
    if (ImGui::SmallButton("Choose import folder...")) {
        const auto selected = choose_telemetry_folder(
            window_, telemetry_watch_folder_);
        if (selected) {
            telemetry_watch_folder_ = *selected;
            status_ = "Telemetry import folder updated.";
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox(
        "Auto-detect Sanwa on USB",
        &auto_detect_sanwa_usb_);
    ImGui::TextWrapped(
        "RaceBox cloud opens the official website in your browser; the "
        "viewer never receives your password. Export CSV or VBO into the "
        "saved folder, then scan it. USB detection only proposes a Sanwa "
        "file and waits for you to confirm the matching run.");

    ImGui::Separator();
    const auto available = ImGui::GetContentRegionAvail();
    const auto left_width = std::clamp(available.x * 0.22F, 210.0F, 280.0F);
    const auto center_width = std::clamp(available.x * 0.43F, 360.0F, 620.0F);

    const auto workspace_tabs_open =
        !compact_race_day ||
        ImGui::BeginTabBar(
            "race-day-workspace-tabs",
            ImGuiTabBarFlags_FittingPolicyScroll);
    if (workspace_tabs_open) {
    const auto runs_open =
        !compact_race_day || ImGui::BeginTabItem("Runs");
    if (runs_open) {
    ImGui::BeginChild(
        "race-day-runs",
        ImVec2(compact_race_day ? 0.0F : left_width, -1.0F),
        ImGuiChildFlags_Borders);
    ImGui::SeparatorText("RUNS");
    for (std::size_t index = 0; index < race_day_.runs.size(); ++index) {
        auto& run = race_day_.runs[index];
        ImGui::PushID(static_cast<int>(index));
        const auto selected = selected_race_day_run_ == static_cast<int>(index);
        const auto label = std::format("{}##run", run.label);
        if (ImGui::Selectable(
                label.c_str(), selected,
                ImGuiSelectableFlags_None,
                ImVec2(
                    ImGui::GetContentRegionAvail().x,
                    34.0F))) {
            selected_race_day_run_ = static_cast<int>(index);
            race_day_current_run_ = selected_race_day_run_;
            if (race_day_previous_run_ == race_day_current_run_) {
                race_day_previous_run_ = std::max(0, race_day_current_run_ - 1);
                if (race_day_previous_run_ == race_day_current_run_ && race_day_.runs.size() > 1) {
                    race_day_previous_run_ = 1;
                }
            }
        }
        if (run.telemetry_files.empty()) {
            ImGui::TextDisabled(
                "%s | EMPTY", race_day::kind_name(run.kind));
        } else {
            auto files_available = true;
            for (std::size_t source_index = 0;
                 source_index < run.telemetry_files.size();
                 ++source_index) {
                files_available =
                    files_available &&
                    race_day::telemetry_source_state(
                        run, source_index) ==
                        race_day::TelemetrySourceState::Available;
            }
            const auto has_primary =
                race_day::has_primary_telemetry(run);
            const auto composition =
                race_day::validate_telemetry_source_composition(
                    run, true);
            const auto state = !composition.ok
                ? "NEEDS REPAIR"
                : !files_available
                ? "NEEDS FILE"
                : has_primary ? "DATA READY" : "CONTROLS ONLY";
            const auto color =
                composition.ok && files_available && has_primary
                ? ImVec4(0.28F, 0.80F, 0.45F, 1.0F)
                : ImVec4(0.95F, 0.68F, 0.18F, 1.0F);
            ImGui::TextColored(
                color, "%s | %s",
                race_day::kind_name(run.kind), state);
        }
        if (ImGui::SmallButton(
                run.telemetry_files.empty()
                    ? "Add data..."
                    : "Add / replace data...")) {
            selected_race_day_run_ = static_cast<int>(index);
            race_day_current_run_ = selected_race_day_run_;
            attach_race_day_telemetry(index);
        }
        ImGui::PopID();
    }
    ImGui::SeparatorText("ADD AS YOU GO");
    if (ImGui::Button("Add practice", ImVec2(-1.0F, 0.0F))) {
        race_day::add_practice(race_day_);
        selected_race_day_run_ = static_cast<int>(race_day_.runs.size()) - 1;
        race_day_current_run_ = selected_race_day_run_;
        race_day_dirty_ = true;
    }
    if (ImGui::Button("Add qualifier", ImVec2(-1.0F, 0.0F))) {
        race_day::add_qualifying(race_day_);
        selected_race_day_run_ = static_cast<int>(race_day_.runs.size()) - 1;
        race_day_current_run_ = selected_race_day_run_;
        race_day_dirty_ = true;
    }
    const char* groups[] = {"A Main", "B Main", "C Main", "D Main"};
    ImGui::SetNextItemWidth(110.0F);
    ImGui::Combo("##main-group", &race_day_main_group_, groups, 4);
    ImGui::SameLine();
    const char* legs[] = {"Single", "Triple"};
    ImGui::SetNextItemWidth(90.0F);
    ImGui::Combo("##main-legs", &race_day_main_legs_, legs, 2);
    if (ImGui::Button("Add main", ImVec2(-1.0F, 0.0F))) {
        const auto added = race_day::add_main_group(
            race_day_, static_cast<char>('A' + race_day_main_group_), race_day_main_legs_ == 0 ? 1 : 3);
        if (!added.empty()) {
            selected_race_day_run_ = static_cast<int>(added.front());
            race_day_current_run_ = selected_race_day_run_;
            race_day_dirty_ = true;
        } else {
            race_day_error_ = "That main is already in the race day";
        }
    }
    if (ImGui::Button("Add custom run", ImVec2(-1.0F, 0.0F))) {
        race_day::add_custom(race_day_, {});
        selected_race_day_run_ = static_cast<int>(race_day_.runs.size()) - 1;
        race_day_current_run_ = selected_race_day_run_;
        race_day_dirty_ = true;
    }
    ImGui::EndChild();
    if (compact_race_day) ImGui::EndTabItem();
    }

    if (!compact_race_day) ImGui::SameLine();
    const auto force_details =
        race_day_detail_run_seen_ != selected_race_day_run_ ||
        pending_race_day_relink_.has_value();
    const auto details_open =
        !compact_race_day ||
        ImGui::BeginTabItem(
            "Selected Run", nullptr,
            force_details
                ? ImGuiTabItemFlags_SetSelected
                : ImGuiTabItemFlags_None);
    if (details_open) {
    ImGui::BeginChild(
        "race-day-details",
        ImVec2(compact_race_day ? 0.0F : center_width, -1.0F),
        ImGuiChildFlags_Borders);
    if (race_day_.runs.empty()) {
        ImGui::TextDisabled("Add a run to begin.");
    } else {
        selected_race_day_run_ = std::clamp(
            selected_race_day_run_, 0, static_cast<int>(race_day_.runs.size()) - 1);
        auto& run = race_day_.runs[static_cast<std::size_t>(selected_race_day_run_)];
        const auto source_composition =
            race_day::validate_telemetry_source_composition(
                run, true);
        auto sources_ready = source_composition.ok;
        for (std::size_t source_index = 0;
             source_index < run.telemetry_files.size(); ++source_index) {
            sources_ready =
                sources_ready &&
                race_day::telemetry_source_state(run, source_index) ==
                    race_day::TelemetrySourceState::Available;
        }
        const auto has_vbo = std::any_of(
            run.telemetry_files.begin(), run.telemetry_files.end(),
            [](const auto& path) {
                return race_day::telemetry_source_kind(path) ==
                    race_day::TelemetrySourceKind::Vbo;
            });
        const auto has_archive = std::any_of(
            run.telemetry_files.begin(), run.telemetry_files.end(),
            [](const auto& path) {
                return race_day::telemetry_source_kind(path) ==
                    race_day::TelemetrySourceKind::NativeArchive;
            });
        const auto has_racebox =
            race_day::has_racebox_csv(run);
        const auto has_sanwa =
            race_day::has_sanwa_csv(run);
        const auto full_comparison_inputs =
            sources_ready &&
            has_vbo && has_racebox && has_sanwa;
        const auto select_data_tab =
            race_day_detail_run_seen_ != selected_race_day_run_;
        race_day_detail_run_seen_ = selected_race_day_run_;
        ImGui::PushID(selected_race_day_run_);
        ImGui::SeparatorText("SELECTED RUN");
        if (input_text_string("Run name", run.label)) race_day_dirty_ = true;
        const auto recorded_display =
            friendly_recorded_time_label(run.recorded_at_utc);
        ImGui::TextDisabled("%s", race_day::kind_name(run.kind));
        if (!run.recorded_at_utc.empty()) {
            ImGui::TextWrapped(
                "Recorded %s", recorded_display.c_str());
        }

        if (ImGui::BeginTabBar("race-day-detail-tabs")) {
            const auto data_flags = select_data_tab
                ? ImGuiTabItemFlags_SetSelected
                : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(
                    "Run Data", nullptr, data_flags)) {
                ImGui::TextWrapped(
                    "Add the recordings for this run. Select the "
                    "RaceBox/VBO file and Sanwa file together when "
                    "possible; the importer will ask whether this was "
                    "Practice, Qualifying, or Race and fill the date.");
                ImGui::PushStyleColor(
                    ImGuiCol_Button,
                    ImVec4(0.68F, 0.06F, 0.14F, 0.95F));
                ImGui::BeginDisabled(race_day_busy_);
                if (ImGui::Button(
                        run.telemetry_files.empty()
                            ? "ADD RECORDING TO THIS RUN..."
                            : "ADD OR REPLACE RUN DATA...",
                        ImVec2(-1.0F, 42.0F))) {
                    attach_race_day_telemetry(
                        static_cast<std::size_t>(
                            selected_race_day_run_));
                }
                ImGui::EndDisabled();
                ImGui::PopStyleColor();
                ImGui::BeginDisabled(
                    race_day_busy_ || folder_scan_busy_);
                if (ImGui::Button(
                        folder_scan_busy_
                            ? "SCANNING IMPORT FOLDER..."
                            : "ADD FROM SAVED IMPORT FOLDER...",
                        ImVec2(-1.0F, 34.0F))) {
                    start_folder_import(
                        selected_race_day_run_);
                }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(
                    race_day_busy_ ||
                    !session_ ||
                    current_session_source_files_.empty());
                if (ImGui::Button(
                        "USE TELEMETRY CURRENTLY OPEN",
                        ImVec2(-1.0F, 34.0F))) {
                    use_open_session_for_race_day_run(
                        static_cast<std::size_t>(
                            selected_race_day_run_));
                }
                ImGui::EndDisabled();

                ImGui::SeparatorText("READINESS");
                ImGui::TextColored(
                    sources_ready
                        ? ui::color(ui::ColorToken::Positive)
                        : ui::color(ui::ColorToken::Warning),
                    "%s",
                    sources_ready
                        ? "Ready to view laps"
                        : "Needs RaceBox, VBO, GPX, or a session archive");
                if (!source_composition.ok &&
                    !source_composition.error.empty()) {
                    ImGui::TextWrapped(
                        "Repair needed: %s",
                        source_composition.error.c_str());
                }
                ImGui::TextColored(
                    full_comparison_inputs
                        ? ui::color(ui::ColorToken::Positive)
                        : ui::color(ui::ColorToken::Warning),
                    "%s",
                    full_comparison_inputs
                        ? "Full setup comparison inputs are present"
                        : has_archive
                            ? "Session archive attached; open it to "
                              "confirm its input and motion channels"
                            : "Limited comparison: VBO + RaceBox CSV + "
                              "Sanwa gives the strongest input/output evidence");

                ImGui::SeparatorText("ATTACHED SOURCES");
                if (run.telemetry_files.empty()) {
                    ImGui::TextDisabled(
                        "No recording is attached to this run yet.");
                }
                std::optional<std::size_t> remove_source;
                for (std::size_t source_index = 0;
                     source_index < run.telemetry_files.size();
                     ++source_index) {
                    const auto& path =
                        run.telemetry_files[source_index];
                    const auto source_state =
                        race_day::telemetry_source_state(
                            run, source_index);
                    const auto kind =
                        race_day::telemetry_source_kind(path);
                    ImGui::PushID(
                        static_cast<int>(source_index));
                    const auto state_label =
                        source_state ==
                            race_day::TelemetrySourceState::Available
                        ? "READY"
                        : source_state ==
                                  race_day::TelemetrySourceState::Changed
                            ? "CHANGED"
                            : "MISSING";
                    ImGui::TextColored(
                        source_state ==
                                race_day::TelemetrySourceState::Available
                            ? ui::color(ui::ColorToken::Positive)
                            : ui::color(ui::ColorToken::Warning),
                        "%s | %s",
                        race_day::telemetry_source_kind_name(kind),
                        state_label);
                    ImGui::TextWrapped(
                        "%s",
                        path_utf8(path.filename()).c_str());
                    ImGui::BeginDisabled(race_day_busy_);
                    if (source_state !=
                        race_day::TelemetrySourceState::Available) {
                        if (ImGui::SmallButton(
                                source_state ==
                                        race_day::TelemetrySourceState::Changed
                                    ? "Review / replace file..."
                                    : "Find moved file...")) {
                            begin_race_day_source_relink(
                                source_index);
                        }
                        ImGui::SameLine();
                    }
                    if (ImGui::SmallButton(
                            "Remove this source")) {
                        remove_source = source_index;
                    }
                    ImGui::EndDisabled();
                    ImGui::Separator();
                    ImGui::PopID();
                }
                if (!has_archive) {
                    const auto missing_source =
                        [&](bool present, const char* label,
                            const char* description) {
                            if (present) return;
                            ImGui::TextColored(
                                ui::color(ui::ColorToken::Warning),
                                "%s | NOT ATTACHED", label);
                            ImGui::TextDisabled(
                                "%s", description);
                            ImGui::Separator();
                        };
                    missing_source(
                        has_vbo, "VBO",
                        "GPS, chassis G, gyro, and altitude");
                    missing_source(
                        has_racebox, "RaceBox CSV",
                        "Lap timing and speed");
                    missing_source(
                        has_sanwa, "Sanwa CSV",
                        "Throttle, brake, and steering");
                }
                if (remove_source) {
                    if (race_day::remove_telemetry_source(
                            run, *remove_source)) {
                        const auto invalidated =
                            invalidate_setup_knowledge_for_run(
                                race_day_, run.id);
                        if (displayed_race_day_run_id_ == run.id) {
                            displayed_race_day_run_id_.clear();
                        }
                        pending_race_day_relink_.reset();
                        race_day_dirty_ = true;
                        race_day_report_.reset();
                        race_day_pending_knowledge_.reset();
                        selected_setup_knowledge_record_ = -1;
                        status_ = invalidated == 0
                            ? "Removed the telemetry source"
                            : std::format(
                                  "Removed the telemetry source and "
                                  "{} outdated comparison result(s)",
                                  invalidated);
                    }
                }

                ImGui::BeginDisabled(!sources_ready);
                if (ImGui::Button(
                        "OPEN THIS RUN IN TELEMETRY VIEWER",
                        ImVec2(-1.0F, 38.0F))) {
                    open_race_day_run_in_viewer(
                        static_cast<std::size_t>(
                            selected_race_day_run_));
                }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(
                    run.telemetry_files.empty() ||
                    race_day_busy_);
                if (ImGui::SmallButton(
                        "Remove all run data")) {
                    ImGui::OpenPopup(
                        "Remove all data from this run?");
                }
                ImGui::EndDisabled();
                if (ImGui::BeginPopupModal(
                        "Remove all data from this run?", nullptr,
                        ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextWrapped(
                        "Remove every telemetry link from %s? "
                        "The source files on disk will not be deleted.",
                        run.label.c_str());
                    if (ImGui::Button("Remove run data")) {
                        const auto invalidated =
                            invalidate_setup_knowledge_for_run(
                                race_day_, run.id);
                        race_day::clear_telemetry_sources(run);
                        if (displayed_race_day_run_id_ == run.id) {
                            displayed_race_day_run_id_.clear();
                        }
                        pending_race_day_relink_.reset();
                        race_day_dirty_ = true;
                        race_day_report_.reset();
                        race_day_pending_knowledge_.reset();
                        selected_setup_knowledge_record_ = -1;
                        status_ = invalidated == 0
                            ? "Removed all data from the selected run"
                            : std::format(
                                  "Removed all run data and {} outdated "
                                  "comparison result(s)",
                                  invalidated);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Before Run")) {
                ImGui::TextWrapped(
                    "Record what the car is starting with. This is the context the Crew Chief needs before comparing runs.");
                if (input_text_multiline_string(
                    "What did you change on the car?##setup", run.setup_changes, ImVec2(-1.0F, 86.0F))) race_day_dirty_ = true;
                if (input_text_multiline_string(
                    "Anything to remember before the run?##pre", run.pre_run_notes, ImVec2(-1.0F, 76.0F))) race_day_dirty_ = true;
                ImGui::SeparatorText("CHECKLIST");
                for (auto& item : run.checklist) {
                    ImGui::PushID(item.id.c_str());
                    if (ImGui::Checkbox(item.label.c_str(), &item.checked)) race_day_dirty_ = true;
                    ImGui::SetNextItemWidth(-1.0F);
                    if (input_text_string("Note##check-note", item.note)) race_day_dirty_ = true;
                    ImGui::PopID();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Conditions & tires")) {
                const auto optional_double = [&](const char* label, std::optional<double>& value,
                                                 const char* suffix, double step) {
                    ImGui::PushID(label);
                    auto known = value.has_value();
                    if (ImGui::Checkbox("Recorded", &known)) {
                        if (known) value = 0.0; else value.reset();
                        race_day_dirty_ = true;
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!known);
                    auto number = value.value_or(0.0);
                    ImGui::SetNextItemWidth(120.0F);
                    if (ImGui::InputDouble(label, &number, step, step * 5.0, "%.1f")) {
                        value = number;
                        race_day_dirty_ = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", suffix);
                    ImGui::EndDisabled();
                    ImGui::PopID();
                };
                const auto optional_int = [&](const char* label, int& value, const char* suffix) {
                    ImGui::PushID(label);
                    auto known = value >= 0;
                    if (ImGui::Checkbox("Recorded", &known)) {
                        value = known ? 0 : -1;
                        race_day_dirty_ = true;
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!known);
                    auto number = std::max(0, value);
                    ImGui::SetNextItemWidth(110.0F);
                    if (ImGui::InputInt(label, &number)) {
                        value = std::max(0, number);
                        race_day_dirty_ = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", suffix);
                    ImGui::EndDisabled();
                    ImGui::PopID();
                };
                optional_double("Ambient temperature", run.conditions.ambient_temperature_c, "C", 0.5);
                optional_double("Track temperature", run.conditions.track_temperature_c, "C", 0.5);
                if (input_text_string("Track condition", run.conditions.track_condition)) race_day_dirty_ = true;
                ImGui::SeparatorText("TIRES");
                if (input_text_string("Tire set ID", run.conditions.tire_set_id)) race_day_dirty_ = true;
                if (input_text_string("Tire compound", run.conditions.tire_compound)) race_day_dirty_ = true;
                optional_int("Runs already on this tire set", run.conditions.tire_runs_before, "runs before this session");
                if (input_text_string("Sauce compound", run.conditions.sauce_compound)) race_day_dirty_ = true;
                optional_int("Sauce timing", run.conditions.sauce_minutes_before, "minutes before run");
                optional_int("Tire warmer time", run.conditions.tire_warmer_minutes, "minutes");
                optional_double("Tire warmer temperature", run.conditions.tire_warmer_temperature_c, "C", 1.0);
                ImGui::SeparatorText("POWER");
                if (input_text_string("Battery pack / ID", run.conditions.battery_pack)) race_day_dirty_ = true;
                optional_double("Battery voltage", run.conditions.battery_voltage, "V", 0.1);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("After Run")) {
                ImGui::TextWrapped(
                    "Write what changed from the driver's seat: rotation, steering load, forward bite, braking, bumps, consistency, or tire feel.");
                if (input_text_multiline_string(
                    "How did the car feel?##post", run.post_run_notes, ImVec2(-1.0F, 240.0F))) race_day_dirty_ = true;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        draw_race_day_source_relink_popup();
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (compact_race_day) ImGui::EndTabItem();
    }

    if (!compact_race_day) ImGui::SameLine();
    const auto analysis_open =
        !compact_race_day || ImGui::BeginTabItem("Crew Chief");
    if (analysis_open) {
    ImGui::BeginChild("race-day-analysis", ImVec2(0.0F, -1.0F), ImGuiChildFlags_Borders);
    ImGui::SeparatorText(
        "CREW CHIEF: PREVIOUS RUN -> CURRENT RUN");
    ImGui::TextWrapped(
        "The deterministic analytics layer compares the two complete recordings first. "
        "The Crew Chief then explains those calculated results with your setup, checklist, tire, temperature, and driver notes.");
    const auto run_combo = [&](const char* label, int& selected) {
        if (race_day_.runs.empty()) return;
        selected = std::clamp(selected, 0, static_cast<int>(race_day_.runs.size()) - 1);
        if (ImGui::BeginCombo(label, race_day_.runs[static_cast<std::size_t>(selected)].label.c_str())) {
            for (std::size_t index = 0; index < race_day_.runs.size(); ++index) {
                const auto active = selected == static_cast<int>(index);
                if (ImGui::Selectable(race_day_.runs[index].label.c_str(), active)) {
                    selected = static_cast<int>(index);
                    race_day_report_.reset();
                }
                if (active) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    };
    run_combo("Previous / baseline", race_day_previous_run_);
    run_combo("Current / changed", race_day_current_run_);
    if (!race_day_.runs.empty()) {
        const auto& previous = race_day_.runs[static_cast<std::size_t>(std::clamp(
            race_day_previous_run_, 0, static_cast<int>(race_day_.runs.size()) - 1))];
        const auto& current = race_day_.runs[static_cast<std::size_t>(std::clamp(
            race_day_current_run_, 0, static_cast<int>(race_day_.runs.size()) - 1))];
        ImGui::TextDisabled("Previous: %zu source(s) | Current: %zu source(s)",
            previous.telemetry_files.size(), current.telemetry_files.size());
        if (!race_day::has_sanwa_csv(previous) ||
            !race_day::has_sanwa_csv(current)) {
            ImGui::TextColored(
                ui::color(ui::ColorToken::Warning),
                "Limited comparison: both runs need Sanwa data to "
                "compare steering, throttle, and brake input.");
        }
        if (current.setup_changes.empty()) {
            ImGui::TextColored(ImVec4(0.95F, 0.68F, 0.18F, 1.0F),
                "Current run has no setup-change note yet.");
        }
        const auto relevant = race_day::relevant_setup_knowledge(
            race_day_, race_day_question_, current);
        ImGui::TextColored(ImVec4(0.30F, 0.78F, 0.95F, 1.0F),
            "%zu matching saved setup result%s available for this question",
            relevant.size(), relevant.size() == 1 ? "" : "s");
    }
    if (input_text_multiline_string(
        "Question##race-day-question", race_day_question_, ImVec2(-1.0F, 92.0F))) {
        race_day_report_.reset();
    }
    const auto run_sources_ready = [&](int run_index) {
        if (run_index < 0 ||
            run_index >= static_cast<int>(race_day_.runs.size())) {
            return false;
        }
        const auto& candidate =
            race_day_.runs[static_cast<std::size_t>(run_index)];
        if (!race_day::validate_telemetry_source_composition(
                candidate, true).ok) {
            return false;
        }
        for (std::size_t source_index = 0;
             source_index < candidate.telemetry_files.size();
             ++source_index) {
            if (race_day::telemetry_source_state(
                    candidate, source_index) !=
                race_day::TelemetrySourceState::Available) {
                return false;
            }
        }
        return true;
    };
    const auto can_analyze = race_day_.runs.size() >= 2 && !race_day_busy_ &&
        race_day_previous_run_ != race_day_current_run_ &&
        race_day_previous_run_ >= 0 && race_day_current_run_ >= 0 &&
        race_day_previous_run_ < static_cast<int>(race_day_.runs.size()) &&
        race_day_current_run_ < static_cast<int>(race_day_.runs.size()) &&
        run_sources_ready(race_day_previous_run_) &&
        run_sources_ready(race_day_current_run_);
    ImGui::BeginDisabled(!can_analyze);
    if (ImGui::Button(
            "ASK CREW CHIEF ABOUT THESE RUNS",
            ImVec2(-1.0F, 42.0F))) {
        analyze_race_day_runs();
    }
    ImGui::EndDisabled();
    if (!can_analyze) {
        if (race_day_previous_run_ == race_day_current_run_) {
            ImGui::TextColored(
                ui::color(ui::ColorToken::Warning),
                "Choose two different runs.");
        } else if (!run_sources_ready(
                       race_day_previous_run_)) {
            ImGui::TextColored(
                ui::color(ui::ColorToken::Warning),
                "The previous run needs an available RaceBox, "
                "VBO, GPX, or session archive.");
        } else if (!run_sources_ready(
                       race_day_current_run_)) {
            ImGui::TextColored(
                ui::color(ui::ColorToken::Warning),
                "The current run needs an available RaceBox, "
                "VBO, GPX, or session archive.");
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30F, 0.78F, 0.95F, 1.0F));
    ImGui::TextWrapped(
        "Privacy: this button sends generated aligned telemetry CSV plus these two runs' notes and conditions "
        "to your Tailscale-only gateway. Original paths and raw files are not sent. The model receives calculated evidence.");
    ImGui::PopStyleColor();
    if (race_day_busy_) {
        ImGui::TextColored(ImVec4(0.95F, 0.68F, 0.18F, 1.0F),
            "Loading both runs and calculating matched speed/input evidence...");
    }
    if (!race_day_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96F, 0.32F, 0.30F, 1.0F));
        ImGui::TextWrapped("%s", race_day_error_.c_str());
        ImGui::PopStyleColor();
    }
    if (race_day_report_) {
        const auto& report = *race_day_report_;
        const auto color = report.verdict == "supported" ? ImVec4(0.20F, 0.82F, 0.42F, 1.0F) :
            report.verdict == "mixed" ? ImVec4(0.95F, 0.68F, 0.18F, 1.0F) :
            ImVec4(0.72F, 0.76F, 0.82F, 1.0F);
        ImGui::SeparatorText("CREW CHIEF ANALYSIS");
        ImGui::TextColored(color, "%s | %d%% confidence",
            report.verdict == "supported" ? "CHANGE SUPPORTED" :
            report.verdict == "mixed" ? "MIXED RESULT" :
            report.verdict == "not_supported" ? "CHANGE NOT SUPPORTED" :
            report.verdict == "data_limited" ? "MORE MATCHED RUNS NEEDED" :
            "NO CLEAR ANSWER", report.confidence);
        ImGui::TextWrapped("%s", report.summary.c_str());
        for (std::size_t index = 0; index < report.observations.size(); ++index) {
            const auto& observation = report.observations[index];
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::TreeNodeEx(observation.area.c_str(),
                index == 0 ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None)) {
                ImGui::TextWrapped("Measured: %s", observation.change.c_str());
                ImGui::TextWrapped("Meaning: %s", observation.meaning.c_str());
                if (!observation.evidence_ids.empty()) {
                    std::string ids;
                    for (const auto& id : observation.evidence_ids) {
                        if (!ids.empty()) ids += " | ";
                        ids += id;
                    }
                    ImGui::TextDisabled("%s", ids.c_str());
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (!report.confounds.empty() && ImGui::CollapsingHeader("Possible confounds")) {
            for (const auto& confound : report.confounds) ImGui::BulletText("%s", confound.c_str());
        }
        ImGui::SeparatorText("NEXT CONTROLLED TEST");
        ImGui::TextWrapped("%s", report.next_test.c_str());
        ImGui::TextDisabled("Causality: %s", report.causality_note.c_str());
        ImGui::TextDisabled(
            "Agent: %s | %s | %s | prior results used: %zu",
            report.model.empty() ? "not reported" : report.model.c_str(),
            report.thinking.empty() ? "thinking not reported" : report.thinking.c_str(),
            report.selected_lane.empty() ? "lane not reported" : report.selected_lane.c_str(),
            report.prior_setup_result_ids.size());
    }
    if (ImGui::CollapsingHeader("What the formulas can and cannot say")) {
        ImGui::BulletText(
            "Side response: compare lateral G in matched speed and steering-input bins; require repeated complete laps.");
        ImGui::BulletText(
            "Forward bite: compare speed-derived acceleration at full throttle in matched speed/steering bins.");
        ImGui::BulletText(
            "Brake indicator: sustained high brake with falling deceleration plus yaw/lateral disturbance is only a possible lockup or low-grip signal.");
        ImGui::TextWrapped(
            "Chassis IMU G is not direct tire load. Wheel-speed sensors are required to confirm a locked tire, "
            "and controlled A/B repetition is required before a setup change is called causal.");
    }

    const auto knowledge_heading = std::format(
        "SETUP KNOWLEDGE ({} SAVED)", race_day_.setup_knowledge.size());
    ImGui::SeparatorText(knowledge_heading.c_str());
    ImGui::TextWrapped(
        "Each completed comparison stores the change, driver result, conditions, measured evidence, "
        "and Crew Chief conclusion inside this Race Day file. Matching records can be referenced in future questions.");
    if (race_day_.setup_knowledge.empty()) {
        ImGui::TextDisabled(
            "No setup results saved yet. Complete a previous-vs-current analysis to create the first record.");
    } else {
        selected_setup_knowledge_record_ = std::clamp(
            selected_setup_knowledge_record_, 0,
            static_cast<int>(race_day_.setup_knowledge.size()) - 1);
        const auto& selected = race_day_.setup_knowledge[
            static_cast<std::size_t>(selected_setup_knowledge_record_)];
        const auto selected_label = std::format(
            "{} -> {} | {} | {}%",
            selected.previous_run_label, selected.current_run_label,
            selected.verdict, selected.confidence);
        if (ImGui::BeginCombo("Saved setup result", selected_label.c_str())) {
            for (std::size_t reverse = race_day_.setup_knowledge.size(); reverse > 0; --reverse) {
                const auto index = reverse - 1;
                const auto& record = race_day_.setup_knowledge[index];
                const auto label = std::format(
                    "{} -> {} | {} | {}%##{}",
                    record.previous_run_label, record.current_run_label,
                    record.verdict, record.confidence, record.id);
                const auto active = selected_setup_knowledge_record_ == static_cast<int>(index);
                if (ImGui::Selectable(label.c_str(), active)) {
                    selected_setup_knowledge_record_ = static_cast<int>(index);
                }
                if (active) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        const auto& record = race_day_.setup_knowledge[
            static_cast<std::size_t>(selected_setup_knowledge_record_)];
        ImGui::TextDisabled("%s | %s | %s",
            record.created_at_utc.c_str(), record.track_name.c_str(), record.id.c_str());
        ImGui::TextWrapped("Setup change: %s",
            record.setup_change.empty() ? "none recorded" : record.setup_change.c_str());
        ImGui::TextWrapped("Driver result: %s",
            record.driver_result.empty() ? "none recorded" : record.driver_result.c_str());
        ImGui::TextWrapped("Crew Chief result: %s", record.summary.c_str());
        if (ImGui::TreeNodeEx(
            "Measured evidence##saved-setup-evidence", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Quality confidence: %d%%", record.evidence.quality_confidence);
            if (record.evidence.lap_time_delta_s) {
                ImGui::Text("Top-three lap median: current %+.3f s vs previous",
                    *record.evidence.lap_time_delta_s);
            } else {
                ImGui::TextDisabled("Top-three lap median: insufficient data");
            }
            if (record.evidence.lateral_response_delta_g) {
                ImGui::Text("Matched lateral response: %+.4f G (%s)",
                    *record.evidence.lateral_response_delta_g,
                    record.evidence.lateral_status.c_str());
            } else {
                ImGui::TextDisabled("Matched lateral response: insufficient data");
            }
            if (record.evidence.forward_bite_delta_g) {
                ImGui::Text("Matched full-throttle acceleration: %+.4f G (%s)",
                    *record.evidence.forward_bite_delta_g,
                    record.evidence.forward_status.c_str());
            } else {
                ImGui::TextDisabled("Matched full-throttle acceleration: insufficient data");
            }
            if (record.evidence.straight_top_speed_delta_kmh) {
                ImGui::Text("Straight top speed: %+.2f km/h (%s)",
                    *record.evidence.straight_top_speed_delta_kmh,
                    record.evidence.top_speed_status.c_str());
                ImGui::Text("Straight cause: %s | entry %s | accel %s",
                    record.evidence.straight_speed_attribution.c_str(),
                    record.evidence.straight_entry_speed_delta_kmh
                        ? std::format("{:+.2f} km/h", *record.evidence.straight_entry_speed_delta_kmh).c_str()
                        : "n/a",
                    record.evidence.straight_acceleration_delta_g
                        ? std::format("{:+.4f} G", *record.evidence.straight_acceleration_delta_g).c_str()
                        : "n/a");
            } else {
                ImGui::TextDisabled("Straight top speed: insufficient data");
            }
            if (record.evidence.brake_decel_delta_g || record.evidence.brake_response_delay_delta_s) {
                ImGui::Text("Brake response: %s | decel %s | delay %s",
                    record.evidence.brake_response_status.c_str(),
                    record.evidence.brake_decel_delta_g
                        ? std::format("{:+.4f} G", *record.evidence.brake_decel_delta_g).c_str()
                        : "n/a",
                    record.evidence.brake_response_delay_delta_s
                        ? std::format("{:+.3f} s", *record.evidence.brake_response_delay_delta_s).c_str()
                        : "n/a");
            }
            if (record.evidence.steering_for_lateral_g_delta_percent ||
                record.evidence.yaw_per_steering_delta_dps) {
                ImGui::Text("Corner balance: %s", record.evidence.corner_balance_status.c_str());
            }
            if (record.evidence.overdriving_index_delta ||
                record.evidence.overdriving_risk_sample_delta_percent ||
                record.evidence.current_late_overdriving_delta_score) {
                ImGui::Text("Overdriving / tire scrub: %s | score %s | risk samples %s | late run %s",
                    record.evidence.overdriving_status.c_str(),
                    record.evidence.overdriving_index_delta
                        ? std::format("{:+.1f}", *record.evidence.overdriving_index_delta).c_str()
                        : "n/a",
                    record.evidence.overdriving_risk_sample_delta_percent
                        ? std::format("{:+.1f}%%", *record.evidence.overdriving_risk_sample_delta_percent).c_str()
                        : "n/a",
                    record.evidence.current_late_overdriving_delta_score
                        ? std::format("{:+.1f}", *record.evidence.current_late_overdriving_delta_score).c_str()
                        : "n/a");
            }
            if (record.evidence.chassis_roll_delta_deg ||
                record.evidence.roll_per_lateral_g_delta_deg ||
                record.evidence.surface_tilt_delta_deg) {
                ImGui::Text("Chassis roll signature: %s | roll %s | per G %s | surface %s (%s, %d samples)",
                    record.evidence.chassis_roll_status.c_str(),
                    record.evidence.chassis_roll_delta_deg
                        ? std::format("{:+.2f} deg", *record.evidence.chassis_roll_delta_deg).c_str()
                        : "n/a",
                    record.evidence.roll_per_lateral_g_delta_deg
                        ? std::format("{:+.2f} deg/G", *record.evidence.roll_per_lateral_g_delta_deg).c_str()
                        : "n/a",
                    record.evidence.surface_tilt_delta_deg
                        ? std::format("{:+.2f} deg", *record.evidence.surface_tilt_delta_deg).c_str()
                        : "n/a",
                    record.evidence.current_surface_tilt_source.c_str(),
                    record.evidence.current_surface_tilt_samples);
            }
            if (record.evidence.roll_rate_delta_dps ||
                record.evidence.roll_rate_per_lateral_g_delta_dps ||
                record.evidence.current_late_roll_rate_delta_dps) {
                ImGui::Text("Roll-rate signature: %s | p90 %s -> %s | delta %s | late run %s | samples %d -> %d",
                    record.evidence.roll_rate_status.c_str(),
                    record.evidence.previous_roll_rate_p90_dps
                        ? std::format("{:.1f} deg/s", *record.evidence.previous_roll_rate_p90_dps).c_str()
                        : "n/a",
                    record.evidence.current_roll_rate_p90_dps
                        ? std::format("{:.1f} deg/s", *record.evidence.current_roll_rate_p90_dps).c_str()
                        : "n/a",
                    record.evidence.roll_rate_delta_dps
                        ? std::format("{:+.1f} deg/s", *record.evidence.roll_rate_delta_dps).c_str()
                        : "n/a",
                    record.evidence.current_late_roll_rate_delta_dps
                        ? std::format("{:+.1f} deg/s", *record.evidence.current_late_roll_rate_delta_dps).c_str()
                        : "n/a",
                    record.evidence.previous_roll_rate_samples,
                    record.evidence.current_roll_rate_samples);
            }
            ImGui::Text("Possible brake/low-grip indicators: %d -> %d",
                record.evidence.previous_brake_indicators,
                record.evidence.current_brake_indicators);
            ImGui::TextDisabled(
                "Track check: %s | Formula: %s v%d",
                record.evidence.track_status.c_str(),
                record.evidence.analytics_contract.c_str(),
                record.evidence.formula_version);
            ImGui::TreePop();
        }
        ImGui::TextWrapped("Recommended next test: %s", record.next_test.c_str());
        if (ImGui::Button("Delete selected setup result")) {
            ImGui::OpenPopup("Delete saved setup result?");
        }
        if (ImGui::BeginPopupModal(
            "Delete saved setup result?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(
                "Delete this stored comparison? The run notes and telemetry attachments will not be changed.");
            if (ImGui::Button("Delete result")) {
                race_day_.setup_knowledge.erase(
                    race_day_.setup_knowledge.begin() + selected_setup_knowledge_record_);
                selected_setup_knowledge_record_ = race_day_.setup_knowledge.empty()
                    ? -1 : std::min(
                        selected_setup_knowledge_record_,
                        static_cast<int>(race_day_.setup_knowledge.size()) - 1);
                race_day_dirty_ = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    ImGui::SeparatorText("ADVANCED LAP CHECK");
    const auto lap_check_open = ImGui::CollapsingHeader(
        "Ask about laps inside the run currently shown in Session");
    if (lap_check_open || crew_chief_busy_) {
        draw_crew_chief();
    }
    ImGui::EndChild();
    if (compact_race_day) ImGui::EndTabItem();
    }
    if (compact_race_day) ImGui::EndTabBar();
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
        const auto context_compare_b_enabled = annotation ? annotation->compare_b_enabled : compare_b_enabled_;
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
            lap_label(reference_index), lap_label(compare_a_index),
            context_compare_b_enabled ? lap_label(compare_b_index) : std::string{"Off"},
            lap_label(playback_index));
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
        ImGui::Text("Telemetry %.3fs | ref %d | A %d | B %s | play %d",
            static_cast<double>(selected->cursor_us) / kSecond, selected->active_lap_index,
            selected->compare_lap_index,
            selected->compare_b_enabled ? std::to_string(selected->compare_b_lap_index).c_str() : "Off",
            selected->playback_lap_index);
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
        if (ImGui::Button("Import review JSON...")) import_annotations();
        ImGui::SameLine();
        if (ImGui::Button("Clear all")) {
            annotations_.clear();
            selected_annotation_id_ = 0;
        }
    } else {
        ImGui::Separator();
        ImGui::BeginDisabled(!session_);
        if (ImGui::Button("Import review JSON...")) import_annotations();
        ImGui::EndDisabled();
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
                {"compare_lap_index", annotation.compare_lap_index},
                {"compare_b_lap_index", annotation.compare_b_lap_index},
                {"compare_b_enabled", annotation.compare_b_enabled},
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

void NativeApp::import_annotations() {
    if (!session_) {
        error_ = "Open the matching telemetry session before importing an annotation review";
        return;
    }
    const auto path = open_annotation_file(window_);
    if (!path) return;
    try {
        std::error_code filesystem_error;
        const auto byte_size =
            std::filesystem::file_size(*path, filesystem_error);
        if (filesystem_error || byte_size > 8ULL * 1024ULL * 1024ULL) {
            throw std::runtime_error(
                "Annotation review is missing or larger than 8 MB");
        }
        std::ifstream file(*path, std::ios::binary);
        if (!file) {
            throw std::runtime_error(
                "Could not open annotation review");
        }
        const auto input = nlohmann::json::parse(file);
        if (!input.is_object() ||
            input.value("format", std::string{}) !=
                "racebox-annotation-review" ||
            input.value("version", 0) != 1) {
            throw std::runtime_error(
                "This is not a supported RaceBox annotation review");
        }

        const auto source_session =
            input.value("session", std::string{});
        if (!source_session.empty() &&
            source_session != session_->name) {
            throw std::runtime_error(std::format(
                "This review belongs to session '{}'. Open that session before importing its pins.",
                source_session));
        }

        const auto bounded_string = [](const nlohmann::json& value,
                                       const char* key,
                                       std::size_t maximum,
                                       std::string fallback = {}) {
            auto result = value.value(key, std::move(fallback));
            if (result.size() > maximum) result.resize(maximum);
            return result;
        };
        const auto imported_values =
            input.value("annotations", nlohmann::json::array());
        if (!imported_values.is_array()) {
            throw std::runtime_error(
                "Annotation review has an invalid pin list");
        }
        if (imported_values.empty()) {
            throw std::runtime_error(
                "Annotation review contains no usable pins");
        }
        if (imported_values.size() > 1'000) {
            throw std::runtime_error(
                "Annotation review contains more than 1,000 pins");
        }
        if (annotations_.size() + imported_values.size() > 2'000) {
            throw std::runtime_error(
                "Import would exceed the 2,000-pin session limit");
        }

        const auto first_imported_id = next_annotation_id_;
        auto staged_next_id = next_annotation_id_;
        std::vector<Annotation> staged;
        staged.reserve(imported_values.size());
        for (const auto& value : imported_values) {
            if (!value.is_object()) {
                throw std::runtime_error(
                    "Annotation review contains an invalid pin");
            }
            Annotation annotation;
            annotation.id = staged_next_id++;
            annotation.surface = bounded_string(
                value, "surface", 160, "Workspace");
            annotation.anchor_window = bounded_string(
                value, "anchor_window", 160);
            annotation.plot_id = bounded_string(
                value, "plot_id", 80);
            annotation.plot_index =
                value.value("plot_index", -3);
            annotation.normalized_x = std::clamp(
                value.value("normalized_x", 0.5F), 0.0F, 1.0F);
            annotation.normalized_y = std::clamp(
                value.value("normalized_y", 0.5F), 0.0F, 1.0F);
            annotation.cursor_us =
                value.value("telemetry_time_us", Timestamp{});
            const auto view = value.value(
                "view_mode", std::string{"Single Lap"});
            annotation.view_mode =
                view == "Compare" ? ViewMode::Compare
                : view == "Continuous" ? ViewMode::Continuous
                                         : ViewMode::SingleLap;
            annotation.active_lap_index =
                value.value("active_lap_index", 0);
            annotation.compare_lap_index =
                value.value("compare_lap_index", 1);
            annotation.compare_b_lap_index =
                value.value("compare_b_lap_index", 2);
            annotation.compare_b_enabled =
                value.value("compare_b_enabled", false);
            annotation.playback_lap_index =
                value.value("playback_lap_index", 0);
            set_text_buffer(annotation.note, bounded_string(
                value, "comment", annotation.note.size() - 1));

            if (!session_->laps.empty()) {
                const auto maximum =
                    static_cast<int>(session_->laps.size()) - 1;
                annotation.active_lap_index = std::clamp(
                    annotation.active_lap_index, 0, maximum);
                annotation.compare_lap_index = std::clamp(
                    annotation.compare_lap_index, 0, maximum);
                annotation.compare_b_lap_index = std::clamp(
                    annotation.compare_b_lap_index, 0, maximum);
                annotation.playback_lap_index = std::clamp(
                    annotation.playback_lap_index, 0, maximum);
                if (!session_->telemetry.empty()) {
                    annotation.cursor_us = std::clamp(
                        annotation.cursor_us,
                        session_->telemetry.time_us.front(),
                        session_->telemetry.time_us.back());
                }
            }
            staged.push_back(std::move(annotation));
        }
        for (auto& annotation : staged) {
            annotations_.push_back(std::move(annotation));
        }
        next_annotation_id_ = staged_next_id;
        selected_annotation_id_ = first_imported_id;
        show_annotation_pins_ = true;
        status_ = std::format(
            "Imported {} annotation pin{} from {}",
            staged.size(), staged.size() == 1 ? "" : "s",
            path_utf8(path->filename()));
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
            {"format", "racebox-driver-analysis"}, {"version", 2},
            {"formula_version", driver_analysis_.formula_version}, {"session", session_->name},
            {"reference_raw_lap", driver_analysis_.reference.raw_lap},
            {"session_notes", std::string(session_notes_.data())},
            {"rules", nlohmann::json::array()}, {"corners", nlohmann::json::array()},
            {"comparisons", nlohmann::json::array()}, {"insights", nlohmann::json::array()},
            {"diagnostics", driver_analysis_.diagnostics}
        };
        const auto& evidence = analysis_rules_.evidence;
        output["evidence_parameters"] = {
            {"minimum_time_floor_s", evidence.minimum_time_floor_s},
            {"sample_period_multiplier", evidence.sample_period_multiplier},
            {"repeatability_sigma_multiplier", evidence.repeatability_sigma_multiplier},
            {"maximum_payback_fraction", evidence.maximum_payback_fraction},
            {"minimum_data_confidence", evidence.minimum_data_confidence},
            {"reliable_data_confidence", evidence.reliable_data_confidence},
            {"likely_comparable_laps", evidence.likely_comparable_laps},
            {"likely_supporting_laps", evidence.likely_supporting_laps},
            {"likely_support_rate", evidence.likely_support_rate},
            {"reliable_comparable_laps", evidence.reliable_comparable_laps},
            {"reliable_supporting_laps", evidence.reliable_supporting_laps},
            {"reliable_support_rate", evidence.reliable_support_rate}
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
            nlohmann::json insight_value{{"id", insight.id}, {"title", insight.title}, {"detail", insight.detail},
                {"corner_id", insight.corner_id}, {"corner_name", insight.corner_name},
                {"slot", insight.comparison == driver_analysis::ComparisonSlot::CompareA ? "compare_a" : "compare_b"},
                {"comparison_raw_lap", insight.comparison_raw_lap}, {"metric", driver_analysis::metric_name(insight.metric)},
                {"rule", insight.rule_stable_id}, {"measured_value", insight.measured_value},
                {"threshold", insight.threshold}, {"estimated_time_effect_s", insight.estimated_time_effect_s},
                {"positive", insight.positive}, {"severity", static_cast<int>(insight.severity)},
                {"confidence", insight.confidence}, {"confidence_band", static_cast<int>(insight.confidence_band)},
                {"outcome", insight_evidence::outcome_name(insight.outcome)},
                {"reliability", insight_evidence::reliability_name(insight.reliability)},
                {"recommendation", insight_evidence::recommendation_name(insight.recommendation)},
                {"prior_phase_effect_s", insight.prior_phase_effect_s}, {"local_effect_s", insight.local_effect_s},
                {"retained_effect_s", insight.retained_effect_s}, {"time_noise_floor_s", insight.time_noise_floor_s},
                {"retained_gain_s", insight.retained_gain_s}, {"comparable_laps", insight.comparable_laps},
                {"supporting_laps", insight.supporting_laps},
                {"median_retained_effect_s", insight.median_retained_effect_s ? nlohmann::json(*insight.median_retained_effect_s) : nlohmann::json(nullptr)},
                {"retained_interval_low_s", insight.retained_interval_low_s ? nlohmann::json(*insight.retained_interval_low_s) : nlohmann::json(nullptr)},
                {"retained_interval_high_s", insight.retained_interval_high_s ? nlohmann::json(*insight.retained_interval_high_s) : nlohmann::json(nullptr)},
                {"downstream_payback_fraction", insight.downstream_payback_fraction ? nlohmann::json(*insight.downstream_payback_fraction) : nlohmann::json(nullptr)},
                {"evidence_reasons", nlohmann::json::array()},
                {"reference_progress", insight.navigation.reference_progress},
                {"reference_timestamp_us", insight.navigation.reference_timestamp_us}};
            for (const auto reason : insight.evidence_reasons) {
                insight_value["evidence_reasons"].push_back(insight_evidence::reason_name(reason));
            }
            output["insights"].push_back(std::move(insight_value));
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
