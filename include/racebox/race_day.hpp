#pragma once

#include "racebox/core.hpp"
#include "racebox/source_identity.hpp"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace racebox::race_day {

inline constexpr std::string_view kFileFormat{"racebox-race-day"};
inline constexpr int kFileVersion = 3;
inline constexpr std::string_view kAnalyticsCsvVersion{"racebox-session-analytics-csv-v1"};
inline constexpr std::string_view kSetupAnalyticsVersion{"racebox-setup-analytics-v3"};
inline constexpr std::size_t kMaximumSetupKnowledgeRecords = 200;

enum class RunKind {
    Practice,
    Qualifying,
    Main,
    Custom,
};

struct ChecklistItem {
    std::string id;
    std::string label;
    bool checked{};
    std::string note;
};

struct Conditions {
    std::optional<double> ambient_temperature_c;
    std::optional<double> track_temperature_c;
    std::string track_condition;
    std::string tire_set_id;
    std::string tire_compound;
    int tire_runs_before{-1};
    std::string sauce_compound;
    int sauce_minutes_before{-1};
    int tire_warmer_minutes{-1};
    std::optional<double> tire_warmer_temperature_c;
    std::string battery_pack;
    std::optional<double> battery_voltage;
};

struct Run {
    std::string id;
    std::string label;
    RunKind kind{RunKind::Custom};
    int ordinal{};
    char main_group{'A'};
    int main_leg{};
    std::vector<std::filesystem::path> telemetry_files;
    // Parallel to telemetry_files. Version-3 Race Day files retain only the
    // basename, byte size, timestamp, and optional caller-provided fingerprint
    // needed for safe relinking; no telemetry content is copied here.
    std::vector<source_identity::SourceIdentity> telemetry_source_identities;
    std::vector<ChecklistItem> checklist;
    std::string pre_run_notes;
    std::string setup_changes;
    std::string post_run_notes;
    Conditions conditions;
};

