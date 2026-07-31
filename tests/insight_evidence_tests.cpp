#include "racebox/core.hpp"
#include "racebox/driver_analysis.hpp"
#include "racebox/insight_evidence.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

using namespace racebox;
namespace analysis = racebox::driver_analysis;
namespace evidence = racebox::insight_evidence;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void close_to(double actual, double expected, double tolerance, const char* message) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
        throw std::runtime_error(message);
    }
}

evidence::CandidateEvidence base_candidate() {
    evidence::CandidateEvidence candidate;
    candidate.sample_period_s = 0.02;
    candidate.timing_repeatability_sigma_s = 0.01;
    candidate.data_confidence = 85;
    candidate.technique_change_detected = true;
    candidate.retained_effect_s = -0.09;
    return candidate;
}

void add_reliable_aggregate(evidence::CandidateEvidence& candidate) {
    candidate.comparable_laps = 8;
    candidate.supporting_laps = 6;
    candidate.median_retained_effect_s = -0.08;
    candidate.retained_interval_low_s = -0.12;
    candidate.retained_interval_high_s = -0.06;
}

const LapInfo& raw_lap(const Session& session, std::int32_t number) {
    const auto iterator = std::find_if(session.laps.begin(), session.laps.end(), [number](const LapInfo& lap) {
        return lap.raw_lap == number;
    });
    if (iterator == session.laps.end()) throw std::runtime_error("Expected golden raw lap is missing");
    return *iterator;
}

}  // namespace

