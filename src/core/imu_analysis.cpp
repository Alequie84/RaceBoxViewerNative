#include "racebox/imu_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace racebox::imu {
namespace {

double median(std::vector<double> values) {
    values.erase(std::remove_if(values.begin(), values.end(), [](double value) { return !std::isfinite(value); }), values.end());
    if (values.empty()) return 0.0;
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

double wrapped_degrees(double value) {
    while (value > 180.0) value -= 360.0;
    while (value < -180.0) value += 360.0;
    return value;
}

double correlation(const std::vector<double>& left, const std::vector<double>& right) {
    if (left.size() != right.size() || left.size() < 3) return 0.0;
    const auto left_mean = std::accumulate(left.begin(), left.end(), 0.0) / static_cast<double>(left.size());
    const auto right_mean = std::accumulate(right.begin(), right.end(), 0.0) / static_cast<double>(right.size());
    double covariance = 0.0, left_energy = 0.0, right_energy = 0.0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto a = left[index] - left_mean;
        const auto b = right[index] - right_mean;
        covariance += a * b;
        left_energy += a * a;
        right_energy += b * b;
    }
    const auto denominator = std::sqrt(left_energy * right_energy);
    return denominator > 1e-12 ? covariance / denominator : 0.0;
}

struct SampleRange {
    std::size_t begin{};
    std::size_t end{};
    bool valid{};
};

SampleRange initial_stationary_range(const TelemetrySeries& telemetry) {
    constexpr auto speed_limit_kmh = 1.5F;
    constexpr Timestamp required_us = 2'000'000;
    auto begin = telemetry.size();
    for (std::size_t index = 0; index <= telemetry.size(); ++index) {
        const auto stationary = index < telemetry.size() && telemetry.speed_kmh[index] <= speed_limit_kmh;
        if (stationary && begin == telemetry.size()) begin = index;
        if ((!stationary || index == telemetry.size()) && begin != telemetry.size()) {
            const auto run_end = index - 1;
            const auto sample_period = run_end + 1 < telemetry.size()
                ? telemetry.time_us[run_end + 1] - telemetry.time_us[run_end]
                : run_end > begin ? telemetry.time_us[run_end] - telemetry.time_us[run_end - 1] : Timestamp{};
            if (telemetry.time_us[run_end] - telemetry.time_us[begin] + sample_period >= required_us) {
                auto calibration_end = begin;
                while (calibration_end < run_end && telemetry.time_us[calibration_end] - telemetry.time_us[begin] < required_us) {
                    ++calibration_end;
                }
                return {begin, calibration_end, true};
            }
            begin = telemetry.size();
        }
    }
    return {};
}

std::array<double, 3> median_axes(const std::array<const std::vector<float>*, 3>& channels,
                                  std::size_t begin, std::size_t end) {
    std::array<std::vector<double>, 3> values;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        values[axis].reserve(end - begin + 1);
        for (auto index = begin; index <= end; ++index) values[axis].push_back((*channels[axis])[index]);
    }
    return {median(std::move(values[0])), median(std::move(values[1])), median(std::move(values[2]))};
}

std::array<double, 3> estimate_bias(const TelemetrySeries& telemetry, const SampleRange& zero_range,
                                    bool& stationary_used) {
    std::array<std::vector<double>, 3> all;
    for (std::size_t index = 0; index < telemetry.size(); ++index) {
        const std::array<double, 3> sample{telemetry.gyro_x_dps[index], telemetry.gyro_y_dps[index], telemetry.gyro_z_dps[index]};
        for (std::size_t axis = 0; axis < 3; ++axis) all[axis].push_back(sample[axis]);
    }
    stationary_used = zero_range.valid;
    if (stationary_used) return median_axes(
        {&telemetry.gyro_x_dps, &telemetry.gyro_y_dps, &telemetry.gyro_z_dps}, zero_range.begin, zero_range.end);
    return {median(std::move(all[0])), median(std::move(all[1])), median(std::move(all[2]))};
}

template <typename Predicate, typename Callback>
void sustained_runs(const TelemetrySeries& telemetry, Timestamp minimum_duration, Predicate predicate, Callback callback) {
    std::size_t begin = telemetry.size();
    for (std::size_t index = 0; index <= telemetry.size(); ++index) {
        const auto active = index < telemetry.size() && predicate(index);
        if (active && begin == telemetry.size()) begin = index;
        if ((!active || index == telemetry.size()) && begin != telemetry.size()) {
            const auto end = index - 1;
            const auto duration = telemetry.time_us[end] - telemetry.time_us[begin] +
                (end + 1 < telemetry.size() ? telemetry.time_us[end + 1] - telemetry.time_us[end] : 0);
            if (duration >= minimum_duration) callback(begin, end);
            begin = telemetry.size();
        }
    }
}

int clamp_confidence(double value) {
    return static_cast<int>(std::lround(std::clamp(value, 0.0, 100.0)));
}

}  // namespace

