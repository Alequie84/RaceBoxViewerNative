#include "racebox/driver_analysis.hpp"
#include "racebox/domain.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

using namespace racebox;
using namespace racebox::driver_analysis;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void close_to(double actual, double expected, double tolerance, const char* message) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
        throw std::runtime_error(message);
    }
}

struct LapShape {
    double north_shift_m{};
    double entry_outward_m{};
    double brake_begin_progress{};
    double turn_in_progress{};
    double first_throttle_progress{};
    double full_throttle_progress{};
    double minimum_speed_adjustment{};
    double exit_speed_adjustment{};
    bool steering_correction{};
    double time_scale{1.0};
};

Session synthetic_session() {
    constexpr std::size_t samples_per_lap = 251;
    constexpr Timestamp step_us = 40'000;  // 25 Hz: two inclusive samples satisfy an 80 ms dwell.
    constexpr double base_latitude = 49.184;
    constexpr double base_longitude = -123.145;
    const auto longitude_metres = 111'320.0 * std::cos(base_latitude * 3.14159265358979323846 / 180.0);

    Session session;
    session.alignment.compatible = true;
    session.alignment.trigger_sign = 1;
    session.alignment.steering_sign = 1;
    session.alignment.radio_anchor_us = 0;
    session.alignment.fine_correction_us = 0;

    const std::array<LapShape, 3> shapes{{
        {0.0, 0.0, 0.12, 0.17, 0.32, 0.40, 0.0, 0.0, false, 1.0},
        {1.8, 0.8, 0.145, 0.195, 0.37, 0.45, -4.0, -3.0, true, 1.0},
        {3.4, 0.0, 0.10, 0.15, 0.28, 0.36, 2.5, 3.0, false, 0.95},
    }};

    Timestamp next_start = 0;
    for (std::size_t lap_number = 0; lap_number < shapes.size(); ++lap_number) {
        const auto begin = session.telemetry.size();
        const auto& shape = shapes[lap_number];
        const auto lap_step_us = static_cast<Timestamp>(static_cast<double>(step_us) * shape.time_scale);
        for (std::size_t sample = 0; sample < samples_per_lap; ++sample) {
            const auto progress = static_cast<double>(sample) / static_cast<double>(samples_per_lap - 1);
            const auto time_us = next_start + static_cast<Timestamp>(sample) * lap_step_us;
            const auto east_m = 100.0 * progress;
            auto north_m = 8.0 * std::sin(4.0 * 3.14159265358979323846 * progress) + shape.north_shift_m;
            if (progress >= 0.18 && progress <= 0.28 && shape.entry_outward_m != 0.0) {
                const auto phase = (progress - 0.18) / 0.10;
                north_m += shape.entry_outward_m * std::sin(phase * 3.14159265358979323846);
            }

            auto speed = 58.0 - 23.0 * std::exp(-std::pow((progress - 0.28) / 0.075, 2.0));
            speed += shape.minimum_speed_adjustment * std::exp(-std::pow((progress - 0.28) / 0.055, 2.0));
            speed += shape.exit_speed_adjustment * std::exp(-std::pow((progress - 0.42) / 0.045, 2.0));

            float trigger = 0.0F;
            if (progress >= shape.brake_begin_progress && progress < 0.25) trigger = -45.0F;
            if (progress >= shape.first_throttle_progress) trigger = 45.0F;
            if (progress >= shape.full_throttle_progress) trigger = 100.0F;

            float steering = progress >= shape.turn_in_progress && progress < 0.40 ? 35.0F : 0.0F;
            if (shape.steering_correction && progress >= 0.33 && progress < 0.36) steering = -30.0F;

            session.telemetry.time_us.push_back(time_us);
            session.telemetry.absolute_time_us.push_back(1'700'000'000'000'000LL + time_us);
            session.telemetry.latitude.push_back(base_latitude + north_m / 110'540.0);
            session.telemetry.longitude.push_back(base_longitude + east_m / longitude_metres);
            session.telemetry.speed_kmh.push_back(static_cast<float>(speed));
            session.telemetry.heading_deg.push_back(0.0F);
            session.telemetry.altitude_m.push_back(2.0F);
            session.telemetry.longitudinal_g.push_back(0.0F);
            session.telemetry.lateral_g.push_back(0.0F);
            session.telemetry.vertical_g.push_back(1.0F);
            session.telemetry.gyro_x_dps.push_back(0.0F);
            session.telemetry.gyro_y_dps.push_back(0.0F);
            session.telemetry.gyro_z_dps.push_back(0.0F);
            session.telemetry.satellites.push_back(18);
            session.telemetry.raw_lap.push_back(static_cast<std::int32_t>(lap_number + 1));

            session.radio.elapsed_us.push_back(time_us);
            session.radio.steering_percent.push_back(steering);
            session.radio.trigger_percent.push_back(trigger);
            session.radio.voltage.push_back(7.4F);
        }
        const auto end = session.telemetry.size() - 1;
        session.laps.push_back({static_cast<std::int32_t>(lap_number + 1), static_cast<std::int32_t>(lap_number + 1),
                                begin, end, static_cast<Timestamp>(samples_per_lap - 1) * lap_step_us, LapPhase::Complete});
        next_start += static_cast<Timestamp>(samples_per_lap) * lap_step_us;
    }
    session.telemetry.validate();
    session.radio.validate();
    return session;
}

const DerivedMetric& metric(const CornerMetrics& corner, MetricKind kind) {
    const auto iterator = std::find_if(corner.metrics.begin(), corner.metrics.end(), [kind](const DerivedMetric& value) {
        return value.kind == kind;
    });
    if (iterator == corner.metrics.end()) throw std::runtime_error("Expected metric is missing");
    return *iterator;
}

}  // namespace

int main() {
    try {
        const auto rules = default_analysis_rules();
        const auto descriptors = rule_descriptors();
        require(descriptors.size() == 8, "Rules UI metadata is incomplete");
        require(descriptors.front().stable_id == "brake_point_delta", "Rule IDs are not stable");
        require(!descriptors.front().formula.empty() && !descriptors.front().unit.empty(), "Rule formula metadata is missing");
        close_to(find_rule(rules, RuleId::BrakePointDelta)->threshold, 0.08, 1e-12, "Brake threshold changed");
        close_to(find_rule(rules, RuleId::EntryLineDeviation)->threshold, 0.35, 1e-12, "Line threshold changed");
        close_to(find_rule(rules, RuleId::RelativeTimeChange)->threshold, 0.05, 1e-12,
                 "Corner time-change threshold changed");
        require(rules.brake_begin_dwell_us == 80'000, "25 Hz brake dwell changed");
        require(rules.full_throttle_dwell_us == 100'000, "Full-throttle dwell changed");

        require(confidence_band(39) == ConfidenceBand::Suppressed, "Sub-40 confidence was not suppressed");
        require(confidence_band(40) == ConfidenceBand::WeakSignal, "Weak confidence lower bound changed");
        require(confidence_band(60) == ConfidenceBand::Actionable, "Actionable confidence lower bound changed");
        require(classify_severity(1.0, 0.04) == Severity::Low, "Low severity rule changed");
        require(classify_severity(1.5, 0.05) == Severity::Medium, "Medium severity rule changed");
        require(classify_severity(2.5, 0.01) == Severity::High, "Threshold severity did not become high");
        require(classify_severity(1.0, 0.15) == Severity::High, "Time-loss severity did not become high");

        auto session = synthetic_session();
        const auto original_latitude = session.telemetry.latitude;
        const auto original_longitude = session.telemetry.longitude;
        const CornerZone corner{"corner-1", "Turn 1", 0.08, 0.16, 0.28, 0.43, 0.52};
        const auto result = analyze_driver_performance(
            session, session.laps[0], &session.laps[1], &session.laps[2], std::span<const CornerZone>(&corner, 1), rules);

        require(result.formula_version == kFormulaVersion, "Formula version was not disclosed");
        require(result.reference.raw_lap == 1 && result.reference.total_distance_m > 100.0, "Reference result is incomplete");
        require(result.comparisons[0].has_value() && result.comparisons[1].has_value(), "A/B comparisons were not both analyzed");
        require(session.telemetry.latitude == original_latitude && session.telemetry.longitude == original_longitude,
                "Analysis modified raw GPS telemetry");

        const auto& comparison_a = *result.comparisons[0];
        require(comparison_a.slot == ComparisonSlot::CompareA && comparison_a.raw_lap == 2, "Compare A identity changed");
        require(comparison_a.line_translation.preserves_raw_telemetry, "GPS correction did not preserve raw telemetry");
        require(comparison_a.line_translation.magnitude_m > 1.5 && comparison_a.line_translation.magnitude_m < 2.3,
                "Whole-lap translation did not recover the synthetic drift");
        require(comparison_a.confidence.line_metrics_enabled, "A recoverable drift incorrectly disabled line metrics");
        require(!comparison_a.events.empty(), "Comparison event detection returned nothing");
        require(std::all_of(comparison_a.events.begin(), comparison_a.events.end(), [](const DetectedEvent& event) {
            return event.progress >= 0.0 && event.progress <= 1.0 && event.timestamp_us >= 0;
        }), "Event cursor progress is outside 0..1");
        require(std::any_of(comparison_a.events.begin(), comparison_a.events.end(), [](const DetectedEvent& event) {
            return event.type == EventType::BrakeBegin;
        }), "Sustained brake event was not detected at 25 Hz");
        require(std::any_of(comparison_a.events.begin(), comparison_a.events.end(), [](const DetectedEvent& event) {
            return event.type == EventType::SteeringCorrection;
        }), "Post-apex steering correction was not detected");

        const auto& a_corner = comparison_a.corners.front();
        const auto& brake_delta = metric(a_corner, MetricKind::BrakePointDelta);
        require(brake_delta.value && *brake_delta.value > 0.15, "Brake-point timing delta is incorrect");
        require(brake_delta.triggered, "Brake threshold did not trigger");
        require(brake_delta.navigation.reference_raw_lap == 1 && brake_delta.navigation.reference_progress >= 0.0 &&
                    brake_delta.navigation.reference_progress <= 1.0 && brake_delta.navigation.reference_timestamp_us >= 0,
                "Insight navigation target is incomplete");
        const auto& speed_delta = metric(a_corner, MetricKind::MinimumSpeedDelta);
        require(speed_delta.value && *speed_delta.value < -3.0 && speed_delta.triggered && !speed_delta.positive,
                "Minimum-speed loss was not assessed");
        const auto& throttle_delta = metric(a_corner, MetricKind::ThrottlePickupDelta);
        require(throttle_delta.value && *throttle_delta.value > 0.30 && throttle_delta.triggered,
                "Throttle-pickup delay was not assessed");
        const auto& line_delta = metric(a_corner, MetricKind::EntryLineDeviation);
        require(line_delta.value.has_value(), "Analysis-only translation did not produce a line metric");

        const auto& comparison_b = *result.comparisons[1];
        require(comparison_b.slot == ComparisonSlot::CompareB && comparison_b.raw_lap == 3, "Compare B identity changed");
        require(comparison_b.line_translation.magnitude_m > 3.0, "Large synthetic GPS drift was not measured");
        require(!comparison_b.confidence.line_metrics_enabled, "Line conclusions were not disabled above 3 m correction");
        require(!metric(comparison_b.corners.front(), MetricKind::EntryLineDeviation).value,
                "A line conclusion survived the 3 m confidence gate");
        require(std::any_of(result.insights.begin(), result.insights.end(), [](const Insight& insight) {
            return insight.comparison == ComparisonSlot::CompareB && insight.positive &&
                   (insight.metric == MetricKind::ExitSpeedDelta || insight.metric == MetricKind::ThrottlePickupDelta);
        }), "Positive feedback was not generated from verified metrics");
        require(std::any_of(result.insights.begin(), result.insights.end(), [](const Insight& insight) {
            return insight.comparison == ComparisonSlot::CompareB && insight.positive &&
                   insight.metric == MetricKind::RelativeTimeChange && insight.rule == RuleId::RelativeTimeChange;
        }), "Reduced corner time did not generate deterministic positive feedback");
        require(std::all_of(result.insights.begin(), result.insights.end(), [](const Insight& insight) {
            return insight.confidence >= 40 && insight.navigation.reference_progress >= 0.0 &&
                   insight.navigation.reference_progress <= 1.0 && !insight.rule_stable_id.empty() &&
                   insight.time_noise_floor_s >= 0.05;
        }), "Suppressed or non-navigable insight escaped the evidence gate");
        require(std::none_of(result.insights.begin(), result.insights.end(), [](const Insight& insight) {
            return insight.recommendation == insight_evidence::Recommendation::RecommendTechnique ||
                   insight.recommendation == insight_evidence::Recommendation::RecommendSequence;
        }), "Two comparison laps were incorrectly treated as repeatable recommendation evidence");
        require(std::all_of(result.insights.begin(), result.insights.end(), [](const Insight& insight) {
            return insight.outcome != insight_evidence::Outcome::Compensation ||
                   insight.recommendation == insight_evidence::Recommendation::None;
        }), "A recovery/compensation card produced a driving recommendation");
        require(std::all_of(result.insights.begin(), result.insights.end(), [](const Insight& insight) {
            return insight.detail.find("compared with the reference lap") != std::string::npos &&
                   insight.title.find("delta") == std::string::npos;
        }), "Insight cards did not use the plain-English driver wording");
        require(std::any_of(result.insights.begin(), result.insights.end(), [](const Insight& insight) {
            return insight.metric == MetricKind::BrakePointDelta && insight.detail.find("Braked") != std::string::npos &&
                   insight.detail.find("(brake point)") != std::string::npos;
        }), "Brake insight did not pair plain English with the motorsport term");

        auto disabled_brake_rule = rules;
        for (auto& setting : disabled_brake_rule.metrics) {
            if (setting.id == RuleId::BrakePointDelta) setting.enabled = false;
        }
        const auto disabled_result = analyze_driver_performance(
            session, session.laps[0], &session.laps[1], nullptr, std::span<const CornerZone>(&corner, 1), disabled_brake_rule);
        const auto& disabled_metric = metric(disabled_result.comparisons[0]->corners.front(), MetricKind::BrakePointDelta);
        require(!disabled_metric.rule_enabled && !disabled_metric.triggered, "Disabled rule still produced a metric trigger");
        require(std::none_of(disabled_result.insights.begin(), disabled_result.insights.end(), [](const Insight& insight) {
            return insight.rule == RuleId::BrakePointDelta;
        }), "Disabled rule still produced an insight");

        auto impossible_brake_rule = rules;
        impossible_brake_rule.brake_begin_percent = 101.0F;
        const auto no_brake_result = analyze_driver_performance(
            session, session.laps[0], nullptr, nullptr, std::span<const CornerZone>(&corner, 1), impossible_brake_rule);
        require(std::none_of(no_brake_result.reference.events.begin(), no_brake_result.reference.events.end(), [](const DetectedEvent& event) {
            return event.type == EventType::BrakeBegin || event.type == EventType::BrakeRelease;
        }), "A brake release was invented when no braking event existed");

        const auto suggestions = suggest_corner_zones(session, session.laps[0], 6);
        require(!suggestions.empty() && suggestions.size() <= 6, "Automatic corner suggestion failed");
        require(std::all_of(suggestions.begin(), suggestions.end(), [](const CornerZone& suggested) {
            return !suggested.id.empty() && suggested.start_progress < suggested.turn_in_progress &&
                   suggested.turn_in_progress <= suggested.apex_progress && suggested.apex_progress <= suggested.exit_progress &&
                   suggested.exit_progress < suggested.end_progress && suggested.end_progress <= 1.0;
        }), "Suggested corner boundaries are invalid");
        for (std::size_t index = 0; index < suggestions.size(); ++index) {
            require(suggestions[index].id == "corner-" + std::to_string(index + 1),
                    "Automatic corner identifiers are not sequential");
            require(suggestions[index].name == "Turn " + std::to_string(index + 1),
                    "Automatic corner names are not sequential");
        }

        const std::filesystem::path golden = GOLDEN_DIR;
        const auto golden_session = load_session({golden / L"session.vbo", golden / L"session.csv", golden / L"sanwa.csv"}).session;
        const auto raw_17 = std::find_if(golden_session.laps.begin(), golden_session.laps.end(), [](const LapInfo& lap) {
            return lap.raw_lap == 17;
        });
        const auto raw_7 = std::find_if(golden_session.laps.begin(), golden_session.laps.end(), [](const LapInfo& lap) {
            return lap.raw_lap == 7;
        });
        require(raw_17 != golden_session.laps.end() && raw_7 != golden_session.laps.end(), "Golden drift-check laps are missing");
        const auto golden_corners = suggest_corner_zones(golden_session, *raw_17);
        require(!golden_corners.empty(), "Golden reference produced no corner suggestions");
        std::cout << "Golden automatic corner count: " << golden_corners.size() << '\n';
        require(golden_corners.size() == 9, "Golden track no longer resolves to its nine significant turns");
        const auto drift_result = analyze_driver_performance(
            golden_session, *raw_17, &*raw_7, nullptr, golden_corners, default_analysis_rules());
        require(drift_result.comparisons[0].has_value(), "Golden offset lap was not analyzed");
        const auto& drift_comparison = *drift_result.comparisons[0];
        std::cout << "Golden raw lap 7 analysis correction: " << drift_comparison.line_translation.magnitude_m
                  << " m, residual " << drift_comparison.line_translation.residual_rms_m << " m\n";
        const auto wide_entry_insights = static_cast<std::size_t>(std::count_if(
            drift_result.insights.begin(), drift_result.insights.end(), [](const Insight& insight) {
                return insight.metric == MetricKind::EntryLineDeviation;
            }));
        std::cout << "Golden raw lap 7 entry-line insights after correction: " << wide_entry_insights << '\n';
        require(drift_comparison.line_translation.magnitude_m > 1.0 && drift_comparison.line_translation.magnitude_m < 2.8,
                "Golden offset lap correction is outside its known drift range");
        require(drift_comparison.confidence.line_metrics_enabled, "Golden offset lap incorrectly disabled line analysis");
        const auto raw_latitude = golden_session.telemetry.latitude[raw_7->begin_index];
        const auto raw_longitude = golden_session.telemetry.longitude[raw_7->begin_index];
        const auto display_coordinate = apply_line_translation(raw_latitude, raw_longitude,
                                                               drift_comparison.line_translation);
        const auto east_m = (display_coordinate.longitude - raw_longitude) * 111'320.0 *
            std::cos(raw_latitude * 3.14159265358979323846 / 180.0);
        const auto north_m = (display_coordinate.latitude - raw_latitude) * 110'540.0;
        close_to(east_m, drift_comparison.line_translation.east_m, 0.001,
                 "Visible trace east correction changed");
        close_to(north_m, drift_comparison.line_translation.north_m, 0.001,
                 "Visible trace north correction changed");
        close_to(golden_session.telemetry.latitude[raw_7->begin_index], raw_latitude, 1e-12,
                 "Visible correction rewrote raw latitude");
        close_to(golden_session.telemetry.longitude[raw_7->begin_index], raw_longitude, 1e-12,
                 "Visible correction rewrote raw longitude");
        require(wide_entry_insights == 0, "Golden offset lap produced a false wide-entry insight after correction");
        require(std::all_of(drift_result.insights.begin(), drift_result.insights.end(), [](const Insight& insight) {
            return insight.time_noise_floor_s > 0.0 &&
                   (insight.recommendation != insight_evidence::Recommendation::RecommendTechnique &&
                    insight.recommendation != insight_evidence::Recommendation::RecommendSequence ||
                    insight.reliability == insight_evidence::Reliability::ReliableAssociation);
        }), "Golden session insight bypassed the integrated evidence gates");

        std::cout << "All deterministic driver-analysis checks passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Driver-analysis test failure: " << exception.what() << '\n';
        return 1;
    }
}