int main() {
    try {
        const auto parameters = evidence::default_parameters();
        require(evidence::kAddonVersion == "driver-analysis-v2-evidence-addon-1", "Add-on version changed unexpectedly");

        auto candidate = base_candidate();
        auto result = evidence::evaluate(candidate, parameters);
        close_to(result.time_noise_floor_s, 0.05, 1e-12, "Minimum timing floor changed");
        require(result.outcome == evidence::Outcome::RetainedGain, "Selected-lap retained gain was not recognized");
        require(result.reliability == evidence::Reliability::Unproven, "One lap was treated as repeatable evidence");
        require(result.recommendation == evidence::Recommendation::Observe, "One lap produced a recommendation");
        require(evidence::has_reason(result, evidence::Reason::AggregateEvidenceMissing),
                "Missing repeated evidence was not disclosed");

        candidate = base_candidate();
        candidate.sample_period_s = 0.04;
        candidate.timing_repeatability_sigma_s = 0.02;
        result = evidence::evaluate(candidate, parameters);
        close_to(result.time_noise_floor_s, 0.08, 1e-12, "Two-sample timing floor was not applied at 25 Hz");

        candidate = base_candidate();
        candidate.timing_repeatability_sigma_s = 0.06;
        result = evidence::evaluate(candidate, parameters);
        close_to(result.time_noise_floor_s, 0.09, 1e-12, "Measured repeatability did not raise the timing floor");
        require(result.outcome == evidence::Outcome::RetainedGain, "A gain exactly at the noise boundary was lost");

        candidate = base_candidate();
        candidate.favorable_local_metric = true;
        candidate.prior_phase_effect_s = 0.12;
        candidate.local_effect_s = -0.04;
        candidate.retained_effect_s = 0.08;
        result = evidence::evaluate(candidate, parameters);
        require(result.outcome == evidence::Outcome::Compensation,
                "A better-looking metric after an overall loss was not classified as compensation");
        require(result.recommendation == evidence::Recommendation::None,
                "Compensation incorrectly generated a recommendation");

        candidate = base_candidate();
        candidate.favorable_local_metric = false;
        candidate.local_effect_s = 0.03;
        candidate.retained_effect_s = 0.08;
        result = evidence::evaluate(candidate, parameters);
        require(result.outcome == evidence::Outcome::NetLoss, "An unambiguous overall loss was not classified as a loss");

        candidate = base_candidate();
        candidate.favorable_local_metric = true;
        candidate.prior_phase_effect_s = 0.10;
        candidate.local_effect_s = -0.10;
        candidate.retained_effect_s = 0.01;
        result = evidence::evaluate(candidate, parameters);
        require(result.outcome == evidence::Outcome::Compensation,
                "A local recovery that only returned to noise was promoted to a gain");

        candidate = base_candidate();
        add_reliable_aggregate(candidate);
        result = evidence::evaluate(candidate, parameters);
        require(result.reliability == evidence::Reliability::ReliableAssociation,
                "Repeated retained gain did not reach reliable-association status");
        require(result.recommendation == evidence::Recommendation::RecommendTechnique,
                "Clean repeated retained gain did not recommend the technique");

        candidate = base_candidate();
        candidate.prior_phase_effect_s = 0.08;
        candidate.local_effect_s = -0.18;
        candidate.retained_effect_s = -0.10;
        add_reliable_aggregate(candidate);
        candidate.median_retained_effect_s = -0.09;
        result = evidence::evaluate(candidate, parameters);
        require(result.outcome == evidence::Outcome::TradeoffGain, "A slower-entry/faster-exit package was not a trade-off");
        require(result.recommendation == evidence::Recommendation::RecommendSequence,
                "A reliable trade-off recommended an isolated action instead of the complete sequence");
        require(evidence::has_reason(result, evidence::Reason::PriorPhaseLoss), "Prior loss was not disclosed");

        candidate = base_candidate();
        candidate.local_effect_s = -0.20;
        candidate.retained_effect_s = -0.08;
        add_reliable_aggregate(candidate);
        result = evidence::evaluate(candidate, parameters);
        require(result.outcome == evidence::Outcome::TradeoffGain, "A mostly paid-back local gain was not marked as a trade-off");
        require(result.reliability == evidence::Reliability::ReliableAssociation,
                "The evidence strength should remain separate from downstream fragility");
        require(result.recommendation == evidence::Recommendation::Validate,
                "More than 50 percent downstream payback should require validation");
        require(result.downstream_payback_fraction && *result.downstream_payback_fraction > 0.50,
                "Downstream payback fraction was not calculated");

        candidate = base_candidate();
        candidate.comparable_laps = 5;
        candidate.supporting_laps = 4;
        candidate.median_retained_effect_s = -0.07;
        candidate.retained_interval_low_s = -0.11;
        candidate.retained_interval_high_s = -0.02;
        result = evidence::evaluate(candidate, parameters);
        require(result.reliability == evidence::Reliability::Likely, "Promising repeated evidence did not become likely");
        require(result.recommendation == evidence::Recommendation::Validate,
                "Likely evidence should request validation rather than make a recommendation");

        candidate = base_candidate();
        add_reliable_aggregate(candidate);
        candidate.supporting_laps = 5;  // 62.5 percent support.
        result = evidence::evaluate(candidate, parameters);
        require(result.reliability == evidence::Reliability::Unproven, "Low support rate was accepted as reliable");
        require(evidence::has_reason(result, evidence::Reason::LowSupportRate), "Low support rate was not disclosed");

        candidate = base_candidate();
        add_reliable_aggregate(candidate);
        candidate.retained_interval_high_s = -0.03;
        result = evidence::evaluate(candidate, parameters);
        require(result.reliability == evidence::Reliability::Likely,
                "An interval crossing the noise floor should remain likely, not reliable");
        require(evidence::has_reason(result, evidence::Reason::ReliabilityIntervalCrossesNoise),
                "Interval/noise conflict was not disclosed");

        candidate = base_candidate();
        add_reliable_aggregate(candidate);
        candidate.track_limit_violation = true;
        result = evidence::evaluate(candidate, parameters);
        require(result.reliability == evidence::Reliability::ReliableAssociation,
                "A guardrail should not rewrite the measured evidence strength");
        require(result.recommendation == evidence::Recommendation::None,
                "Track-limit evidence incorrectly produced a recommendation");

        candidate = base_candidate();
        add_reliable_aggregate(candidate);
        candidate.extra_steering_correction = true;
        result = evidence::evaluate(candidate, parameters);
        require(result.recommendation == evidence::Recommendation::None,
                "Extra steering correction did not block the recommendation");

        candidate = base_candidate();
        candidate.data_confidence = 59;
        result = evidence::evaluate(candidate, parameters);
        require(result.outcome == evidence::Outcome::DataLimited &&
                    result.recommendation == evidence::Recommendation::None,
                "Low-quality data escaped the hard gate");

        candidate = base_candidate();
        candidate.context_changed = true;
        result = evidence::evaluate(candidate, parameters);
        require(result.outcome == evidence::Outcome::DataLimited,
                "A setup/conditions change was not treated as data limited");

        candidate = base_candidate();
        candidate.line_metric = true;
        candidate.line_conclusion_enabled = false;
        result = evidence::evaluate(candidate, parameters);
        require(result.outcome == evidence::Outcome::DataLimited,
                "A disabled GPS line conclusion survived the evidence gate");

        candidate = base_candidate();
        candidate.sample_period_s = 0.0;
        result = evidence::evaluate(candidate, parameters);
        require(result.outcome == evidence::Outcome::DataLimited &&
                    evidence::has_reason(result, evidence::Reason::InvalidInput),
                "Invalid evidence was not rejected");

        auto invalid_parameters = parameters;
        invalid_parameters.maximum_payback_fraction = 1.1;
        result = evidence::evaluate(base_candidate(), invalid_parameters);
        require(result.outcome == evidence::Outcome::DataLimited &&
                    evidence::has_reason(result, evidence::Reason::InvalidInput),
                "Invalid evidence parameters were not rejected");

        // Golden-session integration harness: the driver-analysis engine now
        // supplies real phase effects and repeated-lap aggregates to the same
        // deterministic classifier exercised above.
        const std::filesystem::path golden = GOLDEN_DIR;
        const auto session = load_session({golden / L"session.vbo", golden / L"session.csv", golden / L"sanwa.csv"}).session;
        const auto& reference = raw_lap(session, 16);  // Mirrors the currently loaded R15 reference.
        const auto& compare_a = raw_lap(session, 7);  // R6, the known translated GPS lap.
        const auto& compare_b = raw_lap(session, 9);  // R8.
        const auto corners = analysis::suggest_corner_zones(session, reference);
        require(!corners.empty(), "Golden integration harness produced no analyzable corners");
        const auto integrated = analysis::analyze_driver_performance(
            session, reference, &compare_a, &compare_b, corners, analysis::default_analysis_rules());
        require(!integrated.insights.empty(), "Golden integration harness produced no insight cards");

        std::size_t compensation_count = 0;
        std::size_t retained_observation_count = 0;
        std::size_t recommendation_count = 0;
        for (const auto& insight : integrated.insights) {
            require(insight.time_noise_floor_s >= parameters.minimum_time_floor_s,
                    "Integrated card did not disclose its dynamic timing floor");
            require(insight.supporting_laps <= insight.comparable_laps,
                    "Integrated card has more supporting laps than comparable laps");
            if (insight.outcome == evidence::Outcome::Compensation) {
                ++compensation_count;
                require(insight.recommendation == evidence::Recommendation::None,
                        "Integrated compensation card produced a recommendation");
            }
            if (insight.outcome == evidence::Outcome::RetainedGain ||
                insight.outcome == evidence::Outcome::TradeoffGain) {
                ++retained_observation_count;
            }
            if (insight.recommendation == evidence::Recommendation::RecommendTechnique ||
                insight.recommendation == evidence::Recommendation::RecommendSequence) {
                ++recommendation_count;
                require(insight.reliability == evidence::Reliability::ReliableAssociation,
                        "Integrated recommendation was not backed by reliable session evidence");
                require(insight.comparable_laps >= parameters.reliable_comparable_laps &&
                            insight.supporting_laps >= parameters.reliable_supporting_laps,
                        "Integrated recommendation bypassed the repeated-lap count gates");
            }
        }
        std::cout << "Golden auto-suggested corners for current R15 reference: " << corners.size() << '\n';
        std::cout << "Golden cards checked by integrated evidence engine: " << integrated.insights.size() << '\n';
        std::cout << "Golden compensation cards detected: " << compensation_count << '\n';
        std::cout << "Golden retained/trade-off gain cards: " << retained_observation_count << '\n';
        std::cout << "Golden reliable recommendations: " << recommendation_count << '\n';

        std::cout << "All recommendation-evidence and integration checks passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Insight-evidence add-on test failure: " << exception.what() << '\n';
        return 1;
    }
}
