#include "racebox/race_day.hpp"

#include <nlohmann/json.hpp>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace racebox::race_day {
namespace {

using json = nlohmann::json;

constexpr std::uintmax_t kMaximumRaceDayBytes =
    32ULL * 1024ULL * 1024ULL;

std::string bounded(std::string value, std::size_t maximum) {
    if (value.size() > maximum) value.resize(maximum);
    return value;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(encoded.data()),
        encoded.size(),
    };
}

std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string encoded(value.size(), u8'\0');
    if (!value.empty()) {
        std::memcpy(encoded.data(), value.data(), value.size());
    }
    return std::filesystem::path(encoded);
}

bool contains_sanwa_header(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    for (int index = 0; index < 12 && std::getline(file, line); ++index) {
        if (line.find("REC TIME") != std::string::npos && line.find("ST(%)") != std::string::npos) return true;
    }
    return false;
}

std::wstring lower_extension(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    return extension;
}

std::string run_id(std::string_view prefix, int ordinal) {
    return std::string(prefix) + "-" + std::to_string(ordinal);
}

int next_ordinal(const Day& day, RunKind kind) {
    int result = 0;
    for (const auto& run : day.runs) {
        if (run.kind == kind) result = std::max(result, run.ordinal);
    }
    return result + 1;
}

Run make_run(std::string id, std::string label, RunKind kind, int ordinal) {
    Run run;
    run.id = std::move(id);
    run.label = std::move(label);
    run.kind = kind;
    run.ordinal = ordinal;
    run.checklist = default_pre_run_checklist();
    return run;
}

const char* kind_key(RunKind kind) noexcept {
    switch (kind) {
        case RunKind::Practice: return "practice";
        case RunKind::Qualifying: return "qualifying";
        case RunKind::Main: return "main";
        case RunKind::Custom: return "custom";
    }
    return "custom";
}

RunKind parse_kind(std::string_view value) noexcept {
    if (value == "practice") return RunKind::Practice;
    if (value == "qualifying") return RunKind::Qualifying;
    if (value == "main") return RunKind::Main;
    return RunKind::Custom;
}

json optional_number(const std::optional<double>& value) {
    return value ? json(*value) : json(nullptr);
}

std::optional<double> read_optional_number(const json& object, const char* key) {
    const auto value = object.find(key);
    if (value == object.end() || value->is_null() || !value->is_number()) return std::nullopt;
    const auto result = value->get<double>();
    return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
}

json conditions_json(const Conditions& conditions) {
    return {
        {"ambient_temperature_c", optional_number(conditions.ambient_temperature_c)},
        {"track_temperature_c", optional_number(conditions.track_temperature_c)},
        {"track_condition", bounded(conditions.track_condition, 400)},
        {"tire_set_id", bounded(conditions.tire_set_id, 200)},
        {"tire_compound", bounded(conditions.tire_compound, 200)},
        {"tire_runs_before", conditions.tire_runs_before},
        {"sauce_compound", bounded(conditions.sauce_compound, 200)},
        {"sauce_minutes_before", conditions.sauce_minutes_before},
        {"tire_warmer_minutes", conditions.tire_warmer_minutes},
        {"tire_warmer_temperature_c", optional_number(conditions.tire_warmer_temperature_c)},
        {"battery_pack", bounded(conditions.battery_pack, 200)},
        {"battery_voltage", optional_number(conditions.battery_voltage)},
    };
}

