#include "racebox/crew_chief.hpp"

#include "racebox/core.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace racebox::crew_chief {
namespace {

using nlohmann::json;

struct Summary {
    std::size_t samples{};
    double mean{};
    double mean_abs{};
    double rms{};
    double p95_abs{};
    double minimum{};
    double maximum{};
};

template <typename Getter>
Summary summarize(std::size_t begin, std::size_t end, Getter getter) {
    std::vector<double> values;
    if (end >= begin) values.reserve(end - begin + 1);
    for (auto index = begin; index <= end; ++index) {
        const auto value = static_cast<double>(getter(index));
        if (std::isfinite(value)) values.push_back(value);
    }
    if (values.empty()) return {};

    Summary result;
    result.samples = values.size();
    result.minimum = *std::min_element(values.begin(), values.end());
    result.maximum = *std::max_element(values.begin(), values.end());
    for (const auto value : values) {
        result.mean += value;
        result.mean_abs += std::abs(value);
        result.rms += value * value;
    }
    const auto count = static_cast<double>(values.size());
    result.mean /= count;
    result.mean_abs /= count;
    result.rms = std::sqrt(result.rms / count);
    std::vector<double> magnitudes;
    magnitudes.reserve(values.size());
    for (const auto value : values) magnitudes.push_back(std::abs(value));
    const auto rank = static_cast<std::size_t>(std::floor(0.95 * static_cast<double>(magnitudes.size() - 1)));
    std::nth_element(magnitudes.begin(), magnitudes.begin() + static_cast<std::ptrdiff_t>(rank), magnitudes.end());
    result.p95_abs = magnitudes[rank];
    return result;
}

json summary_json(const Summary& value, std::string_view unit) {
    return {
        {"samples", value.samples},
        {"mean", value.mean},
        {"mean_abs", value.mean_abs},
        {"rms", value.rms},
        {"p95_abs", value.p95_abs},
        {"minimum", value.minimum},
        {"maximum", value.maximum},
        {"unit", unit},
    };
}

json delta_json(const Summary& before, const Summary& after, std::string_view unit) {
    return {
        {"mean", after.mean - before.mean},
        {"mean_abs", after.mean_abs - before.mean_abs},
        {"rms", after.rms - before.rms},
        {"p95_abs", after.p95_abs - before.p95_abs},
        {"minimum", after.minimum - before.minimum},
        {"maximum", after.maximum - before.maximum},
        {"unit", unit},
    };
}

struct LapSummaries {
    Summary speed;
    Summary lateral;
    Summary longitudinal;
    Summary vertical;
    Summary yaw;
    Summary steering;
    Summary throttle;
    Summary brake;
    double throttle_active_percent{};
    double brake_active_percent{};
    double full_throttle_percent{};
    double satellite_mean{};
    std::uint8_t satellite_minimum{};
    std::size_t valid_radio_samples{};
};

LapSummaries summarize_lap(
    const Session& session,
    const LapInfo& lap,
    const imu::Analysis* imu_analysis) {
    const auto size = session.telemetry.size();
    if (size == 0) return {};
    const auto begin = std::min(lap.begin_index, size - 1);
    const auto end = std::min(std::max(lap.end_index, begin), size - 1);
    const auto acceleration_zero = imu_analysis && imu_analysis->available
        ? imu_analysis->acceleration_zero_g
        : std::array<double, 3>{};

    LapSummaries result;
    result.speed = summarize(begin, end, [&](const auto index) { return session.telemetry.speed_kmh[index]; });
    result.lateral = summarize(begin, end, [&](const auto index) {
        return session.telemetry.lateral_g[index] - acceleration_zero[1];
    });
    result.longitudinal = summarize(begin, end, [&](const auto index) {
        return session.telemetry.longitudinal_g[index] - acceleration_zero[0];
    });
    result.vertical = summarize(begin, end, [&](const auto index) {
        return session.telemetry.vertical_g[index] - acceleration_zero[2];
    });
    result.yaw = summarize(begin, end, [&](const auto index) {
        if (imu_analysis && imu_analysis->calibration.valid &&
            index < imu_analysis->vehicle_yaw_rate_dps.size()) {
            return static_cast<double>(imu_analysis->vehicle_yaw_rate_dps[index]);
        }
        return static_cast<double>(session.telemetry.gyro_z_dps[index]);
    });

    std::vector<double> steering;
    std::vector<double> throttle;
    std::vector<double> brake;
    steering.reserve(end - begin + 1);
    throttle.reserve(end - begin + 1);
    brake.reserve(end - begin + 1);
    std::size_t throttle_active = 0;
    std::size_t brake_active = 0;
    std::size_t full_throttle = 0;
    double satellite_sum = 0.0;
    auto satellite_minimum = std::numeric_limits<std::uint8_t>::max();
    for (auto index = begin; index <= end; ++index) {
        const auto controls = sample_radio(session, session.telemetry.time_us[index]);
        if (controls.valid) {
            steering.push_back(controls.steering);
            throttle.push_back(controls.throttle);
            brake.push_back(controls.brake);
            throttle_active += controls.throttle > 10.0F;
            brake_active += controls.brake > 10.0F;
            full_throttle += controls.throttle > 90.0F;
        }
        const auto satellites = session.telemetry.satellites[index];
        satellite_sum += satellites;
        satellite_minimum = std::min(satellite_minimum, satellites);
    }
    const auto vector_summary = [](const std::vector<double>& values) {
        if (values.empty()) return Summary{};
        return summarize(std::size_t{}, values.size() - 1, [&](const auto index) { return values[index]; });
    };
    result.steering = vector_summary(steering);
    result.throttle = vector_summary(throttle);
    result.brake = vector_summary(brake);
    result.valid_radio_samples = steering.size();
    if (!steering.empty()) {
        const auto count = static_cast<double>(steering.size());
        result.throttle_active_percent = 100.0 * static_cast<double>(throttle_active) / count;
        result.brake_active_percent = 100.0 * static_cast<double>(brake_active) / count;
        result.full_throttle_percent = 100.0 * static_cast<double>(full_throttle) / count;
    }
    const auto telemetry_count = static_cast<double>(end - begin + 1);
    result.satellite_mean = satellite_sum / telemetry_count;
    result.satellite_minimum =
        satellite_minimum == std::numeric_limits<std::uint8_t>::max() ? 0 : satellite_minimum;
    return result;
}

json lap_json(const LapInfo& lap, const LapSummaries& value, const imu::Analysis* imu_analysis) {
    return {
        {"raw_lap", lap.raw_lap},
        {"race_lap", lap.race_lap},
        {"duration_s", static_cast<double>(lap.duration_us) / 1'000'000.0},
        {"complete", lap.phase == LapPhase::Complete},
        {"telemetry_samples", value.speed.samples},
        {"radio_samples", value.valid_radio_samples},
        {"satellites", {{"mean", value.satellite_mean}, {"minimum", value.satellite_minimum}}},
        {"channels", {
            {"speed", summary_json(value.speed, "km/h")},
            {"lateral_load", summary_json(value.lateral, "g")},
            {"longitudinal_load", summary_json(value.longitudinal, "g")},
            {"vertical_load", summary_json(value.vertical, "g")},
            {"yaw_rate", summary_json(value.yaw, "deg/s")},
            {"steering", summary_json(value.steering, "percent")},
            {"throttle", summary_json(value.throttle, "percent")},
            {"brake", summary_json(value.brake, "percent")},
        }},
        {"time_percent", {
            {"throttle_above_10", value.throttle_active_percent},
            {"brake_above_10", value.brake_active_percent},
            {"throttle_above_90", value.full_throttle_percent},
        }},
        {"imu", {
            {"available", imu_analysis && imu_analysis->available},
            {"stationary_zero_used", imu_analysis && imu_analysis->initial_stationary_zero_used},
            {"vehicle_yaw_calibrated", imu_analysis && imu_analysis->calibration.valid},
        }},
    };
}

json lap_delta_json(const LapSummaries& before, const LapSummaries& after) {
    return {
        {"duration_s", nullptr},
        {"channels", {
            {"speed", delta_json(before.speed, after.speed, "km/h")},
            {"lateral_load", delta_json(before.lateral, after.lateral, "g")},
            {"longitudinal_load", delta_json(before.longitudinal, after.longitudinal, "g")},
            {"vertical_load", delta_json(before.vertical, after.vertical, "g")},
            {"yaw_rate", delta_json(before.yaw, after.yaw, "deg/s")},
            {"steering", delta_json(before.steering, after.steering, "percent")},
            {"throttle", delta_json(before.throttle, after.throttle, "percent")},
            {"brake", delta_json(before.brake, after.brake, "percent")},
        }},
        {"time_percent", {
            {"throttle_above_10", after.throttle_active_percent - before.throttle_active_percent},
            {"brake_above_10", after.brake_active_percent - before.brake_active_percent},
            {"throttle_above_90", after.full_throttle_percent - before.full_throttle_percent},
        }},
    };
}

std::string bounded_string(const json& value, const char* key, std::size_t maximum) {
    auto text = value.value(key, std::string{});
    if (text.size() > maximum) text.resize(maximum);
    return text;
}

std::optional<double> optional_finite_number(const json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || found->is_null() || !found->is_number()) return std::nullopt;
    const auto number = found->get<double>();
    return std::isfinite(number) ? std::optional<double>(number) : std::nullopt;
}

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("Crew Chief URL is not valid UTF-8");
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        output.data(), size);
    return output;
}

