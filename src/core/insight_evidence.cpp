#include "racebox/insight_evidence.hpp"

#include <algorithm>
#include <cmath>

namespace racebox::insight_evidence {
namespace {

void add_reason(Result& result, Reason reason) {
    if (!has_reason(result, reason)) result.reasons.push_back(reason);
}

bool finite(double value) noexcept {
    return std::isfinite(value);
}

bool valid_parameters(const Parameters& parameters) noexcept {
    return finite(parameters.minimum_time_floor_s) && parameters.minimum_time_floor_s > 0.0 &&
           finite(parameters.sample_period_multiplier) && parameters.sample_period_multiplier > 0.0 &&
           finite(parameters.repeatability_sigma_multiplier) && parameters.repeatability_sigma_multiplier >= 0.0 &&
           finite(parameters.maximum_payback_fraction) && parameters.maximum_payback_fraction >= 0.0 &&
           parameters.maximum_payback_fraction <= 1.0 && parameters.minimum_data_confidence >= 0 &&
           parameters.minimum_data_confidence <= 100 &&
           parameters.reliable_data_confidence >= parameters.minimum_data_confidence &&
           parameters.reliable_data_confidence <= 100 &&
           parameters.likely_comparable_laps > 0 && parameters.likely_supporting_laps > 0 &&
           parameters.likely_supporting_laps <= parameters.likely_comparable_laps &&
           parameters.reliable_comparable_laps >= parameters.likely_comparable_laps &&
           parameters.reliable_supporting_laps >= parameters.likely_supporting_laps &&
           parameters.reliable_supporting_laps <= parameters.reliable_comparable_laps &&
           finite(parameters.likely_support_rate) && parameters.likely_support_rate >= 0.0 &&
           parameters.likely_support_rate <= 1.0 && finite(parameters.reliable_support_rate) &&
           parameters.reliable_support_rate >= parameters.likely_support_rate &&
           parameters.reliable_support_rate <= 1.0;
}

bool valid_evidence(const CandidateEvidence& evidence) noexcept {
    if (!finite(evidence.sample_period_s) || evidence.sample_period_s <= 0.0 ||
        !finite(evidence.timing_repeatability_sigma_s) || evidence.timing_repeatability_sigma_s < 0.0 ||
        !finite(evidence.prior_phase_effect_s) || !finite(evidence.local_effect_s) ||
        !finite(evidence.retained_effect_s) || evidence.data_confidence < 0 || evidence.data_confidence > 100 ||
        evidence.supporting_laps > evidence.comparable_laps) {
        return false;
    }
    if (evidence.median_retained_effect_s && !finite(*evidence.median_retained_effect_s)) return false;
    const auto has_low = evidence.retained_interval_low_s.has_value();
    const auto has_high = evidence.retained_interval_high_s.has_value();
    if (has_low != has_high) return false;
    if (has_low && (!finite(*evidence.retained_interval_low_s) || !finite(*evidence.retained_interval_high_s) ||
                       *evidence.retained_interval_low_s > *evidence.retained_interval_high_s)) {
        return false;
    }
    return true;
}

double support_rate(const CandidateEvidence& evidence) noexcept {
    if (evidence.comparable_laps == 0) return 0.0;
    return static_cast<double>(evidence.supporting_laps) / static_cast<double>(evidence.comparable_laps);
}

}  // namespace

Parameters default_parameters() noexcept {
    return {};
}

Result evaluate(const CandidateEvidence& evidence, const Parameters& parameters) {
    Result result;
    if (!valid_parameters(parameters) || !valid_evidence(evidence)) {
        add_reason(result, Reason::InvalidInput);
        return result;
    }

    result.time_noise_floor_s = std::max({parameters.minimum_time_floor_s,
        parameters.sample_period_multiplier * evidence.sample_period_s,
        parameters.repeatability_sigma_multiplier * evidence.timing_repeatability_sigma_s});
    result.retained_gain_s = -evidence.retained_effect_s;

    const auto hard_data_limit = [&]() {
        bool limited = false;
        if (evidence.data_confidence < parameters.minimum_data_confidence) {
            add_reason(result, Reason::LowDataConfidence);
            limited = true;
        }
        if (!evidence.complete_lap) {
            add_reason(result, Reason::IncompleteLap);
            limited = true;
        }
        if (evidence.telemetry_gap) {
            add_reason(result, Reason::TelemetryGap);
            limited = true;
        }
        if (evidence.context_changed) {
            add_reason(result, Reason::ContextChanged);
            limited = true;
        }
        if (evidence.line_metric && !evidence.line_conclusion_enabled) {
            add_reason(result, Reason::LineMetricDisabled);
            limited = true;
        }
        return limited;
    }();
    if (hard_data_limit) return result;

    const auto noise = result.time_noise_floor_s;
    const auto prior_loss = evidence.prior_phase_effect_s >= noise;
    const auto local_gain = evidence.local_effect_s <= -noise;
    const auto retained_gain = evidence.retained_effect_s <= -noise;
    const auto retained_loss = evidence.retained_effect_s >= noise;
    const auto favorable_signal = evidence.favorable_local_metric || local_gain;

    if (local_gain) {
        const auto local_gain_s = -evidence.local_effect_s;
        result.downstream_payback_fraction = std::max(0.0, 1.0 - result.retained_gain_s / local_gain_s);
    }

    if (!evidence.technique_change_detected) {
        result.outcome = Outcome::Inconclusive;
        add_reason(result, Reason::BelowNoiseFloor);
    } else if (retained_loss) {
        result.outcome = favorable_signal ? Outcome::Compensation : Outcome::NetLoss;
    } else if (!retained_gain) {
        result.outcome = prior_loss && favorable_signal ? Outcome::Compensation : Outcome::Inconclusive;
        add_reason(result, Reason::BelowNoiseFloor);
    } else {
        const auto excessive_payback = result.downstream_payback_fraction &&
            *result.downstream_payback_fraction > parameters.maximum_payback_fraction;
        result.outcome = prior_loss || excessive_payback ? Outcome::TradeoffGain : Outcome::RetainedGain;
        if (prior_loss) add_reason(result, Reason::PriorPhaseLoss);
        if (excessive_payback) add_reason(result, Reason::DownstreamPayback);
    }

    if (evidence.track_limit_violation) add_reason(result, Reason::TrackLimitViolation);
    if (evidence.extra_steering_correction) add_reason(result, Reason::ExtraSteeringCorrection);
    if (evidence.possible_instability) add_reason(result, Reason::PossibleInstability);

    const auto positive_outcome = result.outcome == Outcome::RetainedGain || result.outcome == Outcome::TradeoffGain;
    if (!positive_outcome) {
        result.recommendation = result.outcome == Outcome::Inconclusive ? Recommendation::Observe : Recommendation::None;
        return result;
    }

    const auto aggregate_present = evidence.median_retained_effect_s.has_value();
    const auto rate = support_rate(evidence);
    const auto aggregate_gain = aggregate_present && *evidence.median_retained_effect_s <= -noise;
    const auto interval_present = evidence.retained_interval_low_s && evidence.retained_interval_high_s;
    const auto interval_beyond_noise = interval_present && *evidence.retained_interval_high_s <= -noise;

    if (!aggregate_present) add_reason(result, Reason::AggregateEvidenceMissing);
    if (evidence.comparable_laps < parameters.likely_comparable_laps) {
        add_reason(result, Reason::InsufficientComparableLaps);
    }
    if (evidence.supporting_laps < parameters.likely_supporting_laps) {
        add_reason(result, Reason::InsufficientSupportingLaps);
    }
    if (rate < parameters.likely_support_rate) add_reason(result, Reason::LowSupportRate);
    if (!interval_beyond_noise) add_reason(result, Reason::ReliabilityIntervalCrossesNoise);

    const auto reliable = aggregate_gain && interval_beyond_noise &&
        evidence.data_confidence >= parameters.reliable_data_confidence &&
        evidence.comparable_laps >= parameters.reliable_comparable_laps &&
        evidence.supporting_laps >= parameters.reliable_supporting_laps && rate >= parameters.reliable_support_rate;
    const auto likely = aggregate_gain && evidence.comparable_laps >= parameters.likely_comparable_laps &&
        evidence.supporting_laps >= parameters.likely_supporting_laps && rate >= parameters.likely_support_rate;

    result.reliability = reliable ? Reliability::ReliableAssociation : likely ? Reliability::Likely : Reliability::Unproven;

    const auto guardrail = evidence.track_limit_violation || evidence.extra_steering_correction ||
        evidence.possible_instability;
    const auto excessive_payback = has_reason(result, Reason::DownstreamPayback);
    if (guardrail) {
        result.recommendation = Recommendation::None;
    } else if (result.reliability == Reliability::ReliableAssociation && !excessive_payback) {
        result.recommendation = result.outcome == Outcome::TradeoffGain
            ? Recommendation::RecommendSequence
            : Recommendation::RecommendTechnique;
    } else if (result.reliability == Reliability::Likely ||
               (result.reliability == Reliability::ReliableAssociation && excessive_payback)) {
        result.recommendation = Recommendation::Validate;
    } else {
        result.recommendation = Recommendation::Observe;
    }
    return result;
}

bool has_reason(const Result& result, Reason reason) noexcept {
    return std::find(result.reasons.begin(), result.reasons.end(), reason) != result.reasons.end();
}

std::string_view outcome_name(Outcome outcome) noexcept {
    switch (outcome) {
    case Outcome::DataLimited: return "data limited";
    case Outcome::Inconclusive: return "inconclusive";
    case Outcome::NetLoss: return "net time loss";
    case Outcome::Compensation: return "recovery / compensation";
    case Outcome::RetainedGain: return "retained gain";
    case Outcome::TradeoffGain: return "trade-off gain";
    }
    return "unknown";
}

std::string_view reliability_name(Reliability reliability) noexcept {
    switch (reliability) {
    case Reliability::Unproven: return "unproven";
    case Reliability::Likely: return "likely";
    case Reliability::ReliableAssociation: return "reliable session association";
    }
    return "unknown";
}

std::string_view recommendation_name(Recommendation recommendation) noexcept {
    switch (recommendation) {
    case Recommendation::None: return "no recommendation";
    case Recommendation::Observe: return "observation only";
    case Recommendation::Validate: return "validate with more laps";
    case Recommendation::RecommendTechnique: return "recommend technique";
    case Recommendation::RecommendSequence: return "recommend complete sequence";
    }
    return "unknown";
}

std::string_view reason_name(Reason reason) noexcept {
    switch (reason) {
    case Reason::InvalidInput: return "invalid input";
    case Reason::LowDataConfidence: return "data confidence below gate";
    case Reason::IncompleteLap: return "lap is incomplete";
    case Reason::TelemetryGap: return "telemetry gap";
    case Reason::ContextChanged: return "setup or conditions changed";
    case Reason::LineMetricDisabled: return "line conclusion disabled";
    case Reason::BelowNoiseFloor: return "time effect is below the noise floor";
    case Reason::PriorPhaseLoss: return "time was lost before the apparent gain";
    case Reason::DownstreamPayback: return "more than half the local gain was paid back downstream";
    case Reason::TrackLimitViolation: return "track-limit violation";
    case Reason::ExtraSteeringCorrection: return "extra steering correction";
    case Reason::PossibleInstability: return "possible instability indicator";
    case Reason::AggregateEvidenceMissing: return "repeated-lap evidence is missing";
    case Reason::InsufficientComparableLaps: return "too few comparable laps";
    case Reason::InsufficientSupportingLaps: return "too few supporting laps";
    case Reason::LowSupportRate: return "support rate is too low";
    case Reason::ReliabilityIntervalCrossesNoise: return "reliability interval crosses the noise floor";
    }
    return "unknown";
}

}  // namespace racebox::insight_evidence