struct SetupKnowledgeEvidence {
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

struct SetupAnalyticsSummary {
    std::string analytics_contract{std::string(kSetupAnalyticsVersion)};
    int formula_version{3};
    int quality_confidence{};
    std::string track_status{"not_checked"};
    std::optional<double> lap_time_delta_s;
    std::string lateral_status{"data_limited"};
    std::optional<double> lateral_response_delta_g;
    std::string forward_status{"data_limited"};
    std::optional<double> forward_bite_delta_g;
    int previous_brake_indicators{};
    int current_brake_indicators{};
    std::string top_speed_status{"data_limited"};
    std::optional<double> straight_top_speed_delta_kmh;
    std::optional<double> straight_entry_speed_delta_kmh;
    std::optional<double> straight_acceleration_delta_g;
    std::string straight_speed_attribution{"data_limited"};
    std::string brake_response_status{"data_limited"};
    std::optional<double> brake_decel_delta_g;
    std::optional<double> brake_response_delay_delta_s;
    std::string corner_balance_status{"data_limited"};
    std::optional<double> steering_for_lateral_g_delta_percent;
    std::optional<double> yaw_per_steering_delta_dps;
    std::string overdriving_status{"data_limited"};
    std::optional<double> overdriving_index_delta;
    std::optional<double> overdriving_risk_sample_delta_percent;
    std::optional<double> current_late_overdriving_delta_score;
    std::string chassis_roll_status{"data_limited"};
    std::optional<double> surface_tilt_delta_deg;
    std::optional<double> chassis_roll_delta_deg;
    std::optional<double> roll_per_lateral_g_delta_deg;
    std::string roll_rate_status{"data_limited"};
    std::optional<double> previous_roll_rate_p90_dps;
    std::optional<double> current_roll_rate_p90_dps;
    std::optional<double> roll_rate_delta_dps;
    std::optional<double> roll_rate_per_lateral_g_delta_dps;
    std::optional<double> current_late_roll_rate_delta_dps;
    int previous_roll_rate_samples{};
    int current_roll_rate_samples{};
    std::string current_surface_tilt_source{"data_limited"};
    int current_surface_tilt_samples{};
};

struct SetupKnowledgeObservation {
    std::string area;
    std::string change;
    std::string meaning;
    std::vector<std::string> evidence_ids;
};

// A bounded, local result record. It deliberately stores no telemetry samples,
// source paths, credentials, or unrestricted model memory.
struct SetupKnowledgeRecord {
    std::string id;
    std::string created_at_utc;
    std::string event_name;
    std::string track_name;
    std::string previous_run_id;
    std::string previous_run_label;
    std::string current_run_id;
    std::string current_run_label;
    std::string handling_question;
    std::string setup_change;
    std::string driver_result;
    Conditions previous_conditions;
    Conditions current_conditions;
    std::string verdict{"inconclusive"};
    int confidence{};
    std::string summary;
    std::vector<SetupKnowledgeObservation> observations;
    std::vector<std::string> confounds;
    std::string next_test;
    std::string causality_note;
    std::string route;
    std::string model;
    std::string thinking;
    std::string selected_lane;
    SetupKnowledgeEvidence evidence;
};

struct Day {
    std::string event_name;
    std::string track_name;
    std::string date;
    std::vector<Run> runs;
    std::vector<SetupKnowledgeRecord> setup_knowledge;
};

[[nodiscard]] std::vector<ChecklistItem> default_pre_run_checklist();
[[nodiscard]] Day standard_day(bool include_q4 = false, bool triple_a_main = false);
Run& add_practice(Day& day);
Run& add_qualifying(Day& day);
std::vector<std::size_t> add_main_group(Day& day, char group, int legs);
Run& add_custom(Day& day, std::string label);

[[nodiscard]] const char* kind_name(RunKind kind) noexcept;
[[nodiscard]] bool has_racebox_csv(const Run& run);
[[nodiscard]] bool has_sanwa_csv(const Run& run);
[[nodiscard]] bool has_primary_telemetry(const Run& run);

enum class TelemetrySourceState {
    Available,
    Changed,
    Missing,
};

// Checks the current file against its persisted basename, size, and modified
// time. A replaced file is never silently accepted merely because the old path
// still exists.
[[nodiscard]] TelemetrySourceState telemetry_source_state(
    const Run& run, std::size_t source_index,
    bool verify_content = false) noexcept;

// Captures identity metadata only for newly attached sources. Existing stored
// identities remain authoritative until an exact relink or explicit user
// confirmation replaces them.
void refresh_telemetry_source_identities(Run& run);

bool save(const Day& day, const std::filesystem::path& destination, std::string& error) noexcept;
bool load(const std::filesystem::path& source, Day& day, std::string& error) noexcept;

// Inserts or replaces one result and keeps only the newest bounded history.
void upsert_setup_knowledge(Day& day, SetupKnowledgeRecord record);

// Finds locally relevant prior results using deterministic track, setup-term,
// handling-term, condition, confidence, and recency scoring. No network or
// language model is involved in retrieval.
[[nodiscard]] std::vector<std::size_t> relevant_setup_knowledge(
    const Day& day,
    std::string_view question,
    const Run& current_run,
    std::size_t maximum_records = 8);

// Builds the bounded v2 gateway memory payload. Raw telemetry and source paths
// cannot enter this object because SetupKnowledgeRecord does not contain them.
[[nodiscard]] nlohmann::json build_prior_setup_results(
    const Day& day,
    std::span<const std::size_t> record_indices);

// Loads only the selected run. Race-day files retain paths, not resident
// telemetry, so a full event does not keep every recording in memory.
[[nodiscard]] LoadResult load_run_telemetry(const Run& run);

// Produces an aligned, bounded CSV specifically for the private deterministic
// analytics layer. It includes RaceBox/VBO measurements plus interpolated Sanwa
// inputs when available. Raw recordings are never modified.
[[nodiscard]] std::string build_analytics_csv(
    const Session& session,
    const imu::Analysis& imu_analysis,
    std::size_t maximum_rows = 100'000);

// Deterministic previous/current setup summary. It compares complete laps by
// physical progress and correlates driver inputs against measured outputs.
[[nodiscard]] SetupAnalyticsSummary analyze_setup_change(
    const Session& previous,
    const imu::Analysis& previous_imu,
    const Session& current,
    const imu::Analysis& current_imu);

}  // namespace racebox::race_day