class InternetHandle {
public:
    explicit InternetHandle(HINTERNET value = nullptr) : value_(value) {}
    ~InternetHandle() { if (value_) WinHttpCloseHandle(value_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    [[nodiscard]] HINTERNET get() const noexcept { return value_; }
private:
    HINTERNET value_{};
};

std::string read_response(HINTERNET request) {
    std::string response;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            throw std::runtime_error("Crew Chief response could not be read");
        }
        if (available == 0) break;
        const auto previous = response.size();
        response.resize(previous + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, response.data() + previous, available, &read)) {
            throw std::runtime_error("Crew Chief response could not be read");
        }
        response.resize(previous + read);
        if (response.size() > 2'000'000) throw std::runtime_error("Crew Chief response was too large");
    }
    return response;
}

}  // namespace

nlohmann::json build_evidence_packet(
    const Session& session,
    const LapInfo& reference,
    const LapInfo& after,
    const driver_analysis::AnalysisResult& analysis,
    driver_analysis::ComparisonSlot slot,
    const imu::Analysis* imu_analysis) {
    const auto before_summary = summarize_lap(session, reference, imu_analysis);
    const auto after_summary = summarize_lap(session, after, imu_analysis);
    auto deltas = lap_delta_json(before_summary, after_summary);
    deltas["duration_s"] =
        static_cast<double>(after.duration_us - reference.duration_us) / 1'000'000.0;

    json packet{
        {"contract", kEvidenceContractVersion},
        {"formula_version", analysis.formula_version},
        {"session", {
            {"name", session.name},
            {"radio_alignment", {
                {"confidence", session.alignment.confidence},
                {"score", session.alignment.merge_confidence},
                {"compatible", session.alignment.compatible},
            }},
        }},
        {"roles", {
            {"before", "reference"},
            {"after", slot == driver_analysis::ComparisonSlot::CompareA ? "compare_a" : "compare_b"},
        }},
        {"before", lap_json(reference, before_summary, imu_analysis)},
        {"after", lap_json(after, after_summary, imu_analysis)},
        {"after_minus_before", std::move(deltas)},
        {"corner_metrics", json::array()},
        {"verified_insights", json::array()},
        {"quality", json::object()},
        {"interpretation_limits", {
            "Values describe association between the selected laps, not proof that the setup change caused them.",
            "Acceleration channels are chassis IMU measurements, not individual tire or suspension loads.",
            "Driver inputs, traffic, surface, battery, tires, weather, damage, and line choice can confound the result.",
            "Possible instability and airborne labels are indicators only; wheel-speed and suspension sensors are unavailable.",
        }},
    };

    const auto comparison_index = slot == driver_analysis::ComparisonSlot::CompareA ? 0U : 1U;
    if (analysis.comparisons[comparison_index]) {
        const auto& comparison = *analysis.comparisons[comparison_index];
        packet["quality"] = {
            {"confidence", comparison.confidence.overall},
            {"satellite_score", comparison.confidence.satellite_score},
            {"sampling_score", comparison.confidence.sampling_score},
            {"repeatability_score", comparison.confidence.repeatability_score},
            {"event_clarity_score", comparison.confidence.event_clarity_score},
            {"gps_translation_m", comparison.line_translation.magnitude_m},
            {"gps_residual_rms_m", comparison.line_translation.residual_rms_m},
            {"line_metrics_enabled", comparison.confidence.line_metrics_enabled},
        };
        for (const auto& corner : comparison.corners) {
            json corner_value{
                {"id", corner.corner_id},
                {"name", corner.corner_name},
                {"start_distance_m", corner.start_distance_m},
                {"end_distance_m", corner.end_distance_m},
                {"metrics", json::array()},
            };
            for (const auto& metric : corner.metrics) {
                corner_value["metrics"].push_back({
                    {"evidence_id", std::string("metric:") + corner.corner_id + ":" +
                        std::string(driver_analysis::metric_name(metric.kind))},
                    {"name", driver_analysis::metric_name(metric.kind)},
                    {"value", metric.value ? json(*metric.value) : json(nullptr)},
                    {"threshold", metric.threshold},
                    {"triggered", metric.triggered},
                    {"positive", metric.positive},
                    {"confidence", metric.confidence},
                });
            }
            packet["corner_metrics"].push_back(std::move(corner_value));
        }
    }
    for (const auto& insight : analysis.insights) {
        if (insight.comparison != slot) continue;
        json reasons = json::array();
        for (const auto reason : insight.evidence_reasons) {
            reasons.push_back(insight_evidence::reason_name(reason));
        }
        packet["verified_insights"].push_back({
            {"evidence_id", insight.id},
            {"title", insight.title},
            {"detail", insight.detail},
            {"corner", insight.corner_name},
            {"metric", driver_analysis::metric_name(insight.metric)},
            {"outcome", insight_evidence::outcome_name(insight.outcome)},
            {"reliability", insight_evidence::reliability_name(insight.reliability)},
            {"recommendation", insight_evidence::recommendation_name(insight.recommendation)},
            {"confidence", insight.confidence},
            {"same_result_laps", insight.supporting_laps},
            {"comparable_laps", insight.comparable_laps},
            {"retained_effect_s", insight.retained_effect_s},
            {"normal_variation_s", insight.time_noise_floor_s},
            {"evidence_reasons", std::move(reasons)},
        });
    }
    return packet;
}

