#include "racebox/core.hpp"
#include "racebox/crew_chief.hpp"
#include "racebox/driver_analysis.hpp"
#include "racebox/imu_analysis.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <stdexcept>

#ifndef GOLDEN_DIR
#define GOLDEN_DIR L"golden"
#endif

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
    try {
        const auto root = std::filesystem::path(GOLDEN_DIR);
        const auto loaded = racebox::load_session({
            root / "session.vbo", root / "session.csv", root / "sanwa.csv"});
        const auto& session = loaded.session;
        const auto complete = [&] {
            std::vector<const racebox::LapInfo*> laps;
            for (const auto& lap : session.laps) {
                if (lap.phase == racebox::LapPhase::Complete) laps.push_back(&lap);
            }
            return laps;
        }();
        require(complete.size() >= 3, "Golden telemetry needs three complete laps");

        const auto imu = racebox::imu::analyze(session.telemetry);
        const auto corners = racebox::driver_analysis::suggest_corner_zones(session, *complete[0], 12);
        const auto analysis = racebox::driver_analysis::analyze_driver_performance(
            session, *complete[0], complete[1], complete[2], corners,
            racebox::driver_analysis::default_analysis_rules());
        const auto evidence = racebox::crew_chief::build_evidence_packet(
            session, *complete[0], *complete[1], analysis,
            racebox::driver_analysis::ComparisonSlot::CompareA, &imu);
        require(evidence.at("contract").get<std::string>() == racebox::crew_chief::kEvidenceContractVersion,
                "Crew Chief evidence contract was not versioned");
        require(evidence.at("before").at("channels").contains("lateral_load"),
                "Before-lap load evidence is missing");
        require(evidence.at("after_minus_before").at("channels").contains("steering"),
                "Steering delta evidence is missing");
        require(evidence.at("interpretation_limits").size() >= 3,
                "Causality limitations were not disclosed");
        require(evidence.dump().find(session.vbo_path.string()) == std::string::npos,
                "Evidence packet leaked a local telemetry file path");

        const nlohmann::json gateway_response{
            {"ok", true},
            {"route", "openclaw/racebox-crew-chief"},
            {"model", "openai/gpt-5.6-sol"},
            {"thinking", "xhigh"},
            {"routing", {{"selected_lane", "correlation"}}},
            {"prior_setup_result_ids", nlohmann::json::array({"setup-result-spring"})},
            {"evidence_summary", {
                {"analytics_contract", "racebox-setup-analytics-v3"},
                {"formula_version", 3},
                {"quality_confidence", 84},
                {"track_status", "compatible"},
                {"track_translation_m", 1.2},
                {"track_residual_rms_m", 0.2},
                {"lap_time_delta_s", 0.08},
                {"lateral_status", "measured"},
                {"lateral_response_delta_g", 0.03},
                {"forward_status", "measured"},
                {"forward_bite_delta_g", -0.02},
                {"previous_brake_indicators", 0},
                {"current_brake_indicators", 1},
                {"top_speed_status", "measured"},
                {"straight_top_speed_delta_kmh", 2.1},
                {"straight_entry_speed_delta_kmh", 1.7},
                {"straight_acceleration_delta_g", 0.01},
                {"straight_speed_attribution", "exit_or_line_driven"},
                {"brake_response_status", "possible_lockup_or_low_grip"},
                {"brake_decel_delta_g", -0.12},
                {"brake_response_delay_delta_s", 0.12},
                {"corner_balance_status", "more_rotation_per_steering"},
                {"steering_for_lateral_g_delta_percent", -4.0},
                {"yaw_per_steering_delta_dps", 0.09},
                {"overdriving_status", "more_overdriving_no_lap_gain"},
                {"overdriving_index_delta", 14.5},
                {"overdriving_risk_sample_delta_percent", 12.0},
                {"current_late_overdriving_delta_score", 9.0},
                {"chassis_roll_status", "more_chassis_roll_signature"},
                {"surface_tilt_delta_deg", 0.1},
                {"chassis_roll_delta_deg", 1.2},
                {"roll_per_lateral_g_delta_deg", 0.8},
                {"roll_rate_status", "faster_roll_build"},
                {"previous_roll_rate_p90_dps", 18.0},
                {"current_roll_rate_p90_dps", 64.0},
                {"roll_rate_delta_dps", 46.0},
                {"roll_rate_per_lateral_g_delta_dps", 31.0},
                {"current_late_roll_rate_delta_dps", 6.0},
                {"previous_roll_rate_samples", 72},
                {"current_roll_rate_samples", 76},
                {"current_surface_tilt_source", "stopped_points"},
                {"current_surface_tilt_samples", 80},
            }},
            {"report", {
                {"verdict", "mixed"},
                {"confidence", 74},
                {"summary", "The change coincided with lower steering work but did not retain a lap-time gain."},
                {"observations", nlohmann::json::array({
                    {{"area", "Steering load"}, {"change", "P95 steering fell."},
                     {"meaning", "The car needed less peak input."},
                     {"evidence_ids", nlohmann::json::array({"metric:turn-1:Turn-in timing delta"})}}
                })},
                {"confounds", nlohmann::json::array({"Only one after lap was selected."})},
                {"next_test", "Repeat the same setting for three clean laps."},
                {"causality_note", "This is an association, not proof of cause."},
            }},
        };
        const auto report = racebox::crew_chief::parse_report(gateway_response);
        require(report.verdict == "mixed" && report.confidence == 74,
                "Crew Chief report fields were not parsed");
        require(report.observations.size() == 1 && report.confounds.size() == 1,
                "Crew Chief report evidence lists were not parsed");
        require(report.model == "openai/gpt-5.6-sol",
                 "Crew Chief route metadata was not retained");
        require(report.thinking == "xhigh" && report.selected_lane == "correlation",
                "Crew Chief intelligence metadata was not retained");
        require(report.prior_setup_result_ids.size() == 1,
                "Crew Chief prior-result references were not retained");
        require(report.evidence.quality_confidence == 84 &&
                report.evidence.lateral_response_delta_g == 0.03,
                "Crew Chief deterministic evidence summary was not retained");
        require(report.evidence.straight_speed_attribution == "exit_or_line_driven" &&
                report.evidence.brake_response_status == "possible_lockup_or_low_grip",
                "Crew Chief v3 straight and brake evidence was not retained");
        require(report.evidence.overdriving_status == "more_overdriving_no_lap_gain" &&
                report.evidence.overdriving_index_delta == 14.5,
                "Crew Chief overdriving evidence was not retained");
        require(report.evidence.chassis_roll_status == "more_chassis_roll_signature" &&
                report.evidence.chassis_roll_delta_deg == 1.2 &&
                report.evidence.current_surface_tilt_samples == 80,
                "Crew Chief chassis-roll evidence was not retained");
        require(report.evidence.roll_rate_status == "faster_roll_build" &&
                report.evidence.roll_rate_delta_dps == 46.0 &&
                report.evidence.current_roll_rate_samples == 76,
                "Crew Chief roll-rate evidence was not retained");

        auto invalid = gateway_response;
        invalid["report"]["verdict"] = "definitely";
        invalid["report"]["confidence"] = 500;
        const auto bounded = racebox::crew_chief::parse_report(invalid);
        require(bounded.verdict == "inconclusive" && bounded.confidence == 100,
                "Crew Chief response validation did not enforce safe bounds");

        std::cout << "Crew Chief evidence and response tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Crew Chief test failure: " << exception.what() << '\n';
        return 1;
    }
}
