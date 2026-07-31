#include "racebox/core.hpp"

#include <nlohmann/json.hpp>

#include <iomanip>
#include <iostream>

namespace {

double seconds(racebox::Timestamp value) { return static_cast<double>(value) / 1'000'000.0; }

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: racebox_cli <session.vbo> <racebox.csv> <sanwa.csv>\n";
        return 2;
    }
    try {
        const auto loaded = racebox::load_session({argv[1], argv[2], argv[3]});
        const auto& session = loaded.session;
        nlohmann::json output = {
            {"session", session.name},
            {"telemetry_samples", session.telemetry.size()},
            {"radio_samples", session.radio.size()},
            {"laps", session.laps.size()},
            {"radio_anchor_seconds", seconds(session.alignment.radio_anchor_us)},
            {"fine_correction_ms", static_cast<double>(session.alignment.fine_correction_us) / 1000.0},
            {"trigger_correlation", session.alignment.trigger_correlation},
            {"direction_agreement", session.alignment.direction_agreement},
            {"steering_yaw_correlation", session.alignment.steering_yaw_correlation},
            {"lap_steering_correlation", session.alignment.lap_steering_correlation},
            {"alignment_confidence", session.alignment.confidence},
            {"theoretical_best_seconds", seconds(session.theoretical_best.duration_us)},
            {"imu", {
                {"available", loaded.imu_analysis.available},
                {"initial_stationary_zero_used", loaded.imu_analysis.initial_stationary_zero_used},
                {"zero_begin_seconds", loaded.imu_analysis.initial_stationary_zero_used
                    ? seconds(session.telemetry.time_us[loaded.imu_analysis.zero_begin_index]) : 0.0},
                {"acceleration_zero_g", loaded.imu_analysis.acceleration_zero_g},
                {"vertical_rest_g", loaded.imu_analysis.vertical_rest_g},
                {"yaw_calibrated", loaded.imu_analysis.calibration.valid},
                {"yaw_heading_correlation", loaded.imu_analysis.calibration.heading_correlation},
                {"yaw_calibration_samples", loaded.imu_analysis.calibration.matched_samples},
                {"events", loaded.imu_analysis.events.size()}
            }},
            {"diagnostics", loaded.diagnostics}
        };
        for (const auto& lap : session.laps) {
            output["lap_times"].push_back({
                {"raw_lap", lap.raw_lap}, {"race_lap", lap.race_lap},
                {"seconds", seconds(lap.duration_us)}, {"phase", static_cast<int>(lap.phase)}});
        }
        for (const auto& event : loaded.imu_analysis.events) {
            output["imu"]["event_list"].push_back({
                {"type", racebox::imu::event_name(event.type)},
                {"seconds", seconds(event.time_us)},
                {"magnitude", event.magnitude},
                {"confidence", event.confidence}
            });
        }
        std::cout << std::setw(2) << output << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "RaceBox load failed: " << exception.what() << '\n';
        return 1;
    }
}
