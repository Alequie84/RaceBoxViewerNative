#pragma once

#include "racebox/insight_evidence.hpp"
#include "racebox/session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace racebox::driver_analysis {

inline constexpr std::string_view kFormulaVersion{"driver-analysis-v2"};

enum class ComparisonSlot : std::uint8_t { CompareA, CompareB };
enum class ConfidenceBand : std::uint8_t { Suppressed, WeakSignal, Actionable };
enum class Severity : std::uint8_t { Low, Medium, High };
enum class EventType : std::uint8_t {
    BrakeBegin,
    BrakeRelease,
    TurnIn,
    Apex,
    FirstThrottle,
    FullThrottle,
    SteeringCorrection
};
enum class MetricKind : std::uint8_t {
    BrakePointDelta,
    TurnInDelta,
    ApexTimingDelta,
    MinimumSpeedDelta,
    ExitSpeedDelta,
    ThrottlePickupDelta,
    EntryLineDeviation,
    RelativeTimeChange
};
enum class RuleId : std::uint8_t {
    BrakePointDelta,
    TurnInDelta,
    ApexTimingDelta,
    MinimumSpeedLoss,
    ExitSpeedDelta,
    ThrottlePickupDelay,
    EntryLineDeviation,
    RelativeTimeChange
};

struct CornerZone {
    std::string id;
    std::string name;
    double start_progress{};
    double turn_in_progress{};
    double apex_progress{};
    double exit_progress{};
    double end_progress{};
};

struct RuleSetting {
    RuleId id{};
    bool enabled{true};
    double threshold{};
};

struct RuleDescriptor {
    RuleId id{};
    std::string_view stable_id;
    std::string_view name;
    std::string_view formula;
    std::string_view unit;
    double default_threshold{};
};

struct AnalysisRules {
    std::array<RuleSetting, 8> metrics{{
        {RuleId::BrakePointDelta, true, 0.08},
        {RuleId::TurnInDelta, true, 0.08},
        {RuleId::ApexTimingDelta, true, 0.08},
        {RuleId::MinimumSpeedLoss, true, 1.0},
        {RuleId::ExitSpeedDelta, true, 1.0},
        {RuleId::ThrottlePickupDelay, true, 0.08},
        {RuleId::EntryLineDeviation, true, 0.35},
        {RuleId::RelativeTimeChange, true, 0.05},
    }};