Analysis analyze(const TelemetrySeries& telemetry, const Thresholds& thresholds) {
    Analysis result;
    result.vehicle_yaw_rate_dps.assign(telemetry.size(), std::numeric_limits<float>::quiet_NaN());
    if (telemetry.empty()) return result;
    telemetry.validate();

    double imu_energy = 0.0;
    std::vector<double> vertical_values;
    vertical_values.reserve(telemetry.size());
    for (std::size_t index = 0; index < telemetry.size(); ++index) {
        imu_energy += std::abs(telemetry.vertical_g[index]) + std::abs(telemetry.gyro_x_dps[index]) +
                      std::abs(telemetry.gyro_y_dps[index]) + std::abs(telemetry.gyro_z_dps[index]);
        vertical_values.push_back(std::abs(static_cast<double>(telemetry.vertical_g[index])));
    }
    result.available = imu_energy > 1e-3;
    if (!result.available) return result;
    const auto zero_range = initial_stationary_range(telemetry);
    result.initial_stationary_zero_used = zero_range.valid;
    if (zero_range.valid) {
        result.zero_begin_index = zero_range.begin;
        result.zero_end_index = zero_range.end;
        result.acceleration_zero_g = median_axes(
            {&telemetry.longitudinal_g, &telemetry.lateral_g, &telemetry.vertical_g}, zero_range.begin, zero_range.end);
        result.vertical_rest_g = std::abs(result.acceleration_zero_g[2]);
    } else {
        result.vertical_rest_g = median(std::move(vertical_values));
    }

    auto& calibration = result.calibration;
    calibration.gyro_bias_dps = estimate_bias(telemetry, zero_range, calibration.stationary_bias_used);
    std::vector<std::array<double, 3>> gyro_samples;
    std::vector<double> heading_rates;
    constexpr std::size_t window = 2;
    for (std::size_t index = window; index + window < telemetry.size(); ++index) {
        if (telemetry.speed_kmh[index] < 5.0F) continue;
        const auto dt = static_cast<double>(telemetry.time_us[index + window] - telemetry.time_us[index - window]) / 1'000'000.0;
        if (dt < 0.08 || dt > 0.5) continue;
        const auto heading_rate = wrapped_degrees(telemetry.heading_deg[index + window] - telemetry.heading_deg[index - window]) / dt;
        if (!std::isfinite(heading_rate) || std::abs(heading_rate) > 720.0) continue;
        const std::array<double, 3> gyro{
            telemetry.gyro_x_dps[index] - calibration.gyro_bias_dps[0],
            telemetry.gyro_y_dps[index] - calibration.gyro_bias_dps[1],
            telemetry.gyro_z_dps[index] - calibration.gyro_bias_dps[2]};
        gyro_samples.push_back(gyro);
        heading_rates.push_back(heading_rate);
    }
    calibration.matched_samples = heading_rates.size();
    std::array<double, 3> covariance{};
    if (!heading_rates.empty()) {
        std::array<double, 3> gyro_mean{};
        for (const auto& sample : gyro_samples) {
            for (std::size_t axis = 0; axis < 3; ++axis) gyro_mean[axis] += sample[axis];
        }
        for (auto& value : gyro_mean) value /= static_cast<double>(gyro_samples.size());
        const auto heading_mean = std::accumulate(heading_rates.begin(), heading_rates.end(), 0.0) / heading_rates.size();
        for (std::size_t index = 0; index < gyro_samples.size(); ++index) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                covariance[axis] += (gyro_samples[index][axis] - gyro_mean[axis]) * (heading_rates[index] - heading_mean);
            }
        }
    }
    const auto norm = std::sqrt(covariance[0] * covariance[0] + covariance[1] * covariance[1] + covariance[2] * covariance[2]);
    if (heading_rates.size() >= 100 && norm > 1e-9) {
        for (std::size_t axis = 0; axis < 3; ++axis) calibration.yaw_projection[axis] = covariance[axis] / norm;
        std::vector<double> projection;
        projection.reserve(gyro_samples.size());
        for (const auto& sample : gyro_samples) projection.push_back(
            sample[0] * calibration.yaw_projection[0] + sample[1] * calibration.yaw_projection[1] +
            sample[2] * calibration.yaw_projection[2]);
        const auto projection_mean = std::accumulate(projection.begin(), projection.end(), 0.0) / projection.size();
        const auto heading_mean = std::accumulate(heading_rates.begin(), heading_rates.end(), 0.0) / heading_rates.size();
        double cross = 0.0, projection_energy = 0.0;
        for (std::size_t index = 0; index < projection.size(); ++index) {
            cross += (projection[index] - projection_mean) * (heading_rates[index] - heading_mean);
            projection_energy += (projection[index] - projection_mean) * (projection[index] - projection_mean);
        }
        calibration.yaw_scale = projection_energy > 1e-12 ? cross / projection_energy : 1.0;
        for (auto& value : projection) value *= calibration.yaw_scale;
        calibration.heading_correlation = correlation(projection, heading_rates);
        calibration.valid = calibration.heading_correlation >= 0.60;
    }

    if (calibration.valid) {
        for (std::size_t index = 0; index < telemetry.size(); ++index) {
            const auto yaw = calibration.yaw_scale * (
                (telemetry.gyro_x_dps[index] - calibration.gyro_bias_dps[0]) * calibration.yaw_projection[0] +
                (telemetry.gyro_y_dps[index] - calibration.gyro_bias_dps[1]) * calibration.yaw_projection[1] +
                (telemetry.gyro_z_dps[index] - calibration.gyro_bias_dps[2]) * calibration.yaw_projection[2]);
            result.vehicle_yaw_rate_dps[index] = static_cast<float>(yaw);
        }
    }

    const auto airborne_limit = std::max(0.15, result.vertical_rest_g * thresholds.airborne_fraction_of_rest_g);
    sustained_runs(telemetry, thresholds.airborne_minimum_us,
        [&](std::size_t index) { return std::abs(telemetry.vertical_g[index]) <= airborne_limit; },
        [&](std::size_t begin, std::size_t end) {
            auto peak = begin;
            for (auto index = begin + 1; index <= end; ++index) {
                if (std::abs(telemetry.vertical_g[index]) < std::abs(telemetry.vertical_g[peak])) peak = index;
            }
            const auto depth = 1.0 - std::abs(telemetry.vertical_g[peak]) / std::max(0.01, result.vertical_rest_g);
            const auto corrected_vertical = telemetry.vertical_g[peak] - result.acceleration_zero_g[2];
            Event airborne{EventType::PossibleAirborne, begin, end, peak, telemetry.time_us[peak],
                           corrected_vertical, clamp_confidence(55.0 + depth * 35.0)};
            result.events.push_back(airborne);

            const auto search_end_time = telemetry.time_us[end] + thresholds.landing_search_us;
            auto landing = end;
            double landing_delta = 0.0;
            for (auto index = end + 1; index < telemetry.size() && telemetry.time_us[index] <= search_end_time; ++index) {
                const auto delta = std::abs(std::abs(static_cast<double>(telemetry.vertical_g[index])) - result.vertical_rest_g);
                if (delta > landing_delta) { landing_delta = delta; landing = index; }
            }
            if (landing_delta >= thresholds.landing_delta_g) {
                result.events.push_back({EventType::PossibleLandingImpact, landing, landing, landing, telemetry.time_us[landing],
                    landing_delta, clamp_confidence(airborne.confidence + 5.0)});
                result.events[result.events.size() - 2].confidence = clamp_confidence(airborne.confidence + 8.0);
            }
        });

    sustained_runs(telemetry, thresholds.high_load_minimum_us,
        [&](std::size_t index) {
            const auto longitudinal = telemetry.longitudinal_g[index] - result.acceleration_zero_g[0];
            const auto lateral = telemetry.lateral_g[index] - result.acceleration_zero_g[1];
            const auto vertical = telemetry.vertical_g[index] - result.acceleration_zero_g[2];
            return std::sqrt(longitudinal * longitudinal + lateral * lateral + vertical * vertical) >=
                   thresholds.high_load_dynamic_g;
        },
        [&](std::size_t begin, std::size_t end) {
            auto peak = begin;
            auto peak_value = 0.0;
            for (auto index = begin; index <= end; ++index) {
                const auto longitudinal = telemetry.longitudinal_g[index] - result.acceleration_zero_g[0];
                const auto lateral = telemetry.lateral_g[index] - result.acceleration_zero_g[1];
                const auto vertical = telemetry.vertical_g[index] - result.acceleration_zero_g[2];
                const auto value = std::sqrt(longitudinal * longitudinal + lateral * lateral + vertical * vertical);
                if (value > peak_value) { peak_value = value; peak = index; }
            }
            result.events.push_back({EventType::HighImuLoad, begin, end, peak, telemetry.time_us[peak], peak_value,
                clamp_confidence(65.0 + (peak_value - thresholds.high_load_dynamic_g) * 15.0)});
        });

    if (calibration.valid) {
        sustained_runs(telemetry, thresholds.rapid_rotation_minimum_us,
            [&](std::size_t index) { return std::abs(result.vehicle_yaw_rate_dps[index]) >= thresholds.rapid_rotation_dps; },
            [&](std::size_t begin, std::size_t end) {
                auto peak = begin;
                for (auto index = begin + 1; index <= end; ++index) {
                    if (std::abs(result.vehicle_yaw_rate_dps[index]) > std::abs(result.vehicle_yaw_rate_dps[peak])) peak = index;
                }
                result.events.push_back({EventType::RapidRotation, begin, end, peak, telemetry.time_us[peak],
                    result.vehicle_yaw_rate_dps[peak], clamp_confidence(45.0 + calibration.heading_correlation * 50.0)});
            });
    }
    std::sort(result.events.begin(), result.events.end(), [](const Event& left, const Event& right) {
        return left.time_us < right.time_us;
    });
    return result;
}

std::string_view event_name(EventType type) noexcept {
    switch (type) {
        case EventType::PossibleAirborne: return "Possible airborne period";
        case EventType::PossibleLandingImpact: return "Possible landing impact";
        case EventType::HighImuLoad: return "High IMU load";
        case EventType::RapidRotation: return "Rapid vehicle rotation";
    }
    return "IMU event";
}

}  // namespace racebox::imu
