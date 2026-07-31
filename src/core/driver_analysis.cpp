#include "racebox/driver_analysis.hpp"

#include "racebox/domain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <utility>

namespace racebox::driver_analysis {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusM = 6'371'000.0;
constexpr double kMicrosecondsPerSecond = 1'000'000.0;

constexpr std::array<RuleDescriptor, 8> kRuleDescriptors{{
    {RuleId::BrakePointDelta, "brake_point_delta", "Brake-point delta",
     "comparison brake-begin elapsed time - reference brake-begin elapsed time", "s", 0.08},
    {RuleId::TurnInDelta, "turn_in_delta", "Turn-in delta",
     "comparison turn-in elapsed time - reference turn-in elapsed time", "s", 0.08},
    {RuleId::ApexTimingDelta, "apex_timing_delta", "Apex timing delta",
     "comparison apex elapsed time - reference apex elapsed time", "s", 0.08},
    {RuleId::MinimumSpeedLoss, "minimum_speed_loss", "Minimum corner-speed difference",
     "comparison minimum speed - reference minimum speed", "km/h", 1.0},
    {RuleId::ExitSpeedDelta, "exit_speed_delta", "Exit-speed difference",
     "comparison speed at the configured exit - reference speed at the configured exit", "km/h", 1.0},
    {RuleId::ThrottlePickupDelay, "throttle_pickup_delay", "Throttle-pickup delay",
     "comparison first-throttle elapsed time - reference first-throttle elapsed time", "s", 0.08},
    {RuleId::EntryLineDeviation, "entry_line_deviation", "Entry-line outward deviation",
     "maximum outward lateral separation during entry after one disclosed whole-lap east/north translation", "m", 0.35},
    {RuleId::RelativeTimeChange, "relative_time_change", "Corner-zone time change",
     "comparison corner-zone elapsed time - reference corner-zone elapsed time", "s", 0.05},
}};

struct PointM {
    double east{};
    double north{};
};

struct GeoFrame {
    double latitude{};
    double longitude{};
    double longitude_metres_per_degree{};
};

struct LapView {
    const Session* session{};
    const LapInfo* lap{};
    std::vector<double> distance_m;
    std::vector<RadioSample> controls;
    double total_distance_m{};
    Timestamp median_step_us{40'000};
};

struct CornerEvents {
    std::vector<DetectedEvent> events;
};

struct MinimumValue {
    double value{};
    std::size_t index{};
};

double to_radians(double degrees) { return degrees * kPi / 180.0; }

double haversine_m(double lat_a, double lon_a, double lat_b, double lon_b) {
    const auto d_lat = to_radians(lat_b - lat_a);
    const auto d_lon = to_radians(lon_b - lon_a);
    const auto a = std::sin(d_lat / 2.0) * std::sin(d_lat / 2.0) +
                   std::cos(to_radians(lat_a)) * std::cos(to_radians(lat_b)) *
                       std::sin(d_lon / 2.0) * std::sin(d_lon / 2.0);
    return 2.0 * kEarthRadiusM * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
}

double median(std::vector<double> values) {
    values.erase(std::remove_if(values.begin(), values.end(), [](double value) { return !std::isfinite(value); }), values.end());
    if (values.empty()) return 0.0;
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

Timestamp median_step(const Session& session, const LapInfo& lap) {
    std::vector<double> steps;
    steps.reserve(lap.end_index - lap.begin_index);
    for (auto index = lap.begin_index + 1; index <= lap.end_index; ++index) {
        const auto delta = session.telemetry.time_us[index] - session.telemetry.time_us[index - 1];
        if (delta > 0 && delta <= 1'000'000) steps.push_back(static_cast<double>(delta));
    }
    const auto result = static_cast<Timestamp>(std::llround(median(std::move(steps))));
    return result > 0 ? result : 40'000;
}

std::optional<LapView> make_lap_view(const Session& session, const LapInfo& lap) {
    const auto count = session.telemetry.size();
    if (count < 2 || lap.begin_index >= count || lap.end_index >= count || lap.begin_index >= lap.end_index) return std::nullopt;

    LapView view;
    view.session = &session;
    view.lap = &lap;
    view.distance_m.resize(lap.end_index - lap.begin_index + 1);
    view.controls.reserve(view.distance_m.size());
    for (auto index = lap.begin_index; index <= lap.end_index; ++index) {
        if (index > lap.begin_index) {
            view.distance_m[index - lap.begin_index] = view.distance_m[index - lap.begin_index - 1] +
                haversine_m(session.telemetry.latitude[index - 1], session.telemetry.longitude[index - 1],
                            session.telemetry.latitude[index], session.telemetry.longitude[index]);
        }
        view.controls.push_back(sample_radio(session, session.telemetry.time_us[index]));
    }
    view.total_distance_m = view.distance_m.back();
    view.median_step_us = median_step(session, lap);
    if (!std::isfinite(view.total_distance_m) || view.total_distance_m <= 0.25) return std::nullopt;
    return view;
}

GeoFrame make_frame(const Session& session, const LapInfo& reference) {
    const auto latitude = session.telemetry.latitude[reference.begin_index];
    return {latitude, session.telemetry.longitude[reference.begin_index], 111'320.0 * std::cos(to_radians(latitude))};
}

PointM point_at_index(const LapView& view, const GeoFrame& frame, std::size_t absolute_index) {
    const auto& telemetry = view.session->telemetry;
    return {
        (telemetry.longitude[absolute_index] - frame.longitude) * frame.longitude_metres_per_degree,
        (telemetry.latitude[absolute_index] - frame.latitude) * 110'540.0,
    };
}

std::size_t local_index_at_progress(const LapView& view, double progress) {
    const auto target = std::clamp(progress, 0.0, 1.0) * view.total_distance_m;
    const auto iterator = std::lower_bound(view.distance_m.begin(), view.distance_m.end(), target);
    if (iterator == view.distance_m.begin()) return 0;
    if (iterator == view.distance_m.end()) return view.distance_m.size() - 1;
    const auto upper = static_cast<std::size_t>(iterator - view.distance_m.begin());
    return target - view.distance_m[upper - 1] <= view.distance_m[upper] - target ? upper - 1 : upper;
}

std::size_t absolute_index_at_progress(const LapView& view, double progress) {
    return view.lap->begin_index + local_index_at_progress(view, progress);
}

double progress_at_index(const LapView& view, std::size_t absolute_index) {
    const auto local = absolute_index - view.lap->begin_index;
    return view.total_distance_m > 0.0 ? std::clamp(view.distance_m[local] / view.total_distance_m, 0.0, 1.0) : 0.0;
}

PointM point_at_progress(const LapView& view, const GeoFrame& frame, double progress) {
    const auto target = std::clamp(progress, 0.0, 1.0) * view.total_distance_m;
    const auto iterator = std::lower_bound(view.distance_m.begin(), view.distance_m.end(), target);
    if (iterator == view.distance_m.begin()) return point_at_index(view, frame, view.lap->begin_index);
    if (iterator == view.distance_m.end()) return point_at_index(view, frame, view.lap->end_index);
    const auto upper = static_cast<std::size_t>(iterator - view.distance_m.begin());
    const auto lower = upper - 1;
    const auto span = view.distance_m[upper] - view.distance_m[lower];
    const auto ratio = span > 0.0 ? (target - view.distance_m[lower]) / span : 0.0;
    const auto a = point_at_index(view, frame, view.lap->begin_index + lower);
    const auto b = point_at_index(view, frame, view.lap->begin_index + upper);
    return {a.east + (b.east - a.east) * ratio, a.north + (b.north - a.north) * ratio};
}

Timestamp timestamp_at_progress(const LapView& view, double progress) {
    const auto target = std::clamp(progress, 0.0, 1.0) * view.total_distance_m;
    const auto iterator = std::lower_bound(view.distance_m.begin(), view.distance_m.end(), target);
    const auto& time = view.session->telemetry.time_us;
    if (iterator == view.distance_m.begin()) return time[view.lap->begin_index];
    if (iterator == view.distance_m.end()) return time[view.lap->end_index];
    const auto upper = static_cast<std::size_t>(iterator - view.distance_m.begin());
    const auto lower = upper - 1;
    const auto span = view.distance_m[upper] - view.distance_m[lower];
    const auto ratio = span > 0.0 ? (target - view.distance_m[lower]) / span : 0.0;
    const auto lower_time = time[view.lap->begin_index + lower];
    const auto upper_time = time[view.lap->begin_index + upper];
    return lower_time + static_cast<Timestamp>(std::llround(static_cast<double>(upper_time - lower_time) * ratio));
}

NavigationTarget navigation_at_progress(const LapView& reference, double progress) {
    const auto index = absolute_index_at_progress(reference, progress);
    const auto timestamp = timestamp_at_progress(reference, progress);
    return {
        reference.lap->raw_lap,
        index,
        timestamp,
        static_cast<double>(timestamp - reference.session->telemetry.time_us[reference.lap->begin_index]) / kMicrosecondsPerSecond,
        std::clamp(progress, 0.0, 1.0) * reference.total_distance_m,
        std::clamp(progress, 0.0, 1.0),
    };
}

NavigationTarget navigation_for_event(const LapView& reference, const DetectedEvent& event) {
    return {
        reference.lap->raw_lap,
        event.sample_index,
        event.timestamp_us,
        event.elapsed_s,
        event.distance_m,
        event.progress,
    };
}

double curvature_at(const LapView& view, const GeoFrame& frame, std::size_t absolute_index) {
    if (absolute_index <= view.lap->begin_index || absolute_index >= view.lap->end_index) return 0.0;
    const auto local = absolute_index - view.lap->begin_index;
    const auto radius = std::min<std::size_t>({3, local, view.distance_m.size() - 1 - local});
    if (radius == 0) return 0.0;
    const auto before_index = absolute_index - radius;
    const auto after_index = absolute_index + radius;
    const auto before = point_at_index(view, frame, before_index);
    const auto center = point_at_index(view, frame, absolute_index);
    const auto after = point_at_index(view, frame, after_index);
    const auto ax = center.east - before.east;
    const auto ay = center.north - before.north;
    const auto bx = after.east - center.east;
    const auto by = after.north - center.north;
    const auto length_a = std::hypot(ax, ay);
    const auto length_b = std::hypot(bx, by);
    if (length_a < 0.02 || length_b < 0.02) return 0.0;
    const auto cross = ax * by - ay * bx;
    const auto dot = ax * bx + ay * by;
    const auto angle = std::atan2(cross, dot);
    return angle / ((length_a + length_b) * 0.5);
}

template <typename Predicate>
std::optional<std::size_t> first_sustained(
    const LapView& view, std::size_t begin_index, std::size_t end_index, Timestamp dwell_us, Predicate predicate) {
    if (begin_index > end_index) return std::nullopt;
    const auto& time = view.session->telemetry.time_us;
    std::optional<std::size_t> run_start;
    for (auto index = begin_index; index <= end_index; ++index) {
        if (!predicate(index)) {
            run_start.reset();
            continue;
        }
        if (!run_start) run_start = index;
        // Inclusive sample duration makes 80 ms equal two samples at 25 Hz.
        const auto inclusive_duration = time[index] - time[*run_start] + view.median_step_us;
        if (inclusive_duration >= dwell_us) return run_start;
    }
    return std::nullopt;
}

DetectedEvent make_event(
    const LapView& view, EventType type, const CornerZone& corner, std::size_t absolute_index, float observed_value) {
    const auto local = absolute_index - view.lap->begin_index;
    const auto timestamp = view.session->telemetry.time_us[absolute_index];
    return {
        type,
        corner.id,
        view.lap->raw_lap,
        absolute_index,
        timestamp,
        static_cast<double>(timestamp - view.session->telemetry.time_us[view.lap->begin_index]) / kMicrosecondsPerSecond,
        view.distance_m[local],
        progress_at_index(view, absolute_index),
        observed_value,
    };
}

const DetectedEvent* event_of_type(const CornerEvents& events, EventType type) {
    const auto iterator = std::find_if(events.events.begin(), events.events.end(), [type](const DetectedEvent& event) {
        return event.type == type;
    });
    return iterator == events.events.end() ? nullptr : &*iterator;
}

CornerEvents detect_corner_events(
    const LapView& view,
    const GeoFrame& frame,
    const CornerZone& corner,
    const PointM& reference_apex,
    const LineTranslation& translation,
    const AnalysisRules& rules) {
    CornerEvents result;
    const auto start = absolute_index_at_progress(view, corner.start_progress);
    const auto configured_turn_in = absolute_index_at_progress(view, corner.turn_in_progress);
    const auto configured_apex = absolute_index_at_progress(view, corner.apex_progress);
    const auto configured_exit = absolute_index_at_progress(view, corner.exit_progress);
    const auto end = absolute_index_at_progress(view, corner.end_progress);

    const auto local_control = [&](std::size_t index) -> const RadioSample& {
        return view.controls[index - view.lap->begin_index];
    };

    const auto brake_begin = first_sustained(view, start, std::max(start, configured_turn_in), rules.brake_begin_dwell_us,
        [&](std::size_t index) {
            const auto& sample = local_control(index);
            return sample.valid && sample.brake >= rules.brake_begin_percent;
        });
    if (brake_begin) {
        result.events.push_back(make_event(view, EventType::BrakeBegin, corner, *brake_begin, local_control(*brake_begin).brake));
    }

    std::optional<std::size_t> brake_release;
    if (brake_begin) {
        brake_release = first_sustained(view, *brake_begin, std::max(*brake_begin, configured_exit),
            rules.brake_release_dwell_us, [&](std::size_t index) {
                const auto& sample = local_control(index);
                return sample.valid && sample.brake < rules.brake_release_percent;
            });
    }
    if (brake_release) {
        result.events.push_back(make_event(view, EventType::BrakeRelease, corner, *brake_release, local_control(*brake_release).brake));
    }

    const auto turn_in = first_sustained(view, start, std::max(start, configured_apex), rules.turn_in_dwell_us,
        [&](std::size_t index) {
            const auto& sample = local_control(index);
            return sample.valid && std::abs(sample.steering) >= rules.turn_in_steering_percent &&
                   std::abs(curvature_at(view, frame, index)) >= rules.turn_in_curvature_per_m;
        });
    if (turn_in) {
        result.events.push_back(make_event(view, EventType::TurnIn, corner, *turn_in, local_control(*turn_in).steering));
    }

    auto apex_index = configured_apex;
    auto best_apex_distance = std::numeric_limits<double>::infinity();
    for (auto index = start; index <= end; ++index) {
        auto point = point_at_index(view, frame, index);
        point.east += translation.east_m;
        point.north += translation.north_m;
        const auto distance = std::hypot(point.east - reference_apex.east, point.north - reference_apex.north);
        if (distance < best_apex_distance) {
            best_apex_distance = distance;
            apex_index = index;
        }
    }
    result.events.push_back(make_event(view, EventType::Apex, corner, apex_index,
                                       static_cast<float>(best_apex_distance)));

    const auto throttle_start = std::max(apex_index, brake_release.value_or(apex_index));
    const auto first_throttle = first_sustained(view, throttle_start, end, rules.first_throttle_dwell_us,
        [&](std::size_t index) {
            const auto& sample = local_control(index);
            return sample.valid && sample.throttle >= rules.first_throttle_percent;
        });
    if (first_throttle) {
        result.events.push_back(make_event(view, EventType::FirstThrottle, corner, *first_throttle, local_control(*first_throttle).throttle));
    }

    const auto full_start = first_throttle.value_or(throttle_start);
    const auto full_throttle = first_sustained(view, full_start, end, rules.full_throttle_dwell_us,
        [&](std::size_t index) {
            const auto& sample = local_control(index);
            return sample.valid && sample.throttle >= rules.full_throttle_percent;
        });
    if (full_throttle) {
        result.events.push_back(make_event(view, EventType::FullThrottle, corner, *full_throttle, local_control(*full_throttle).throttle));
    }

    int initial_sign = 0;
    float peak_magnitude = 0.0F;
    float trough_after_peak = std::numeric_limits<float>::infinity();
    bool steering_unwound = false;
    for (auto index = apex_index; index <= end; ++index) {
        const auto& sample = local_control(index);
        if (!sample.valid) continue;
        const auto magnitude = std::abs(sample.steering);
        const auto sign = sample.steering > 0.0F ? 1 : (sample.steering < 0.0F ? -1 : 0);
        if (initial_sign == 0) {
            if (magnitude < rules.steering_correction_percent) continue;
            initial_sign = sign;
            peak_magnitude = magnitude;
            continue;
        }
        if (sign != 0 && sign != initial_sign && magnitude >= rules.steering_correction_percent) {
            result.events.push_back(make_event(view, EventType::SteeringCorrection, corner, index, sample.steering));
            break;
        }
        if (sign == initial_sign) peak_magnitude = std::max(peak_magnitude, magnitude);
        if (peak_magnitude - magnitude >= rules.steering_correction_percent) {
            steering_unwound = true;
            trough_after_peak = std::min(trough_after_peak, magnitude);
        }
        if (steering_unwound && sign == initial_sign && magnitude - trough_after_peak >= rules.steering_correction_percent) {
            result.events.push_back(make_event(view, EventType::SteeringCorrection, corner, index, sample.steering));
            break;
        }
    }

    std::sort(result.events.begin(), result.events.end(), [](const DetectedEvent& left, const DetectedEvent& right) {
        return left.sample_index < right.sample_index;
    });
    return result;
}

LineTranslation calculate_translation(const LapView& reference, const LapView& comparison, const GeoFrame& frame) {
    std::vector<double> east_offsets;
    std::vector<double> north_offsets;
    east_offsets.reserve(97);
    north_offsets.reserve(97);
    for (int point = 2; point <= 98; ++point) {
        const auto progress = static_cast<double>(point) / 100.0;
        const auto reference_point = point_at_progress(reference, frame, progress);
        const auto comparison_point = point_at_progress(comparison, frame, progress);
        east_offsets.push_back(reference_point.east - comparison_point.east);
        north_offsets.push_back(reference_point.north - comparison_point.north);
    }
    LineTranslation result;
    result.east_m = median(east_offsets);
    result.north_m = median(north_offsets);
    result.magnitude_m = std::hypot(result.east_m, result.north_m);
    double squared = 0.0;
    for (std::size_t index = 0; index < east_offsets.size(); ++index) {
        const auto residual_east = east_offsets[index] - result.east_m;
        const auto residual_north = north_offsets[index] - result.north_m;
        squared += residual_east * residual_east + residual_north * residual_north;
    }
    result.residual_rms_m = east_offsets.empty() ? 0.0 : std::sqrt(squared / static_cast<double>(east_offsets.size()));
    return result;
}

int satellite_score(const LapView& view) {
    const auto& satellites = view.session->telemetry.satellites;
    const auto begin = satellites.begin() + static_cast<std::ptrdiff_t>(view.lap->begin_index);
    const auto end = satellites.begin() + static_cast<std::ptrdiff_t>(view.lap->end_index + 1);
    const auto average = std::accumulate(begin, end, 0.0) / static_cast<double>(end - begin);
    if (average >= 16.0) return 100;
    if (average >= 12.0) return 90;
    if (average >= 8.0) return 70;
    if (average >= 5.0) return 45;
    return 20;
}

int sampling_score(const LapView& view) {
    if (view.median_step_us <= 40'000) return 100;
    if (view.median_step_us <= 80'000) return 90;
    if (view.median_step_us <= 120'000) return 75;
    if (view.median_step_us <= 200'000) return 50;
    return 25;
}

int repeatability_score(double residual_rms_m) {
    if (residual_rms_m <= 0.25) return 100;
    if (residual_rms_m <= 0.50) return 90;
    if (residual_rms_m <= 0.75) return 70;
    if (residual_rms_m <= 1.00) return 55;
    if (residual_rms_m <= 1.50) return 35;
    if (residual_rms_m <= 3.00) return 20;
    return 10;
}

int complete_lap_repeatability_score(const Session& session, const LapView& reference, const GeoFrame& frame) {
    std::vector<double> residuals;
    residuals.reserve(session.laps.size());
    for (const auto& lap : session.laps) {
        if (lap.phase != LapPhase::Complete ||
            (lap.raw_lap == reference.lap->raw_lap && lap.begin_index == reference.lap->begin_index)) {
            continue;
        }
        const auto view = make_lap_view(session, lap);
        if (!view) continue;
        residuals.push_back(calculate_translation(reference, *view, frame).residual_rms_m);
    }
    return residuals.empty() ? 100 : repeatability_score(median(std::move(residuals)));
}

int translation_score(double correction_m) {
    if (correction_m <= 0.75) return 100;
    if (correction_m <= 1.5) return static_cast<int>(std::lround(100.0 - (correction_m - 0.75) / 0.75 * 20.0));
    if (correction_m <= 3.0) return static_cast<int>(std::lround(80.0 - (correction_m - 1.5) / 1.5 * 50.0));
    return 10;
}

ConfidenceBreakdown calculate_confidence(
    const LapView& comparison,
    const LineTranslation& translation,
    int complete_lap_repeatability_score,
    std::size_t detected_events,
    std::size_t expected_events) {
    ConfidenceBreakdown result;
    result.satellite_score = satellite_score(comparison);
    result.sampling_score = sampling_score(comparison);
    result.repeatability_score = std::min(repeatability_score(translation.residual_rms_m), complete_lap_repeatability_score);
    result.event_clarity_score = expected_events > 0
        ? static_cast<int>(std::lround(std::min(1.0, static_cast<double>(detected_events) / static_cast<double>(expected_events)) * 100.0))
        : 100;
    result.translation_score = translation_score(translation.magnitude_m);
    result.overall = static_cast<int>(std::lround(
        result.satellite_score * 0.20 + result.sampling_score * 0.20 + result.repeatability_score * 0.20 +
        result.event_clarity_score * 0.15 + result.translation_score * 0.25));
    result.overall = std::clamp(result.overall, 0, 100);
    result.band = confidence_band(result.overall);
    result.line_metrics_enabled = translation.magnitude_m <= 3.0;
    return result;
}

MinimumValue minimum_speed(const LapView& view, double start_progress, double end_progress) {
    const auto start = absolute_index_at_progress(view, start_progress);
    const auto end = absolute_index_at_progress(view, end_progress);
    MinimumValue result{std::numeric_limits<double>::infinity(), start};
    for (auto index = start; index <= end; ++index) {
        const auto speed = static_cast<double>(view.session->telemetry.speed_kmh[index]);
        if (speed < result.value) result = {speed, index};
    }
    return result;
}

double speed_at_progress(const LapView& view, double progress) {
    return static_cast<double>(view.session->telemetry.speed_kmh[absolute_index_at_progress(view, progress)]);
}

struct LineDeviation {
    double outward_m{};
    double progress{};
};

LineDeviation entry_line_deviation(
    const LapView& reference,
    const LapView& comparison,
    const GeoFrame& frame,
    const CornerZone& corner,
    const LineTranslation& translation) {
    LineDeviation result{0.0, corner.apex_progress};
    const auto before = point_at_progress(reference, frame, std::max(corner.start_progress, corner.apex_progress - 0.01));
    const auto at = point_at_progress(reference, frame, corner.apex_progress);
    const auto after = point_at_progress(reference, frame, std::min(corner.end_progress, corner.apex_progress + 0.01));
    const auto cross = (at.east - before.east) * (after.north - at.north) -
                       (at.north - before.north) * (after.east - at.east);
    const auto turn_sign = cross > 1e-5 ? 1.0 : (cross < -1e-5 ? -1.0 : 0.0);

    for (int sample = 0; sample <= 24; ++sample) {
        const auto ratio = static_cast<double>(sample) / 24.0;
        const auto progress = corner.turn_in_progress + (corner.apex_progress - corner.turn_in_progress) * ratio;
        const auto reference_point = point_at_progress(reference, frame, progress);
        auto comparison_point = point_at_progress(comparison, frame, progress);
        comparison_point.east += translation.east_m;
        comparison_point.north += translation.north_m;
        const auto tangent_before = point_at_progress(reference, frame, std::max(0.0, progress - 0.002));
        const auto tangent_after = point_at_progress(reference, frame, std::min(1.0, progress + 0.002));
        const auto tangent_x = tangent_after.east - tangent_before.east;
        const auto tangent_y = tangent_after.north - tangent_before.north;
        const auto tangent_length = std::hypot(tangent_x, tangent_y);
        if (tangent_length < 0.01) continue;
        const auto left_x = -tangent_y / tangent_length;
        const auto left_y = tangent_x / tangent_length;
        const auto lateral = (comparison_point.east - reference_point.east) * left_x +
                             (comparison_point.north - reference_point.north) * left_y;
        const auto outward = turn_sign == 0.0 ? std::abs(lateral) : -turn_sign * lateral;
        if (outward > result.outward_m) result = {outward, progress};
    }
    return result;
}

bool valid_corner(const CornerZone& corner) {
    return !corner.id.empty() && corner.start_progress >= 0.0 && corner.end_progress <= 1.0 &&
           corner.start_progress < corner.turn_in_progress && corner.turn_in_progress <= corner.apex_progress &&
           corner.apex_progress <= corner.exit_progress && corner.exit_progress < corner.end_progress;
}

const RuleDescriptor* descriptor_for(RuleId id) {
    const auto iterator = std::find_if(kRuleDescriptors.begin(), kRuleDescriptors.end(), [id](const RuleDescriptor& descriptor) {
        return descriptor.id == id;
    });
    return iterator == kRuleDescriptors.end() ? nullptr : &*iterator;
}

std::string formatted_value(double value, std::string_view unit) {
    std::ostringstream stream;
    stream << std::showpos << std::fixed << std::setprecision(unit == "m" ? 2 : 3) << value << ' ' << unit;
    return stream.str();
}

std::string plain_metric_change(MetricKind kind, double value) {
    const auto magnitude = std::abs(value);
    switch (kind) {
    case MetricKind::BrakePointDelta:
        return std::format("Braked {:.3f} s {} (brake point)", magnitude, value >= 0.0 ? "later" : "earlier");
    case MetricKind::TurnInDelta:
        return std::format("Started steering {:.3f} s {} (turn-in)", magnitude, value >= 0.0 ? "later" : "earlier");
    case MetricKind::ApexTimingDelta:
        return std::format("Reached the middle of the corner {:.3f} s {} (apex)", magnitude,
            value >= 0.0 ? "later" : "earlier");
    case MetricKind::MinimumSpeedDelta:
        return std::format("Slowest corner speed was {:.1f} km/h {}", magnitude, value >= 0.0 ? "higher" : "lower");
    case MetricKind::ExitSpeedDelta:
        return std::format("Exited the corner {:.1f} km/h {}", magnitude, value >= 0.0 ? "faster" : "slower");
    case MetricKind::ThrottlePickupDelta:
        return std::format("Got back on throttle {:.3f} s {} (throttle pickup)", magnitude,
            value >= 0.0 ? "later" : "sooner");
    case MetricKind::EntryLineDeviation:
        return std::format("Entered the corner {:.2f} m {} (entry line)", magnitude,
            value >= 0.0 ? "farther outside" : "farther inside");
    case MetricKind::RelativeTimeChange:
        return std::format("Took {:.3f} s {} through the corner", magnitude, value >= 0.0 ? "longer" : "less");
    }
    return "Driving input changed";
}

std::optional<Insight> make_insight(
    const DerivedMetric& metric,
    const CornerZone& corner,
    ComparisonSlot slot,
    std::int32_t raw_lap,
    double time_effect_s) {
    if (!metric.triggered || !metric.value || !metric.rule_id || metric.confidence < 40) return std::nullopt;
    const auto* descriptor = descriptor_for(*metric.rule_id);
    if (!descriptor) return std::nullopt;
    const auto slot_id = slot == ComparisonSlot::CompareA ? "a" : "b";
    Insight insight;
    insight.id = std::string(slot_id) + "-lap-" + std::to_string(raw_lap) + "-" + corner.id + "-" +
                 std::string(descriptor->stable_id);
    insight.title = plain_metric_change(metric.kind, *metric.value);
    insight.detail = corner.name + ": " + descriptor->name.data() + " " + formatted_value(*metric.value, descriptor->unit) +
                     " vs reference (threshold " + formatted_value(metric.threshold, descriptor->unit) + ")";
    insight.corner_id = corner.id;
    insight.corner_name = corner.name;
    insight.comparison = slot;
    insight.comparison_raw_lap = raw_lap;
    insight.metric = metric.kind;
    insight.rule = *metric.rule_id;
    insight.rule_stable_id = descriptor->stable_id;
    insight.measured_value = *metric.value;
    insight.threshold = metric.threshold;
    insight.estimated_time_effect_s = time_effect_s;
    insight.positive = metric.positive;
    insight.severity = metric.severity;
    insight.confidence = metric.confidence;
    insight.confidence_band = metric.confidence_band;
    insight.navigation = metric.navigation;
    return insight;
}

DerivedMetric make_metric(
    MetricKind kind,
    std::optional<RuleId> rule_id,
    std::optional<double> value,
    const AnalysisRules& rules,
    int confidence,
    const NavigationTarget& navigation,
    double time_effect_s,
    bool line_metric = false) {
    DerivedMetric result;
    result.kind = kind;
    result.rule_id = rule_id;
    result.value = value;
    result.confidence = std::clamp(confidence, 0, 100);
    result.confidence_band = confidence_band(result.confidence);
    result.navigation = navigation;
    if (!rule_id) return result;
    const auto* setting = find_rule(rules, *rule_id);
    if (!setting) return result;
    result.threshold = setting->threshold;
    result.rule_enabled = setting->enabled;
    if (!value || !setting->enabled || setting->threshold <= 0.0 || (line_metric && confidence <= 0)) return result;

    switch (kind) {
    case MetricKind::BrakePointDelta:
    case MetricKind::TurnInDelta:
    case MetricKind::ApexTimingDelta:
        result.triggered = std::abs(*value) > setting->threshold;
        break;
    case MetricKind::MinimumSpeedDelta:
    case MetricKind::ExitSpeedDelta:
        result.triggered = std::abs(*value) > setting->threshold;
        result.positive = *value > 0.0;
        break;
    case MetricKind::ThrottlePickupDelta:
        result.triggered = std::abs(*value) > setting->threshold;
        result.positive = *value < 0.0;
        break;
    case MetricKind::EntryLineDeviation:
        result.triggered = *value > setting->threshold;
        break;
    case MetricKind::RelativeTimeChange:
        result.triggered = std::abs(*value) > setting->threshold;
        result.positive = *value < 0.0;
        break;
    }
    if (result.triggered) {
        result.severity = classify_severity(std::abs(*value) / setting->threshold, std::abs(time_effect_s));
    }
    return result;
}

std::optional<double> event_delta(const CornerEvents& reference, const CornerEvents& comparison, EventType type) {
    const auto* reference_event = event_of_type(reference, type);
    const auto* comparison_event = event_of_type(comparison, type);
    if (!reference_event || !comparison_event) return std::nullopt;
    return comparison_event->elapsed_s - reference_event->elapsed_s;
}

NavigationTarget event_navigation_or_progress(
    const LapView& reference, const CornerEvents& events, EventType type, double fallback_progress) {
    const auto* event = event_of_type(events, type);
    return event ? navigation_for_event(reference, *event) : navigation_at_progress(reference, fallback_progress);
}

CornerMetrics calculate_corner_metrics(
    const LapView& reference,
    const LapView& comparison,
    const GeoFrame& frame,
    const CornerZone& corner,
    const CornerEvents& reference_events,
    const CornerEvents& comparison_events,
    const LineTranslation& translation,
    const ConfidenceBreakdown& confidence,
    const AnalysisRules& rules) {
    CornerMetrics result;
    result.corner_id = corner.id;
    result.corner_name = corner.name;
    result.start_distance_m = corner.start_progress * reference.total_distance_m;
    result.end_distance_m = corner.end_progress * reference.total_distance_m;

    const auto reference_start_time = timestamp_at_progress(reference, corner.start_progress);
    const auto reference_end_time = timestamp_at_progress(reference, corner.end_progress);
    const auto comparison_start_time = timestamp_at_progress(comparison, corner.start_progress);
    const auto comparison_end_time = timestamp_at_progress(comparison, corner.end_progress);
    const auto relative_time_change =
        static_cast<double>((comparison_end_time - comparison_start_time) - (reference_end_time - reference_start_time)) /
        kMicrosecondsPerSecond;

    const auto reference_minimum = minimum_speed(reference, corner.turn_in_progress, corner.exit_progress);
    const auto comparison_minimum = minimum_speed(comparison, corner.turn_in_progress, corner.exit_progress);
    const auto minimum_speed_delta = comparison_minimum.value - reference_minimum.value;
    const auto exit_speed_delta = speed_at_progress(comparison, corner.exit_progress) - speed_at_progress(reference, corner.exit_progress);

    const auto brake_delta = event_delta(reference_events, comparison_events, EventType::BrakeBegin);
    const auto turn_in_delta = event_delta(reference_events, comparison_events, EventType::TurnIn);
    const auto apex_delta = confidence.line_metrics_enabled
        ? event_delta(reference_events, comparison_events, EventType::Apex)
        : std::optional<double>{};
    const auto throttle_delta = event_delta(reference_events, comparison_events, EventType::FirstThrottle);

    result.metrics.push_back(make_metric(MetricKind::BrakePointDelta, RuleId::BrakePointDelta, brake_delta, rules,
        confidence.overall, event_navigation_or_progress(reference, reference_events, EventType::BrakeBegin, corner.start_progress),
        relative_time_change));
    result.metrics.push_back(make_metric(MetricKind::TurnInDelta, RuleId::TurnInDelta, turn_in_delta, rules,
        confidence.overall, event_navigation_or_progress(reference, reference_events, EventType::TurnIn, corner.turn_in_progress),
        relative_time_change));
    result.metrics.push_back(make_metric(MetricKind::ApexTimingDelta, RuleId::ApexTimingDelta, apex_delta, rules,
        confidence.line_metrics_enabled ? std::min(confidence.overall, confidence.translation_score) : 0,
        event_navigation_or_progress(reference, reference_events, EventType::Apex, corner.apex_progress), relative_time_change,
        true));
    result.metrics.push_back(make_metric(MetricKind::MinimumSpeedDelta, RuleId::MinimumSpeedLoss, minimum_speed_delta, rules,
        confidence.overall, navigation_at_progress(reference, progress_at_index(reference, reference_minimum.index)), relative_time_change));
    result.metrics.push_back(make_metric(MetricKind::ExitSpeedDelta, RuleId::ExitSpeedDelta, exit_speed_delta, rules,
        confidence.overall, navigation_at_progress(reference, corner.exit_progress), relative_time_change));
    result.metrics.push_back(make_metric(MetricKind::ThrottlePickupDelta, RuleId::ThrottlePickupDelay, throttle_delta, rules,
        confidence.overall, event_navigation_or_progress(reference, reference_events, EventType::FirstThrottle, corner.apex_progress),
        relative_time_change));

    if (confidence.line_metrics_enabled) {
        const auto deviation = entry_line_deviation(reference, comparison, frame, corner, translation);
        const auto line_confidence = std::min({confidence.overall, confidence.translation_score, confidence.repeatability_score});
        result.metrics.push_back(make_metric(MetricKind::EntryLineDeviation, RuleId::EntryLineDeviation, deviation.outward_m, rules,
            line_confidence, navigation_at_progress(reference, deviation.progress), relative_time_change, true));
    } else {
        result.metrics.push_back(make_metric(MetricKind::EntryLineDeviation, RuleId::EntryLineDeviation, std::nullopt, rules,
            0, navigation_at_progress(reference, corner.apex_progress), relative_time_change, true));
    }

    result.metrics.push_back(make_metric(MetricKind::RelativeTimeChange, RuleId::RelativeTimeChange,
        relative_time_change, rules, confidence.overall, navigation_at_progress(reference, corner.end_progress),
        relative_time_change));
    return result;
}

std::optional<double> metric_value(const CornerMetrics& metrics, MetricKind kind) {
    const auto iterator = std::find_if(metrics.metrics.begin(), metrics.metrics.end(), [kind](const DerivedMetric& metric) {
        return metric.kind == kind;
    });
    return iterator == metrics.metrics.end() ? std::nullopt : iterator->value;
}

std::optional<ComparisonAnalysis> analyze_comparison(
    const Session& session,
    const LapView& reference,
    const LapInfo* comparison_lap,
    ComparisonSlot slot,
    const GeoFrame& frame,
    std::span<const CornerZone> corners,
    const std::vector<CornerEvents>& reference_corner_events,
    int complete_lap_repeatability,
    const AnalysisRules& rules,
    AnalysisResult& aggregate,
    bool emit_insights = true) {
    if (!comparison_lap) return std::nullopt;
    if (comparison_lap->raw_lap == reference.lap->raw_lap && comparison_lap->begin_index == reference.lap->begin_index) {
        aggregate.diagnostics.emplace_back("A comparison slot duplicated the reference lap and was ignored");
        return std::nullopt;
    }
    const auto view_value = make_lap_view(session, *comparison_lap);
    if (!view_value) {
        aggregate.diagnostics.emplace_back("Comparison lap " + std::to_string(comparison_lap->raw_lap) + " has invalid samples");
        return std::nullopt;
    }
    const auto& comparison = *view_value;
    ComparisonAnalysis result;
    result.slot = slot;
    result.raw_lap = comparison_lap->raw_lap;
    result.line_translation = calculate_translation(reference, comparison, frame);

    std::vector<CornerEvents> comparison_corner_events;
    comparison_corner_events.reserve(corners.size());
    for (const auto& corner : corners) {
        const auto apex = point_at_progress(reference, frame, corner.apex_progress);
        auto events = detect_corner_events(comparison, frame, corner, apex, result.line_translation, rules);
        result.events.insert(result.events.end(), events.events.begin(), events.events.end());
        comparison_corner_events.push_back(std::move(events));
    }

    const auto expected_events = corners.size() * 6;
    const auto detected_for_confidence = static_cast<std::size_t>(std::count_if(
        result.events.begin(), result.events.end(), [](const DetectedEvent& event) {
            return event.type != EventType::SteeringCorrection;
        }));
    result.confidence = calculate_confidence(
        comparison, result.line_translation, complete_lap_repeatability, detected_for_confidence, expected_events);
    if (!result.confidence.line_metrics_enabled) {
        aggregate.diagnostics.emplace_back("Lap " + std::to_string(comparison_lap->raw_lap) +
            " needs more than 3 m whole-lap GPS correction; line conclusions are disabled");
    }

    for (std::size_t index = 0; index < corners.size(); ++index) {
        auto metrics = calculate_corner_metrics(reference, comparison, frame, corners[index], reference_corner_events[index],
                                                comparison_corner_events[index], result.line_translation, result.confidence, rules);
        const auto time_effect = metric_value(metrics, MetricKind::RelativeTimeChange).value_or(0.0);
        if (emit_insights) {
            for (const auto& metric : metrics.metrics) {
                if (auto insight = make_insight(metric, corners[index], slot, comparison_lap->raw_lap, time_effect)) {
                    aggregate.insights.push_back(std::move(*insight));
                }
            }
        }
        result.corners.push_back(std::move(metrics));
    }
    return result;
}

struct ComparableLapAnalysis {
    const LapInfo* lap{};
    ComparisonAnalysis analysis;
};

struct PhaseEffects {
    double prior_phase_effect_s{};
    double local_effect_s{};
    double retained_effect_s{};
};

double elapsed_delta_at_progress(const LapView& reference, const LapView& comparison, double progress) {
    const auto reference_elapsed = timestamp_at_progress(reference, progress) -
        reference.session->telemetry.time_us[reference.lap->begin_index];
    const auto comparison_elapsed = timestamp_at_progress(comparison, progress) -
        comparison.session->telemetry.time_us[comparison.lap->begin_index];
    return static_cast<double>(comparison_elapsed - reference_elapsed) / kMicrosecondsPerSecond;
}

double next_driver_decision_progress(std::span<const CornerZone> corners, std::size_t corner_index) {
    const auto& corner = corners[corner_index];
    for (std::size_t index = corner_index + 1; index < corners.size(); ++index) {
        if (corners[index].start_progress > corner.end_progress + 1e-6) return corners[index].start_progress;
    }
    return 1.0;
}

PhaseEffects phase_effects(
    const LapView& reference,
    const LapView& comparison,
    const CornerZone& corner,
    double action_progress,
    double retained_progress) {
    action_progress = std::clamp(action_progress, corner.start_progress, corner.end_progress);
    retained_progress = std::clamp(std::max(retained_progress, corner.end_progress), corner.end_progress, 1.0);
    const auto start_delta = elapsed_delta_at_progress(reference, comparison, corner.start_progress);
    const auto action_delta = elapsed_delta_at_progress(reference, comparison, action_progress);
    const auto end_delta = elapsed_delta_at_progress(reference, comparison, corner.end_progress);
    const auto retained_delta = elapsed_delta_at_progress(reference, comparison, retained_progress);
    return {action_delta - start_delta, end_delta - action_delta, retained_delta - start_delta};
}

bool has_telemetry_gap(const Session& session, const LapView& view) {
    const auto maximum_step = std::max<Timestamp>(250'000, view.median_step_us * 3);
    for (auto index = view.lap->begin_index + 1; index <= view.lap->end_index; ++index) {
        const auto step = session.telemetry.time_us[index] - session.telemetry.time_us[index - 1];
        if (step <= 0 || step > maximum_step) return true;
    }
    return false;
}

double robust_median(std::vector<double> values) {
    values.erase(std::remove_if(values.begin(), values.end(), [](double value) { return !std::isfinite(value); }), values.end());
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) * 0.5 : values[middle];
}

double robust_sigma(std::span<const double> values) {
    if (values.size() < 2) return 0.0;
    const auto center = robust_median(std::vector<double>(values.begin(), values.end()));
    std::vector<double> deviations;
    deviations.reserve(values.size());
    for (const auto value : values) deviations.push_back(std::abs(value - center));
    return robust_median(std::move(deviations)) * 1.4826;
}

const CornerMetrics* find_corner_metrics(const ComparisonAnalysis& analysis, std::string_view corner_id) {
    const auto iterator = std::find_if(analysis.corners.begin(), analysis.corners.end(), [&](const CornerMetrics& corner) {
        return corner.corner_id == corner_id;
    });
    return iterator == analysis.corners.end() ? nullptr : &*iterator;
}

const DerivedMetric* find_metric(const CornerMetrics& corner, MetricKind kind) {
    const auto iterator = std::find_if(corner.metrics.begin(), corner.metrics.end(), [&](const DerivedMetric& metric) {
        return metric.kind == kind;
    });
    return iterator == corner.metrics.end() ? nullptr : &*iterator;
}

bool same_metric_direction(double selected, double candidate) noexcept {
    if (selected == 0.0 || candidate == 0.0) return selected == candidate;
    return std::signbit(selected) == std::signbit(candidate);
}

bool is_line_metric(MetricKind kind) noexcept {
    return kind == MetricKind::ApexTimingDelta || kind == MetricKind::EntryLineDeviation;
}

std::string evidence_title(const insight_evidence::Result& evidence) {
    switch (evidence.outcome) {
    case insight_evidence::Outcome::DataLimited: return "Not enough data to judge";
    case insight_evidence::Outcome::Inconclusive: return "No clear time difference";
    case insight_evidence::Outcome::NetLoss: return "This approach lost time";
    case insight_evidence::Outcome::Compensation: return "Recovered time, but remained behind";
    case insight_evidence::Outcome::RetainedGain:
        if (evidence.recommendation == insight_evidence::Recommendation::RecommendTechnique) {
            return "Repeatable improvement";
        }
        if (evidence.recommendation == insight_evidence::Recommendation::Validate) {
            return "Promising improvement - test again";
        }
        return "Possible improvement - needs more laps";
    case insight_evidence::Outcome::TradeoffGain:
        if (evidence.recommendation == insight_evidence::Recommendation::RecommendSequence) {
            return "Repeatable whole-corner improvement";
        }
        if (evidence.recommendation == insight_evidence::Recommendation::Validate) {
            return "Whole-corner improvement - test again";
        }
        return "Possible whole-corner improvement";
    }
    return "Driving change detected";
}

std::string evidence_detail(
    const Insight& insight,
    const insight_evidence::Result& evidence) {
    std::ostringstream stream;
    stream << plain_metric_change(insight.metric, insight.measured_value) << " compared with the reference lap. ";
    stream << std::fixed << std::setprecision(3);
    switch (evidence.outcome) {
    case insight_evidence::Outcome::DataLimited:
        stream << "The recording quality is not good enough to suggest a driving change yet.";
        break;
    case insight_evidence::Outcome::Inconclusive:
        stream << "At the next braking or steering decision, the difference was smaller than the normal "
               << evidence.time_noise_floor_s << " s timing variation.";
        break;
    case insight_evidence::Outcome::NetLoss:
        stream << "By the next braking or steering decision, this approach was "
               << std::max(0.0, -evidence.retained_gain_s) << " s slower overall. Do not copy it as an improvement.";
        break;
    case insight_evidence::Outcome::Compensation:
        stream << "This recovered part of the time lost earlier, but the lap was not ahead by the next braking or steering decision.";
        break;
    case insight_evidence::Outcome::RetainedGain:
        stream << "The lap was still " << evidence.retained_gain_s
               << " s ahead at the next braking or steering decision.";
        break;
    case insight_evidence::Outcome::TradeoffGain:
        stream << "The complete corner approach was still " << evidence.retained_gain_s
               << " s ahead afterward. Copy the whole sequence rather than one isolated input.";
        break;
    }
    return stream.str();
}

void apply_recommendation_evidence(
    const Session& session,
    const LapView& reference,
    std::span<const CornerZone> corners,
    const AnalysisRules& rules,
    const std::vector<ComparableLapAnalysis>& comparable_analyses,
    AnalysisResult& result) {
    std::size_t compensation_count = 0;
    std::size_t retained_count = 0;
    std::size_t recommendation_count = 0;

    for (auto& insight : result.insights) {
        const auto selected_lap = std::find_if(session.laps.begin(), session.laps.end(), [&](const LapInfo& lap) {
            return lap.raw_lap == insight.comparison_raw_lap;
        });
        const auto selected_view_value = selected_lap == session.laps.end()
            ? std::optional<LapView>{}
            : make_lap_view(session, *selected_lap);
        const auto corner_iterator = std::find_if(corners.begin(), corners.end(), [&](const CornerZone& corner) {
            return corner.id == insight.corner_id;
        });
        if (!selected_view_value || corner_iterator == corners.end()) continue;
        const auto corner_index = static_cast<std::size_t>(std::distance(corners.begin(), corner_iterator));
        const auto retained_progress = next_driver_decision_progress(corners, corner_index);
        const auto action_progress = insight.metric == MetricKind::RelativeTimeChange
            ? corner_iterator->start_progress
            : insight.navigation.reference_progress;
        const auto selected_effects = phase_effects(
            reference, *selected_view_value, *corner_iterator, action_progress, retained_progress);

        std::vector<double> retained_effects;
        retained_effects.reserve(comparable_analyses.size());
        for (const auto& candidate : comparable_analyses) {
            if (!candidate.lap || candidate.lap->phase != LapPhase::Complete ||
                candidate.analysis.confidence.overall < rules.evidence.minimum_data_confidence ||
                (is_line_metric(insight.metric) && !candidate.analysis.confidence.line_metrics_enabled)) {
                continue;
            }
            const auto* candidate_corner = find_corner_metrics(candidate.analysis, insight.corner_id);
            const auto* candidate_metric = candidate_corner ? find_metric(*candidate_corner, insight.metric) : nullptr;
            if (!candidate_metric || !candidate_metric->value || !candidate_metric->triggered ||
                !same_metric_direction(insight.measured_value, *candidate_metric->value)) {
                continue;
            }
            const auto candidate_view = make_lap_view(session, *candidate.lap);
            if (!candidate_view || has_telemetry_gap(session, *candidate_view)) continue;
            const auto candidate_action = insight.metric == MetricKind::RelativeTimeChange
                ? corner_iterator->start_progress
                : candidate_metric->navigation.reference_progress;
            retained_effects.push_back(phase_effects(
                reference, *candidate_view, *corner_iterator, candidate_action, retained_progress).retained_effect_s);
        }

        const auto repeatability_sigma = robust_sigma(
            std::span<const double>(retained_effects.data(), retained_effects.size()));
        const auto sample_period_s = static_cast<double>(selected_view_value->median_step_us) / kMicrosecondsPerSecond;
        const auto noise_floor = std::max({rules.evidence.minimum_time_floor_s,
            rules.evidence.sample_period_multiplier * sample_period_s,
            rules.evidence.repeatability_sigma_multiplier * repeatability_sigma});
        const auto supporting_laps = static_cast<std::size_t>(std::count_if(
            retained_effects.begin(), retained_effects.end(), [&](double effect) { return effect <= -noise_floor; }));

        insight_evidence::CandidateEvidence candidate;
        candidate.sample_period_s = sample_period_s;
        candidate.timing_repeatability_sigma_s = repeatability_sigma;
        candidate.data_confidence = insight.confidence;
        candidate.technique_change_detected = true;
        candidate.favorable_local_metric = insight.positive;
        candidate.prior_phase_effect_s = selected_effects.prior_phase_effect_s;
        candidate.local_effect_s = selected_effects.local_effect_s;
        candidate.retained_effect_s = selected_effects.retained_effect_s;
        candidate.comparable_laps = retained_effects.size();
        candidate.supporting_laps = supporting_laps;
        if (retained_effects.size() >= 2) {
            const auto center = robust_median(retained_effects);
            const auto half_width = 1.96 * repeatability_sigma /
                std::sqrt(static_cast<double>(retained_effects.size()));
            candidate.median_retained_effect_s = center;
            candidate.retained_interval_low_s = center - half_width;
            candidate.retained_interval_high_s = center + half_width;
        }
        candidate.complete_lap = selected_lap->phase == LapPhase::Complete;
        candidate.telemetry_gap = has_telemetry_gap(session, *selected_view_value);
        const std::vector<DetectedEvent>* selected_events = nullptr;
        if (insight.comparison == ComparisonSlot::CompareA && result.comparisons[0]) {
            selected_events = &result.comparisons[0]->events;
        } else if (insight.comparison == ComparisonSlot::CompareB && result.comparisons[1]) {
            selected_events = &result.comparisons[1]->events;
        }
        candidate.extra_steering_correction = selected_events && std::any_of(
            selected_events->begin(), selected_events->end(), [&](const DetectedEvent& event) {
                return event.corner_id == insight.corner_id && event.type == EventType::SteeringCorrection;
            });
        candidate.line_metric = is_line_metric(insight.metric);
        candidate.line_conclusion_enabled = !candidate.line_metric ||
            (insight.comparison == ComparisonSlot::CompareA && result.comparisons[0] &&
                result.comparisons[0]->confidence.line_metrics_enabled) ||
            (insight.comparison == ComparisonSlot::CompareB && result.comparisons[1] &&
                result.comparisons[1]->confidence.line_metrics_enabled);

        const auto evidence = insight_evidence::evaluate(candidate, rules.evidence);
        insight.outcome = evidence.outcome;
        insight.reliability = evidence.reliability;
        insight.recommendation = evidence.recommendation;
        insight.prior_phase_effect_s = candidate.prior_phase_effect_s;
        insight.local_effect_s = candidate.local_effect_s;
        insight.retained_effect_s = candidate.retained_effect_s;
        insight.time_noise_floor_s = evidence.time_noise_floor_s;
        insight.retained_gain_s = evidence.retained_gain_s;
        insight.downstream_payback_fraction = evidence.downstream_payback_fraction;
        insight.comparable_laps = candidate.comparable_laps;
        insight.supporting_laps = candidate.supporting_laps;
        insight.median_retained_effect_s = candidate.median_retained_effect_s;
        insight.retained_interval_low_s = candidate.retained_interval_low_s;
        insight.retained_interval_high_s = candidate.retained_interval_high_s;
        insight.evidence_reasons = evidence.reasons;
        if (descriptor_for(insight.rule)) {
            insight.title = evidence_title(evidence);
            insight.detail = evidence_detail(insight, evidence);
        }

        if (evidence.outcome == insight_evidence::Outcome::Compensation) ++compensation_count;
        if (evidence.outcome == insight_evidence::Outcome::RetainedGain ||
            evidence.outcome == insight_evidence::Outcome::TradeoffGain) {
            ++retained_count;
        }
        if (evidence.recommendation == insight_evidence::Recommendation::RecommendTechnique ||
            evidence.recommendation == insight_evidence::Recommendation::RecommendSequence) {
            ++recommendation_count;
        }
    }
    result.diagnostics.push_back(std::format(
        "Evidence v2: {} recovery/compensation, {} retained-gain observations, {} repeatable recommendations",
        compensation_count, retained_count, recommendation_count));
}

}  // namespace

AnalysisRules default_analysis_rules() { return {}; }

std::span<const RuleDescriptor> rule_descriptors() { return kRuleDescriptors; }

const RuleSetting* find_rule(const AnalysisRules& rules, RuleId id) {
    const auto iterator = std::find_if(rules.metrics.begin(), rules.metrics.end(), [id](const RuleSetting& setting) {
        return setting.id == id;
    });
    return iterator == rules.metrics.end() ? nullptr : &*iterator;
}

ConfidenceBand confidence_band(int score) noexcept {
    if (score < 40) return ConfidenceBand::Suppressed;
    if (score < 60) return ConfidenceBand::WeakSignal;
    return ConfidenceBand::Actionable;
}

Severity classify_severity(double threshold_multiple, double absolute_time_effect_s) noexcept {
    threshold_multiple = std::abs(threshold_multiple);
    absolute_time_effect_s = std::abs(absolute_time_effect_s);
    if (threshold_multiple >= 2.5 || absolute_time_effect_s >= 0.15) return Severity::High;
    if (threshold_multiple < 1.5 && absolute_time_effect_s < 0.05) return Severity::Low;
    return Severity::Medium;
}

std::string_view event_name(EventType type) noexcept {
    switch (type) {
    case EventType::BrakeBegin: return "Brake begins";
    case EventType::BrakeRelease: return "Brake release";
    case EventType::TurnIn: return "Turn-in";
    case EventType::Apex: return "Apex";
    case EventType::FirstThrottle: return "First throttle";
    case EventType::FullThrottle: return "Full throttle";
    case EventType::SteeringCorrection: return "Steering correction";
    }
    return "Event";
}

std::string_view metric_name(MetricKind kind) noexcept {
    switch (kind) {
    case MetricKind::BrakePointDelta: return "Brake-point delta";
    case MetricKind::TurnInDelta: return "Turn-in delta";
    case MetricKind::ApexTimingDelta: return "Apex timing delta";
    case MetricKind::MinimumSpeedDelta: return "Minimum speed difference";
    case MetricKind::ExitSpeedDelta: return "Exit speed difference";
    case MetricKind::ThrottlePickupDelta: return "Throttle-pickup delay";
    case MetricKind::EntryLineDeviation: return "Entry-line outward deviation";
    case MetricKind::RelativeTimeChange: return "Relative-time change";
    }
    return "Metric";
}

std::vector<CornerZone> suggest_corner_zones(
    const Session& session, const LapInfo& reference, std::size_t maximum_corners) {
    std::vector<CornerZone> result;
    if (maximum_corners == 0) return result;
    const auto view_value = make_lap_view(session, reference);
    if (!view_value) return result;
    const auto& view = *view_value;
    const auto frame = make_frame(session, reference);

    struct Candidate { double score{}; double distance_m{}; };
    std::vector<Candidate> candidates;
    for (auto index = reference.begin_index + 3; index + 3 <= reference.end_index; ++index) {
        const auto curvature = std::abs(curvature_at(view, frame, index));
        const auto local = index - reference.begin_index;
        const auto before = static_cast<double>(session.telemetry.speed_kmh[index - 3]);
        const auto after = static_cast<double>(session.telemetry.speed_kmh[index + 3]);
        const auto current = static_cast<double>(session.telemetry.speed_kmh[index]);
        const auto speed_drop = std::max(0.0, (before + after) * 0.5 - current);
        const auto& control = view.controls[local];
        const auto steering = control.valid ? std::abs(static_cast<double>(control.steering)) : 0.0;
        if (curvature < 0.003 && speed_drop < 2.0) continue;
        const auto score = curvature * 80.0 + speed_drop * 0.15 + steering * 0.01;
        candidates.push_back({score, view.distance_m[local]});
    }
    if (candidates.empty()) return result;
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.score > right.score;
    });
    // A sharp hairpin can be orders of magnitude stronger than harmless GPS or
    // steering ripples. Keep only candidates that remain significant relative
    // to the strongest bend instead of filling maximum_corners unconditionally.
    const auto minimum_significant_score = candidates.front().score * 0.05;
    const auto minimum_spacing = std::max(4.0, view.total_distance_m * 0.05);
    std::vector<Candidate> selected;
    for (const auto& candidate : candidates) {
        if (candidate.score < minimum_significant_score) break;
        if (candidate.distance_m < minimum_spacing || candidate.distance_m > view.total_distance_m - minimum_spacing) continue;
        const auto overlaps = std::any_of(selected.begin(), selected.end(), [&](const Candidate& existing) {
            return std::abs(existing.distance_m - candidate.distance_m) < minimum_spacing;
        });
        if (!overlaps) selected.push_back(candidate);
        if (selected.size() >= maximum_corners) break;
    }
    std::sort(selected.begin(), selected.end(), [](const Candidate& left, const Candidate& right) {
        return left.distance_m < right.distance_m;
    });
    const auto half_width = std::clamp(view.total_distance_m * 0.06, 3.0, 10.0);
    for (std::size_t index = 0; index < selected.size(); ++index) {
        const auto center = selected[index].distance_m;
        const auto normalize = [&](double distance) { return std::clamp(distance / view.total_distance_m, 0.0, 1.0); };
        CornerZone corner;
        corner.id = "corner-" + std::to_string(index + 1);
        corner.name = "Turn " + std::to_string(index + 1);
        corner.start_progress = normalize(center - half_width);
        corner.turn_in_progress = normalize(center - half_width * 0.35);
        corner.apex_progress = normalize(center);
        corner.exit_progress = normalize(center + half_width * 0.35);
        corner.end_progress = normalize(center + half_width);
        if (valid_corner(corner)) result.push_back(std::move(corner));
    }
    return result;
}

AnalysisResult analyze_driver_performance(
    const Session& session,
    const LapInfo& reference_lap,
    const LapInfo* compare_a,
    const LapInfo* compare_b,
    std::span<const CornerZone> corners,
    const AnalysisRules& rules) {
    AnalysisResult result;
    const auto reference_value = make_lap_view(session, reference_lap);
    if (!reference_value) {
        result.diagnostics.emplace_back("Reference lap has invalid samples");
        return result;
    }
    const auto& reference = *reference_value;
    const auto frame = make_frame(session, reference_lap);
    const auto complete_lap_repeatability = complete_lap_repeatability_score(session, reference, frame);

    result.corners.reserve(corners.size());
    for (const auto& corner : corners) {
        if (valid_corner(corner)) {
            result.corners.push_back(corner);
        } else {
            result.diagnostics.emplace_back("Corner '" + corner.id + "' has invalid or unordered normalized boundaries");
        }
    }
    if (result.corners.empty()) {
        result.diagnostics.emplace_back("No valid corner zones were supplied");
    }

    result.reference.raw_lap = reference_lap.raw_lap;
    result.reference.total_distance_m = reference.total_distance_m;
    std::vector<CornerEvents> reference_corner_events;
    reference_corner_events.reserve(result.corners.size());
    const LineTranslation no_translation;
    for (const auto& corner : result.corners) {
        const auto apex = point_at_progress(reference, frame, corner.apex_progress);
        auto events = detect_corner_events(reference, frame, corner, apex, no_translation, rules);
        result.reference.events.insert(result.reference.events.end(), events.events.begin(), events.events.end());
        reference_corner_events.push_back(std::move(events));
    }

    result.comparisons[0] = analyze_comparison(session, reference, compare_a, ComparisonSlot::CompareA, frame,
                                               result.corners, reference_corner_events, complete_lap_repeatability, rules, result);
    result.comparisons[1] = analyze_comparison(session, reference, compare_b, ComparisonSlot::CompareB, frame,
                                               result.corners, reference_corner_events, complete_lap_repeatability, rules, result);

    // Build a session-wide evidence population without emitting duplicate cards.
    // A selected lap can expose a candidate technique, but only repeatable outcomes
    // from complete, comparable laps are allowed to promote it to a recommendation.
    std::vector<ComparableLapAnalysis> comparable_analyses;
    comparable_analyses.reserve(session.laps.size());
    for (const auto& lap : session.laps) {
        if (lap.phase != LapPhase::Complete || lap.raw_lap == reference_lap.raw_lap) continue;
        AnalysisResult aggregate;
        auto analysis = analyze_comparison(session, reference, &lap, ComparisonSlot::CompareA, frame,
                                           result.corners, reference_corner_events,
                                           complete_lap_repeatability, rules, aggregate, false);
        if (analysis) comparable_analyses.push_back({&lap, std::move(*analysis)});
    }
    apply_recommendation_evidence(session, reference, result.corners, rules, comparable_analyses, result);

    std::sort(result.insights.begin(), result.insights.end(), [](const Insight& left, const Insight& right) {
        if (left.navigation.reference_progress != right.navigation.reference_progress) {
            return left.navigation.reference_progress < right.navigation.reference_progress;
        }
        if (left.severity != right.severity) return left.severity > right.severity;
        return left.id < right.id;
    });
    return result;
}

TranslatedCoordinate apply_line_translation(double latitude, double longitude,
                                             const LineTranslation& translation) noexcept {
    constexpr double degrees_to_radians = 3.14159265358979323846 / 180.0;
    const auto longitude_metres = 111'320.0 * std::cos(latitude * degrees_to_radians);
    return {
        latitude + translation.north_m / 110'540.0,
        std::abs(longitude_metres) > 1.0 ? longitude + translation.east_m / longitude_metres : longitude,
    };
}

}  // namespace racebox::driver_analysis
