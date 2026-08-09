#include "racebox/crew_chief.hpp"

#include "racebox/core.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
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

LapSummaries summarize_range(
    const Session& session,
    std::size_t begin,
    std::size_t end,
    const imu::Analysis* imu_analysis) {
    const auto size = session.telemetry.size();
    if (size == 0) return {};
    begin = std::min(begin, size - 1);
    end = std::min(std::max(end, begin), size - 1);
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

LapSummaries summarize_lap(
    const Session& session,
    const LapInfo& lap,
    const imu::Analysis* imu_analysis) {
    return summarize_range(
        session, lap.begin_index, lap.end_index, imu_analysis);
}

std::pair<std::size_t, std::size_t> lap_progress_range(
    const Session& session,
    const LapInfo& lap,
    double start_progress,
    double end_progress) {
    if (session.telemetry.empty() || lap.end_index <= lap.begin_index) {
        return {lap.begin_index, lap.end_index};
    }
    const auto count = lap.end_index - lap.begin_index + 1;
    std::vector<double> distance(count);
    for (std::size_t offset = 1; offset < count; ++offset) {
        const auto previous = lap.begin_index + offset - 1;
        const auto current = lap.begin_index + offset;
        const auto average_latitude =
            (session.telemetry.latitude[previous] +
             session.telemetry.latitude[current]) * 0.5;
        constexpr double degrees_to_radians =
            3.14159265358979323846 / 180.0;
        const auto longitude_scale =
            111'320.0 * std::cos(average_latitude * degrees_to_radians);
        const auto east =
            (session.telemetry.longitude[current] -
             session.telemetry.longitude[previous]) * longitude_scale;
        const auto north =
            (session.telemetry.latitude[current] -
             session.telemetry.latitude[previous]) * 110'540.0;
        const auto step = std::hypot(east, north);
        distance[offset] = distance[offset - 1] +
            (std::isfinite(step) && step < 20.0 ? step : 0.0);
    }
    const auto total = distance.back();
    if (!std::isfinite(total) || total <= 0.001) {
        return {lap.begin_index, lap.end_index};
    }
    const auto index_at = [&](double progress) {
        const auto target = std::clamp(progress, 0.0, 1.0) * total;
        const auto found = std::lower_bound(distance.begin(), distance.end(), target);
        const auto offset = found == distance.end()
            ? distance.size() - 1
            : static_cast<std::size_t>(std::distance(distance.begin(), found));
        return lap.begin_index + offset;
    };
    const auto begin = index_at(start_progress);
    const auto end = std::max(begin, index_at(end_progress));
    return {begin, end};
}

LapSummaries summarize_lap_progress(
    const Session& session,
    const LapInfo& lap,
    double start_progress,
    double end_progress,
    const imu::Analysis* imu_analysis) {
    const auto [begin, end] = lap_progress_range(
        session, lap, start_progress, end_progress);
    return summarize_range(session, begin, end, imu_analysis);
}

json channel_summaries_json(const LapSummaries& value) {
    return {
        {"speed", summary_json(value.speed, "km/h")},
        {"lateral_load", summary_json(value.lateral, "g")},
        {"longitudinal_load", summary_json(value.longitudinal, "g")},
        {"vertical_load", summary_json(value.vertical, "g")},
        {"yaw_rate", summary_json(value.yaw, "deg/s")},
        {"steering", summary_json(value.steering, "percent")},
        {"throttle", summary_json(value.throttle, "percent")},
        {"brake", summary_json(value.brake, "percent")},
    };
}

json active_time_json(const LapSummaries& value) {
    return {
        {"throttle_above_10", value.throttle_active_percent},
        {"brake_above_10", value.brake_active_percent},
        {"throttle_above_90", value.full_throttle_percent},
    };
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
        {"channels", channel_summaries_json(value)},
        {"time_percent", active_time_json(value)},
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
            const auto zone = std::find_if(
                analysis.corners.begin(), analysis.corners.end(),
                [&](const driver_analysis::CornerZone& candidate) {
                    return candidate.id == corner.corner_id;
                });
            if (zone != analysis.corners.end()) {
                const auto before_corner = summarize_lap_progress(
                    session, reference, zone->start_progress,
                    zone->end_progress, imu_analysis);
                const auto after_corner = summarize_lap_progress(
                    session, after, zone->start_progress,
                    zone->end_progress, imu_analysis);
                corner_value["channel_evidence"] = {
                    {"distance_aligned_window", true},
                    {"start_progress_percent", zone->start_progress * 100.0},
                    {"end_progress_percent", zone->end_progress * 100.0},
                    {"reference", {
                        {"telemetry_samples", before_corner.speed.samples},
                        {"radio_samples", before_corner.valid_radio_samples},
                        {"channels", channel_summaries_json(before_corner)},
                        {"time_percent", active_time_json(before_corner)},
                    }},
                    {"compare", {
                        {"telemetry_samples", after_corner.speed.samples},
                        {"radio_samples", after_corner.valid_radio_samples},
                        {"channels", channel_summaries_json(after_corner)},
                        {"time_percent", active_time_json(after_corner)},
                    }},
                    {"compare_minus_reference", lap_delta_json(
                        before_corner, after_corner)},
                };
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
            {"driver_coaching", insight.coaching},
            {"corner", insight.corner_name},
            {"turn_direction", insight.turn_direction},
            {"control_evidence", insight.controls_measured ? "radio_measured" : "suggestion_only_no_radio_inputs"},
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

struct JsonHttpResponse {
    bool ok{};
    int status{};
    json value;
    std::string error;
};

std::string companion_endpoint(std::string_view endpoint, std::string_view path) {
    const auto scheme = endpoint.find("://");
    if (scheme == std::string_view::npos) {
        throw std::runtime_error("Crew Chief endpoint URL is invalid");
    }
    const auto path_begin = endpoint.find('/', scheme + 3);
    const auto base = path_begin == std::string_view::npos
        ? endpoint : endpoint.substr(0, path_begin);
    if (base.empty() || path.empty() || path.front() != '/') {
        throw std::runtime_error("Crew Chief companion path is invalid");
    }
    return std::string(base) + std::string(path);
}

JsonHttpResponse request_json(
    std::string_view method,
    std::string_view endpoint,
    std::string_view bearer_token,
    const json* body,
    std::size_t maximum_body_bytes,
    std::uint32_t timeout_ms) {
    JsonHttpResponse result;
    try {
        const auto body_text = body ? body->dump() : std::string{};
        if (body_text.size() > maximum_body_bytes) {
            throw std::runtime_error("Crew Chief companion request was too large");
        }
        const auto endpoint_wide = widen(endpoint);
        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwSchemeLength = static_cast<DWORD>(-1);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(endpoint_wide.c_str(), static_cast<DWORD>(endpoint_wide.size()), 0, &components)) {
            throw std::runtime_error("Crew Chief companion URL is invalid");
        }
        const std::wstring host(components.lpszHostName, components.dwHostNameLength);
        std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
        if (components.dwExtraInfoLength > 0) {
            path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
        }
        if (path.empty()) path = L"/";
        const auto secure = components.nScheme == INTERNET_SCHEME_HTTPS;
        InternetHandle session(WinHttpOpen(L"RaceBoxTelemetryViewer/2 Companion",
            WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session.get()) throw std::runtime_error("Crew Chief companion network session could not start");
        WinHttpSetTimeouts(session.get(), 10'000, 10'000,
                           static_cast<int>(timeout_ms), static_cast<int>(timeout_ms));
        InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
        if (!connection.get()) throw std::runtime_error("Crew Chief companion server could not be reached");
        const auto method_wide = widen(method);
        InternetHandle request(WinHttpOpenRequest(connection.get(), method_wide.c_str(), path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
        if (!request.get()) throw std::runtime_error("Crew Chief companion request could not be created");
        std::wstring headers = L"Accept: application/json\r\n";
        if (body) headers += L"Content-Type: application/json\r\n";
        if (!bearer_token.empty()) {
            headers += L"Authorization: Bearer ";
            headers += widen(bearer_token);
            headers += L"\r\n";
        }
        LPVOID data = body
            ? static_cast<LPVOID>(const_cast<char*>(body_text.data()))
            : WINHTTP_NO_REQUEST_DATA;
        const auto length = body ? static_cast<DWORD>(body_text.size()) : 0U;
        if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1),
            data, length, length, 0) || !WinHttpReceiveResponse(request.get(), nullptr)) {
            throw std::runtime_error("Crew Chief companion server did not answer");
        }
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
        result.status = static_cast<int>(status);
        const auto response_text = read_response(request.get());
        if (!response_text.empty()) result.value = json::parse(response_text);
        if (status < 200 || status >= 300) {
            result.error = result.value.value("error", response_text.substr(0, 500));
            return result;
        }
        result.ok = true;
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

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
        const auto response_value = json::parse(response_text);
        result.report = parse_report(response_value);
        if (const auto vision = response_value.find("setup_sheet_vision");
            vision != response_value.end() && vision->is_object()) {
            result.setup_sheet_vision.requested = vision->value("requested", false);
            result.setup_sheet_vision.available = vision->value("available", false);
            result.setup_sheet_vision.used = vision->value("used", false);
            result.setup_sheet_vision.retained = vision->value("retained", false);
            result.setup_sheet_vision.structured_values_used =
                vision->value("structured_values_used", false);
            result.setup_sheet_vision.image_count = std::min<std::size_t>(
                vision->value("image_count", std::size_t{}), 4);
        }
        result.ok = true;
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

json bounded_history(std::span<const ChatTurn> history) {
    json history_value = json::array();
    constexpr std::size_t kMaximumActiveTurns = 10;
    const auto first = history.size() > kMaximumActiveTurns
        ? history.size() - (kMaximumActiveTurns - 1) : 0;
    if (first > 0) {
        std::string memory = "Earlier conversation memory:";
        const auto memory_first = first > 16 ? first - 16 : 0;
        for (auto index = memory_first; index < first; ++index) {
            auto excerpt = history[index].content;
            if (excerpt.size() > 180) excerpt.resize(180);
            memory += "\n";
            memory += history[index].role == "assistant" ? "Crew Chief: " : "Driver: ";
            memory += excerpt;
            if (memory.size() >= 3900) break;
        }
        if (memory.size() > 4000) memory.resize(4000);
        history_value.push_back({
            {"role", "assistant"},
            {"content", std::move(memory)},
        });
    }
    for (auto index = first; index < history.size(); ++index) {
        history_value.push_back({
            {"role", history[index].role == "assistant" ? "assistant" : "user"},
            {"content", history[index].content.substr(0, 4000)},
        });
    }
    return history_value;
}

std::string session_question_for_router(std::string_view question) {
    std::string lowered(question);
    std::transform(
        lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const auto contains_any = [&](std::initializer_list<std::string_view> markers) {
        return std::any_of(
            markers.begin(), markers.end(),
            [&](std::string_view marker) {
                return lowered.find(marker) != std::string::npos;
            });
    };
    const auto driver_input = contains_any({
        "steering", "throttle", "brake input", "transmitter input"});
    const auto vehicle_response = contains_any({
        "lateral g", "longitudinal g", "cornering response", "rotation",
        "grip", "acceleration", "deceleration", "speed reduction"});
    auto routed = std::string(question).substr(0, 4000);
    if (driver_input && vehicle_response &&
        lowered.find("correlation") == std::string::npos &&
        lowered.find("correlate") == std::string::npos) {
        constexpr std::string_view prefix{
            "Correlation review requested for driver input versus measured vehicle response. "};
        const auto maximum_question = 4000 - prefix.size();
        routed = std::string(prefix) + routed.substr(0, maximum_question);
    }
    return routed;
}

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    static constexpr std::string_view alphabet{
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto remaining = bytes.size() - index;
        const auto value = (static_cast<std::uint32_t>(bytes[index]) << 16U) |
            (remaining > 1 ? static_cast<std::uint32_t>(bytes[index + 1]) << 8U : 0U) |
            (remaining > 2 ? static_cast<std::uint32_t>(bytes[index + 2]) : 0U);
        result.push_back(alphabet[(value >> 18U) & 0x3fU]);
        result.push_back(alphabet[(value >> 12U) & 0x3fU]);
        result.push_back(remaining > 1 ? alphabet[(value >> 6U) & 0x3fU] : '=');
        result.push_back(remaining > 2 ? alphabet[value & 0x3fU] : '=');
    }
    return result;
}

std::string sha256_hex(std::span<const std::uint8_t> bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("Could not verify setup-sheet image integrity");
    }
    std::array<std::uint8_t, 32> digest{};
    const auto status = BCryptHash(
        algorithm, nullptr, 0,
        const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(bytes.data())),
        static_cast<ULONG>(bytes.size()), digest.data(),
        static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        throw std::runtime_error("Could not verify setup-sheet image integrity");
    }
    static constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result[index * 2] = hexadecimal[digest[index] >> 4U];
        result[index * 2 + 1] = hexadecimal[digest[index] & 0x0fU];
    }
    return result;
}

json setup_sheet_vision_json(const SetupSheetVision& vision) {
    constexpr std::size_t maximum_image_bytes = 3 * 1024 * 1024;
    constexpr std::size_t maximum_total_bytes = 8 * 1024 * 1024;
    std::size_t total_bytes = 0;
    const auto run_json = [&](const SetupSheetVisionRun& run) {
        if (run.revision_id.empty() || run.revision_id.size() > 160) {
            throw std::runtime_error("Setup-sheet image revision is missing or invalid");
        }
        if (run.pages.empty() || run.pages.size() > 2) {
            throw std::runtime_error("Setup-sheet vision needs one or two pages per run");
        }
        json pages = json::array();
        std::vector<int> seen_pages;
        for (const auto& page : run.pages) {
            if (page.page < 1 || page.page > 100 ||
                std::find(seen_pages.begin(), seen_pages.end(), page.page) != seen_pages.end()) {
                throw std::runtime_error("Setup-sheet image page number is invalid");
            }
            seen_pages.push_back(page.page);
            if (page.mime_type != "image/png" && page.mime_type != "image/jpeg") {
                throw std::runtime_error("Setup-sheet image must be PNG or JPEG");
            }
            if (page.bytes.empty() || page.bytes.size() > maximum_image_bytes) {
                throw std::runtime_error("Setup-sheet image is too large");
            }
            total_bytes += page.bytes.size();
            if (total_bytes > maximum_total_bytes) {
                throw std::runtime_error("Setup-sheet images are too large");
            }
            pages.push_back({
                {"page", page.page},
                {"mime_type", page.mime_type},
                {"sha256", sha256_hex(page.bytes)},
                {"data_base64", base64_encode(page.bytes)},
            });
        }
        return json{
            {"revision_id", run.revision_id},
            {"pages", std::move(pages)},
        };
    };
    return {
        {"contract", kSetupSheetVisionVersion},
        {"previous", run_json(vision.previous)},
        {"current", run_json(vision.current)},
    };
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
    std::uint32_t timeout_ms,
    const SetupSheetVision* setup_sheet_vision) {
    try {
        const auto body = build_race_day_request(
            previous_context, previous_analytics_csv,
            current_context, current_analytics_csv, question,
            prior_setup_results, history, setup_sheet_vision);
        return post_report_request(endpoint, bearer_token, body, 24'000'000, timeout_ms);
    } catch (const std::exception& exception) {
        Response result;
        result.error = exception.what();
        return result;
    }
}

nlohmann::json build_race_day_request(
    const nlohmann::json& previous_context,
    std::string_view previous_analytics_csv,
    const nlohmann::json& current_context,
    std::string_view current_analytics_csv,
    std::string_view question,
    const nlohmann::json& prior_setup_results,
    std::span<const ChatTurn> history,
    const SetupSheetVision* setup_sheet_vision) {
    json body{
        {"contract", kRaceDayRequestVersion},
        {"question", std::string(question).substr(0, 4000)},
        {"previous", {
            {"context", previous_context.is_object() ? previous_context : json::object()},
            {"analytics_csv", previous_analytics_csv},
        }},
        {"current", {
            {"context", current_context.is_object() ? current_context : json::object()},
            {"analytics_csv", current_analytics_csv},
        }},
        {"prior_setup_results", prior_setup_results.is_array()
            ? prior_setup_results : json::array()},
        {"history", bounded_history(history)},
    };
    if (setup_sheet_vision) {
        body["setup_sheet_vision"] = setup_sheet_vision_json(*setup_sheet_vision);
    }
    return body;
}

nlohmann::json build_session_chat_request(
    const nlohmann::json& session_context,
    std::string_view analytics_csv,
    std::string_view question,
    const nlohmann::json& prior_setup_results,
    std::span<const ChatTurn> history) {
    return json{
        {"contract", kSessionChatRequestVersion},
        {"question", session_question_for_router(question)},
        {"session", {
            {"context", session_context.is_object() ? session_context : json::object()},
            {"analytics_csv", analytics_csv},
        }},
        {"prior_setup_results", prior_setup_results.is_array()
            ? prior_setup_results : json::array()},
        {"history", bounded_history(history)},
    };
}

Response request_session_chat(
    std::string_view endpoint,
    std::string_view bearer_token,
    const nlohmann::json& session_context,
    std::string_view analytics_csv,
    std::string_view question,
    const nlohmann::json& prior_setup_results,
    std::span<const ChatTurn> history,
    std::uint32_t timeout_ms) {
    try {
        const auto body = build_session_chat_request(
            session_context, analytics_csv, question, prior_setup_results, history);
        return post_report_request(endpoint, bearer_token, body, 7'500'000, timeout_ms);
    } catch (const std::exception& exception) {
        Response result;
        result.error = exception.what();
        return result;
    }
}

CompanionSessionResponse create_companion_session(
    std::string_view endpoint,
    std::string_view bearer_token,
    std::string_view title,
    const nlohmann::json& session_context,
    std::string_view analytics_csv,
    const nlohmann::json& prior_setup_results,
    std::span<const ChatTurn> history,
    std::uint32_t timeout_ms) {
    CompanionSessionResponse result;
    try {
        json history_value = json::array();
        const auto first = history.size() > 2'000 ? history.size() - 2'000 : 0;
        for (auto index = first; index < history.size(); ++index) {
            const auto& turn = history[index];
            history_value.push_back({
                {"id", turn.id.substr(0, 120)},
                {"role", turn.role == "assistant" ? "assistant" : "user"},
                {"content", turn.content.substr(0, 4000)},
                {"created_at", turn.created_at},
                {"origin", turn.origin.substr(0, 80)},
            });
        }
        const json body{
            {"contract", kCompanionContractVersion},
            {"title", std::string(title).substr(0, 200)},
            {"session", {
                {"context", session_context.is_object() ? session_context : json::object()},
                {"analytics_csv", analytics_csv},
            }},
            {"prior_setup_results", prior_setup_results.is_array()
                ? prior_setup_results : json::array()},
            {"history", std::move(history_value)},
        };
        const auto response = request_json(
            "POST", companion_endpoint(endpoint, "/v1/crew-chief/companion/sessions"),
            bearer_token, &body, 7'500'000, timeout_ms);
        result.http_status = response.status;
        result.error = response.error;
        if (!response.ok) return result;
        if (response.value.value("contract", std::string{}) != kCompanionContractVersion) {
            throw std::runtime_error("Crew Chief companion contract did not match");
        }
        result.session_id = response.value.value("session_id", std::string{}).substr(0, 120);
        result.viewer_token = response.value.value("viewer_token", std::string{}).substr(0, 512);
        result.pairing_id = response.value.value("pairing_id", std::string{}).substr(0, 120);
        result.pairing_code = response.value.value("pairing_code", std::string{}).substr(0, 12);
        result.pair_uri = response.value.value("pair_uri", std::string{}).substr(0, 1'000);
        result.pairing_expires_at = response.value.value("pairing_expires_at", std::int64_t{});
        if (result.session_id.empty() || result.viewer_token.empty() || result.pairing_code.empty()) {
            throw std::runtime_error("Crew Chief companion session response was incomplete");
        }
        result.ok = true;
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

CompanionMessagesResponse request_companion_messages(
    std::string_view endpoint,
    std::string_view session_id,
    std::string_view session_token,
    std::uint64_t after_cursor,
    std::uint32_t timeout_ms) {
    CompanionMessagesResponse result;
    try {
        const auto path = "/v1/crew-chief/companion/sessions/" + std::string(session_id) +
            "/messages?after=" + std::to_string(after_cursor);
        const auto response = request_json(
            "GET", companion_endpoint(endpoint, path), session_token,
            nullptr, 0, timeout_ms);
        result.http_status = response.status;
        result.error = response.error;
        if (!response.ok) return result;
        result.next_cursor = response.value.value("next_cursor", std::uint64_t{});
        result.total = std::min<std::size_t>(
            response.value.value("total", std::size_t{}), 2'000);
        if (const auto messages = response.value.find("messages");
            messages != response.value.end() && messages->is_array()) {
            for (const auto& item : *messages) {
                if (!item.is_object() || result.messages.size() >= 500) break;
                CompanionMessage message;
                message.cursor = item.value("cursor", std::uint64_t{});
                message.turn.id = item.value("id", std::string{}).substr(0, 120);
                message.turn.role = item.value("role", std::string{"user"}) == "assistant"
                    ? "assistant" : "user";
                message.turn.content = item.value("content", std::string{}).substr(0, 4000);
                message.turn.created_at = item.value("created_at", std::int64_t{});
                message.turn.origin = item.value("origin", std::string{}).substr(0, 80);
                if (const auto report = item.find("report");
                    report != item.end() && report->is_object()) {
                    try { message.report = parse_report(*report); } catch (...) {}
                }
                if (!message.turn.id.empty() && !message.turn.content.empty()) {
                    result.messages.push_back(std::move(message));
                }
            }
        }
        result.ok = true;
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

CompanionJobResponse submit_companion_message(
    std::string_view endpoint,
    std::string_view session_id,
    std::string_view session_token,
    std::string_view message_id,
    std::string_view text,
    std::uint32_t timeout_ms) {
    CompanionJobResponse result;
    try {
        const json body{
            {"contract", kCompanionContractVersion},
            {"message_id", std::string(message_id).substr(0, 120)},
            {"text", std::string(text).substr(0, 4000)},
        };
        const auto path = "/v1/crew-chief/companion/sessions/" + std::string(session_id) + "/messages";
        const auto response = request_json(
            "POST", companion_endpoint(endpoint, path), session_token,
            &body, 16'384, timeout_ms);
        result.http_status = response.status;
        result.error = response.error;
        if (!response.ok) return result;
        result.job_id = response.value.value("job_id", std::string{}).substr(0, 120);
        result.status = response.value.value("status", std::string{}).substr(0, 32);
        result.duplicate = response.value.value("duplicate", false);
        result.ok = !result.job_id.empty();
        if (!result.ok) result.error = "Crew Chief companion job response was incomplete";
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

CompanionJobResponse request_companion_job(
    std::string_view endpoint,
    std::string_view session_token,
    std::string_view job_id,
    std::uint32_t timeout_ms) {
    CompanionJobResponse result;
    try {
        const auto path = "/v1/crew-chief/companion/jobs/" + std::string(job_id);
        const auto response = request_json(
            "GET", companion_endpoint(endpoint, path), session_token,
            nullptr, 0, timeout_ms);
        result.http_status = response.status;
        result.error = response.error;
        if (!response.ok) return result;
        result = parse_companion_job_response(response.value);
        result.http_status = response.status;
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

CompanionJobResponse parse_companion_job_response(const nlohmann::json& value) {
    CompanionJobResponse result;
    if (!value.is_object()) {
        result.error = "Crew Chief companion job response was invalid";
        return result;
    }
    const auto bounded_string = [&](const char* key, std::size_t maximum) {
        const auto found = value.find(key);
        return found != value.end() && found->is_string()
            ? found->get<std::string>().substr(0, maximum)
            : std::string{};
    };
    result.job_id = bounded_string("job_id", 120);
    result.status = bounded_string("status", 32);
    result.assistant_message_id = bounded_string("assistant_message_id", 120);
    result.error = bounded_string("error", 1'000);
    result.ok = !result.job_id.empty();
    if (!result.ok && result.error.empty()) {
        result.error = "Crew Chief companion job response was incomplete";
    }
    return result;
}

CompanionActionResponse clear_companion_messages(
    std::string_view endpoint,
    std::string_view session_id,
    std::string_view session_token,
    std::uint32_t timeout_ms) {
    CompanionActionResponse result;
    try {
        const auto path = "/v1/crew-chief/companion/sessions/" + std::string(session_id) + "/messages";
        const auto response = request_json(
            "DELETE", companion_endpoint(endpoint, path), session_token,
            nullptr, 0, timeout_ms);
        result.http_status = response.status;
        result.error = response.error;
        if (!response.ok) return result;
        result.backup_name = response.value.value("backup_name", std::string{}).substr(0, 260);
        result.cleared_messages = response.value.value("cleared_messages", std::size_t{});
        result.ok = response.value.value("backup_created", false);
        if (!result.ok) result.error = "Crew Chief did not confirm the transcript backup";
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

}  // namespace racebox::crew_chief
