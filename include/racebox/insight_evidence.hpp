#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace racebox::insight_evidence {

// The evidence classifier stays UI-independent so outcome, reliability, and
// recommendation gates remain deterministic and directly unit-testable.
inline constexpr std::string_view kAddonVersion{"driver-analysis-v2-evidence-addon-1"};

enum class Outcome : std::uint8_t {
    DataLimited,
    Inconclusive,
    NetLoss,
    Compensation,
    RetainedGain,
    TradeoffGain,
};

enum class Reliability : std::uint8_t {
    Unproven,
    Likely,
    ReliableAssociation,
};

enum class Recommendation : std::uint8_t {
    None,
    Observe,
    Validate,
    RecommendTechnique,
    RecommendSequence,
};

enum class Reason : std::uint8_t {
    InvalidInput,
    LowDataConfidence,
    IncompleteLap,
    TelemetryGap,
    ContextChanged,
    LineMetricDisabled,
    BelowNoiseFloor,
    PriorPhaseLoss,
    DownstreamPayback,
    TrackLimitViolation,
    ExtraSteeringCorrection,
    PossibleInstability,
    AggregateEvidenceMissing,
    InsufficientComparableLaps,
    InsufficientSupportingLaps,
    LowSupportRate,
    ReliabilityIntervalCrossesNoise,
};

struct Parameters {
    double minimum_time_floor_s{0.05};
    double sample_period_multiplier{2.0};
    double repeatability_sigma_multiplier{1.5};
    double maximum_payback_fraction{0.50};

    int minimum_data_confidence{60};
    int reliable_data_confidence{75};

    std::size_t likely_comparable_laps{4};
    std::size_t likely_supporting_laps{3};
    double likely_support_rate{0.70};

    std::size_t reliable_comparable_laps{8};
    std::size_t reliable_supporting_laps{5};
    double reliable_support_rate{0.75};
};

struct CandidateEvidence {
    // Timing effects use the same sign as the relative-time graph:
    // positive is time lost; negative is time gained.
    double sample_period_s{};
    double timing_repeatability_sigma_s{};
    int data_confidence{};

    bool technique_change_detected{};
    bool favorable_local_metric{};
    double prior_phase_effect_s{};
    double local_effect_s{};
    double retained_effect_s{};

    // Aggregate fields describe repeated quality-screened matching laps. They are
    // deliberately optional so one selected comparison cannot impersonate
    // repeatable evidence.
    std::size_t comparable_laps{};
    std::size_t supporting_laps{};
    std::optional<double> median_retained_effect_s;
    std::optional<double> retained_interval_low_s;
    std::optional<double> retained_interval_high_s;

    bool complete_lap{true};
    bool telemetry_gap{};
    bool context_changed{};
    bool track_limit_violation{};
    bool extra_steering_correction{};
    bool possible_instability{};
    bool line_metric{};
    bool line_conclusion_enabled{true};
};

struct Result {
    Outcome outcome{Outcome::DataLimited};
    Reliability reliability{Reliability::Unproven};
    Recommendation recommendation{Recommendation::None};
    double time_noise_floor_s{};
    double retained_gain_s{};
    std::optional<double> downstream_payback_fraction;
    std::vector<Reason> reasons;
};

[[nodiscard]] Parameters default_parameters() noexcept;
[[nodiscard]] Result evaluate(const CandidateEvidence& evidence, const Parameters& parameters = default_parameters());
[[nodiscard]] bool has_reason(const Result& result, Reason reason) noexcept;
[[nodiscard]] std::string_view outcome_name(Outcome outcome) noexcept;
[[nodiscard]] std::string_view reliability_name(Reliability reliability) noexcept;
[[nodiscard]] std::string_view recommendation_name(Recommendation recommendation) noexcept;
[[nodiscard]] std::string_view reason_name(Reason reason) noexcept;

}  // namespace racebox::insight_evidence
