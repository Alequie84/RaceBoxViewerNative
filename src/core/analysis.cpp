#include "racebox/domain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

namespace racebox {
namespace {

constexpr Timestamp kSecond = 1'000'000;
constexpr double kEarthRadiusM = 6'371'000.0;

std::size_t nearest_index(const std::vector<Timestamp>& values, Timestamp target) {
    if (values.empty()) return 0;
    const auto iterator = std::lower_bound(values.begin(), values.end(), target);
    if (iterator == values.begin()) return 0;
    if (iterator == values.end()) return values.size() - 1;
    const auto upper = static_cast<std::size_t>(iterator - values.begin());
    return target - values[upper - 1] <= values[upper] - target ? upper - 1 : upper;
}

double pearson(const std::vector<double>& x, const std::vector<double>& y) {
    if (x.size() < 4 || x.size() != y.size()) return 0.0;
    const auto mean_x = std::accumulate(x.begin(), x.end(), 0.0) / static_cast<double>(x.size());
    const auto mean_y = std::accumulate(y.begin(), y.end(), 0.0) / static_cast<double>(y.size());
    double numerator = 0.0, square_x = 0.0, square_y = 0.0;
    for (std::size_t index = 0; index < x.size(); ++index) {
        const auto dx = x[index] - mean_x;
        const auto dy = y[index] - mean_y;
        numerator += dx * dy;
        square_x += dx * dx;
        square_y += dy * dy;
    }
    return square_x > 0.0 && square_y > 0.0 ? numerator / std::sqrt(square_x * square_y) : 0.0;
}

double median(std::vector<double> values) {
    values.erase(std::remove_if(values.begin(), values.end(), [](double value) { return !std::isfinite(value); }), values.end());
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

double response_at(const TelemetrySeries& telemetry, Timestamp command_time) {
    const auto center = nearest_index(telemetry.time_us, command_time + 180'000);
    const auto before = nearest_index(telemetry.time_us, command_time - 120'000);
    const auto after = nearest_index(telemetry.time_us, command_time + 480'000);
    if (after <= before || telemetry.time_us[after] <= telemetry.time_us[before]) return std::numeric_limits<double>::quiet_NaN();
    const auto seconds = static_cast<double>(telemetry.time_us[after] - telemetry.time_us[before]) / kSecond;
    const auto accel_g = (((telemetry.speed_kmh[after] - telemetry.speed_kmh[before]) / 3.6) / seconds) / 9.81;
    const auto longitudinal = -static_cast<double>(telemetry.longitudinal_g[center]);
    return std::clamp(accel_g * 0.7 + longitudinal * 0.3, -1.0, 1.0);
}

double heading_rate_at(const TelemetrySeries& telemetry, Timestamp time) {
    const auto before = nearest_index(telemetry.time_us, time - 100'000);
    const auto after = nearest_index(telemetry.time_us, time + 100'000);
    if (after <= before || telemetry.time_us[after] <= telemetry.time_us[before]) return std::numeric_limits<double>::quiet_NaN();
    auto delta = static_cast<double>(telemetry.heading_deg[after] - telemetry.heading_deg[before]);
    while (delta > 180.0) delta -= 360.0;
    while (delta < -180.0) delta += 360.0;
    return delta / (static_cast<double>(telemetry.time_us[after] - telemetry.time_us[before]) / kSecond);
}

double gps_path_yaw_rate_at(const TelemetrySeries& telemetry, Timestamp time) {
    constexpr Timestamp kCourseWindow = 300'000;
    const auto middle = nearest_index(telemetry.time_us, time);
    const auto before = nearest_index(telemetry.time_us, time - kCourseWindow);
    const auto after = nearest_index(telemetry.time_us, time + kCourseWindow);
    if (before >= middle || middle >= after) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    constexpr double degrees_to_radians =
        3.14159265358979323846 / 180.0;
    const auto course = [&](std::size_t first, std::size_t last) {
        const auto average_latitude =
            (telemetry.latitude[first] + telemetry.latitude[last]) * 0.5;
        const auto east =
            (telemetry.longitude[last] - telemetry.longitude[first]) *
            111'320.0 * std::cos(average_latitude * degrees_to_radians);
        const auto north =
            (telemetry.latitude[last] - telemetry.latitude[first]) * 110'540.0;
        return std::pair{
            std::atan2(east, north) / degrees_to_radians,
            std::hypot(east, north)};
    };
    const auto [course_in, distance_in] = course(before, middle);
    const auto [course_out, distance_out] = course(middle, after);
    if (distance_in < 0.35 || distance_out < 0.35) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    auto delta = course_out - course_in;
    while (delta > 180.0) delta -= 360.0;
    while (delta < -180.0) delta += 360.0;
    const auto centre_separation =
        static_cast<double>(telemetry.time_us[after] - telemetry.time_us[before]) /
        (2.0 * kSecond);
    return centre_separation > 0.0
        ? delta / centre_separation
        : std::numeric_limits<double>::quiet_NaN();
}

struct TriggerScore {
    Timestamp correction{};
    int sign{1};
    double score{};
    double correlation{};
    double direction{};
    std::size_t overlap{};
};

struct LaunchCue {
    Timestamp radio_onset_us{};
    Timestamp telemetry_onset_us{};
    Timestamp correction_us{};
    int trigger_sign{1};
    bool altitude_descent_supported{};
};

struct VehicleLaunchCue {
    Timestamp onset_us{};
    bool altitude_descent_supported{};
};

std::optional<Timestamp> first_sustained_forward_trigger(
    const RadioSeries& radio, int sign) {
    constexpr Timestamp kMinimumNeutralLead = 2 * kSecond;
    // A transmitter tap can be a brake check or an accidental blip while the
    // driver walks to the stand. The physical launch cue needs a deliberate,
    // sustained pull before it is allowed to set the clock.
    constexpr Timestamp kMinimumCommandDuration = 300'000;
    for (std::size_t index = 0; index < radio.size() &&
         radio.elapsed_us[index] < kMinimumNeutralLead; ++index) {
        if (radio.trigger_percent[index] * sign >= 8.0F) {
            return std::nullopt;
        }
    }
    for (std::size_t begin = 0; begin < radio.size(); ++begin) {
        if (radio.elapsed_us[begin] < kMinimumNeutralLead ||
            radio.trigger_percent[begin] * sign < 8.0F) {
            continue;
        }
        auto end = begin;
        while (end + 1 < radio.size() &&
               radio.trigger_percent[end + 1] * sign >= 8.0F) {
            ++end;
        }
        if (radio.elapsed_us[end] - radio.elapsed_us[begin] >=
            kMinimumCommandDuration) {
            return radio.elapsed_us[begin];
        }
        begin = end;
    }
    return std::nullopt;
}

std::optional<VehicleLaunchCue> first_sustained_vehicle_launch(
    const TelemetrySeries& telemetry) {
    constexpr Timestamp kStationaryWindow = 10 * kSecond;
    constexpr Timestamp kMinimumObservedStationaryTime = 8 * kSecond;
    constexpr Timestamp kMovementWindow = 5 * kSecond;
    for (std::size_t index = 0; index < telemetry.size(); ++index) {
        const auto time = telemetry.time_us[index];
        if (telemetry.speed_kmh[index] < 0.8F ||
            (index > 0 && telemetry.speed_kmh[index - 1] >= 0.8F)) {
            continue;
        }

        const auto stationary_begin = std::lower_bound(
            telemetry.time_us.begin(), telemetry.time_us.end(),
            time - kStationaryWindow);
        const auto stationary_first = static_cast<std::size_t>(
            stationary_begin - telemetry.time_us.begin());
        if (stationary_first >= index ||
            time - telemetry.time_us[stationary_first] <
                kMinimumObservedStationaryTime) {
            continue;
        }
        std::size_t stationary_count = 0;
        for (auto before = stationary_first; before < index; ++before) {
            if (telemetry.speed_kmh[before] <= 1.0F) ++stationary_count;
        }
        const auto stationary_samples = index - stationary_first;
        if (stationary_samples < 4 ||
            stationary_count * 10 < stationary_samples * 9) {
            continue;
        }

        const auto movement_end_iterator = std::lower_bound(
            telemetry.time_us.begin(), telemetry.time_us.end(),
            time + kMovementWindow);
        if (movement_end_iterator == telemetry.time_us.end()) break;
        const auto movement_end = static_cast<std::size_t>(
            movement_end_iterator - telemetry.time_us.begin());
        std::size_t movement_count = 0;
        float maximum_speed = 0.0F;
        for (auto after = index; after <= movement_end; ++after) {
            if (telemetry.speed_kmh[after] >= 3.0F) ++movement_count;
            maximum_speed = std::max(
                maximum_speed, telemetry.speed_kmh[after]);
        }
        const auto movement_samples = movement_end - index + 1;
        if (movement_count * 10 >= movement_samples * 7 &&
            maximum_speed >= 10.0F) {
            auto last_handling = index > 0 ? index - 1 : 0;
            while (last_handling > 0 &&
                   telemetry.speed_kmh[last_handling] <= 3.0F) {
                --last_handling;
            }
            const auto placement_time = telemetry.time_us[last_handling];
            const auto altitude_mean = [&](Timestamp begin, Timestamp end) {
                const auto first = std::lower_bound(
                    telemetry.time_us.begin(), telemetry.time_us.end(), begin);
                const auto last = std::lower_bound(
                    telemetry.time_us.begin(), telemetry.time_us.end(), end);
                double total = 0.0;
                std::size_t count = 0;
                for (auto sample = first; sample != last; ++sample) {
                    const auto altitude_index = static_cast<std::size_t>(
                        sample - telemetry.time_us.begin());
                    total += telemetry.altitude_m[altitude_index];
                    ++count;
                }
                return count ? std::optional(total / count) : std::nullopt;
            };
            const auto held_altitude = altitude_mean(
                placement_time - 5 * kSecond, placement_time);
            const auto placed_altitude = altitude_mean(
                placement_time, placement_time + 5 * kSecond);
            const auto descended = held_altitude && placed_altitude &&
                *held_altitude - *placed_altitude >= 0.5;
            const auto vertical_quiet = [&] {
                const auto first = std::lower_bound(
                    telemetry.time_us.begin(), telemetry.time_us.end(),
                    placement_time);
                const auto last = std::lower_bound(
                    telemetry.time_us.begin(), telemetry.time_us.end(),
                    placement_time + 5 * kSecond);
                double sum = 0.0;
                double squared = 0.0;
                std::size_t count = 0;
                for (auto sample = first; sample != last; ++sample) {
                    const auto vertical_index = static_cast<std::size_t>(
                        sample - telemetry.time_us.begin());
                    const auto value = static_cast<double>(
                        telemetry.vertical_g[vertical_index]);
                    if (!std::isfinite(value)) continue;
                    sum += value;
                    squared += value * value;
                    ++count;
                }
                if (count < 10) return false;
                const auto mean = sum / static_cast<double>(count);
                const auto variance = std::max(
                    0.0, squared / static_cast<double>(count) - mean * mean);
                return std::sqrt(variance) <= 0.15;
            }();
            return VehicleLaunchCue{time, descended && vertical_quiet};
        }
    }
    return std::nullopt;
}

TriggerScore score_trigger(const TelemetrySeries& telemetry, const RadioSeries& radio, Timestamp anchor, Timestamp correction, int sign) {
    std::vector<double> commands;
    std::vector<double> responses;
    commands.reserve(radio.size() / 2);
    responses.reserve(radio.size() / 2);
    std::size_t direction_checks = 0, direction_matches = 0;
    for (std::size_t index = 0; index < radio.size(); index += 2) {
        const auto command = static_cast<double>(radio.trigger_percent[index] * sign) / 100.0;
        if (std::abs(command) < 0.08) continue;
        const auto time = anchor + correction + radio.elapsed_us[index];
        if (time < telemetry.time_us.front() || time > telemetry.time_us.back()) continue;
        const auto response = response_at(telemetry, time);
        if (!std::isfinite(response)) continue;
        commands.push_back(command);
        responses.push_back(response);
        if (std::abs(response) > 0.025) {
            ++direction_checks;
            if (std::signbit(command) == std::signbit(response)) ++direction_matches;
        }
    }
    const auto correlation = pearson(commands, responses);
    const auto direction = direction_checks ? static_cast<double>(direction_matches) / direction_checks : 0.5;
    const auto evidence = std::min(1.0, static_cast<double>(commands.size()) / 8.0);
    const auto score = std::clamp(((correlation + 1.0) * 0.35 + direction * 0.30) * evidence, 0.0, 1.0);
    return {correction, sign, score, correlation, direction, commands.size()};
}

struct RawRadioSample { bool valid{}; double steering{}; double trigger{}; };

RawRadioSample interpolate_radio(const RadioSeries& radio, Timestamp elapsed) {
    if (radio.empty() || elapsed < radio.elapsed_us.front() || elapsed > radio.elapsed_us.back()) return {};
    const auto upper = nearest_index(radio.elapsed_us, elapsed);
    std::size_t lower_index = upper, upper_index = upper;
    if (radio.elapsed_us[upper] < elapsed && upper + 1 < radio.size()) upper_index = upper + 1;
    if (radio.elapsed_us[upper] > elapsed && upper > 0) lower_index = upper - 1;
    const auto gap = radio.elapsed_us[upper_index] - radio.elapsed_us[lower_index];
    if (gap > 120'000) return {};
    const auto ratio = gap > 0 ? static_cast<double>(elapsed - radio.elapsed_us[lower_index]) / gap : 0.0;
    const auto interpolate = [&](const std::vector<float>& values) {
        return static_cast<double>(values[lower_index]) + (values[upper_index] - values[lower_index]) * ratio;
    };
    return {true, interpolate(radio.steering_percent), interpolate(radio.trigger_percent)};
}

double haversine_m(double lat_a, double lon_a, double lat_b, double lon_b) {
    constexpr double pi = 3.14159265358979323846;
    const auto to_rad = [](double degrees) { return degrees * pi / 180.0; };
    const auto d_lat = to_rad(lat_b - lat_a);
    const auto d_lon = to_rad(lon_b - lon_a);
    const auto a = std::sin(d_lat / 2.0) * std::sin(d_lat / 2.0) +
                   std::cos(to_rad(lat_a)) * std::cos(to_rad(lat_b)) * std::sin(d_lon / 2.0) * std::sin(d_lon / 2.0);
    return 2.0 * kEarthRadiusM * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

std::optional<std::size_t> nearest_marker_index(const TelemetrySeries& telemetry, const LapInfo& lap, const PhysicalMarker& marker) {
    auto best_distance = std::numeric_limits<double>::infinity();
    std::size_t best_index = lap.begin_index;
    for (auto index = lap.begin_index; index <= lap.end_index; ++index) {
        const auto distance = haversine_m(telemetry.latitude[index], telemetry.longitude[index], marker.latitude, marker.longitude);
        if (distance < best_distance) {
            best_distance = distance;
            best_index = index;
        }
    }
    return best_distance <= 8.0 ? std::optional(best_index) : std::nullopt;
}

}  // namespace

std::vector<LapInfo> build_lap_index(const TelemetrySeries& telemetry) {
    std::vector<LapInfo> laps;
    if (telemetry.empty()) return laps;
    std::size_t begin = 0;
    while (begin < telemetry.size()) {
        const auto raw_lap = telemetry.raw_lap[begin];
        std::size_t end = begin;
        while (end + 1 < telemetry.size() && telemetry.raw_lap[end + 1] == raw_lap) ++end;
        if (raw_lap > 0 && end > begin) {
            laps.push_back({raw_lap, 0, begin, end, telemetry.time_us[end] - telemetry.time_us[begin], LapPhase::Invalid});
        }
        begin = end + 1;
    }
    std::vector<double> plausible;
    for (const auto& lap : laps) {
        const auto seconds = static_cast<double>(lap.duration_us) / kSecond;
        if (seconds >= 5.0 && seconds <= 120.0) plausible.push_back(seconds);
    }
    const auto typical = median(plausible);
    int race_lap = 0;
    for (std::size_t index = 0; index < laps.size(); ++index) {
        auto& lap = laps[index];
        const auto seconds = static_cast<double>(lap.duration_us) / kSecond;
        if (typical > 0.0 && seconds >= typical * 0.55 && seconds <= typical * 1.8) {
            lap.phase = LapPhase::Complete;
            lap.race_lap = ++race_lap;
        } else if (index == 0) {
            lap.phase = LapPhase::OutLap;
        } else if (index + 1 == laps.size()) {
            lap.phase = LapPhase::InLap;
        }
    }
    return laps;
}

AlignmentResult align_radio(const TelemetrySeries& telemetry, const RadioSeries& radio) {
    AlignmentResult result;
    if (telemetry.empty() || radio.empty()) {
        result.reason = "Telemetry or radio data is empty";
        return result;
    }
    result.compatible = true;
    result.used_end_anchor = true;
    result.radio_anchor_us = telemetry.time_us.back() - radio.duration_us();
    std::vector<LaunchCue> launch_cues;
    const auto telemetry_onset =
        first_sustained_vehicle_launch(telemetry);
    if (telemetry_onset) {
        for (const int sign : {1, -1}) {
            const auto radio_onset =
                first_sustained_forward_trigger(radio, sign);
            if (!radio_onset) continue;
            const auto nominal_command_time =
                result.radio_anchor_us + *radio_onset;
            const auto correction = telemetry_onset->onset_us -
                nominal_command_time - result.throttle_response_us;
            const auto launch_signal = score_trigger(
                telemetry, radio, result.radio_anchor_us, correction, sign);
            if (launch_signal.score >= 0.60 &&
                launch_signal.direction >= 0.55) {
                launch_cues.push_back(
                    {*radio_onset, telemetry_onset->onset_us, correction, sign,
                        telemetry_onset->altitude_descent_supported});
            }
        }
    }

    const auto evaluate = [&](Timestamp correction, int sign) {
        auto candidate = score_trigger(
            telemetry, radio, result.radio_anchor_us, correction, sign);
        for (const auto& cue : launch_cues) {
            if (cue.trigger_sign != sign) continue;
            const auto launch_error = std::llabs(
                correction - cue.correction_us);
            constexpr Timestamp kLaunchTolerance = 600'000;
            if (launch_error <= kLaunchTolerance) {
                const auto agreement = 1.0 -
                    static_cast<double>(launch_error) / kLaunchTolerance;
                candidate.score += agreement * 0.25;
            }
        }
        return candidate;
    };

    TriggerScore best;
    const auto search = [&](int sign, Timestamp begin, Timestamp end) {
        for (auto correction = begin; correction <= end;
             correction += 50'000) {
            const auto candidate = evaluate(correction, sign);
            if (candidate.score > best.score) best = candidate;
        }
    };
    for (const int sign : {1, -1}) {
        search(sign, -kSecond, kSecond);
    }
    for (const auto& cue : launch_cues) {
        search(cue.trigger_sign, cue.correction_us - kSecond,
            cue.correction_us + kSecond);
    }

    const auto fine_start = best.correction - 100'000;
    const auto fine_end = best.correction + 100'000;
    for (auto correction = fine_start; correction <= fine_end;
         correction += 1'000) {
        const auto candidate = evaluate(correction, best.sign);
        if (candidate.score > best.score) best = candidate;
    }
    result.fine_correction_us = best.correction;
    result.trigger_sign = best.sign;

    const auto selected_launch = std::find_if(
        launch_cues.begin(), launch_cues.end(), [&](const LaunchCue& cue) {
            return cue.trigger_sign == result.trigger_sign &&
                std::llabs(cue.correction_us -
                    result.fine_correction_us) <= 600'000;
        });
    result.reason = selected_launch != launch_cues.end()
        ? selected_launch->altitude_descent_supported
            ? "Final self-alignment ignored the manually set Sanwa time; a downward RaceBox altitude trend plus quiet vertical G supported car placement, and the first sustained forward trigger was matched to the first genuine GPS departure after the car sat stationary on the track; steering wiggles and brake-direction candidates did not set the clock"
            : "Final self-alignment ignored the manually set Sanwa time and matched the first sustained forward trigger to the first genuine GPS departure after the car sat stationary on the track; steering wiggles and brake-direction candidates did not set the clock; altitude was not required"
        : radio.filename_time_us
            ? "Manually set Sanwa filename time was ignored; recording end supplied a rough fallback and signals supplied the final correction because no safe stationary-launch cue was present"
            : "Sanwa recording has no usable time; recording end supplied a rough fallback and signals supplied the final correction because no safe stationary-launch cue was present";

    double best_heading_steering = 0.0;
    double best_gps_steering = 0.0;
    Timestamp best_heading_lag = 200'000;
    Timestamp best_gps_lag = 200'000;
    std::size_t best_gps_samples = 0;
    for (Timestamp lag = 0; lag <= 400'000; lag += 20'000) {
        std::vector<double> heading_commands, heading_yaw_rates;
        std::vector<double> gps_commands, gps_yaw_rates;
        heading_commands.reserve(radio.size() / 4);
        heading_yaw_rates.reserve(radio.size() / 4);
        gps_commands.reserve(radio.size() / 4);
        gps_yaw_rates.reserve(radio.size() / 4);
        for (std::size_t index = 0; index < radio.size(); index += 4) {
            const auto command = static_cast<double>(radio.steering_percent[index]) / 100.0;
            if (std::abs(command) < 0.08) continue;
            const auto time = result.radio_anchor_us + result.fine_correction_us + radio.elapsed_us[index] + lag;
            if (time < telemetry.time_us.front() || time > telemetry.time_us.back()) continue;
            const auto row = nearest_index(telemetry.time_us, time);
            if (telemetry.speed_kmh[row] < 5.0F) continue;
            const auto heading_yaw = heading_rate_at(telemetry, time);
            if (std::isfinite(heading_yaw)) {
                heading_commands.push_back(command);
                heading_yaw_rates.push_back(heading_yaw);
            }
            const auto gps_yaw = gps_path_yaw_rate_at(telemetry, time);
            if (std::isfinite(gps_yaw)) {
                gps_commands.push_back(command);
                gps_yaw_rates.push_back(gps_yaw);
            }
        }
        const auto heading_correlation = pearson(
            heading_commands, heading_yaw_rates);
        if (std::abs(heading_correlation) >
            std::abs(best_heading_steering)) {
            best_heading_steering = heading_correlation;
            best_heading_lag = lag;
        }
        const auto gps_correlation = pearson(gps_commands, gps_yaw_rates);
        if (gps_commands.size() >= 100 &&
            std::abs(gps_correlation) > std::abs(best_gps_steering)) {
            best_gps_steering = gps_correlation;
            best_gps_lag = lag;
            best_gps_samples = gps_commands.size();
        }
    }
    const auto use_gps_path = best_gps_samples >= 100 &&
        std::abs(best_gps_steering) >= 0.35;
    const auto steering_polarity_evidence = use_gps_path
        ? best_gps_steering : best_heading_steering;
    result.steering_sign = steering_polarity_evidence < 0.0 ? -1 : 1;
    result.steering_response_us = use_gps_path
        ? best_gps_lag : best_heading_lag;
    result.steering_yaw_source = use_gps_path ? "gps_path" : "racebox_heading";
    result.gps_yaw_samples = use_gps_path ? best_gps_samples : 0;

    std::vector<double> commands, responses, steering, yaw;
    std::vector<double> steering_heading, heading_yaw;
    std::vector<double> gps_yaw_for_heading, heading_for_gps;
    std::size_t direction_checks = 0, direction_matches = 0;
    for (std::size_t index = 0; index < telemetry.size(); index += 2) {
        const auto radio_elapsed = telemetry.time_us[index] - result.radio_anchor_us - result.fine_correction_us;
        const auto sample = interpolate_radio(radio, radio_elapsed);
        if (!sample.valid || telemetry.speed_kmh[index] < 5.0F) continue;
        const auto raw_trigger = sample.trigger * result.trigger_sign;
        const auto signed_control = std::abs(raw_trigger) > 2.0 ? std::copysign((std::abs(raw_trigger) - 2.0) / 98.0, raw_trigger) : 0.0;
        if (std::abs(signed_control) >= 0.08) {
            const auto response = response_at(telemetry, telemetry.time_us[index]);
            if (std::isfinite(response)) {
                commands.push_back(signed_control);
                responses.push_back(response);
                if (std::abs(response) > 0.025) {
                    ++direction_checks;
                    if (std::signbit(signed_control) == std::signbit(response)) ++direction_matches;
                }
            }
        }
        const auto corrected_steering = sample.steering * result.steering_sign / 100.0;
        if (std::abs(corrected_steering) >= 0.08) {
            const auto response_time = telemetry.time_us[index] +
                result.steering_response_us;
            const auto racebox_heading_yaw = heading_rate_at(
                telemetry, response_time);
            const auto gps_yaw = gps_path_yaw_rate_at(
                telemetry, response_time);
            const auto selected_yaw = use_gps_path
                ? gps_yaw : racebox_heading_yaw;
            if (std::isfinite(selected_yaw)) {
                steering.push_back(corrected_steering);
                yaw.push_back(selected_yaw);
            }
            if (std::isfinite(racebox_heading_yaw)) {
                steering_heading.push_back(corrected_steering);
                heading_yaw.push_back(racebox_heading_yaw);
            }
            if (std::isfinite(racebox_heading_yaw) && std::isfinite(gps_yaw)) {
                heading_for_gps.push_back(racebox_heading_yaw);
                gps_yaw_for_heading.push_back(gps_yaw);
            }
        }
    }
    result.trigger_correlation = pearson(commands, responses);
    result.direction_agreement = direction_checks ? static_cast<double>(direction_matches) / direction_checks : 0.0;
    result.steering_yaw_correlation = pearson(steering, yaw);
    result.steering_heading_correlation = pearson(
        steering_heading, heading_yaw);
    result.heading_gps_yaw_correlation = pearson(
        heading_for_gps, gps_yaw_for_heading);
    result.launch_cue_used = selected_launch != launch_cues.end();
    result.altitude_supported = result.launch_cue_used &&
        selected_launch->altitude_descent_supported;
    result.reason += use_gps_path
        ? "; steering left/right polarity was set independently from GPS path yaw so the map direction is authoritative"
        : "; GPS path yaw was too weak for a direction check, so steering polarity used the RaceBox heading fallback";
    if (use_gps_path && result.heading_gps_yaw_correlation < -0.50) {
        result.reason +=
            "; the RaceBox heading direction was inverted relative to the GPS trace and was not allowed to flip left/right";
    }

    Session temporary;
    temporary.telemetry = telemetry;
    temporary.radio = radio;
    temporary.alignment = result;
    temporary.laps = build_lap_index(telemetry);
    const LapInfo* reference = nullptr;
    for (const auto& lap : temporary.laps) {
        if (lap.phase != LapPhase::Complete) continue;
        if (!reference || lap.duration_us < reference->duration_us) reference = &lap;
    }
    std::vector<double> lap_correlations;
    if (reference) {
        for (const auto& lap : temporary.laps) {
            if (lap.phase != LapPhase::Complete) continue;
            std::vector<double> reference_values, lap_values;
            for (int point = 0; point < 240; ++point) {
                const auto fraction = static_cast<double>(point) / 239.0;
                const auto reference_index = reference->begin_index + static_cast<std::size_t>(std::llround(fraction * (reference->end_index - reference->begin_index)));
                const auto lap_index = lap.begin_index + static_cast<std::size_t>(std::llround(fraction * (lap.end_index - lap.begin_index)));
                const auto reference_sample = interpolate_radio(radio, telemetry.time_us[reference_index] - result.radio_anchor_us - result.fine_correction_us);
                const auto lap_sample = interpolate_radio(radio, telemetry.time_us[lap_index] - result.radio_anchor_us - result.fine_correction_us);
                if (!reference_sample.valid || !lap_sample.valid) continue;
                reference_values.push_back(reference_sample.steering * result.steering_sign);
                lap_values.push_back(lap_sample.steering * result.steering_sign);
            }
            if (reference_values.size() >= 100) lap_correlations.push_back(pearson(reference_values, lap_values));
        }
    }
    result.lap_steering_correlation = median(lap_correlations);
    const auto lap_evidence_trusted = reference
        ? result.lap_steering_correlation >= 0.70
        : selected_launch != launch_cues.end();
    const auto trusted = std::abs(result.trigger_correlation) >= 0.35 &&
        result.direction_agreement >= 0.60 &&
        result.steering_yaw_correlation >= 0.60 &&
        lap_evidence_trusted;
    result.confidence = trusted ? "high" : "low";
    if (!trusted) {
        result.compatible = false;
        result.reason +=
            "; alignment evidence is too weak, so Sanwa controls are withheld";
    }
    return result;
}

RadioSample sample_radio(const Session& session, Timestamp telemetry_time_us, float deadband) {
    if (!session.alignment.compatible) return {};
    const auto elapsed = telemetry_time_us - session.alignment.radio_anchor_us - session.alignment.fine_correction_us;
    const auto raw = interpolate_radio(session.radio, elapsed);
    if (!raw.valid) return {};
    const auto trigger = static_cast<float>(raw.trigger * session.alignment.trigger_sign);
    const auto span = std::max(1.0F, 100.0F - deadband);
    const auto scale = [&](float value) { return std::clamp((value - deadband) / span * 100.0F, 0.0F, 100.0F); };
    return {
        true,
        trigger > deadband ? scale(trigger) : 0.0F,
        trigger < -deadband ? scale(-trigger) : 0.0F,
        static_cast<float>(std::clamp(raw.steering * session.alignment.steering_sign, -100.0, 100.0))};
}

std::vector<PhysicalMarker> default_sector_markers(const Session& session, int sector_count) {
    std::vector<PhysicalMarker> markers;
    if (sector_count < 2 || session.telemetry.empty()) return markers;
    const LapInfo* fastest = nullptr;
    for (const auto& lap : session.laps) {
        if (lap.phase != LapPhase::Complete) continue;
        if (!fastest || lap.duration_us < fastest->duration_us) fastest = &lap;
    }
    if (!fastest) return markers;
    std::vector<double> cumulative(fastest->end_index - fastest->begin_index + 1, 0.0);
    for (std::size_t local = 1; local < cumulative.size(); ++local) {
        const auto index = fastest->begin_index + local;
        cumulative[local] = cumulative[local - 1] + haversine_m(
            session.telemetry.latitude[index - 1], session.telemetry.longitude[index - 1],
            session.telemetry.latitude[index], session.telemetry.longitude[index]);
    }
    for (int sector = 1; sector < sector_count; ++sector) {
        const auto fraction = static_cast<double>(sector) / sector_count;
        const auto target = cumulative.back() * fraction;
        const auto iterator = std::lower_bound(cumulative.begin(), cumulative.end(), target);
        const auto local = static_cast<std::size_t>(std::distance(cumulative.begin(), iterator));
        const auto index = fastest->begin_index + std::min(local, cumulative.size() - 1);
        markers.push_back({session.telemetry.latitude[index], session.telemetry.longitude[index], fraction});
    }
    return markers;
}

bool move_sector_marker(Session& session, std::size_t marker_index, double reference_fraction) {
    if (marker_index >= session.sector_markers.size() || session.telemetry.empty()) return false;
    const LapInfo* fastest = nullptr;
    for (const auto& lap : session.laps) {
        if (lap.phase != LapPhase::Complete) continue;
        if (!fastest || lap.duration_us < fastest->duration_us) fastest = &lap;
    }
    if (!fastest || fastest->end_index <= fastest->begin_index) return false;
    const auto lower = marker_index == 0 ? 0.02 : session.sector_markers[marker_index - 1].reference_fraction + 0.02;
    const auto upper = marker_index + 1 == session.sector_markers.size() ? 0.98 : session.sector_markers[marker_index + 1].reference_fraction - 0.02;
    const auto fraction = std::clamp(reference_fraction, lower, upper);
    std::vector<double> cumulative(fastest->end_index - fastest->begin_index + 1, 0.0);
    for (std::size_t local = 1; local < cumulative.size(); ++local) {
        const auto index = fastest->begin_index + local;
        cumulative[local] = cumulative[local - 1] + haversine_m(
            session.telemetry.latitude[index - 1], session.telemetry.longitude[index - 1],
            session.telemetry.latitude[index], session.telemetry.longitude[index]);
    }
    if (cumulative.back() <= 0.0) return false;
    const auto iterator = std::lower_bound(cumulative.begin(), cumulative.end(), cumulative.back() * fraction);
    const auto local = std::min<std::size_t>(static_cast<std::size_t>(std::distance(cumulative.begin(), iterator)), cumulative.size() - 1);
    const auto index = fastest->begin_index + local;
    session.sector_markers[marker_index] = {
        session.telemetry.latitude[index], session.telemetry.longitude[index], fraction};
    session.theoretical_best = calculate_theoretical_best(session);
    return true;
}

TheoreticalBest calculate_theoretical_best(const Session& session) {
    TheoreticalBest result;
    if (session.sector_markers.empty()) return result;
    const auto sector_count = session.sector_markers.size() + 1;
    std::vector<std::optional<SectorResult>> best(sector_count);
    for (const auto& lap : session.laps) {
        if (lap.phase != LapPhase::Complete) continue;
        std::vector<std::size_t> boundaries{lap.begin_index};
        bool valid = true;
        for (const auto& marker : session.sector_markers) {
            const auto index = nearest_marker_index(session.telemetry, lap, marker);
            if (!index || *index <= boundaries.back()) {
                valid = false;
                break;
            }
            boundaries.push_back(*index);
        }
        boundaries.push_back(lap.end_index);
        if (!valid) continue;
        for (std::size_t sector = 0; sector < sector_count; ++sector) {
            const auto duration = session.telemetry.time_us[boundaries[sector + 1]] - session.telemetry.time_us[boundaries[sector]];
            if (!best[sector] || duration < best[sector]->duration_us) {
                best[sector] = SectorResult{static_cast<int>(sector + 1), duration, lap.raw_lap, boundaries[sector], boundaries[sector + 1]};
            }
        }
    }
    for (const auto& sector : best) {
        if (!sector) return {};
        result.duration_us += sector->duration_us;
        result.sectors.push_back(*sector);
    }
    return result;
}

MapPixel map_coordinate_to_pixel(const MapBackground& background, double latitude, double longitude) {
    if (!background.georeferenced || background.metres_per_pixel <= 0.0F) return {};
    constexpr double degrees_to_radians = 3.14159265358979323846 / 180.0;
    const auto longitude_metres = 111'320.0 * std::cos(background.reference_latitude * degrees_to_radians);
    const auto east = (longitude - background.reference_longitude) * longitude_metres;
    const auto north = (latitude - background.reference_latitude) * 110'540.0;
    const auto angle = static_cast<double>(background.rotation_degrees) * degrees_to_radians;
    const auto cosine = std::cos(angle);
    const auto sine = std::sin(angle);
    const auto scale = static_cast<double>(background.metres_per_pixel);
    return {
        background.reference_pixel_x + (east * cosine + north * sine) / scale,
        background.reference_pixel_y + (east * sine - north * cosine) / scale};
}

MapCoordinate map_pixel_to_coordinate(const MapBackground& background, double pixel_x, double pixel_y) {
    if (!background.georeferenced || background.metres_per_pixel <= 0.0F) return {};
    constexpr double degrees_to_radians = 3.14159265358979323846 / 180.0;
    const auto angle = static_cast<double>(background.rotation_degrees) * degrees_to_radians;
    const auto cosine = std::cos(angle);
    const auto sine = std::sin(angle);
    const auto x = (pixel_x - background.reference_pixel_x) * background.metres_per_pixel;
    const auto y = (pixel_y - background.reference_pixel_y) * background.metres_per_pixel;
    const auto east = x * cosine + y * sine;
    const auto north = x * sine - y * cosine;
    const auto longitude_metres = 111'320.0 * std::cos(background.reference_latitude * degrees_to_radians);
    return {
        background.reference_latitude + north / 110'540.0,
        background.reference_longitude + east / longitude_metres};
}

MapPixelBounds resolve_map_image_crop(const MapBackground& background, double image_width,
                                      double image_height) noexcept {
    if (!std::isfinite(image_width) || !std::isfinite(image_height) || image_width <= 0.0 || image_height <= 0.0) {
        return {};
    }
    const MapPixelBounds full{0.0, 0.0, image_width, image_height};
    const auto left = static_cast<double>(background.source_crop_left_px);
    const auto top = static_cast<double>(background.source_crop_top_px);
    const auto right = static_cast<double>(background.source_crop_right_px);
    const auto bottom = static_cast<double>(background.source_crop_bottom_px);
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom) ||
        right <= left || bottom <= top) {
        return full;
    }
    MapPixelBounds crop{
        std::clamp(left, 0.0, image_width),
        std::clamp(top, 0.0, image_height),
        std::clamp(right, 0.0, image_width),
        std::clamp(bottom, 0.0, image_height)};
    if (crop.right <= crop.left || crop.bottom <= crop.top) return full;
    return crop;
}

}  // namespace racebox
