#pragma once

#include "racebox/session.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace racebox::imu {

enum class EventType {
    PossibleAirborne,
    PossibleLandingImpact,
    HighImuLoad,
    RapidRotation,
};

struct Thresholds {
    double airborne_fraction_of_rest_g{0.40};
    Timestamp airborne_minimum_us{80'000};
    double landing_delta_g{0.35};
    Timestamp landing_search_us{1'000'000};
    double high_load_dynamic_g{3.50};
    Timestamp high_load_minimum_us{40'000};
    double rapid_rotation_dps{220.0};
    Timestamp rapid_rotation_minimum_us{80'000};
};

struct Calibration {
    bool valid{};
    bool stationary_bias_used{};
    std::array<double, 3> gyro_bias_dps{};
    std::array<double, 3> yaw_projection{};
    double yaw_scale{1.0};
    double heading_correlation{};
    std::size_t matched_samples{};
};

struct Event {
    EventType type{};
    std::size_t begin_index{};
    std::size_t end_index{};
    std::size_t peak_index{};
    Timestamp time_us{};
    double magnitude{};
    int confidence{};
};

struct Analysis {
    bool available{};
    bool initial_stationary_zero_used{};
    std::size_t zero_begin_index{};
    std::size_t zero_end_index{};
    std::array<double, 3> acceleration_zero_g{};
    double vertical_rest_g{};
    Calibration calibration;
    std::vector<float> vehicle_yaw_rate_dps;
    std::vector<Event> events;
};

[[nodiscard]] Analysis analyze(const TelemetrySeries& telemetry, const Thresholds& thresholds = {});
[[nodiscard]] std::string_view event_name(EventType type) noexcept;

}  // namespace racebox::imu