Conditions read_conditions(const json& value) {
    Conditions result;
    if (!value.is_object()) return result;
    result.ambient_temperature_c = read_optional_number(value, "ambient_temperature_c");
    result.track_temperature_c = read_optional_number(value, "track_temperature_c");
    result.track_condition = bounded(value.value("track_condition", std::string{}), 400);
    result.tire_set_id = bounded(value.value("tire_set_id", std::string{}), 200);
    result.tire_compound = bounded(value.value("tire_compound", std::string{}), 200);
    result.tire_runs_before = std::clamp(value.value("tire_runs_before", -1), -1, 10'000);
    result.sauce_compound = bounded(value.value("sauce_compound", std::string{}), 200);
    result.sauce_minutes_before = std::clamp(value.value("sauce_minutes_before", -1), -1, 10'000);
    result.tire_warmer_minutes = std::clamp(value.value("tire_warmer_minutes", -1), -1, 10'000);
    result.tire_warmer_temperature_c = read_optional_number(value, "tire_warmer_temperature_c");
    result.battery_pack = bounded(value.value("battery_pack", std::string{}), 200);
    result.battery_voltage = read_optional_number(value, "battery_voltage");
    return result;
}

json evidence_json(const SetupKnowledgeEvidence& evidence) {
    return {
        {"analytics_contract", bounded(evidence.analytics_contract, 120)},
        {"formula_version", evidence.formula_version},
        {"quality_confidence", std::clamp(evidence.quality_confidence, 0, 100)},
        {"track_status", bounded(evidence.track_status, 80)},
        {"track_translation_m", optional_number(evidence.track_translation_m)},
        {"track_residual_rms_m", optional_number(evidence.track_residual_rms_m)},
        {"lap_time_delta_s", optional_number(evidence.lap_time_delta_s)},
        {"lateral_status", bounded(evidence.lateral_status, 80)},
        {"lateral_response_delta_g", optional_number(evidence.lateral_response_delta_g)},
        {"forward_status", bounded(evidence.forward_status, 80)},
        {"forward_bite_delta_g", optional_number(evidence.forward_bite_delta_g)},
        {"previous_brake_indicators", std::max(0, evidence.previous_brake_indicators)},
        {"current_brake_indicators", std::max(0, evidence.current_brake_indicators)},
        {"top_speed_status", bounded(evidence.top_speed_status, 80)},
        {"straight_top_speed_delta_kmh", optional_number(evidence.straight_top_speed_delta_kmh)},
        {"straight_entry_speed_delta_kmh", optional_number(evidence.straight_entry_speed_delta_kmh)},
        {"straight_acceleration_delta_g", optional_number(evidence.straight_acceleration_delta_g)},
        {"straight_speed_attribution", bounded(evidence.straight_speed_attribution, 120)},
        {"brake_response_status", bounded(evidence.brake_response_status, 80)},
        {"brake_decel_delta_g", optional_number(evidence.brake_decel_delta_g)},
        {"brake_response_delay_delta_s", optional_number(evidence.brake_response_delay_delta_s)},
        {"corner_balance_status", bounded(evidence.corner_balance_status, 80)},
        {"steering_for_lateral_g_delta_percent", optional_number(evidence.steering_for_lateral_g_delta_percent)},
        {"yaw_per_steering_delta_dps", optional_number(evidence.yaw_per_steering_delta_dps)},
        {"overdriving_status", bounded(evidence.overdriving_status, 80)},
        {"overdriving_index_delta", optional_number(evidence.overdriving_index_delta)},
        {"overdriving_risk_sample_delta_percent", optional_number(evidence.overdriving_risk_sample_delta_percent)},
        {"current_late_overdriving_delta_score", optional_number(evidence.current_late_overdriving_delta_score)},
        {"chassis_roll_status", bounded(evidence.chassis_roll_status, 80)},
        {"surface_tilt_delta_deg", optional_number(evidence.surface_tilt_delta_deg)},
        {"chassis_roll_delta_deg", optional_number(evidence.chassis_roll_delta_deg)},
        {"roll_per_lateral_g_delta_deg", optional_number(evidence.roll_per_lateral_g_delta_deg)},
        {"roll_rate_status", bounded(evidence.roll_rate_status, 80)},
        {"previous_roll_rate_p90_dps", optional_number(evidence.previous_roll_rate_p90_dps)},
        {"current_roll_rate_p90_dps", optional_number(evidence.current_roll_rate_p90_dps)},
        {"roll_rate_delta_dps", optional_number(evidence.roll_rate_delta_dps)},
        {"roll_rate_per_lateral_g_delta_dps", optional_number(evidence.roll_rate_per_lateral_g_delta_dps)},
        {"current_late_roll_rate_delta_dps", optional_number(evidence.current_late_roll_rate_delta_dps)},
        {"previous_roll_rate_samples", std::max(0, evidence.previous_roll_rate_samples)},
        {"current_roll_rate_samples", std::max(0, evidence.current_roll_rate_samples)},
        {"current_surface_tilt_source", bounded(evidence.current_surface_tilt_source, 80)},
        {"current_surface_tilt_samples", std::max(0, evidence.current_surface_tilt_samples)},
    };
}

SetupKnowledgeEvidence read_evidence(const json& value) {
    SetupKnowledgeEvidence result;
    if (!value.is_object()) return result;
    result.analytics_contract = bounded(value.value("analytics_contract", std::string{}), 120);
    result.formula_version = std::clamp(value.value("formula_version", 0), 0, 10'000);
    result.quality_confidence = std::clamp(value.value("quality_confidence", 0), 0, 100);
    result.track_status = bounded(value.value("track_status", std::string{}), 80);
    result.track_translation_m = read_optional_number(value, "track_translation_m");
    result.track_residual_rms_m = read_optional_number(value, "track_residual_rms_m");
    result.lap_time_delta_s = read_optional_number(value, "lap_time_delta_s");
    result.lateral_status = bounded(value.value("lateral_status", std::string{}), 80);
    result.lateral_response_delta_g = read_optional_number(value, "lateral_response_delta_g");
    result.forward_status = bounded(value.value("forward_status", std::string{}), 80);
    result.forward_bite_delta_g = read_optional_number(value, "forward_bite_delta_g");
    result.previous_brake_indicators =
        std::clamp(value.value("previous_brake_indicators", 0), 0, 100'000);
    result.current_brake_indicators =
        std::clamp(value.value("current_brake_indicators", 0), 0, 100'000);
    result.top_speed_status = bounded(value.value("top_speed_status", std::string{}), 80);
    result.straight_top_speed_delta_kmh = read_optional_number(value, "straight_top_speed_delta_kmh");
    result.straight_entry_speed_delta_kmh = read_optional_number(value, "straight_entry_speed_delta_kmh");
    result.straight_acceleration_delta_g = read_optional_number(value, "straight_acceleration_delta_g");
    result.straight_speed_attribution = bounded(value.value("straight_speed_attribution", std::string{}), 120);
    result.brake_response_status = bounded(value.value("brake_response_status", std::string{}), 80);
    result.brake_decel_delta_g = read_optional_number(value, "brake_decel_delta_g");
    result.brake_response_delay_delta_s = read_optional_number(value, "brake_response_delay_delta_s");
    result.corner_balance_status = bounded(value.value("corner_balance_status", std::string{}), 80);
    result.steering_for_lateral_g_delta_percent = read_optional_number(value, "steering_for_lateral_g_delta_percent");
    result.yaw_per_steering_delta_dps = read_optional_number(value, "yaw_per_steering_delta_dps");
    result.overdriving_status = bounded(value.value("overdriving_status", std::string{}), 80);
    result.overdriving_index_delta = read_optional_number(value, "overdriving_index_delta");
    result.overdriving_risk_sample_delta_percent =
        read_optional_number(value, "overdriving_risk_sample_delta_percent");
    result.current_late_overdriving_delta_score =
        read_optional_number(value, "current_late_overdriving_delta_score");
    result.chassis_roll_status = bounded(value.value("chassis_roll_status", std::string{}), 80);
    result.surface_tilt_delta_deg = read_optional_number(value, "surface_tilt_delta_deg");
    result.chassis_roll_delta_deg = read_optional_number(value, "chassis_roll_delta_deg");
    result.roll_per_lateral_g_delta_deg = read_optional_number(value, "roll_per_lateral_g_delta_deg");
    result.roll_rate_status = bounded(value.value("roll_rate_status", std::string{}), 80);
    result.previous_roll_rate_p90_dps = read_optional_number(value, "previous_roll_rate_p90_dps");
    result.current_roll_rate_p90_dps = read_optional_number(value, "current_roll_rate_p90_dps");
    result.roll_rate_delta_dps = read_optional_number(value, "roll_rate_delta_dps");
    result.roll_rate_per_lateral_g_delta_dps =
        read_optional_number(value, "roll_rate_per_lateral_g_delta_dps");
    result.current_late_roll_rate_delta_dps = read_optional_number(value, "current_late_roll_rate_delta_dps");
    result.previous_roll_rate_samples = std::clamp(value.value("previous_roll_rate_samples", 0), 0, 100'000);
    result.current_roll_rate_samples = std::clamp(value.value("current_roll_rate_samples", 0), 0, 100'000);
    result.current_surface_tilt_source = bounded(value.value("current_surface_tilt_source", std::string{}), 80);
    result.current_surface_tilt_samples = std::clamp(value.value("current_surface_tilt_samples", 0), 0, 100'000);
    return result;
}

std::string lower_ascii(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

std::optional<double> median(std::vector<double> values) {
    values.erase(std::remove_if(values.begin(), values.end(), [](double value) {
        return !std::isfinite(value);
    }), values.end());
    if (values.empty()) return std::nullopt;
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    auto result = *middle;
    if (values.size() % 2 == 0) {
        const auto lower = std::max_element(values.begin(), middle);
        result = (*lower + result) * 0.5;
    }
    return result;
}

std::optional<double> percentile(std::vector<double> values, double fraction) {
    values.erase(std::remove_if(values.begin(), values.end(), [](double value) {
        return !std::isfinite(value);
    }), values.end());
    if (values.empty()) return std::nullopt;
    std::sort(values.begin(), values.end());
    fraction = std::clamp(fraction, 0.0, 1.0);
    const auto position = fraction * static_cast<double>(values.size() - 1);
    const auto low = static_cast<std::size_t>(std::floor(position));
    const auto high = static_cast<std::size_t>(std::ceil(position));
    if (low == high) return values[low];
    const auto weight = position - static_cast<double>(low);
    return values[low] * (1.0 - weight) + values[high] * weight;
}

struct CornerDemandSample {
    double steering_percent{};
    double lateral_g{};
    double non_brake_decel_g{};
    double steering_correction{};
};

std::optional<double> roll_angle_deg(double lateral_g, double vertical_g) {
    if (!std::isfinite(lateral_g) || !std::isfinite(vertical_g) || std::abs(vertical_g) < 0.15) {
        return std::nullopt;
    }
    constexpr auto radians_to_degrees = 180.0 / 3.14159265358979323846;
    return std::atan2(lateral_g, std::abs(vertical_g)) * radians_to_degrees;
}

struct SetupRunFeatures {
    int complete_laps{};
    int quality_confidence{};
    std::optional<double> top_three_lap_s;
    std::optional<double> lateral_response_g_per_steer;
    std::optional<double> steering_for_lateral_percent;
    std::optional<double> yaw_per_steering_dps;
    std::optional<double> forward_accel_g;
    std::optional<double> straight_entry_speed_kmh;
    std::optional<double> straight_top_speed_kmh;
    std::optional<double> straight_accel_g;
    std::optional<double> brake_decel_g;
    std::optional<double> brake_response_delay_s;
    std::optional<double> overdriving_index;
    std::optional<double> overdriving_risk_sample_percent;
    std::optional<double> late_overdriving_delta_score;
    std::optional<double> surface_tilt_roll_deg;
    std::optional<double> chassis_roll_signature_deg;
    std::optional<double> roll_per_lateral_g_deg;
    std::optional<double> roll_rate_p90_dps;
    std::optional<double> roll_rate_per_lateral_g_dps;
    std::optional<double> late_roll_rate_delta_dps;
    std::string surface_tilt_source{"data_limited"};
    int surface_tilt_samples{};
    int roll_rate_samples{};
    int brake_indicators{};
};

SetupRunFeatures summarize_setup_run(const Session& session, const imu::Analysis& imu_analysis) {
    SetupRunFeatures result;
    const auto& telemetry = session.telemetry;
    std::vector<double> lap_times;
    std::vector<double> lateral_response;
    std::vector<double> steering_for_lateral;
    std::vector<double> yaw_per_steering;
    std::vector<double> forward_accel;
    std::vector<double> straight_entry_speed;
    std::vector<double> straight_top_speed;
    std::vector<double> straight_accel;
    std::vector<double> brake_decel;
    std::vector<double> brake_delay;
    std::vector<CornerDemandSample> corner_demand_samples;
    std::vector<double> corner_roll_signature;
    std::vector<double> roll_per_lateral_g;
    std::vector<double> roll_rate_abs;
    std::vector<double> roll_rate_per_lateral_g;

    const auto yaw_valid = imu_analysis.calibration.heading_correlation >= 0.60 &&
        imu_analysis.vehicle_yaw_rate_dps.size() == telemetry.size();

    std::vector<double> stopped_tilt;
    std::vector<double> straight_tilt;
    for (std::size_t index = 0; index < telemetry.size(); ++index) {
        const auto roll = roll_angle_deg(telemetry.lateral_g[index], telemetry.vertical_g[index]);
        if (!roll) continue;
        const auto radio = sample_radio(session, telemetry.time_us[index]);
        if (telemetry.speed_kmh[index] <= 1.5F) {
            stopped_tilt.push_back(*roll);
        } else if (telemetry.speed_kmh[index] >= 8.0F &&
                   std::abs(static_cast<double>(telemetry.lateral_g[index])) <= 0.20 &&
                   (!radio.valid || (std::abs(radio.steering) <= 5.0F && radio.brake <= 5.0F))) {
            straight_tilt.push_back(*roll);
        }
    }
    if (stopped_tilt.size() >= 25) {
        result.surface_tilt_roll_deg = median(stopped_tilt);
        result.surface_tilt_source = "stopped_points";
        result.surface_tilt_samples = static_cast<int>(stopped_tilt.size());
    } else if (straight_tilt.size() >= 25) {
        result.surface_tilt_roll_deg = median(straight_tilt);
        result.surface_tilt_source = "straight";
        result.surface_tilt_samples = static_cast<int>(straight_tilt.size());
    }

    std::vector<double> smoothed_roll_angle(telemetry.size(), std::numeric_limits<double>::quiet_NaN());
    if (result.surface_tilt_roll_deg) {
        std::vector<double> corrected_roll_angle(telemetry.size(), std::numeric_limits<double>::quiet_NaN());
        for (std::size_t index = 0; index < telemetry.size(); ++index) {
            if (const auto roll = roll_angle_deg(telemetry.lateral_g[index], telemetry.vertical_g[index])) {
                corrected_roll_angle[index] = *roll - *result.surface_tilt_roll_deg;
            }
        }
        for (std::size_t index = 0; index < telemetry.size(); ++index) {
            double sum = 0.0;
            int count = 0;
            const auto begin = index > 2 ? index - 2 : 0;
            const auto end = std::min(telemetry.size() - 1, index + 2);
            for (auto sample = begin; sample <= end; ++sample) {
                if (!std::isfinite(corrected_roll_angle[sample])) continue;
                const auto dt_s = std::abs(static_cast<double>(telemetry.time_us[sample]) -
                    static_cast<double>(telemetry.time_us[index])) / 1'000'000.0;
                if (dt_s > 0.100) continue;
                sum += corrected_roll_angle[sample];
                ++count;
            }
            if (count >= 2) smoothed_roll_angle[index] = sum / static_cast<double>(count);
        }
    }

    for (const auto& lap : session.laps) {
        if (lap.phase != LapPhase::Complete || lap.end_index <= lap.begin_index ||
            lap.end_index >= telemetry.size()) {
            continue;
        }
        ++result.complete_laps;
        lap_times.push_back(static_cast<double>(lap.duration_us) / 1'000'000.0);
        const auto begin_time = telemetry.time_us[lap.begin_index];

        bool in_straight = false;
        std::size_t straight_begin = 0;
        std::size_t straight_end = 0;
        std::size_t best_straight_begin = 0;
        std::size_t best_straight_end = 0;
        double best_speed_gain = -1.0;

        for (auto index = lap.begin_index; index <= lap.end_index; ++index) {
            const auto radio = sample_radio(session, telemetry.time_us[index]);
            double non_brake_decel_g = 0.0;
            double steering_correction = 0.0;
            if (index > lap.begin_index) {
                const auto dt_s = static_cast<double>(telemetry.time_us[index] - telemetry.time_us[index - 1]) / 1'000'000.0;
                const auto previous_radio = sample_radio(session, telemetry.time_us[index - 1]);
                if (dt_s >= 0.015 && dt_s <= 0.250 && previous_radio.valid) {
                const auto accel_g = ((telemetry.speed_kmh[index] - telemetry.speed_kmh[index - 1]) / 3.6) /
                    dt_s / 9.80665;
                if (radio.valid && radio.brake <= 10.0F) non_brake_decel_g = std::max(0.0, -accel_g);
                if (radio.valid) {
                    const auto steering_delta = std::abs(static_cast<double>(radio.steering - previous_radio.steering));
                    steering_correction = steering_delta >= 15.0 ? 1.0 : 0.0;
                }
            }
            }
            if (radio.valid && std::abs(radio.steering) >= 8.0F && radio.brake <= 5.0F &&
                telemetry.speed_kmh[index] >= 8.0F) {
                const auto abs_steer = std::abs(static_cast<double>(radio.steering));
                const auto abs_lat = std::abs(static_cast<double>(telemetry.lateral_g[index]));
                lateral_response.push_back(abs_lat / std::max(8.0, abs_steer));
                if (abs_lat >= 0.45) steering_for_lateral.push_back(abs_steer / abs_lat);
                if (yaw_valid) {
                    yaw_per_steering.push_back(
                        std::abs(imu_analysis.vehicle_yaw_rate_dps[index]) / std::max(8.0, abs_steer));
                }
            }
            if (radio.valid && std::abs(radio.steering) >= 15.0F && radio.brake <= 15.0F &&
                telemetry.speed_kmh[index] >= 8.0F) {
                corner_demand_samples.push_back({
                    std::abs(static_cast<double>(radio.steering)),
                    std::abs(static_cast<double>(telemetry.lateral_g[index])),
                    non_brake_decel_g,
                    steering_correction});
                if (index >= lap.begin_index + 2 && index + 2 <= lap.end_index &&
                    std::isfinite(smoothed_roll_angle[index - 2]) &&
                    std::isfinite(smoothed_roll_angle[index + 2])) {
                    const auto dt_s = static_cast<double>(
                        telemetry.time_us[index + 2] - telemetry.time_us[index - 2]) / 1'000'000.0;
                    if (dt_s >= 0.080 && dt_s <= 0.240) {
                        const auto rate_dps = (smoothed_roll_angle[index + 2] -
                            smoothed_roll_angle[index - 2]) / dt_s;
                        if (std::isfinite(rate_dps)) {
                            const auto abs_rate = std::abs(rate_dps);
                            roll_rate_abs.push_back(abs_rate);
                            roll_rate_per_lateral_g.push_back(
                                abs_rate / std::max(0.35, std::abs(static_cast<double>(telemetry.lateral_g[index]))));
                            ++result.roll_rate_samples;
                        }
                    }
                }
                if (result.surface_tilt_roll_deg && std::abs(static_cast<double>(telemetry.lateral_g[index])) >= 0.35) {
                    if (const auto roll = roll_angle_deg(telemetry.lateral_g[index], telemetry.vertical_g[index])) {
                        const auto corrected = std::abs(*roll - *result.surface_tilt_roll_deg);
                        corner_roll_signature.push_back(corrected);
                        roll_per_lateral_g.push_back(
                            corrected / std::max(0.35, std::abs(static_cast<double>(telemetry.lateral_g[index]))));
                    }
                }
            }

            if (index > lap.begin_index) {
                const auto dt_s = static_cast<double>(telemetry.time_us[index] - telemetry.time_us[index - 1]) / 1'000'000.0;
                const auto previous_radio = sample_radio(session, telemetry.time_us[index - 1]);
                if (dt_s >= 0.015 && dt_s <= 0.250 && previous_radio.valid &&
                    previous_radio.throttle >= 90.0F && previous_radio.brake <= 5.0F &&
                    std::abs(previous_radio.steering) <= 20.0F && telemetry.speed_kmh[index - 1] >= 5.0F) {
                    const auto accel_g = ((telemetry.speed_kmh[index] - telemetry.speed_kmh[index - 1]) / 3.6) /
                        dt_s / 9.80665;
                    forward_accel.push_back(accel_g);
                }
            }

            const auto straight_candidate = radio.valid && radio.throttle >= 80.0F &&
                radio.brake <= 5.0F && std::abs(radio.steering) <= 35.0F;
            if (straight_candidate && !in_straight) {
                in_straight = true;
                straight_begin = index;
                straight_end = index;
            } else if (straight_candidate) {
                straight_end = index;
            }
            if ((!straight_candidate || index == lap.end_index) && in_straight) {
                const auto duration_s =
                    static_cast<double>(telemetry.time_us[straight_end] - telemetry.time_us[straight_begin]) / 1'000'000.0;
                if (duration_s >= 0.25) {
                    const auto begin_speed = telemetry.speed_kmh[straight_begin];
                    const auto end_speed = telemetry.speed_kmh[straight_end];
                    const auto gain = static_cast<double>(end_speed - begin_speed);
                    if (gain > best_speed_gain) {
                        best_speed_gain = gain;
                        best_straight_begin = straight_begin;
                        best_straight_end = straight_end;
                    }
                }
                in_straight = false;
            }

            if (radio.valid && radio.brake >= 70.0F && radio.throttle <= 5.0F &&
                telemetry.speed_kmh[index] >= 10.0F) {
                const auto zone_start = index;
                auto zone_end = index;
                while (zone_end + 1 <= lap.end_index) {
                    const auto next_radio = sample_radio(session, telemetry.time_us[zone_end + 1]);
                    if (!next_radio.valid || next_radio.brake < 70.0F || next_radio.throttle > 5.0F) break;
                    ++zone_end;
                }
                const auto duration_s =
                    static_cast<double>(telemetry.time_us[zone_end] - telemetry.time_us[zone_start]) / 1'000'000.0;
                if (duration_s >= 0.08) {
                    double peak_decel = 0.0;
                    std::optional<double> delay_s;
                    for (auto brake_index = zone_start + 1; brake_index <= zone_end; ++brake_index) {
                        const auto dt_s = static_cast<double>(
                            telemetry.time_us[brake_index] - telemetry.time_us[brake_index - 1]) / 1'000'000.0;
                        if (dt_s < 0.015 || dt_s > 0.250) continue;
                        const auto accel_g =
                            ((telemetry.speed_kmh[brake_index] - telemetry.speed_kmh[brake_index - 1]) / 3.6) /
                            dt_s / 9.80665;
                        peak_decel = std::min(peak_decel, accel_g);
                        if (!delay_s && accel_g <= -0.20) {
                            delay_s = static_cast<double>(telemetry.time_us[brake_index] - telemetry.time_us[zone_start]) /
                                1'000'000.0;
                        }
                    }
                    brake_decel.push_back(-peak_decel);
                    if (delay_s) brake_delay.push_back(*delay_s);
                    if (!delay_s || *delay_s >= 0.08 || -peak_decel < 0.20) ++result.brake_indicators;
                }
                index = zone_end;
            }
        }

        if (best_speed_gain >= 0.0 && best_straight_end > best_straight_begin) {
            const auto duration_s =
                static_cast<double>(telemetry.time_us[best_straight_end] - telemetry.time_us[best_straight_begin]) /
                1'000'000.0;
            straight_entry_speed.push_back(telemetry.speed_kmh[best_straight_begin]);
            double top_speed = telemetry.speed_kmh[best_straight_begin];
            for (auto index = best_straight_begin; index <= best_straight_end; ++index) {
                top_speed = std::max(top_speed, static_cast<double>(telemetry.speed_kmh[index]));
            }
            straight_top_speed.push_back(top_speed);
            straight_accel.push_back(((telemetry.speed_kmh[best_straight_end] -
                telemetry.speed_kmh[best_straight_begin]) / 3.6) / duration_s / 9.80665);
        }

        (void)begin_time;
    }

    std::sort(lap_times.begin(), lap_times.end());
    if (lap_times.size() >= 3) {
        result.top_three_lap_s = median({lap_times[0], lap_times[1], lap_times[2]});
    } else {
        result.top_three_lap_s = median(lap_times);
    }
    result.lateral_response_g_per_steer = median(lateral_response);
    result.steering_for_lateral_percent = median(steering_for_lateral);
    result.yaw_per_steering_dps = median(yaw_per_steering);
    result.forward_accel_g = median(forward_accel);
    result.straight_entry_speed_kmh = median(straight_entry_speed);
    result.straight_top_speed_kmh = median(straight_top_speed);
    result.straight_accel_g = median(straight_accel);
    result.brake_decel_g = median(brake_decel);
    result.brake_response_delay_s = median(brake_delay);
    result.chassis_roll_signature_deg = median(corner_roll_signature);
    result.roll_per_lateral_g_deg = median(roll_per_lateral_g);
    result.roll_rate_p90_dps = percentile(roll_rate_abs, 0.90);
    result.roll_rate_per_lateral_g_dps = percentile(roll_rate_per_lateral_g, 0.90);
    if (roll_rate_abs.size() >= 40) {
        const auto half = roll_rate_abs.size() / 2;
        const auto early = percentile(std::vector<double>(roll_rate_abs.begin(), roll_rate_abs.begin() + half), 0.90);
        const auto late = percentile(std::vector<double>(roll_rate_abs.begin() + half, roll_rate_abs.end()), 0.90);
        if (early && late) result.late_roll_rate_delta_dps = *late - *early;
    }

    if (corner_demand_samples.size() >= 20) {
        std::vector<double> response_ratios;
        response_ratios.reserve(corner_demand_samples.size());
        for (const auto& sample : corner_demand_samples) {
            if (sample.steering_percent >= 15.0 && sample.lateral_g >= 0.05) {
                response_ratios.push_back(sample.lateral_g / sample.steering_percent);
            }
        }
        if (const auto efficient_response = percentile(response_ratios, 0.75)) {
            std::vector<double> scores;
            scores.reserve(corner_demand_samples.size());
            int risk_samples = 0;
            for (const auto& sample : corner_demand_samples) {
                const auto expected_lateral = *efficient_response * sample.steering_percent;
                const auto weak_response = expected_lateral > 0.05
                    ? std::clamp((expected_lateral - sample.lateral_g) / expected_lateral, 0.0, 1.0)
                    : 0.0;
                const auto steering_stress = std::clamp((sample.steering_percent - 25.0) / 50.0, 0.0, 1.0);
                const auto scrub_decel = std::clamp(sample.non_brake_decel_g / 0.25, 0.0, 1.0);
                const auto score = 100.0 * steering_stress *
                    (0.50 * weak_response + 0.35 * scrub_decel + 0.15 * sample.steering_correction);
                scores.push_back(score);
                if (score >= 20.0) ++risk_samples;
            }
            result.overdriving_index = percentile(scores, 0.75);
            result.overdriving_risk_sample_percent = scores.empty()
                ? std::nullopt
                : std::optional<double>(static_cast<double>(risk_samples) / static_cast<double>(scores.size()) * 100.0);
            if (scores.size() >= 40) {
                const auto half = scores.size() / 2;
                const auto early = percentile(std::vector<double>(scores.begin(), scores.begin() + half), 0.75);
                const auto late = percentile(std::vector<double>(scores.begin() + half, scores.end()), 0.75);
                if (early && late) result.late_overdriving_delta_score = *late - *early;
            }
        }
    }

    result.quality_confidence = std::clamp(30 + result.complete_laps * 3 +
        (session.alignment.compatible ? 25 : 0) + (yaw_valid ? 10 : 0), 0, 100);
    return result;
}

std::string classify_straight_attribution(
    std::optional<double> entry_delta,
    std::optional<double> accel_delta,
    std::optional<double> top_delta) {
    if (!entry_delta || !accel_delta || !top_delta) return "data_limited";
    const auto abs_top = std::abs(*top_delta);
    if (abs_top < 0.75) return "within_noise";
    if (std::abs(*entry_delta) >= abs_top * 0.60 && std::abs(*accel_delta) < 0.04) {
        return *entry_delta > 0.0 ? "exit_or_line_driven" : "exit_or_line_loss";
    }
    if (std::abs(*entry_delta) < 0.75 && std::abs(*accel_delta) >= 0.04) {
        return *accel_delta > 0.0 ? "forward_bite_or_power_delivery" : "reduced_forward_bite";
    }
    if (*entry_delta > 0.75 && *accel_delta > 0.04) return "exit_plus_acceleration";
    if (*entry_delta < -0.75 && *accel_delta > 0.04) return "recovery_after_poor_exit";
    return "mixed_or_driving_line";
}

std::string classify_overdriving(
    std::optional<double> index_delta,
    std::optional<double> risk_sample_delta,
    std::optional<double> current_late_delta,
    std::optional<double> lap_time_delta) {
    if (!index_delta && !risk_sample_delta && !current_late_delta) return "data_limited";
    const auto index = index_delta.value_or(0.0);
    const auto risk = risk_sample_delta.value_or(0.0);
    const auto late = current_late_delta.value_or(0.0);
    const auto more = index >= 8.0 || risk >= 6.0;
    const auto less = index <= -8.0 || risk <= -6.0;
    if (more) {
        if (lap_time_delta && *lap_time_delta >= -0.05) return "more_overdriving_no_lap_gain";
        if (late >= 8.0) return "more_overdriving_late_tire_risk";
        return "more_overdriving";
    }
    if (less) return "less_overdriving";
    if (late >= 10.0) return "late_run_tire_scrub_risk";
    return "within_noise";
}

std::string classify_chassis_roll(
    std::optional<double> roll_delta,
    std::optional<double> roll_per_lateral_delta,
    std::optional<double> surface_tilt_delta) {
    if (!roll_delta && !roll_per_lateral_delta) return "data_limited";
    if (surface_tilt_delta && std::abs(*surface_tilt_delta) >= 2.0) {
        return "surface_tilt_changed_check_reference";
    }
    const auto roll = roll_delta.value_or(0.0);
    const auto normalized = roll_per_lateral_delta.value_or(0.0);
    if (roll >= 0.75 || normalized >= 0.75) return "more_chassis_roll_signature";
    if (roll <= -0.75 || normalized <= -0.75) return "less_chassis_roll_signature";
    return "within_noise";
}

std::string classify_roll_rate(
    std::optional<double> previous_p90,
    std::optional<double> current_p90,
    std::optional<double> roll_rate_delta,
    std::optional<double> normalized_delta,
    std::optional<double> surface_tilt_delta,
    int previous_samples,
    int current_samples) {
    if (!previous_p90 || !current_p90 || !roll_rate_delta ||
        previous_samples < 25 || current_samples < 25) {
        return "data_limited";
    }
    if (surface_tilt_delta && std::abs(*surface_tilt_delta) >= 2.0) {
        return "surface_tilt_changed_check_reference";
    }
    const auto adaptive_threshold = std::max(8.0, std::abs(*previous_p90) * 0.15);
    const auto normalized_threshold = 8.0;
    if (*roll_rate_delta >= adaptive_threshold ||
        (normalized_delta && *normalized_delta >= normalized_threshold)) {
        return "faster_roll_build";
    }
    if (*roll_rate_delta <= -adaptive_threshold ||
        (normalized_delta && *normalized_delta <= -normalized_threshold)) {
        return "slower_roll_build";
    }
    return "within_noise";
}

std::unordered_set<std::string> searchable_terms(std::string_view value) {
    static const std::unordered_set<std::string> ignored{
        "a", "an", "and", "are", "as", "at", "be", "but", "by", "car", "did",
        "do", "for", "from", "had", "has", "have", "how", "i", "in", "is", "it",
        "made", "more", "my", "of", "on", "or", "run", "setup", "that", "the",
        "this", "to", "was", "what", "when", "with",
    };
    std::unordered_set<std::string> result;
    std::string term;
    const auto flush = [&] {
        if (term.size() >= 2 && !ignored.contains(term)) result.insert(term);
        term.clear();
    };
    for (const auto character : value) {
        if (std::isalnum(static_cast<unsigned char>(character))) {
            term.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        } else {
            flush();
        }
    }
    flush();
    return result;
}

std::string knowledge_search_text(const SetupKnowledgeRecord& record) {
    std::string result = record.handling_question + " " + record.setup_change + " " +
        record.driver_result + " " + record.summary + " " + record.next_test;
    for (const auto& observation : record.observations) {
        result += " " + observation.area + " " + observation.change + " " + observation.meaning;
    }
    return result;
}

bool same_text(std::string_view left, std::string_view right) {
    return !left.empty() && !right.empty() && lower_ascii(left) == lower_ascii(right);
}

std::filesystem::path stored_path(const std::filesystem::path& path, const std::filesystem::path& base) {
    if (path.empty()) return {};
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error) return path;
    const auto relative = std::filesystem::relative(absolute, base, error);
    if (!error && !relative.empty()) return relative;
    return absolute;
}

std::filesystem::path resolved_path(const std::filesystem::path& path, const std::filesystem::path& base) {
    if (path.empty() || path.is_absolute()) return path;
    return (base / path).lexically_normal();
}

std::optional<source_identity::ModifiedTimeUnixNs> modified_time_unix_ns(
    const std::filesystem::path& path) {
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error) return std::nullopt;
    // C++20 library support for file_clock::to_sys is uneven on current
    // MSVC. Capture both clocks together and translate through their current
    // offset; source matching intentionally allows two seconds for filesystem
    // timestamp rounding and this conversion's sub-millisecond jitter.
    const auto file_now =
        std::filesystem::file_time_type::clock::now();
    const auto system_now = std::chrono::system_clock::now();
    const auto system_time = system_now + (modified - file_now);
    const auto nanoseconds =
        std::chrono::time_point_cast<std::chrono::nanoseconds>(system_time)
            .time_since_epoch()
            .count();
    return static_cast<source_identity::ModifiedTimeUnixNs>(nanoseconds);
}

std::optional<std::string> file_fingerprint(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(
                buffer[static_cast<std::size_t>(index)]);
            hash *= prime;
        }
    }
    if (!input.eof()) return std::nullopt;
    std::ostringstream result;
    result << "fnv1a64:" << std::hex << std::setfill('0')
           << std::setw(16) << hash;
    return result.str();
}

source_identity::SourceIdentity metadata_for_path(
    const std::filesystem::path& path) {
    std::error_code error;
    if (!path.empty() && std::filesystem::is_regular_file(path, error) &&
        !error) {
        const auto size = std::filesystem::file_size(path, error);
        if (!error) {
            return source_identity::make_source_identity(
                path_to_utf8(path.filename()), size,
                modified_time_unix_ns(path));
        }
    }
    return source_identity::make_source_identity(
        path_to_utf8(path.filename()), 0);
}

source_identity::SourceIdentity identity_for_path(
    const std::filesystem::path& path,
    const source_identity::SourceIdentity* fallback = nullptr) {
    auto captured = metadata_for_path(path);
    if (captured.size_bytes > 0 || std::filesystem::is_regular_file(path)) {
        if (const auto fingerprint = file_fingerprint(path)) {
            captured.content_fingerprint = *fingerprint;
        } else if (fallback && fallback->content_fingerprint &&
                   fallback->size_bytes == captured.size_bytes) {
            captured.content_fingerprint = fallback->content_fingerprint;
        }
        return captured;
    }
    if (fallback) return *fallback;
    return captured;
}

json source_identity_json(const source_identity::SourceIdentity& identity) {
    json result{
        {"filename", bounded(identity.canonical_filename, 1024)},
        {"size_bytes", identity.size_bytes},
    };
    if (identity.modified_time_unix_ns) {
        result["modified_time_unix_ns"] =
            *identity.modified_time_unix_ns;
    }
    if (identity.content_fingerprint) {
        result["content_fingerprint"] =
            bounded(*identity.content_fingerprint, 512);
    }
    return result;
}

source_identity::SourceIdentity read_source_identity(
    const json& value,
    const std::filesystem::path& path) {
    if (!value.is_object()) return identity_for_path(path);
    const auto filename = bounded(
        value.value("filename", path_to_utf8(path.filename())), 1024);
    const auto size = value.value("size_bytes", std::uint64_t{});
    std::optional<source_identity::ModifiedTimeUnixNs> modified;
    if (const auto timestamp = value.find("modified_time_unix_ns");
        timestamp != value.end() && timestamp->is_number_integer()) {
        modified = timestamp->get<source_identity::ModifiedTimeUnixNs>();
    }
    std::optional<std::string_view> fingerprint;
    std::string fingerprint_storage;
    if (const auto stored = value.find("content_fingerprint");
        stored != value.end() && stored->is_string()) {
        fingerprint_storage = bounded(stored->get<std::string>(), 512);
        if (!fingerprint_storage.empty()) fingerprint = fingerprint_storage;
    }
    return source_identity::make_source_identity(
        filename, size, modified, fingerprint);
}

LoadResult load_single_source(const LoadRequest& request) {
    LoadResult result;
    if (!request.gpx.empty()) {
        result.session.name = request.gpx.stem().string();
        result.session.telemetry = parse_gpx(request.gpx, result.diagnostics);
    } else if (!request.racebox_csv.empty()) {
        result.session.name = request.racebox_csv.stem().string();
        result.session.csv_path = request.racebox_csv;
        result.session.telemetry = parse_racebox_csv(request.racebox_csv, result.diagnostics);
    } else if (!request.vbo.empty()) {
        result.session.name = request.vbo.stem().string();
        result.session.vbo_path = request.vbo;
        result.session.telemetry = parse_vbo(request.vbo, result.diagnostics);
    } else {
        throw std::runtime_error("The selected race-day run has no primary telemetry file");
    }
    if (result.session.telemetry.empty()) throw std::runtime_error("The selected race-day telemetry contains no samples");
    if (!request.sanwa_csv.empty()) {
        result.session.sanwa_path = request.sanwa_csv;
        result.session.radio = parse_sanwa_csv(request.sanwa_csv, result.diagnostics);
        result.session.alignment = align_radio(result.session.telemetry, result.session.radio);
    }
    result.session.laps = build_lap_index(result.session.telemetry);
    if (result.session.laps.empty()) {
        result.session.laps.push_back({1, 0, 0, result.session.telemetry.size() - 1,
            result.session.telemetry.time_us.back() - result.session.telemetry.time_us.front(), LapPhase::InLap});
    }
    result.session.sector_markers = default_sector_markers(result.session);
    result.session.theoretical_best = calculate_theoretical_best(result.session);
    result.imu_analysis = imu::analyze(result.session.telemetry);
    return result;
}

std::string phase_key(LapPhase phase) {
    switch (phase) {
        case LapPhase::Complete: return "complete";
        case LapPhase::OutLap: return "outlap";
        case LapPhase::InLap: return "inlap";
        case LapPhase::Invalid: return "invalid";
    }
    return "invalid";
}

double distance_metres(double latitude_a, double longitude_a, double latitude_b, double longitude_b) {
    constexpr double earth_radius_m = 6'371'000.0;
    constexpr double degrees_to_radians = 3.14159265358979323846 / 180.0;
    const auto lat_a = latitude_a * degrees_to_radians;
    const auto lat_b = latitude_b * degrees_to_radians;
    const auto delta_lat = (latitude_b - latitude_a) * degrees_to_radians;
    const auto delta_lon = (longitude_b - longitude_a) * degrees_to_radians;
    const auto sine_lat = std::sin(delta_lat * 0.5);
    const auto sine_lon = std::sin(delta_lon * 0.5);
    const auto value = sine_lat * sine_lat + std::cos(lat_a) * std::cos(lat_b) * sine_lon * sine_lon;
    return earth_radius_m * 2.0 * std::atan2(std::sqrt(value), std::sqrt(std::max(0.0, 1.0 - value)));
}

}  // namespace

std::vector<ChecklistItem> default_pre_run_checklist() {
    return {
        {"wheels", "Wheel nuts and tire glue checked", false, {}},
        {"steering", "Steering moves freely and trim is checked", false, {}},
        {"ride-height", "Ride height / droop checked", false, {}},
        {"fasteners", "Chassis, suspension, and body fasteners checked", false, {}},
        {"battery", "Battery charged, secured, and identified", false, {}},
        {"tires", "Correct tire set and run count recorded", false, {}},
        {"sauce", "Tire sauce compound and timing recorded", false, {}},
        {"warmers", "Tire warmer time and temperature recorded", false, {}},
        {"logger", "RaceBox, Sanwa logger, and transponder recording", false, {}},
    };
}

Day standard_day(bool include_q4, bool triple_a_main) {
    Day day;
    for (int index = 0; index < 3; ++index) add_practice(day);
    for (int index = 0; index < 3; ++index) add_qualifying(day);
    if (include_q4) add_qualifying(day);
    add_main_group(day, 'A', triple_a_main ? 3 : 1);
    return day;
}

Run& add_practice(Day& day) {
    const auto ordinal = next_ordinal(day, RunKind::Practice);
    day.runs.push_back(make_run(run_id("practice", ordinal), "Practice " + std::to_string(ordinal),
                                RunKind::Practice, ordinal));
    return day.runs.back();
}

Run& add_qualifying(Day& day) {
    const auto ordinal = next_ordinal(day, RunKind::Qualifying);
    day.runs.push_back(make_run(run_id("qualifying", ordinal), "Q" + std::to_string(ordinal),
                                RunKind::Qualifying, ordinal));
    return day.runs.back();
}

std::vector<std::size_t> add_main_group(Day& day, char group, int legs) {
    group = static_cast<char>(std::clamp(static_cast<int>(std::toupper(static_cast<unsigned char>(group))),
                                         static_cast<int>('A'), static_cast<int>('D')));
    legs = legs >= 3 ? 3 : 1;
    std::vector<std::size_t> added;
    for (int leg = 1; leg <= legs; ++leg) {
        const auto id = std::string("main-") + static_cast<char>(std::tolower(static_cast<unsigned char>(group))) +
                        "-" + std::to_string(leg);
        const auto existing = std::find_if(day.runs.begin(), day.runs.end(),
            [&](const Run& run) { return run.id == id; });
        if (existing != day.runs.end()) continue;
        auto label = std::string(1, group) + " Main";
        if (legs == 3) label += " " + std::to_string(leg);
        auto run = make_run(id, std::move(label), RunKind::Main, leg);
        run.main_group = group;
        run.main_leg = leg;
        day.runs.push_back(std::move(run));
        added.push_back(day.runs.size() - 1);
    }
    return added;
}

Run& add_race(Day& day, std::string label) {
    auto ordinal = next_ordinal(day, RunKind::Main);
    auto id = run_id("race", ordinal);
    while (std::any_of(
        day.runs.begin(), day.runs.end(),
        [&](const Run& run) { return run.id == id; })) {
        ++ordinal;
        id = run_id("race", ordinal);
    }
    if (label.empty()) label = "Race " + std::to_string(ordinal);
    auto run = make_run(
        std::move(id), bounded(std::move(label), 120),
        RunKind::Main, ordinal);
    run.main_group = 'A';
    run.main_leg = 0;
    day.runs.push_back(std::move(run));
    return day.runs.back();
}

Run& add_custom(Day& day, std::string label) {
    const auto ordinal = next_ordinal(day, RunKind::Custom);
    if (label.empty()) label = "Run " + std::to_string(ordinal);
    day.runs.push_back(make_run(run_id("custom", ordinal), bounded(std::move(label), 120),
                                RunKind::Custom, ordinal));
    return day.runs.back();
}

const char* kind_name(RunKind kind) noexcept {
    switch (kind) {
        case RunKind::Practice: return "Practice";
        case RunKind::Qualifying: return "Qualifying";
        case RunKind::Main: return "Main";
        case RunKind::Custom: return "Custom";
    }
    return "Custom";
}

TelemetrySourceKind telemetry_source_kind(
    const std::filesystem::path& path) {
    const auto extension = lower_extension(path);
    if (extension == L".vbo") return TelemetrySourceKind::Vbo;
    if (extension == L".csv") {
        return contains_sanwa_header(path)
            ? TelemetrySourceKind::SanwaCsv
            : TelemetrySourceKind::RaceBoxCsv;
    }
    if (extension == L".gpx") return TelemetrySourceKind::Gpx;
    if (extension == L".rbxsession" || extension == L".rbxlap") {
        return TelemetrySourceKind::NativeArchive;
    }
    return TelemetrySourceKind::Unsupported;
}

const char* telemetry_source_kind_name(
    TelemetrySourceKind kind) noexcept {
    switch (kind) {
        case TelemetrySourceKind::Vbo: return "VBO";
        case TelemetrySourceKind::RaceBoxCsv: return "RaceBox CSV";
        case TelemetrySourceKind::SanwaCsv: return "Sanwa CSV";
        case TelemetrySourceKind::Gpx: return "GPX";
        case TelemetrySourceKind::NativeArchive: return "native archive";
        case TelemetrySourceKind::Unsupported: return "unsupported";
    }
    return "unsupported";
}

TelemetrySourceValidation validate_telemetry_source_composition(
    const Run& run,
    bool require_primary) {
    TelemetrySourceValidation result;
    std::vector<TelemetrySourceKind> kinds;
    kinds.reserve(run.telemetry_files.size());
    for (const auto& path : run.telemetry_files) {
        const auto kind = telemetry_source_kind(path);
        if (kind == TelemetrySourceKind::Unsupported) {
            result.error = "Run contains an unsupported telemetry source: " +
                path_to_utf8(path.filename());
            return result;
        }
        if (std::find(kinds.begin(), kinds.end(), kind) != kinds.end()) {
            result.error = std::string("Run contains more than one ") +
                telemetry_source_kind_name(kind) + " source";
            return result;
        }
        kinds.push_back(kind);
        result.has_primary = result.has_primary ||
            kind == TelemetrySourceKind::Vbo ||
            kind == TelemetrySourceKind::RaceBoxCsv ||
            kind == TelemetrySourceKind::Gpx ||
            kind == TelemetrySourceKind::NativeArchive;
    }

    const auto has = [&](TelemetrySourceKind kind) {
        return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
    };
    if (has(TelemetrySourceKind::NativeArchive) && kinds.size() != 1) {
        result.error =
            "A native session archive cannot be mixed with other telemetry sources";
        return result;
    }
    if (has(TelemetrySourceKind::Gpx) &&
        (has(TelemetrySourceKind::Vbo) ||
         has(TelemetrySourceKind::RaceBoxCsv))) {
        result.error =
            "GPX cannot be mixed with VBO or RaceBox CSV telemetry";
        return result;
    }
    if (require_primary && !result.has_primary) {
        result.error =
            "Run needs a VBO, RaceBox CSV, GPX, or native session archive";
        return result;
    }
    result.ok = true;
    return result;
}

AttachSourcesResult attach_or_replace_telemetry_sources(
    Run& run,
    std::span<const std::filesystem::path> paths) {
    AttachSourcesResult result;
    if (paths.empty()) {
        result.ok = true;
        return result;
    }
    if (paths.size() > 8) {
        result.error = "A run can contain at most 8 telemetry sources";
        return result;
    }

    struct IncomingSource {
        std::filesystem::path path;
        TelemetrySourceKind kind{TelemetrySourceKind::Unsupported};
        source_identity::SourceIdentity identity;
    };
    std::vector<IncomingSource> incoming;
    incoming.reserve(paths.size());
    std::vector<TelemetrySourceKind> selected_kinds;
    selected_kinds.reserve(paths.size());
    for (const auto& path : paths) {
        std::error_code file_error;
        if (path.empty() ||
            !std::filesystem::is_regular_file(path, file_error) ||
            file_error) {
            result.error = "Selected telemetry source is missing or is not a file";
            return result;
        }
        const auto kind = telemetry_source_kind(path);
        if (kind == TelemetrySourceKind::Unsupported) {
            result.error = "Unsupported telemetry source: " +
                path_to_utf8(path.filename());
            return result;
        }
        if (std::find(selected_kinds.begin(), selected_kinds.end(), kind) !=
            selected_kinds.end()) {
            result.error = std::string("Select only one ") +
                telemetry_source_kind_name(kind) + " source at a time";
            return result;
        }
        selected_kinds.push_back(kind);
        incoming.push_back({path, kind, identity_for_path(path)});
    }

    const auto selected_has = [&](TelemetrySourceKind kind) {
        return std::find(selected_kinds.begin(), selected_kinds.end(), kind) !=
            selected_kinds.end();
    };
    const auto selected_archive =
        selected_has(TelemetrySourceKind::NativeArchive);
    if (selected_archive && incoming.size() != 1) {
        result.error =
            "A native session archive must be the only selected source";
        return result;
    }
    const auto selected_gpx = selected_has(TelemetrySourceKind::Gpx);
    const auto selected_vbo_or_racebox =
        selected_has(TelemetrySourceKind::Vbo) ||
        selected_has(TelemetrySourceKind::RaceBoxCsv);
    if (selected_gpx && selected_vbo_or_racebox) {
        result.error =
            "GPX cannot be mixed with VBO or RaceBox CSV telemetry";
        return result;
    }
    const auto existing_archive = std::any_of(
        run.telemetry_files.begin(), run.telemetry_files.end(),
        [](const auto& path) {
            return telemetry_source_kind(path) ==
                TelemetrySourceKind::NativeArchive;
        });
    if (existing_archive && !selected_archive &&
        !selected_gpx && !selected_vbo_or_racebox) {
        result.error =
            "A native session archive is self-contained. Select new "
            "primary telemetry together with supplemental files to "
            "replace it.";
        return result;
    }

    Run staged = run;
    refresh_telemetry_source_identities(staged);

    if (selected_archive) {
        result.replaced = staged.telemetry_files.empty() ? 0 : 1;
        result.added = staged.telemetry_files.empty() ? 1 : 0;
        staged.telemetry_files = {incoming.front().path};
        staged.telemetry_source_identities = {
            std::move(incoming.front().identity)};
    } else {
        auto transition_replaced_source = false;
        const auto erase_matching = [&](const auto& predicate) {
            for (std::size_t index = staged.telemetry_files.size();
                 index-- > 0;) {
                if (!predicate(
                        telemetry_source_kind(
                            staged.telemetry_files[index]))) {
                    continue;
                }
                staged.telemetry_files.erase(
                    staged.telemetry_files.begin() +
                    static_cast<std::ptrdiff_t>(index));
                staged.telemetry_source_identities.erase(
                    staged.telemetry_source_identities.begin() +
                    static_cast<std::ptrdiff_t>(index));
                transition_replaced_source = true;
            }
        };

        // Raw files deliberately replace an existing all-in-one archive.
        erase_matching([](TelemetrySourceKind kind) {
            return kind == TelemetrySourceKind::NativeArchive;
        });
        if (selected_gpx) {
            erase_matching([](TelemetrySourceKind kind) {
                return kind == TelemetrySourceKind::Vbo ||
                    kind == TelemetrySourceKind::RaceBoxCsv;
            });
        } else if (selected_vbo_or_racebox) {
            erase_matching([](TelemetrySourceKind kind) {
                return kind == TelemetrySourceKind::Gpx;
            });
        }

        for (auto& selected : incoming) {
            auto replaced_same_kind = false;
            for (std::size_t index = staged.telemetry_files.size();
                 index-- > 0;) {
                if (telemetry_source_kind(staged.telemetry_files[index]) !=
                    selected.kind) {
                    continue;
                }
                staged.telemetry_files.erase(
                    staged.telemetry_files.begin() +
                    static_cast<std::ptrdiff_t>(index));
                staged.telemetry_source_identities.erase(
                    staged.telemetry_source_identities.begin() +
                    static_cast<std::ptrdiff_t>(index));
                replaced_same_kind = true;
            }
            if (replaced_same_kind || transition_replaced_source) {
                ++result.replaced;
                transition_replaced_source = false;
            } else {
                ++result.added;
            }
            staged.telemetry_files.push_back(std::move(selected.path));
            staged.telemetry_source_identities.push_back(
                std::move(selected.identity));
        }
    }

    if (staged.telemetry_files.size() > 8) {
        result.added = 0;
        result.replaced = 0;
        result.error = "A run can contain at most 8 telemetry sources";
        return result;
    }
    const auto composition =
        validate_telemetry_source_composition(staged);
    if (!composition.ok) {
        result.added = 0;
        result.replaced = 0;
        result.error = composition.error;
        return result;
    }

    run.telemetry_files = std::move(staged.telemetry_files);
    run.telemetry_source_identities =
        std::move(staged.telemetry_source_identities);
    result.ok = true;
    return result;
}

bool remove_telemetry_source(
    Run& run, std::size_t source_index) {
    if (source_index >= run.telemetry_files.size()) return false;
    Run staged = run;
    refresh_telemetry_source_identities(staged);
    staged.telemetry_files.erase(
        staged.telemetry_files.begin() +
        static_cast<std::ptrdiff_t>(source_index));
    staged.telemetry_source_identities.erase(
        staged.telemetry_source_identities.begin() +
        static_cast<std::ptrdiff_t>(source_index));
    run.telemetry_files = std::move(staged.telemetry_files);
    run.telemetry_source_identities =
        std::move(staged.telemetry_source_identities);
    if (run.telemetry_files.empty()) run.recorded_at_utc.clear();
    return true;
}

void clear_telemetry_sources(Run& run) noexcept {
    run.telemetry_files.clear();
    run.telemetry_source_identities.clear();
    run.recorded_at_utc.clear();
}

bool has_racebox_csv(const Run& run) {
    return std::any_of(run.telemetry_files.begin(), run.telemetry_files.end(), [](const auto& path) {
        return telemetry_source_kind(path) ==
            TelemetrySourceKind::RaceBoxCsv;
    });
}

bool has_sanwa_csv(const Run& run) {
    return std::any_of(run.telemetry_files.begin(), run.telemetry_files.end(), [](const auto& path) {
        return telemetry_source_kind(path) ==
            TelemetrySourceKind::SanwaCsv;
    });
}

bool has_primary_telemetry(const Run& run) {
    return std::any_of(run.telemetry_files.begin(), run.telemetry_files.end(), [](const auto& path) {
        const auto kind = telemetry_source_kind(path);
        return kind == TelemetrySourceKind::Vbo ||
            kind == TelemetrySourceKind::RaceBoxCsv ||
            kind == TelemetrySourceKind::Gpx ||
            kind == TelemetrySourceKind::NativeArchive;
    });
}

TelemetrySourceState telemetry_source_state(
    const Run& run, std::size_t source_index,
    bool verify_content) noexcept {
    try {
        if (source_index >= run.telemetry_files.size()) {
            return TelemetrySourceState::Missing;
        }
        std::error_code error;
        const auto& path = run.telemetry_files[source_index];
        if (!std::filesystem::is_regular_file(path, error) || error) {
            return TelemetrySourceState::Missing;
        }
        if (source_index >= run.telemetry_source_identities.size()) {
            // In-memory callers from older integrations did not carry source
            // identities. They remain loadable; the next explicit attachment
            // refresh captures an identity.
            return TelemetrySourceState::Available;
        }
        const auto& expected =
            run.telemetry_source_identities[source_index];
        auto current = metadata_for_path(path);
        if (verify_content && expected.content_fingerprint) {
            current.content_fingerprint =
                file_fingerprint(path);
        }
        const std::array candidates{
            source_identity::Candidate{"current", current},
        };
        const auto match =
            source_identity::match_source(expected, candidates);
        if (match.suggestions.empty()) {
            return TelemetrySourceState::Changed;
        }
        const auto& evidence = match.suggestions.front();
        const auto timestamp_matches =
            !expected.modified_time_unix_ns ||
            evidence.modified_time_match;
        const auto fingerprint_matches =
            !verify_content || !expected.content_fingerprint ||
            evidence.fingerprint_match;
        return evidence.filename_match && evidence.size_match &&
                       timestamp_matches && fingerprint_matches
            ? TelemetrySourceState::Available
            : TelemetrySourceState::Changed;
    } catch (...) {
        return TelemetrySourceState::Missing;
    }
}

void refresh_telemetry_source_identities(Run& run) {
    std::vector<source_identity::SourceIdentity> refreshed;
    refreshed.reserve(run.telemetry_files.size());
    for (std::size_t index = 0; index < run.telemetry_files.size(); ++index) {
        const auto* previous = index < run.telemetry_source_identities.size()
            ? &run.telemetry_source_identities[index]
            : nullptr;
        refreshed.push_back(previous
            ? *previous
            : identity_for_path(run.telemetry_files[index]));
    }
    run.telemetry_source_identities = std::move(refreshed);
}

bool save(const Day& day, const std::filesystem::path& destination, std::string& error) noexcept {
    auto temporary = destination;
    temporary += L".writing";
    try {
        const auto base = destination.parent_path().empty() ? std::filesystem::current_path() : destination.parent_path();
        json root{
            {"format", kFileFormat},
            {"version", kFileVersion},
            {"event_name", bounded(day.event_name, 200)},
             {"track_name", bounded(day.track_name, 200)},
             {"date", bounded(day.date, 40)},
             {"runs", json::array()},
             {"setup_knowledge", json::array()},
         };
        for (const auto& run : day.runs) {
            json value{
                {"id", bounded(run.id, 120)},
                {"label", bounded(run.label, 120)},
                {"kind", kind_key(run.kind)},
                {"ordinal", run.ordinal},
                {"main_group", std::string(1, run.main_group)},
                {"main_leg", run.main_leg},
                {"recorded_at_utc", bounded(run.recorded_at_utc, 40)},
                {"telemetry_files", json::array()},
                {"telemetry_sources", json::array()},
                {"pre_run_notes", bounded(run.pre_run_notes, 16'000)},
                {"setup_changes", bounded(run.setup_changes, 16'000)},
                {"post_run_notes", bounded(run.post_run_notes, 16'000)},
                {"conditions", conditions_json(run.conditions)},
                {"checklist", json::array()},
            };
            for (std::size_t index = 0;
                 index < run.telemetry_files.size() && index < 8;
                 ++index) {
                const auto& path = run.telemetry_files[index];
                const auto* previous =
                    index < run.telemetry_source_identities.size()
                    ? &run.telemetry_source_identities[index]
                    : nullptr;
                const auto identity = previous
                    ? *previous
                    : identity_for_path(path);
                const auto stored =
                    path_to_utf8(stored_path(path, base));
                value["telemetry_files"].push_back(stored);
                value["telemetry_sources"].push_back({
                    {"path", stored},
                    {"identity", source_identity_json(identity)},
                });
            }
            for (const auto& item : run.checklist) {
                value["checklist"].push_back({
                    {"id", bounded(item.id, 120)}, {"label", bounded(item.label, 300)},
                    {"checked", item.checked}, {"note", bounded(item.note, 1000)},
                });
            }
            root["runs"].push_back(std::move(value));
        }
        const auto knowledge_first = day.setup_knowledge.size() > kMaximumSetupKnowledgeRecords
            ? day.setup_knowledge.size() - kMaximumSetupKnowledgeRecords : 0;
        for (auto index = knowledge_first; index < day.setup_knowledge.size(); ++index) {
            const auto& record = day.setup_knowledge[index];
            json observations = json::array();
            for (const auto& observation : record.observations) {
                json evidence_ids = json::array();
                for (const auto& id : observation.evidence_ids) {
                    if (evidence_ids.size() >= 8) break;
                    evidence_ids.push_back(bounded(id, 180));
                }
                if (observations.size() >= 8) break;
                observations.push_back({
                    {"area", bounded(observation.area, 120)},
                    {"change", bounded(observation.change, 800)},
                    {"meaning", bounded(observation.meaning, 1400)},
                    {"evidence_ids", std::move(evidence_ids)},
                });
            }
            json confounds = json::array();
            for (const auto& confound : record.confounds) {
                if (confounds.size() >= 8) break;
                confounds.push_back(bounded(confound, 800));
            }
            root["setup_knowledge"].push_back({
                {"id", bounded(record.id, 160)},
                {"created_at_utc", bounded(record.created_at_utc, 40)},
                {"event_name", bounded(record.event_name, 200)},
                {"track_name", bounded(record.track_name, 200)},
                {"previous_run_id", bounded(record.previous_run_id, 120)},
                {"previous_run_label", bounded(record.previous_run_label, 120)},
                {"current_run_id", bounded(record.current_run_id, 120)},
                {"current_run_label", bounded(record.current_run_label, 120)},
                {"handling_question", bounded(record.handling_question, 4000)},
                {"setup_change", bounded(record.setup_change, 4000)},
                {"driver_result", bounded(record.driver_result, 4000)},
                {"previous_conditions", conditions_json(record.previous_conditions)},
                {"current_conditions", conditions_json(record.current_conditions)},
                {"verdict", bounded(record.verdict, 32)},
                {"confidence", std::clamp(record.confidence, 0, 100)},
                {"summary", bounded(record.summary, 4000)},
                {"observations", std::move(observations)},
                {"confounds", std::move(confounds)},
                {"next_test", bounded(record.next_test, 2000)},
                {"causality_note", bounded(record.causality_note, 1200)},
                {"route", bounded(record.route, 160)},
                {"model", bounded(record.model, 160)},
                {"thinking", bounded(record.thinking, 32)},
                {"selected_lane", bounded(record.selected_lane, 80)},
                {"evidence", evidence_json(record.evidence)},
            });
        }

        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        if (!destination.parent_path().empty()) std::filesystem::create_directories(destination.parent_path());
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("Could not create the temporary race-day file");
            output << root.dump(2) << '\n';
            output.flush();
            if (!output) throw std::runtime_error("Could not finish writing the race-day file");
        }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw std::runtime_error("Could not replace the race-day file");
        }
#else
        std::error_code replace_error;
        std::filesystem::rename(
            temporary, destination, replace_error);
        if (replace_error) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw std::runtime_error(
                "Could not replace the race-day file: " +
                replace_error.message());
        }
#endif
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        error = exception.what();
        return false;
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        error = "Could not save the race-day file";
        return false;
    }
}

bool load(const std::filesystem::path& source, Day& day, std::string& error) noexcept {
    try {
        std::error_code size_error;
        const auto byte_size =
            std::filesystem::file_size(source, size_error);
        if (size_error || byte_size > kMaximumRaceDayBytes) {
            throw std::runtime_error(
                "Race-day file is missing or larger than 32 MB");
        }
        std::ifstream input(source, std::ios::binary);
        if (!input) throw std::runtime_error("Could not open the race-day file");
        const auto root = json::parse(input);
        if (!root.is_object() || root.value("format", std::string{}) != kFileFormat) {
            throw std::runtime_error("This is not a RaceBox race-day file");
        }
        const auto file_version = root.value("version", 0);
        if (file_version < 1) {
            throw std::runtime_error("The race-day file version is invalid");
        }
        if (file_version > kFileVersion) {
            throw std::runtime_error("The race-day file is newer than this application");
        }
        Day loaded;
        loaded.event_name = bounded(root.value("event_name", std::string{}), 200);
        loaded.track_name = bounded(root.value("track_name", std::string{}), 200);
        loaded.date = bounded(root.value("date", std::string{}), 40);
        const auto base = source.parent_path().empty() ? std::filesystem::current_path() : source.parent_path();
        std::unordered_set<std::string> ids;
        for (const auto& value : root.value("runs", json::array())) {
            if (!value.is_object() || loaded.runs.size() >= 100) break;
            Run run;
            run.id = bounded(value.value("id", std::string{}), 120);
            if (run.id.empty() || !ids.insert(run.id).second) continue;
            run.label = bounded(value.value("label", std::string{"Run"}), 120);
            run.kind = parse_kind(value.value("kind", std::string{"custom"}));
            run.ordinal = std::max(0, value.value("ordinal", 0));
            const auto group = value.value("main_group", std::string{"A"});
            run.main_group = group.empty() ? 'A' : static_cast<char>(std::toupper(static_cast<unsigned char>(group.front())));
            run.main_leg = std::clamp(value.value("main_leg", 0), 0, 3);
            run.recorded_at_utc = bounded(
                value.value("recorded_at_utc", std::string{}), 40);
            run.pre_run_notes = bounded(value.value("pre_run_notes", std::string{}), 16'000);
            run.setup_changes = bounded(value.value("setup_changes", std::string{}), 16'000);
            run.post_run_notes = bounded(value.value("post_run_notes", std::string{}), 16'000);
            if (const auto conditions = value.find("conditions"); conditions != value.end()) {
                run.conditions = read_conditions(*conditions);
            }
            if (file_version >= 3 && value.contains("telemetry_sources") &&
                value["telemetry_sources"].is_array() &&
                !value["telemetry_sources"].empty()) {
                for (const auto& source_value :
                     value["telemetry_sources"]) {
                    if (!source_value.is_object() ||
                        run.telemetry_files.size() >= 8) {
                        break;
                    }
                    const auto stored = source_value.find("path");
                    if (stored == source_value.end() ||
                        !stored->is_string()) {
                        continue;
                    }
                    const auto path = resolved_path(
                        path_from_utf8(bounded(
                            stored->get<std::string>(), 4096)),
                        base);
                    run.telemetry_files.push_back(path);
                    const auto identity =
                        source_value.find("identity");
                    run.telemetry_source_identities.push_back(
                        identity == source_value.end()
                            ? identity_for_path(path)
                            : read_source_identity(*identity, path));
                }
            } else {
                for (const auto& path :
                     value.value("telemetry_files", json::array())) {
                    if (!path.is_string() ||
                        run.telemetry_files.size() >= 8) {
                        break;
                    }
                    const auto resolved = resolved_path(
                        path_from_utf8(bounded(
                            path.get<std::string>(), 4096)),
                        base);
                    run.telemetry_files.push_back(resolved);
                    run.telemetry_source_identities.push_back(
                        identity_for_path(resolved));
                }
            }
            for (const auto& item : value.value("checklist", json::array())) {
                if (!item.is_object() || run.checklist.size() >= 32) break;
                run.checklist.push_back({
                    bounded(item.value("id", std::string{}), 120),
                    bounded(item.value("label", std::string{}), 300),
                    item.value("checked", false),
                    bounded(item.value("note", std::string{}), 1000),
                });
            }
            if (run.checklist.empty()) run.checklist = default_pre_run_checklist();
            loaded.runs.push_back(std::move(run));
        }
        std::unordered_set<std::string> knowledge_ids;
        for (const auto& value : root.value("setup_knowledge", json::array())) {
            if (!value.is_object() || loaded.setup_knowledge.size() >= kMaximumSetupKnowledgeRecords) break;
            SetupKnowledgeRecord record;
            record.id = bounded(value.value("id", std::string{}), 160);
            if (record.id.empty() || !knowledge_ids.insert(record.id).second) continue;
            record.created_at_utc = bounded(value.value("created_at_utc", std::string{}), 40);
            record.event_name = bounded(value.value("event_name", std::string{}), 200);
            record.track_name = bounded(value.value("track_name", std::string{}), 200);
            record.previous_run_id = bounded(value.value("previous_run_id", std::string{}), 120);
            record.previous_run_label = bounded(value.value("previous_run_label", std::string{}), 120);
            record.current_run_id = bounded(value.value("current_run_id", std::string{}), 120);
            record.current_run_label = bounded(value.value("current_run_label", std::string{}), 120);
            record.handling_question = bounded(value.value("handling_question", std::string{}), 4000);
            record.setup_change = bounded(value.value("setup_change", std::string{}), 4000);
            record.driver_result = bounded(value.value("driver_result", std::string{}), 4000);
            if (const auto conditions = value.find("previous_conditions"); conditions != value.end()) {
                record.previous_conditions = read_conditions(*conditions);
            }
            if (const auto conditions = value.find("current_conditions"); conditions != value.end()) {
                record.current_conditions = read_conditions(*conditions);
            }
            record.verdict = bounded(value.value("verdict", std::string{"inconclusive"}), 32);
            record.confidence = std::clamp(value.value("confidence", 0), 0, 100);
            record.summary = bounded(value.value("summary", std::string{}), 4000);
            for (const auto& observation : value.value("observations", json::array())) {
                if (!observation.is_object() || record.observations.size() >= 8) break;
                SetupKnowledgeObservation result{
                    bounded(observation.value("area", std::string{}), 120),
                    bounded(observation.value("change", std::string{}), 800),
                    bounded(observation.value("meaning", std::string{}), 1400),
                    {},
                };
                for (const auto& id : observation.value("evidence_ids", json::array())) {
                    if (!id.is_string() || result.evidence_ids.size() >= 8) break;
                    result.evidence_ids.push_back(bounded(id.get<std::string>(), 180));
                }
                record.observations.push_back(std::move(result));
            }
            for (const auto& confound : value.value("confounds", json::array())) {
                if (!confound.is_string() || record.confounds.size() >= 8) break;
                record.confounds.push_back(bounded(confound.get<std::string>(), 800));
            }
            record.next_test = bounded(value.value("next_test", std::string{}), 2000);
            record.causality_note = bounded(value.value("causality_note", std::string{}), 1200);
            record.route = bounded(value.value("route", std::string{}), 160);
            record.model = bounded(value.value("model", std::string{}), 160);
            record.thinking = bounded(value.value("thinking", std::string{}), 32);
            record.selected_lane = bounded(value.value("selected_lane", std::string{}), 80);
            if (const auto evidence = value.find("evidence"); evidence != value.end()) {
                record.evidence = read_evidence(*evidence);
            }
            loaded.setup_knowledge.push_back(std::move(record));
        }
        day = std::move(loaded);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    } catch (...) {
        error = "Could not load the race-day file";
        return false;
    }
}

void upsert_setup_knowledge(Day& day, SetupKnowledgeRecord record) {
    if (record.id.empty()) return;
    const auto existing = std::find_if(day.setup_knowledge.begin(), day.setup_knowledge.end(),
        [&](const SetupKnowledgeRecord& item) { return item.id == record.id; });
    if (existing != day.setup_knowledge.end()) {
        day.setup_knowledge.erase(existing);
    }
    day.setup_knowledge.push_back(std::move(record));
    if (day.setup_knowledge.size() > kMaximumSetupKnowledgeRecords) {
        day.setup_knowledge.erase(
            day.setup_knowledge.begin(),
            day.setup_knowledge.begin() +
                static_cast<std::ptrdiff_t>(day.setup_knowledge.size() - kMaximumSetupKnowledgeRecords));
    }
}

std::vector<std::size_t> relevant_setup_knowledge(
    const Day& day,
    std::string_view question,
    const Run& current_run,
    std::size_t maximum_records) {
    maximum_records = std::min(maximum_records, static_cast<std::size_t>(8));
    if (maximum_records == 0 || day.setup_knowledge.empty()) return {};

    const auto query_terms = searchable_terms(
        std::string(question) + " " + current_run.setup_changes + " " + current_run.post_run_notes);
    struct Candidate {
        std::size_t index{};
        int score{};
    };
    std::vector<Candidate> candidates;
    candidates.reserve(day.setup_knowledge.size());
    for (std::size_t index = 0; index < day.setup_knowledge.size(); ++index) {
        const auto& record = day.setup_knowledge[index];
        int score = 0;
        if (same_text(day.track_name, record.track_name)) score += 50;
        const auto terms = searchable_terms(knowledge_search_text(record));
        for (const auto& term : query_terms) {
            if (terms.contains(term)) score += 6;
        }
        if (same_text(current_run.conditions.tire_compound, record.current_conditions.tire_compound)) score += 8;
        if (same_text(current_run.conditions.sauce_compound, record.current_conditions.sauce_compound)) score += 5;
        if (same_text(current_run.conditions.track_condition, record.current_conditions.track_condition)) score += 5;
        if (current_run.conditions.tire_runs_before >= 0 &&
            record.current_conditions.tire_runs_before >= 0 &&
            std::abs(current_run.conditions.tire_runs_before -
                     record.current_conditions.tire_runs_before) <= 1) {
            score += 4;
        }
        score += std::clamp(record.confidence, 0, 100) / 20;
        if (score > 0) candidates.push_back({index, score});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [&](const Candidate& left, const Candidate& right) {
        if (left.score != right.score) return left.score > right.score;
        return left.index > right.index;
    });
    if (candidates.size() > maximum_records) candidates.resize(maximum_records);
    std::vector<std::size_t> result;
    result.reserve(candidates.size());
    for (const auto& candidate : candidates) result.push_back(candidate.index);
    return result;
}

nlohmann::json build_prior_setup_results(
    const Day& day,
    std::span<const std::size_t> record_indices) {
    json results = json::array();
    for (const auto index : record_indices) {
        if (index >= day.setup_knowledge.size() || results.size() >= 8) continue;
        const auto& record = day.setup_knowledge[index];
        json observations = json::array();
        for (const auto& observation : record.observations) {
            if (observations.size() >= 5) break;
            json ids = json::array();
            for (const auto& id : observation.evidence_ids) {
                if (ids.size() >= 5) break;
                ids.push_back(bounded(id, 180));
            }
            observations.push_back({
                {"area", bounded(observation.area, 120)},
                {"change", bounded(observation.change, 800)},
                {"meaning", bounded(observation.meaning, 1400)},
                {"evidence_ids", std::move(ids)},
            });
        }
        json confounds = json::array();
        for (const auto& confound : record.confounds) {
            if (confounds.size() >= 5) break;
            confounds.push_back(bounded(confound, 800));
        }
        results.push_back({
            {"record_id", bounded(record.id, 160)},
            {"created_at_utc", bounded(record.created_at_utc, 40)},
            {"track_name", bounded(record.track_name, 200)},
            {"previous_run", bounded(record.previous_run_label, 120)},
            {"current_run", bounded(record.current_run_label, 120)},
            {"handling_question", bounded(record.handling_question, 2000)},
            {"setup_change", bounded(record.setup_change, 2000)},
            {"driver_result", bounded(record.driver_result, 2000)},
            {"previous_conditions", conditions_json(record.previous_conditions)},
            {"current_conditions", conditions_json(record.current_conditions)},
            {"verdict", bounded(record.verdict, 32)},
            {"confidence", std::clamp(record.confidence, 0, 100)},
            {"summary", bounded(record.summary, 2500)},
            {"observations", std::move(observations)},
            {"confounds", std::move(confounds)},
            {"next_test", bounded(record.next_test, 1200)},
            {"causality_note", bounded(record.causality_note, 800)},
            {"evidence_summary", evidence_json(record.evidence)},
        });
    }
    return results;
}

LoadResult load_run_telemetry(const Run& run) {
    if (run.telemetry_files.empty()) throw std::runtime_error("No telemetry files are attached to this run");
    const auto composition =
        validate_telemetry_source_composition(run, true);
    if (!composition.ok) throw std::runtime_error(composition.error);
    for (std::size_t index = 0; index < run.telemetry_files.size();
         ++index) {
        switch (telemetry_source_state(run, index, true)) {
            case TelemetrySourceState::Available:
                break;
            case TelemetrySourceState::Changed:
                throw std::runtime_error(
                    "A telemetry attachment changed after it was saved. Review and confirm the file in Race Day first.");
            case TelemetrySourceState::Missing:
                throw std::runtime_error(
                    "A telemetry attachment is missing. Relink it in Race Day first.");
        }
    }
    if (run.telemetry_files.size() == 1) {
        const auto extension = lower_extension(run.telemetry_files.front());
        if (extension == L".rbxsession" || extension == L".rbxlap") {
            LoadResult result;
            std::string error;
            if (!load_session_archive(run.telemetry_files.front(), result.session, error)) {
                throw std::runtime_error(error);
            }
            result.imu_analysis = imu::analyze(result.session.telemetry);
            result.diagnostics.emplace_back("Loaded race-day native session archive");
            return result;
        }
    }

    LoadRequest request;
    for (const auto& file : run.telemetry_files) {
        const auto extension = lower_extension(file);
        if (extension == L".vbo") request.vbo = file;
        else if (extension == L".csv" && contains_sanwa_header(file)) request.sanwa_csv = file;
        else if (extension == L".csv") request.racebox_csv = file;
        else if (extension == L".gpx") request.gpx = file;
    }
    if (!request.vbo.empty() && !request.racebox_csv.empty()) return load_session(request);
    return load_single_source(request);
}

std::string build_analytics_csv(
    const Session& session,
    const imu::Analysis& imu_analysis,
    std::size_t maximum_rows) {
    if (session.telemetry.empty()) throw std::runtime_error("Cannot build analytics CSV from empty telemetry");
    maximum_rows = std::max<std::size_t>(1, maximum_rows);
    const auto step = std::max<std::size_t>(1, (session.telemetry.size() + maximum_rows - 1) / maximum_rows);

    std::vector<const LapInfo*> lap_at(session.telemetry.size(), nullptr);
    std::vector<double> progress_at(session.telemetry.size(), 0.0);
    for (const auto& lap : session.laps) {
        if (lap.begin_index >= session.telemetry.size()) continue;
        const auto end = std::min(lap.end_index, session.telemetry.size() - 1);
        for (auto index = lap.begin_index; index <= end; ++index) lap_at[index] = &lap;
        std::vector<double> cumulative(end - lap.begin_index + 1, 0.0);
        for (std::size_t local = 1; local < cumulative.size(); ++local) {
            const auto index = lap.begin_index + local;
            cumulative[local] = cumulative[local - 1] + distance_metres(
                session.telemetry.latitude[index - 1], session.telemetry.longitude[index - 1],
                session.telemetry.latitude[index], session.telemetry.longitude[index]);
        }
        const auto total = cumulative.back();
        for (std::size_t local = 0; local < cumulative.size(); ++local) {
            progress_at[lap.begin_index + local] = total > 0.0 ? cumulative[local] / total * 100.0 : 0.0;
        }
    }

    const auto& telemetry = session.telemetry;
    const auto zero = imu_analysis.initial_stationary_zero_used
        ? imu_analysis.acceleration_zero_g
        : std::array<double, 3>{0.0, 0.0, 0.0};
    std::ostringstream output;
    output << "#contract," << kAnalyticsCsvVersion << '\n';
    output << "#setup_contract," << kSetupAnalyticsVersion << '\n';
    output << "#sample_step," << step << '\n';
    output << "time_s,absolute_time_us,lap_raw,lap_race,lap_phase,lap_elapsed_s,lap_progress_percent,"
              "latitude,longitude,speed_kmh,"
              "lateral_g,longitudinal_g,vertical_g,yaw_rate_dps,raw_lateral_g,raw_vertical_g,steering_percent,"
              "throttle_percent,brake_percent,satellites\n";
    output << std::fixed << std::setprecision(6);
    for (std::size_t index = 0; index < telemetry.size(); index += step) {
        const auto* lap = lap_at[index];
        const auto radio = sample_radio(session, telemetry.time_us[index]);
        const auto elapsed = lap
            ? static_cast<double>(telemetry.time_us[index] - telemetry.time_us[lap->begin_index]) / 1'000'000.0
            : 0.0;
        output << static_cast<double>(telemetry.time_us[index]) / 1'000'000.0 << ','
               << telemetry.absolute_time_us[index] << ','
               << (lap ? lap->raw_lap : telemetry.raw_lap[index]) << ','
               << (lap ? lap->race_lap : 0) << ','
               << (lap ? phase_key(lap->phase) : "invalid") << ','
               << elapsed << ','
               << progress_at[index] << ','
               << telemetry.latitude[index] << ','
               << telemetry.longitude[index] << ','
               << telemetry.speed_kmh[index] << ','
               << static_cast<double>(telemetry.lateral_g[index]) - zero[1] << ','
               << static_cast<double>(telemetry.longitudinal_g[index]) - zero[0] << ','
               << static_cast<double>(telemetry.vertical_g[index]) - zero[2] << ',';
        if (index < imu_analysis.vehicle_yaw_rate_dps.size() && imu_analysis.calibration.heading_correlation >= 0.60) {
            output << imu_analysis.vehicle_yaw_rate_dps[index];
        }
        output << ',' << telemetry.lateral_g[index] << ',' << telemetry.vertical_g[index] << ',';
        if (radio.valid) {
            output << radio.steering << ',' << radio.throttle << ',' << radio.brake;
        } else {
            output << ",,";
        }
        output << ',' << static_cast<int>(telemetry.satellites[index]) << '\n';
    }
    return output.str();
}

SetupAnalyticsSummary analyze_setup_change(
    const Session& previous,
    const imu::Analysis& previous_imu,
    const Session& current,
    const imu::Analysis& current_imu) {
    const auto before = summarize_setup_run(previous, previous_imu);
    const auto after = summarize_setup_run(current, current_imu);

    SetupAnalyticsSummary result;
    result.quality_confidence = std::min(before.quality_confidence, after.quality_confidence);
    result.track_status = before.complete_laps > 0 && after.complete_laps > 0 ? "comparable_progress" : "data_limited";

    if (before.top_three_lap_s && after.top_three_lap_s) {
        result.lap_time_delta_s = *after.top_three_lap_s - *before.top_three_lap_s;
    }
    if (before.lateral_response_g_per_steer && after.lateral_response_g_per_steer) {
        result.lateral_response_delta_g =
            (*after.lateral_response_g_per_steer - *before.lateral_response_g_per_steer) * 50.0;
        result.lateral_status = std::abs(*result.lateral_response_delta_g) >= 0.02 ? "measured" : "within_noise";
    }
    if (before.forward_accel_g && after.forward_accel_g) {
        result.forward_bite_delta_g = *after.forward_accel_g - *before.forward_accel_g;
        result.forward_status = std::abs(*result.forward_bite_delta_g) >= 0.04 ? "measured" : "within_noise";
    }
    result.previous_brake_indicators = before.brake_indicators;
    result.current_brake_indicators = after.brake_indicators;

    if (before.straight_top_speed_kmh && after.straight_top_speed_kmh) {
        result.straight_top_speed_delta_kmh = *after.straight_top_speed_kmh - *before.straight_top_speed_kmh;
        result.top_speed_status =
            std::abs(*result.straight_top_speed_delta_kmh) >= 0.75 ? "measured" : "within_noise";
    }
    if (before.straight_entry_speed_kmh && after.straight_entry_speed_kmh) {
        result.straight_entry_speed_delta_kmh = *after.straight_entry_speed_kmh - *before.straight_entry_speed_kmh;
    }
    if (before.straight_accel_g && after.straight_accel_g) {
        result.straight_acceleration_delta_g = *after.straight_accel_g - *before.straight_accel_g;
    }
    result.straight_speed_attribution = classify_straight_attribution(
        result.straight_entry_speed_delta_kmh,
        result.straight_acceleration_delta_g,
        result.straight_top_speed_delta_kmh);

    if (before.brake_decel_g && after.brake_decel_g) {
        result.brake_decel_delta_g = *after.brake_decel_g - *before.brake_decel_g;
        result.brake_response_status = std::abs(*result.brake_decel_delta_g) >= 0.10 ? "measured" : "within_noise";
    }
    if (before.brake_response_delay_s && after.brake_response_delay_s) {
        result.brake_response_delay_delta_s = *after.brake_response_delay_s - *before.brake_response_delay_s;
        if (*result.brake_response_delay_delta_s >= 0.08) result.brake_response_status = "possible_lockup_or_low_grip";
    }

    if (before.steering_for_lateral_percent && after.steering_for_lateral_percent) {
        result.steering_for_lateral_g_delta_percent =
            *after.steering_for_lateral_percent - *before.steering_for_lateral_percent;
    }
    if (before.yaw_per_steering_dps && after.yaw_per_steering_dps) {
        result.yaw_per_steering_delta_dps = *after.yaw_per_steering_dps - *before.yaw_per_steering_dps;
    }
    if (result.steering_for_lateral_g_delta_percent || result.yaw_per_steering_delta_dps) {
        result.corner_balance_status = "measured";
        if (result.steering_for_lateral_g_delta_percent &&
            *result.steering_for_lateral_g_delta_percent > 8.0) {
            result.corner_balance_status = "more_steering_for_same_load";
        }
        if (result.yaw_per_steering_delta_dps && *result.yaw_per_steering_delta_dps > 0.08) {
            result.corner_balance_status = "more_rotation_per_steering";
        }
    }

    if (before.overdriving_index && after.overdriving_index) {
        result.overdriving_index_delta = *after.overdriving_index - *before.overdriving_index;
    }
    if (before.overdriving_risk_sample_percent && after.overdriving_risk_sample_percent) {
        result.overdriving_risk_sample_delta_percent =
            *after.overdriving_risk_sample_percent - *before.overdriving_risk_sample_percent;
    }
    result.current_late_overdriving_delta_score = after.late_overdriving_delta_score;
    result.overdriving_status = classify_overdriving(
        result.overdriving_index_delta,
        result.overdriving_risk_sample_delta_percent,
        result.current_late_overdriving_delta_score,
        result.lap_time_delta_s);

    if (before.surface_tilt_roll_deg && after.surface_tilt_roll_deg) {
        result.surface_tilt_delta_deg = *after.surface_tilt_roll_deg - *before.surface_tilt_roll_deg;
    }
    if (before.chassis_roll_signature_deg && after.chassis_roll_signature_deg) {
        result.chassis_roll_delta_deg = *after.chassis_roll_signature_deg - *before.chassis_roll_signature_deg;
    }
    if (before.roll_per_lateral_g_deg && after.roll_per_lateral_g_deg) {
        result.roll_per_lateral_g_delta_deg = *after.roll_per_lateral_g_deg - *before.roll_per_lateral_g_deg;
    }
    result.previous_roll_rate_p90_dps = before.roll_rate_p90_dps;
    result.current_roll_rate_p90_dps = after.roll_rate_p90_dps;
    result.previous_roll_rate_samples = before.roll_rate_samples;
    result.current_roll_rate_samples = after.roll_rate_samples;
    if (before.roll_rate_p90_dps && after.roll_rate_p90_dps) {
        result.roll_rate_delta_dps = *after.roll_rate_p90_dps - *before.roll_rate_p90_dps;
    }
    if (before.roll_rate_per_lateral_g_dps && after.roll_rate_per_lateral_g_dps) {
        result.roll_rate_per_lateral_g_delta_dps =
            *after.roll_rate_per_lateral_g_dps - *before.roll_rate_per_lateral_g_dps;
    }
    result.current_late_roll_rate_delta_dps = after.late_roll_rate_delta_dps;
    result.roll_rate_status = classify_roll_rate(
        result.previous_roll_rate_p90_dps,
        result.current_roll_rate_p90_dps,
        result.roll_rate_delta_dps,
        result.roll_rate_per_lateral_g_delta_dps,
        result.surface_tilt_delta_deg,
        result.previous_roll_rate_samples,
        result.current_roll_rate_samples);
    result.current_surface_tilt_source = after.surface_tilt_source;
    result.current_surface_tilt_samples = after.surface_tilt_samples;
    result.chassis_roll_status = classify_chassis_roll(
        result.chassis_roll_delta_deg,
        result.roll_per_lateral_g_delta_deg,
        result.surface_tilt_delta_deg);

    return result;
}

}  // namespace racebox::race_day