Report parse_report(const nlohmann::json& value) {
    const auto* report_value = &value;
    if (const auto report = value.find("report"); report != value.end() && report->is_object()) {
        report_value = &*report;
    }
    if (!report_value->is_object()) throw std::runtime_error("Crew Chief response did not contain a report");
    Report result;
    result.verdict = bounded_string(*report_value, "verdict", 32);
    static constexpr std::array<std::string_view, 5> verdicts{
        "supported", "mixed", "inconclusive", "not_supported", "data_limited"};
    if (std::find(verdicts.begin(), verdicts.end(), result.verdict) == verdicts.end()) {
        result.verdict = "inconclusive";
    }
    result.confidence = std::clamp(report_value->value("confidence", 0), 0, 100);
    result.summary = bounded_string(*report_value, "summary", 4000);
    result.next_test = bounded_string(*report_value, "next_test", 2000);
    result.causality_note = bounded_string(*report_value, "causality_note", 1200);
    result.route = bounded_string(value, "route", 160);
    result.model = bounded_string(value, "model", 160);
    result.thinking = bounded_string(value, "thinking", 32);
    if (const auto routing = value.find("routing"); routing != value.end() && routing->is_object()) {
        result.selected_lane = bounded_string(*routing, "selected_lane", 80);
    }
    if (const auto ids = value.find("prior_setup_result_ids"); ids != value.end() && ids->is_array()) {
        for (const auto& id : *ids) {
            if (!id.is_string() || result.prior_setup_result_ids.size() >= 8) break;
            auto text = id.get<std::string>();
            if (text.size() > 160) text.resize(160);
            result.prior_setup_result_ids.push_back(std::move(text));
        }
    }
    if (const auto summary = value.find("evidence_summary");
        summary != value.end() && summary->is_object()) {
        result.evidence.analytics_contract = bounded_string(*summary, "analytics_contract", 120);
        result.evidence.formula_version = std::clamp(summary->value("formula_version", 0), 0, 10'000);
        result.evidence.quality_confidence =
            std::clamp(summary->value("quality_confidence", 0), 0, 100);
        result.evidence.track_status = bounded_string(*summary, "track_status", 80);
        result.evidence.track_translation_m = optional_finite_number(*summary, "track_translation_m");
        result.evidence.track_residual_rms_m =
            optional_finite_number(*summary, "track_residual_rms_m");
        result.evidence.lap_time_delta_s = optional_finite_number(*summary, "lap_time_delta_s");
        result.evidence.lateral_status = bounded_string(*summary, "lateral_status", 80);
        result.evidence.lateral_response_delta_g =
            optional_finite_number(*summary, "lateral_response_delta_g");
        result.evidence.forward_status = bounded_string(*summary, "forward_status", 80);
        result.evidence.forward_bite_delta_g =
            optional_finite_number(*summary, "forward_bite_delta_g");
        result.evidence.previous_brake_indicators =
            std::clamp(summary->value("previous_brake_indicators", 0), 0, 100'000);
        result.evidence.current_brake_indicators =
            std::clamp(summary->value("current_brake_indicators", 0), 0, 100'000);
        result.evidence.top_speed_status = bounded_string(*summary, "top_speed_status", 80);
        result.evidence.straight_top_speed_delta_kmh =
            optional_finite_number(*summary, "straight_top_speed_delta_kmh");
        result.evidence.straight_entry_speed_delta_kmh =
            optional_finite_number(*summary, "straight_entry_speed_delta_kmh");
        result.evidence.straight_acceleration_delta_g =
            optional_finite_number(*summary, "straight_acceleration_delta_g");
        result.evidence.straight_speed_attribution =
            bounded_string(*summary, "straight_speed_attribution", 120);
        result.evidence.brake_response_status = bounded_string(*summary, "brake_response_status", 80);
        result.evidence.brake_decel_delta_g = optional_finite_number(*summary, "brake_decel_delta_g");
        result.evidence.brake_response_delay_delta_s =
            optional_finite_number(*summary, "brake_response_delay_delta_s");
        result.evidence.corner_balance_status = bounded_string(*summary, "corner_balance_status", 80);
        result.evidence.steering_for_lateral_g_delta_percent =
            optional_finite_number(*summary, "steering_for_lateral_g_delta_percent");
        result.evidence.yaw_per_steering_delta_dps =
            optional_finite_number(*summary, "yaw_per_steering_delta_dps");
        result.evidence.overdriving_status = bounded_string(*summary, "overdriving_status", 80);
        result.evidence.overdriving_index_delta =
            optional_finite_number(*summary, "overdriving_index_delta");
        result.evidence.overdriving_risk_sample_delta_percent =
            optional_finite_number(*summary, "overdriving_risk_sample_delta_percent");
        result.evidence.current_late_overdriving_delta_score =
            optional_finite_number(*summary, "current_late_overdriving_delta_score");
        result.evidence.chassis_roll_status = bounded_string(*summary, "chassis_roll_status", 80);
        result.evidence.surface_tilt_delta_deg =
            optional_finite_number(*summary, "surface_tilt_delta_deg");
        result.evidence.chassis_roll_delta_deg =
            optional_finite_number(*summary, "chassis_roll_delta_deg");
        result.evidence.roll_per_lateral_g_delta_deg =
            optional_finite_number(*summary, "roll_per_lateral_g_delta_deg");
        result.evidence.roll_rate_status = bounded_string(*summary, "roll_rate_status", 80);
        result.evidence.previous_roll_rate_p90_dps =
            optional_finite_number(*summary, "previous_roll_rate_p90_dps");
        result.evidence.current_roll_rate_p90_dps =
            optional_finite_number(*summary, "current_roll_rate_p90_dps");
        result.evidence.roll_rate_delta_dps =
            optional_finite_number(*summary, "roll_rate_delta_dps");
        result.evidence.roll_rate_per_lateral_g_delta_dps =
            optional_finite_number(*summary, "roll_rate_per_lateral_g_delta_dps");
        result.evidence.current_late_roll_rate_delta_dps =
            optional_finite_number(*summary, "current_late_roll_rate_delta_dps");
        result.evidence.previous_roll_rate_samples =
            std::clamp(summary->value("previous_roll_rate_samples", 0), 0, 100'000);
        result.evidence.current_roll_rate_samples =
            std::clamp(summary->value("current_roll_rate_samples", 0), 0, 100'000);
        result.evidence.current_surface_tilt_source =
            bounded_string(*summary, "current_surface_tilt_source", 80);
        result.evidence.current_surface_tilt_samples =
            std::clamp(summary->value("current_surface_tilt_samples", 0), 0, 100'000);
    }
    if (const auto observations = report_value->find("observations");
        observations != report_value->end() && observations->is_array()) {
        for (const auto& item : *observations) {
            if (!item.is_object() || result.observations.size() >= 8) break;
            Observation observation;
            observation.area = bounded_string(item, "area", 120);
            observation.change = bounded_string(item, "change", 800);
            observation.meaning = bounded_string(item, "meaning", 1400);
            if (const auto ids = item.find("evidence_ids"); ids != item.end() && ids->is_array()) {
                for (const auto& id : *ids) {
                    if (!id.is_string() || observation.evidence_ids.size() >= 8) break;
                    auto text = id.get<std::string>();
                    if (text.size() > 180) text.resize(180);
                    observation.evidence_ids.push_back(std::move(text));
                }
            }
            result.observations.push_back(std::move(observation));
        }
    }
    if (const auto confounds = report_value->find("confounds");
        confounds != report_value->end() && confounds->is_array()) {
        for (const auto& item : *confounds) {
            if (!item.is_string() || result.confounds.size() >= 8) break;
            auto text = item.get<std::string>();
            if (text.size() > 800) text.resize(800);
            result.confounds.push_back(std::move(text));
        }
    }
    if (result.summary.empty()) throw std::runtime_error("Crew Chief response summary was empty");
    return result;
}

namespace {

Response post_report_request(
    std::string_view endpoint,
    std::string_view bearer_token,
    const json& body,
    std::size_t maximum_body_bytes,
    std::uint32_t timeout_ms) {
    Response result;
    try {
        if (endpoint.empty()) throw std::runtime_error("Crew Chief endpoint is empty");
        const auto body_text = body.dump();
        if (body_text.size() > maximum_body_bytes) {
            throw std::runtime_error("Crew Chief request was too large");
        }

        const auto endpoint_wide = widen(endpoint);
        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwSchemeLength = static_cast<DWORD>(-1);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(endpoint_wide.c_str(), static_cast<DWORD>(endpoint_wide.size()), 0, &components)) {
            throw std::runtime_error("Crew Chief endpoint URL is invalid");
        }
        const std::wstring host(components.lpszHostName, components.dwHostNameLength);
        std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
        if (components.dwExtraInfoLength > 0) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
        if (path.empty()) path = L"/";
        const auto secure = components.nScheme == INTERNET_SCHEME_HTTPS;

        InternetHandle session(WinHttpOpen(L"RaceBoxTelemetryViewer/2 CrewChief",
            WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session.get()) throw std::runtime_error("Crew Chief network session could not start");
        WinHttpSetTimeouts(session.get(), 10'000, 10'000,
                           static_cast<int>(timeout_ms), static_cast<int>(timeout_ms));
        InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
        if (!connection.get()) throw std::runtime_error("Crew Chief server could not be reached");
        InternetHandle request(WinHttpOpenRequest(connection.get(), L"POST", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
        if (!request.get()) throw std::runtime_error("Crew Chief request could not be created");
        std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
        if (!bearer_token.empty()) {
            headers += L"Authorization: Bearer ";
            headers += widen(bearer_token);
            headers += L"\r\n";
        }
        if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1),
            const_cast<char*>(body_text.data()), static_cast<DWORD>(body_text.size()),
            static_cast<DWORD>(body_text.size()), 0) ||
            !WinHttpReceiveResponse(request.get(), nullptr)) {
            throw std::runtime_error("Crew Chief server did not answer");
        }
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
        result.http_status = static_cast<int>(status);
        const auto response_text = read_response(request.get());
        if (status < 200 || status >= 300) {
            std::string detail = response_text.substr(0, 500);
            try {
                const auto error_value = json::parse(response_text);
                detail = error_value.value("error", error_value.value("detail", detail));
            } catch (...) {
            }
            throw std::runtime_error("Crew Chief returned HTTP " + std::to_string(status) + ": " + detail);
        }
        result.report = parse_report(json::parse(response_text));
        result.ok = true;
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

json bounded_history(std::span<const ChatTurn> history) {
    json history_value = json::array();
    const auto first = history.size() > 10 ? history.size() - 10 : 0;
    for (auto index = first; index < history.size(); ++index) {
        history_value.push_back({
            {"role", history[index].role == "assistant" ? "assistant" : "user"},
            {"content", history[index].content.substr(0, 4000)},
        });
    }
    return history_value;
}

}  // namespace

Response request_report(
    std::string_view endpoint,
    std::string_view bearer_token,
    std::string_view setup_change,
    std::string_view question,
    const nlohmann::json& evidence,
    const nlohmann::json& prior_setup_results,
    std::span<const ChatTurn> history,
    std::uint32_t timeout_ms) {
    try {
        const json body{
            {"setup_change", std::string(setup_change).substr(0, 4000)},
            {"question", std::string(question).substr(0, 4000)},
            {"evidence", evidence},
            {"prior_setup_results", prior_setup_results.is_array()
                ? prior_setup_results : json::array()},
            {"history", bounded_history(history)},
        };
        return post_report_request(endpoint, bearer_token, body, 1'500'000, timeout_ms);
    } catch (const std::exception& exception) {
        Response result;
        result.error = exception.what();
        return result;
    }
}

Response request_race_day_report(
    std::string_view endpoint,
    std::string_view bearer_token,
    const nlohmann::json& previous_context,
    std::string_view previous_analytics_csv,
    const nlohmann::json& current_context,
    std::string_view current_analytics_csv,
    std::string_view question,
    const nlohmann::json& prior_setup_results,
    std::span<const ChatTurn> history,
    std::uint32_t timeout_ms) {
    try {
        const json body{
            {"contract", kRaceDayRequestVersion},
            {"question", std::string(question).substr(0, 4000)},
            {"previous", {
                {"context", previous_context},
                {"analytics_csv", previous_analytics_csv},
            }},
            {"current", {
                {"context", current_context},
                {"analytics_csv", current_analytics_csv},
            }},
            {"prior_setup_results", prior_setup_results.is_array()
                ? prior_setup_results : json::array()},
            {"history", bounded_history(history)},
        };
        return post_report_request(endpoint, bearer_token, body, 12'000'000, timeout_ms);
    } catch (const std::exception& exception) {
        Response result;
        result.error = exception.what();
        return result;
    }
}

}  // namespace racebox::crew_chief
