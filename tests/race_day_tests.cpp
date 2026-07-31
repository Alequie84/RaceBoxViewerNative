#include "racebox/race_day.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

racebox::Session synthetic_session() {
    racebox::Session session;
    auto& telemetry = session.telemetry;
    for (int index = 0; index < 8; ++index) {
        telemetry.time_us.push_back(index * 40'000);
        telemetry.absolute_time_us.push_back(1'700'000'000'000'000LL + index * 40'000);
        telemetry.latitude.push_back(49.184 + index * 0.000001);
        telemetry.longitude.push_back(-123.145 + index * 0.000001);
        telemetry.speed_kmh.push_back(10.0F + index);
        telemetry.heading_deg.push_back(0.0F);
        telemetry.altitude_m.push_back(2.0F);
        telemetry.longitudinal_g.push_back(0.1F);
        telemetry.lateral_g.push_back(0.2F);
        telemetry.vertical_g.push_back(1.0F);
        telemetry.gyro_x_dps.push_back(0.0F);
        telemetry.gyro_y_dps.push_back(0.0F);
        telemetry.gyro_z_dps.push_back(0.0F);
        telemetry.satellites.push_back(14);
        telemetry.raw_lap.push_back(1);
    }
    telemetry.validate();
    session.laps.push_back({1, 1, 0, 7, 280'000, racebox::LapPhase::Complete});
    session.alignment.compatible = false;
    return session;
}

racebox::Session synthetic_dynamics_session(
    const std::vector<float>& speeds,
    const std::vector<float>& triggers,
    const std::vector<float>& steering,
    const std::vector<float>& lateral_g,
    const std::vector<float>& longitudinal_g) {
    racebox::Session session;
    auto& telemetry = session.telemetry;
    const auto count = speeds.size();
    for (std::size_t index = 0; index < count; ++index) {
        telemetry.time_us.push_back(static_cast<racebox::Timestamp>(index) * 40'000);
        telemetry.absolute_time_us.push_back(1'700'000'000'000'000LL + static_cast<racebox::Timestamp>(index) * 40'000);
        telemetry.latitude.push_back(49.184 + static_cast<double>(index) * 0.000001);
        telemetry.longitude.push_back(-123.145 + static_cast<double>(index) * 0.000001);
        telemetry.speed_kmh.push_back(speeds[index]);
        telemetry.heading_deg.push_back(0.0F);
        telemetry.altitude_m.push_back(2.0F);
        telemetry.longitudinal_g.push_back(longitudinal_g[index]);
        telemetry.lateral_g.push_back(lateral_g[index]);
        telemetry.vertical_g.push_back(1.0F);
        telemetry.gyro_x_dps.push_back(0.0F);
        telemetry.gyro_y_dps.push_back(0.0F);
        telemetry.gyro_z_dps.push_back(0.0F);
        telemetry.satellites.push_back(14);
        telemetry.raw_lap.push_back(1);
        session.radio.elapsed_us.push_back(static_cast<racebox::Timestamp>(index) * 40'000);
        session.radio.trigger_percent.push_back(triggers[index]);
        session.radio.steering_percent.push_back(steering[index]);
        session.radio.voltage.push_back(6.0F);
    }
    telemetry.validate();
    session.radio.validate();
    session.laps.push_back({1, 1, 0, count - 1,
        static_cast<racebox::Timestamp>((count - 1) * 40'000), racebox::LapPhase::Complete});
    session.alignment.compatible = true;
    session.alignment.trigger_sign = 1;
    session.alignment.steering_sign = 1;
    return session;
}

racebox::Session synthetic_roll_session(float stopped_lateral_g, float corner_lateral_g, float corner_vertical_g) {
    racebox::Session session;
    auto& telemetry = session.telemetry;
    for (std::size_t index = 0; index < 80; ++index) {
        const auto stopped = index < 30;
        telemetry.time_us.push_back(static_cast<racebox::Timestamp>(index) * 40'000);
        telemetry.absolute_time_us.push_back(1'700'000'000'000'000LL + static_cast<racebox::Timestamp>(index) * 40'000);
        telemetry.latitude.push_back(49.184 + static_cast<double>(index) * 0.000001);
        telemetry.longitude.push_back(-123.145 + static_cast<double>(index) * 0.000001);
        telemetry.speed_kmh.push_back(stopped ? 0.0F : 34.0F);
        telemetry.heading_deg.push_back(0.0F);
        telemetry.altitude_m.push_back(2.0F);
        telemetry.longitudinal_g.push_back(0.0F);
        telemetry.lateral_g.push_back(stopped ? stopped_lateral_g : corner_lateral_g);
        telemetry.vertical_g.push_back(stopped ? 1.0F : corner_vertical_g);
        telemetry.gyro_x_dps.push_back(0.0F);
        telemetry.gyro_y_dps.push_back(0.0F);
        telemetry.gyro_z_dps.push_back(0.0F);
        telemetry.satellites.push_back(14);
        telemetry.raw_lap.push_back(1);
        session.radio.elapsed_us.push_back(static_cast<racebox::Timestamp>(index) * 40'000);
        session.radio.trigger_percent.push_back(0.0F);
        session.radio.steering_percent.push_back(stopped ? 0.0F : 45.0F);
        session.radio.voltage.push_back(6.0F);
    }
    telemetry.validate();
    session.radio.validate();
    session.laps.push_back({1, 1, 0, 79, 79 * 40'000, racebox::LapPhase::Complete});
    session.alignment.compatible = true;
    session.alignment.trigger_sign = 1;
    session.alignment.steering_sign = 1;
    return session;
}

racebox::Session synthetic_roll_rate_session(double amplitude_deg, double frequency_per_sample) {
    racebox::Session session;
    auto& telemetry = session.telemetry;
    constexpr double radians_per_degree = 3.14159265358979323846 / 180.0;
    for (std::size_t index = 0; index < 120; ++index) {
        const auto stopped = index < 30;
        const auto angle_deg = stopped
            ? 2.0
            : 28.0 + amplitude_deg * std::sin(static_cast<double>(index - 30) * frequency_per_sample);
        const auto vertical_g = 1.0F;
        const auto lateral_g = static_cast<float>(std::tan(angle_deg * radians_per_degree) * vertical_g);
        telemetry.time_us.push_back(static_cast<racebox::Timestamp>(index) * 40'000);
        telemetry.absolute_time_us.push_back(1'700'000'000'000'000LL + static_cast<racebox::Timestamp>(index) * 40'000);
        telemetry.latitude.push_back(49.184 + static_cast<double>(index) * 0.000001);
        telemetry.longitude.push_back(-123.145 + static_cast<double>(index) * 0.000001);
        telemetry.speed_kmh.push_back(stopped ? 0.0F : 34.0F);
        telemetry.heading_deg.push_back(0.0F);
        telemetry.altitude_m.push_back(2.0F);
        telemetry.longitudinal_g.push_back(0.0F);
        telemetry.lateral_g.push_back(lateral_g);
        telemetry.vertical_g.push_back(vertical_g);
        telemetry.gyro_x_dps.push_back(0.0F);
        telemetry.gyro_y_dps.push_back(0.0F);
        telemetry.gyro_z_dps.push_back(0.0F);
        telemetry.satellites.push_back(14);
        telemetry.raw_lap.push_back(1);
        session.radio.elapsed_us.push_back(static_cast<racebox::Timestamp>(index) * 40'000);
        session.radio.trigger_percent.push_back(0.0F);
        session.radio.steering_percent.push_back(stopped ? 0.0F : 45.0F);
        session.radio.voltage.push_back(6.0F);
    }
    telemetry.validate();
    session.radio.validate();
    session.laps.push_back({1, 1, 0, 119, 119 * 40'000, racebox::LapPhase::Complete});
    session.alignment.compatible = true;
    session.alignment.trigger_sign = 1;
    session.alignment.steering_sign = 1;
    return session;
}

}  // namespace

int main() {
    try {
        auto day = racebox::race_day::standard_day(true, true);
        day.event_name = "Club race";
        day.track_name = "RC Raceway";
        require(day.runs.size() == 10, "P1-3, Q1-4, and triple A Main were not created");
        require(day.runs[0].label == "Practice 1", "Practice naming changed");
        require(day.runs[3].label == "Q1", "Qualifying naming changed");
        require(day.runs[7].label == "A Main 1", "Triple-main naming changed");
        require(day.runs.front().checklist.size() >= 8, "Pre-run checklist was not populated");

        const auto added_b = racebox::race_day::add_main_group(day, 'B', 1);
        require(added_b.size() == 1 && day.runs[added_b.front()].label == "B Main",
                "Single B Main was not created");
        require(racebox::race_day::add_main_group(day, 'B', 1).empty(),
                "Duplicate main group was not rejected");

        auto& current = day.runs[1];
        current.conditions.tire_set_id = "Set 3";
        current.conditions.tire_runs_before = 4;
        current.conditions.sauce_compound = "Yellow";
        current.conditions.sauce_minutes_before = 20;
        current.conditions.tire_warmer_minutes = 10;
        current.conditions.tire_warmer_temperature_c = 60.0;
        current.setup_changes = "Rear spring 2.6 to 2.8";
        current.post_run_notes = "More rotation, but nervous on power.";
        current.checklist.front().checked = true;

        racebox::race_day::SetupKnowledgeRecord spring_result;
        spring_result.id = "setup-result-spring";
        spring_result.created_at_utc = "2026-07-26T20:00:00Z";
        spring_result.event_name = day.event_name;
        spring_result.track_name = day.track_name;
        spring_result.previous_run_id = day.runs[0].id;
        spring_result.previous_run_label = day.runs[0].label;
        spring_result.current_run_id = current.id;
        spring_result.current_run_label = current.label;
        spring_result.handling_question = "Did the rear spring improve rotation without losing rear grip?";
        spring_result.setup_change = current.setup_changes;
        spring_result.driver_result = current.post_run_notes;
        spring_result.previous_conditions = day.runs[0].conditions;
        spring_result.current_conditions = current.conditions;
        spring_result.verdict = "mixed";
        spring_result.confidence = 78;
        spring_result.summary = "Rotation improved, but the car was nervous on power.";
        spring_result.observations.push_back({
            "Lateral response", "+0.03 G matched response", "More response was measured.",
            {"dynamics:lateral:matched-speed-steering"}});
        spring_result.next_test = "Repeat A/B/A with the same tire set.";
        spring_result.causality_note = "One comparison is association, not proof.";
        spring_result.model = "openai/gpt-5.6-sol";
        spring_result.thinking = "xhigh";
        spring_result.selected_lane = "correlation";
        spring_result.evidence.analytics_contract = "racebox-setup-analytics-v3";
        spring_result.evidence.formula_version = 3;
        spring_result.evidence.quality_confidence = 84;
        spring_result.evidence.track_status = "compatible";
        spring_result.evidence.lap_time_delta_s = 0.08;
        spring_result.evidence.lateral_status = "measured";
        spring_result.evidence.lateral_response_delta_g = 0.03;
        spring_result.evidence.forward_status = "measured";
        spring_result.evidence.forward_bite_delta_g = -0.02;
        spring_result.evidence.previous_brake_indicators = 0;
        spring_result.evidence.current_brake_indicators = 1;
        racebox::race_day::upsert_setup_knowledge(day, spring_result);

        racebox::race_day::SetupKnowledgeRecord tire_result = spring_result;
        tire_result.id = "setup-result-tire";
        tire_result.track_name = "Different track";
        tire_result.handling_question = "Did a different tire sauce improve warmup?";
        tire_result.setup_change = "Changed tire sauce";
        tire_result.summary = "Warmup improved.";
        racebox::race_day::upsert_setup_knowledge(day, tire_result);

        const auto relevant = racebox::race_day::relevant_setup_knowledge(
            day, "The rear feels nervous under power after the spring change", current);
        require(!relevant.empty(), "Setup-knowledge retrieval returned no matching records");
        require(day.setup_knowledge[relevant.front()].id == spring_result.id,
                "Setup-knowledge retrieval did not prioritize the matching track and change");
        const auto prior_payload = racebox::race_day::build_prior_setup_results(day, relevant);
        require(prior_payload.is_array() && !prior_payload.empty(),
                "Prior setup-result payload was not built");
        require(prior_payload.dump().find("telemetry_files") == std::string::npos,
                "Prior setup-result payload leaked telemetry attachment metadata");

        const auto path = std::filesystem::temp_directory_path() / L"racebox-race-day-test.rbxday";
        std::string error;
        require(racebox::race_day::save(day, path, error), error.c_str());
        racebox::race_day::Day restored;
        require(racebox::race_day::load(path, restored, error), error.c_str());
        std::error_code ignored;
        require(restored.runs.size() == day.runs.size(), "Race-day run count did not round trip");
        require(restored.runs[1].conditions.tire_runs_before == 4, "Tire run count did not persist");
        require(restored.runs[1].conditions.tire_warmer_temperature_c == 60.0,
                "Tire warmer temperature did not persist");
        require(restored.runs[1].checklist.front().checked, "Checklist state did not persist");
        require(restored.runs[1].setup_changes == current.setup_changes, "Setup changes did not persist");
        require(restored.setup_knowledge.size() == 2, "Setup-knowledge records did not persist");
        require(restored.setup_knowledge.front().evidence.quality_confidence == 84,
                "Setup-knowledge evidence did not round trip");
        require(restored.setup_knowledge.front().thinking == "xhigh",
                "Setup-knowledge agent metadata did not round trip");

        const auto legacy_path =
            std::filesystem::temp_directory_path() / L"racebox-race-day-v1-test.rbxday";
        {
            nlohmann::json legacy;
            std::ifstream saved(path, std::ios::binary);
            saved >> legacy;
            legacy["version"] = 1;
            legacy.erase("setup_knowledge");
            std::ofstream output(legacy_path, std::ios::binary | std::ios::trunc);
            output << legacy.dump(2);
        }
        racebox::race_day::Day legacy_restored;
        require(racebox::race_day::load(legacy_path, legacy_restored, error),
                "Version-1 race-day files lost backward compatibility");
        require(legacy_restored.setup_knowledge.empty(),
                "Version-1 race-day file unexpectedly created setup knowledge");
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(legacy_path, ignored);

        const auto session = synthetic_session();
        const auto imu = racebox::imu::analyze(session.telemetry);
        const auto csv = racebox::race_day::build_analytics_csv(session, imu);
        require(csv.find("racebox-session-analytics-csv-v1") != std::string::npos,
                "Analytics CSV contract is missing");
        require(csv.find("racebox-setup-analytics-v3") != std::string::npos,
                "Setup analytics contract is missing");
        require(csv.find("steering_percent,throttle_percent,brake_percent") != std::string::npos,
                "Analytics CSV input channels are missing");
        require(csv.find("lap_progress_percent,latitude,longitude") != std::string::npos,
                "Analytics CSV physical track matching fields are missing");
        require(csv.find(",complete,") != std::string::npos, "Complete-lap labels are missing");

        racebox::race_day::Run golden_run;
        golden_run.telemetry_files = {
            std::filesystem::path(GOLDEN_DIR) / L"session.vbo",
            std::filesystem::path(GOLDEN_DIR) / L"session.csv",
            std::filesystem::path(GOLDEN_DIR) / L"sanwa.csv",
        };
        const auto golden = racebox::race_day::load_run_telemetry(golden_run);
        require(golden.session.telemetry.size() == 13'863, "Race-day loader changed the golden GPS row count");
        require(golden.session.radio.size() == 43'199, "Race-day loader did not include Sanwa data");
        const auto golden_csv = racebox::race_day::build_analytics_csv(golden.session, golden.imu_analysis);
        require(golden_csv.size() < 6'000'000, "Golden analytics CSV exceeded the gateway per-run bound");
        require(golden_csv.find("100.000000") != std::string::npos,
                "Golden analytics CSV did not contain aligned control values");

        const auto neutral_g = std::vector<float>(20, 0.0F);
        auto corner_lat = std::vector<float>(20, 0.2F);
        auto corner_steer = std::vector<float>(20, 0.0F);
        for (int index = 2; index <= 6; ++index) {
            corner_lat[static_cast<std::size_t>(index)] = 0.9F;
            corner_steer[static_cast<std::size_t>(index)] = 50.0F;
        }
        std::vector<float> trigger(20, 0.0F);
        for (int index = 7; index <= 14; ++index) trigger[static_cast<std::size_t>(index)] = 100.0F;
        for (int index = 15; index <= 18; ++index) trigger[static_cast<std::size_t>(index)] = -100.0F;
        auto before_exit = synthetic_dynamics_session(
            {40,41,42,43,44,45,46,50,53,56,59,62,65,68,71,71,65,59,53,48},
            trigger, corner_steer, corner_lat, neutral_g);
        auto after_exit = synthetic_dynamics_session(
            {40,41,42,43,44,45,46,55,58,61,64,67,70,73,76,76,70,64,58,53},
            trigger, corner_steer, corner_lat, neutral_g);
        const auto exit_summary = racebox::race_day::analyze_setup_change(
            before_exit, racebox::imu::analyze(before_exit.telemetry),
            after_exit, racebox::imu::analyze(after_exit.telemetry));
        require(exit_summary.straight_top_speed_delta_kmh && *exit_summary.straight_top_speed_delta_kmh > 4.0,
                "Straight top-speed delta was not measured");
        require(exit_summary.straight_entry_speed_delta_kmh && *exit_summary.straight_entry_speed_delta_kmh > 4.0,
                "Straight entry-speed delta was not measured");
        require(exit_summary.straight_speed_attribution == "exit_or_line_driven",
                "Straight speed from better exit was misclassified");

        auto after_accel = synthetic_dynamics_session(
            {40,41,42,43,44,45,46,50,55,60,65,70,75,80,85,85,79,73,67,60},
            trigger, corner_steer, corner_lat, neutral_g);
        const auto accel_summary = racebox::race_day::analyze_setup_change(
            before_exit, racebox::imu::analyze(before_exit.telemetry),
            after_accel, racebox::imu::analyze(after_accel.telemetry));
        require(accel_summary.straight_acceleration_delta_g && *accel_summary.straight_acceleration_delta_g > 0.10,
                "Straight acceleration delta was not measured");
        require(accel_summary.straight_speed_attribution == "forward_bite_or_power_delivery",
                "Straight speed from acceleration was misclassified");

        auto delayed_brake = synthetic_dynamics_session(
            {40,41,42,43,44,45,46,50,53,56,59,62,65,68,71,71,71,71,63,57},
            trigger, corner_steer, corner_lat, neutral_g);
        const auto brake_summary = racebox::race_day::analyze_setup_change(
            before_exit, racebox::imu::analyze(before_exit.telemetry),
            delayed_brake, racebox::imu::analyze(delayed_brake.telemetry));
        require(brake_summary.current_brake_indicators > brake_summary.previous_brake_indicators,
                "Possible brake lockup/low-grip indicator did not increase");
        require(brake_summary.brake_response_delay_delta_s && *brake_summary.brake_response_delay_delta_s >= 0.07,
                "Brake response delay was not measured");

        std::vector<float> clean_speed(80, 42.0F);
        std::vector<float> clean_trigger(80, 0.0F);
        std::vector<float> clean_steering(80, 35.0F);
        std::vector<float> clean_lateral(80, 0.70F);
        std::vector<float> overdrive_speed(80, 42.0F);
        std::vector<float> overdrive_steering(80, 70.0F);
        std::vector<float> overdrive_lateral(80, 0.80F);
        for (std::size_t index = 40; index < overdrive_speed.size(); ++index) {
            overdrive_speed[index] = 42.0F - static_cast<float>(index - 40) * 0.08F;
            overdrive_lateral[index] = 0.25F;
            overdrive_steering[index] = index % 2 == 0 ? 70.0F : 88.0F;
        }
        const auto clean = synthetic_dynamics_session(
            clean_speed, clean_trigger, clean_steering, clean_lateral, std::vector<float>(80, 0.0F));
        const auto overdriven = synthetic_dynamics_session(
            overdrive_speed, clean_trigger, overdrive_steering, overdrive_lateral, std::vector<float>(80, 0.0F));
        const auto overdrive_summary = racebox::race_day::analyze_setup_change(
            clean, racebox::imu::analyze(clean.telemetry),
            overdriven, racebox::imu::analyze(overdriven.telemetry));
        require(overdrive_summary.overdriving_index_delta &&
                *overdrive_summary.overdriving_index_delta > 8.0,
                "Overdriving index did not detect extra tire scrub demand");
        require(overdrive_summary.overdriving_risk_sample_delta_percent &&
                *overdrive_summary.overdriving_risk_sample_delta_percent > 10.0,
                "Overdriving risk-sample percentage did not increase");
        require(overdrive_summary.overdriving_status == "more_overdriving_no_lap_gain",
                "Overdriving without lap gain was not classified");

        const auto flatter_roll = synthetic_roll_session(0.04F, 0.60F, 1.00F);
        const auto more_roll = synthetic_roll_session(0.04F, 0.60F, 0.80F);
        const auto roll_summary = racebox::race_day::analyze_setup_change(
            flatter_roll, racebox::imu::analyze(flatter_roll.telemetry),
            more_roll, racebox::imu::analyze(more_roll.telemetry));
        require(roll_summary.chassis_roll_delta_deg &&
                *roll_summary.chassis_roll_delta_deg > 4.0,
                "Tilt-corrected chassis-roll signature did not increase");
        require(roll_summary.surface_tilt_delta_deg &&
                std::abs(*roll_summary.surface_tilt_delta_deg) < 0.01,
                "Stopped-point surface tilt correction changed unexpectedly");
        require(roll_summary.chassis_roll_status == "more_chassis_roll_signature",
                "Chassis-roll signature increase was not classified");

        const auto slower_roll_build = synthetic_roll_rate_session(3.0, 0.10);
        const auto faster_roll_build = synthetic_roll_rate_session(10.0, 0.25);
        const auto roll_rate_summary = racebox::race_day::analyze_setup_change(
            slower_roll_build, racebox::imu::analyze(slower_roll_build.telemetry),
            faster_roll_build, racebox::imu::analyze(faster_roll_build.telemetry));
        require(roll_rate_summary.roll_rate_delta_dps &&
                *roll_rate_summary.roll_rate_delta_dps > 35.0,
                "Tilt-corrected roll-rate signature did not increase");
        require(roll_rate_summary.previous_roll_rate_samples >= 25 &&
                roll_rate_summary.current_roll_rate_samples >= 25,
                "Roll-rate signature did not retain enough cornering samples");
        require(roll_rate_summary.roll_rate_status == "faster_roll_build",
                "Roll-rate signature increase was not classified");

        std::cout << "Race-day model, persistence, checklist, and CSV tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Race-day test failure: " << exception.what() << '\n';
        return 1;
    }
}