    Timestamp brake_begin_dwell_us{80'000};
    Timestamp brake_release_dwell_us{80'000};
    Timestamp turn_in_dwell_us{80'000};
    Timestamp first_throttle_dwell_us{80'000};
    Timestamp full_throttle_dwell_us{100'000};
    float brake_begin_percent{10.0F};
    float brake_release_percent{5.0F};
    float turn_in_steering_percent{15.0F};
    float first_throttle_percent{10.0F};
    float full_throttle_percent{90.0F};
    float steering_correction_percent{20.0F};
    double turn_in_curvature_per_m{0.006};
    insight_evidence::Parameters evidence{};
};

struct NavigationTarget {
    std::int32_t reference_raw_lap{};
    std::size_t reference_sample_index{};
    Timestamp reference_timestamp_us{};
    double reference_elapsed_s{};
    double reference_distance_m{};
    double reference_progress{};
};

struct DetectedEvent {
    EventType type{};
    std::string corner_id;
    std::int32_t raw_lap{};
    std::size_t sample_index{};
    Timestamp timestamp_us{};
    double elapsed_s{};
    double distance_m{};
    double progress{};
    float observed_value{};
};

struct LineTranslation {
    // East/north offsets used by line analysis and, when explicitly enabled, a
    // derived display overlay. Session telemetry is never changed.
    double east_m{};
    double north_m{};
    double magnitude_m{};
    double residual_rms_m{};
    bool preserves_raw_telemetry{true};
};

struct TranslatedCoordinate {
    double latitude{};
    double longitude{};
};

struct ConfidenceBreakdown {
    int satellite_score{};
    int sampling_score{};
    int repeatability_score{};
    int event_clarity_score{};
    int translation_score{};
    int overall{};
    ConfidenceBand band{ConfidenceBand::Suppressed};
    bool line_metrics_enabled{};
};

struct DerivedMetric {
    MetricKind kind{};
    std::optional<RuleId> rule_id;
    std::optional<double> value;
    double threshold{};
    bool rule_enabled{};
    bool triggered{};
    bool positive{};
    Severity severity{Severity::Low};
    int confidence{};
    ConfidenceBand confidence_band{ConfidenceBand::Suppressed};
    NavigationTarget navigation;
};

struct CornerMetrics {
    std::string corner_id;
    std::string corner_name;
    double start_distance_m{};
    double end_distance_m{};
    std::vector<DerivedMetric> metrics;
};

struct LapEventAnalysis {
    std::int32_t raw_lap{};
    double total_distance_m{};
    std::vector<DetectedEvent> events;
};

struct ComparisonAnalysis {
    ComparisonSlot slot{ComparisonSlot::CompareA};
    std::int32_t raw_lap{};
    LineTranslation line_translation;
    ConfidenceBreakdown confidence;
    std::vector<DetectedEvent> events;
    std::vector<CornerMetrics> corners;
};

struct Insight {
    std::string id;
    std::string title;
    std::string detail;
    std::string coaching;
    std::string corner_id;
    std::string corner_name;
    std::string turn_direction;
    ComparisonSlot comparison{ComparisonSlot::CompareA};
    std::int32_t comparison_raw_lap{};
    MetricKind metric{};
    RuleId rule{};
    std::string rule_stable_id;
    double measured_value{};
    double threshold{};
    double estimated_time_effect_s{};
    bool positive{};
    Severity severity{Severity::Low};
    int confidence{};
    ConfidenceBand confidence_band{ConfidenceBand::Suppressed};
    insight_evidence::Outcome outcome{insight_evidence::Outcome::DataLimited};
    insight_evidence::Reliability reliability{insight_evidence::Reliability::Unproven};
    insight_evidence::Recommendation recommendation{insight_evidence::Recommendation::None};
    double prior_phase_effect_s{};
    double local_effect_s{};
    double retained_effect_s{};
    double time_noise_floor_s{};
    double retained_gain_s{};
    std::optional<double> downstream_payback_fraction;
    std::size_t comparable_laps{};
    std::size_t supporting_laps{};
    std::optional<double> median_retained_effect_s;
    std::optional<double> retained_interval_low_s;
    std::optional<double> retained_interval_high_s;
    std::vector<insight_evidence::Reason> evidence_reasons;
    bool controls_measured{};
    NavigationTarget navigation;
};

struct AnalysisResult {
    std::string formula_version{std::string(kFormulaVersion)};
    std::vector<CornerZone> corners;
    LapEventAnalysis reference;
    std::array<std::optional<ComparisonAnalysis>, 2> comparisons;
    std::vector<Insight> insights;
    std::vector<std::string> diagnostics;
};

[[nodiscard]] AnalysisRules default_analysis_rules();
[[nodiscard]] std::span<const RuleDescriptor> rule_descriptors();
[[nodiscard]] const RuleSetting* find_rule(const AnalysisRules& rules, RuleId id);
[[nodiscard]] ConfidenceBand confidence_band(int score) noexcept;
[[nodiscard]] Severity classify_severity(double threshold_multiple, double absolute_time_effect_s) noexcept;
[[nodiscard]] std::string_view event_name(EventType type) noexcept;
[[nodiscard]] std::string_view metric_name(MetricKind kind) noexcept;
[[nodiscard]] TranslatedCoordinate apply_line_translation(
    double latitude, double longitude, const LineTranslation& translation) noexcept;

// Suggests deterministic, editable normalized corner boundaries from the reference path,
// steering, curvature, and speed troughs. Empty input or an invalid lap yields no corners.
[[nodiscard]] std::vector<CornerZone> suggest_corner_zones(
    const Session& session, const LapInfo& reference, std::size_t maximum_corners = 12);

// Comparisons are distance-normalized to the reference. Null comparison pointers are
// allowed. The input session is read-only and the returned line translation is diagnostic.
[[nodiscard]] AnalysisResult analyze_driver_performance(
    const Session& session,
    const LapInfo& reference,
    const LapInfo* compare_a,
    const LapInfo* compare_b,
    std::span<const CornerZone> corners,
    const AnalysisRules& rules);

}  // namespace racebox::driver_analysis
