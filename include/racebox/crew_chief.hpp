#pragma once

#include "racebox/driver_analysis.hpp"
#include "racebox/imu_analysis.hpp"
#include "racebox/session.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace racebox::crew_chief {

inline constexpr std::string_view kEvidenceContractVersion{"racebox-crew-chief-evidence-v1"};
inline constexpr std::string_view kRaceDayRequestVersion{"racebox-race-day-request-v3"};

struct ChatTurn {
    std::string role;
    std::string content;
};

struct Observation {
    std::string area;
    std::string change;
    std::string meaning;
    std::vector<std::string> evidence_ids;
};

struct EvidenceSummary {
    std::string analytics_contract;
    int formula_version{};
    int quality_confidence{};
    std::string track_status;
    std::optional<double> track_translation_m;
    std::optional<double> track_residual_rms_m;
    std::optional<double> lap_time_delta_s;
    std::string lateral_status;
    std::optional<double> lateral_response_delta_g;
    std::string forward_status;
    std::optional<double> forward_bite_delta_g;
    int previous_brake_indicators{};
    int current_brake_indicators{};
    std::string top_speed_status;
    std::optional<double> straight_top_speed_delta_kmh;
    std::optional<double> straight_entry_speed_delta_kmh;
    std::optional<double> straight_acceleration_delta_g;
    std::string straight_speed_attribution;
    std::string brake_response_status;
    std::optional<double> brake_decel_delta_g;
    std::optional<double> brake_response_delay_delta_s;
    std::string corner_balance_status;
    std::optional<double> steering_for_lateral_g_delta_percent;
    std::optional<double> yaw_per_steering_delta_dps;
    std::string overdriving_status;
    std::optional<double> overdriving_index_delta;
    std::optional<double> overdriving_risk_sample_delta_percent;
    std::optional<double> current_late_overdriving_delta_score;
    std::string chassis_roll_status;
    std::optional<double> surface_tilt_delta_deg;
    std::optional<double> chassis_roll_delta_deg;
    std::optional<double> roll_per_lateral_g_delta_deg;
    std::string roll_rate_status;
    std::optional<double> previous_roll_rate_p90_dps;
    std::optional<double> current_roll_rate_p90_dps;
    std::optional<double> roll_rate_delta_dps;
    std::optional<double> roll_rate_per_lateral_g_delta_dps;
    std::optional<double> current_late_roll_rate_delta_dps;
    int previous_roll_rate_samples{};
    int current_roll_rate_samples{};
    std::string current_surface_tilt_source;
    int current_surface_tilt_samples{};
};

struct Report {
    std::string verdict{"inconclusive"};
    int confidence{};
    std::string summary;
    std::vector<Observation> observations;
    std::vector<std::string> confounds;
    std::string next_test;
    std::string causality_note;
    std::string route;
    std::string model;
    std::string thinking;
    std::string selected_lane;
    std::vector<std::string> prior_setup_result_ids;
    EvidenceSummary evidence;
};

struct Response {
    bool ok{};
    int http_status{};
    Report report;
    std::string error;
};

// Builds a bounded, deterministic evidence packet. "Reference" is the before
// lap and the selected comparison is the after lap. It contains calculated
// values only; no image, graph screenshot, file path, annotation, or secret is
// included.
[[nodiscard]] nlohmann::json build_evidence_packet(
    const Session& session,
    const LapInfo& reference,
    const LapInfo& after,
    const driver_analysis::AnalysisResult& analysis,
    driver_analysis::ComparisonSlot slot,
    const imu::Analysis* imu_analysis = nullptr);

// Accepts either a direct report object or the gateway envelope
// {"ok":true,"report":{...}} and applies strict length/count bounds.
[[nodiscard]] Report parse_report(const nlohmann::json& value);

// Blocking request intended to run on a worker thread. The bearer token is
// optional because the default service is bound only to the user's Tailscale
// address; deployments may still require one through an environment variable.
[[nodiscard]] Response request_report(
    std::string_view endpoint,
    std::string_view bearer_token,
    std::string_view setup_change,
    std::string_view question,
    const nlohmann::json& evidence,
    const nlohmann::json& prior_setup_results,
    std::span<const ChatTurn> history,
    std::uint32_t timeout_ms = 180'000);

// Race-day analysis is an explicit, higher-disclosure path. The private
// Tailscale gateway receives two generated canonical telemetry CSV strings plus
// the user's run notes/conditions, calculates deterministic vehicle-dynamics
// indicators, and sends only those results to the language model.
[[nodiscard]] Response request_race_day_report(
    std::string_view endpoint,
    std::string_view bearer_token,
    const nlohmann::json& previous_context,
    std::string_view previous_analytics_csv,
    const nlohmann::json& current_context,
    std::string_view current_analytics_csv,
    std::string_view question,
    const nlohmann::json& prior_setup_results,
    std::span<const ChatTurn> history = {},
    std::uint32_t timeout_ms = 240'000);

}  // namespace racebox::crew_chief
