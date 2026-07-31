#include "racebox/imu_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

racebox::TelemetrySeries synthetic_imu() {
    racebox::TelemetrySeries telemetry;
    telemetry.reserve(300);
    double heading = 20.0;
    for (std::size_t index = 0; index < 300; ++index) {
        const auto moving = index >= 50;
        auto yaw = moving ? 85.0 * std::sin(static_cast<double>(index) * 0.08) : 0.0;
        if (index >= 200 && index <= 203) yaw = 240.0;
        if (moving) heading += yaw * 0.04;
        while (heading >= 360.0) heading -= 360.0;
        while (heading < 0.0) heading += 360.0;
        telemetry.time_us.push_back(static_cast<racebox::Timestamp>(index) * 40'000);
        telemetry.absolute_time_us.push_back(1'700'000'000'000'000LL + static_cast<std::int64_t>(index) * 40'000);
        telemetry.latitude.push_back(49.0);
        telemetry.longitude.push_back(-123.0);
        telemetry.speed_kmh.push_back(moving ? 25.0F : 0.0F);
        telemetry.heading_deg.push_back(static_cast<float>(heading));
        telemetry.altitude_m.push_back(3.0F);
        telemetry.longitudinal_g.push_back(index >= 250 && index <= 251 ? 4.0F : 0.0F);
        telemetry.lateral_g.push_back(0.0F);
        telemetry.vertical_g.push_back(index >= 130 && index <= 133 ? 0.10F : index == 134 ? 1.80F : 1.0F);
        telemetry.gyro_x_dps.push_back(static_cast<float>(2.0 + yaw * 0.20));
        telemetry.gyro_y_dps.push_back(static_cast<float>(-3.0 - yaw * 0.50));
        telemetry.gyro_z_dps.push_back(static_cast<float>(5.0 + yaw * 0.84));
        telemetry.satellites.push_back(18);
        telemetry.raw_lap.push_back(1);
    }
    telemetry.validate();
    return telemetry;
}

}  // namespace

int main() {
    try {
        const auto telemetry = synthetic_imu();
        const auto analysis = racebox::imu::analyze(telemetry);
        require(analysis.available, "synthetic IMU was not detected");
        require(analysis.initial_stationary_zero_used, "initial stationary zero was not found");
        require(analysis.zero_begin_index == 0 && analysis.zero_end_index == 49,
                "initial stationary zero used the wrong samples");
        require(std::abs(analysis.acceleration_zero_g[0]) < 1e-6 &&
                    std::abs(analysis.acceleration_zero_g[1]) < 1e-6 &&
                    std::abs(analysis.acceleration_zero_g[2] - 1.0) < 1e-6,
                "stationary acceleration did not calibrate to zero");
        require(std::abs(analysis.calibration.gyro_bias_dps[0] - 2.0) < 1e-3 &&
                    std::abs(analysis.calibration.gyro_bias_dps[1] + 3.0) < 1e-3 &&
                    std::abs(analysis.calibration.gyro_bias_dps[2] - 5.0) < 1e-3,
                "initial stationary gyro bias was not retained");
        require(analysis.calibration.valid, "vehicle yaw calibration failed");
        require(analysis.calibration.stationary_bias_used, "stationary gyro bias was not used");
        require(analysis.calibration.heading_correlation > 0.90, "vehicle yaw calibration correlation is weak");
        require(analysis.vehicle_yaw_rate_dps.size() == telemetry.size(), "derived yaw channel is the wrong size");

        auto airborne = false, landing = false, high_load = false, rotation = false;
        for (const auto& event : analysis.events) {
            airborne |= event.type == racebox::imu::EventType::PossibleAirborne;
            landing |= event.type == racebox::imu::EventType::PossibleLandingImpact;
            high_load |= event.type == racebox::imu::EventType::HighImuLoad;
            rotation |= event.type == racebox::imu::EventType::RapidRotation;
        }
        require(airborne, "sustained low vertical load was not detected");
        require(landing, "landing following low vertical load was not detected");
        require(high_load, "high combined IMU load was not detected");
        require(rotation, "sustained rapid rotation was not detected");

        auto zero = telemetry;
        std::fill(zero.vertical_g.begin(), zero.vertical_g.end(), 0.0F);
        std::fill(zero.gyro_x_dps.begin(), zero.gyro_x_dps.end(), 0.0F);
        std::fill(zero.gyro_y_dps.begin(), zero.gyro_y_dps.end(), 0.0F);
        std::fill(zero.gyro_z_dps.begin(), zero.gyro_z_dps.end(), 0.0F);
        require(!racebox::imu::analyze(zero).available, "zero-filled compatibility channels were treated as real IMU data");
        std::cout << "IMU analysis tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "IMU analysis test failure: " << exception.what() << '\n';
        return 1;
    }
}
