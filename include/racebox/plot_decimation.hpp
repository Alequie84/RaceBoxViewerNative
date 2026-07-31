#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

namespace racebox {

// Returns time-ordered source indices while retaining local extrema across all
// supplied channels. The accessor is called as value(sample, channel).
// First/last samples are always retained when the budget allows.
template <typename ValueAccessor>
[[nodiscard]] std::vector<std::size_t> peak_preserving_indices(
    std::size_t sample_count,
    std::size_t maximum_points,
    std::size_t channel_count,
    ValueAccessor value) {
    if (sample_count == 0 || maximum_points == 0) return {};
    if (maximum_points == 1) return {0};
    if (sample_count <= maximum_points) {
        std::vector<std::size_t> all(sample_count);
        std::iota(all.begin(), all.end(), std::size_t{});
        return all;
    }
    if (maximum_points == 2 || channel_count == 0) {
        return {0, sample_count - 1};
    }

    std::vector<double> minimum(
        channel_count, std::numeric_limits<double>::infinity());
    std::vector<double> maximum(
        channel_count, -std::numeric_limits<double>::infinity());
    for (std::size_t sample = 0; sample < sample_count; ++sample) {
        for (std::size_t channel = 0; channel < channel_count; ++channel) {
            const auto current = static_cast<double>(value(sample, channel));
            if (!std::isfinite(current)) continue;
            minimum[channel] = std::min(minimum[channel], current);
            maximum[channel] = std::max(maximum[channel], current);
        }
    }

    const auto prominence = [&](std::size_t sample) {
        double result = 0.0;
        for (std::size_t channel = 0; channel < channel_count; ++channel) {
            const auto previous =
                static_cast<double>(value(sample - 1, channel));
            const auto current =
                static_cast<double>(value(sample, channel));
            const auto next =
                static_cast<double>(value(sample + 1, channel));
            if (!std::isfinite(previous) || !std::isfinite(current) ||
                !std::isfinite(next)) {
                continue;
            }
            const auto range =
                std::max(1.0e-9, maximum[channel] - minimum[channel]);
            const auto curvature =
                std::abs(current - (previous + next) * 0.5) / range;
            const auto edge =
                std::max(std::abs(current - previous),
                         std::abs(next - current)) /
                range;
            result = std::max(result, curvature + edge * 0.25);
        }
        return result;
    };

    const auto interior_budget = maximum_points - 2;
    std::vector<std::size_t> selected;
    selected.reserve(maximum_points);
    selected.push_back(0);
    std::vector<std::uint8_t> chosen(sample_count);
    chosen.front() = chosen.back() = 1;

    // Reserve part of the budget for even time coverage so long smooth
    // sections remain readable. The rest is selected globally by normalized
    // local prominence. Unlike a fixed "two points per bucket" envelope, this
    // does not discard a third short event merely because several channels
    // peak close together.
    const auto coverage_budget = interior_budget >= 3
        ? interior_budget / 3
        : std::size_t{};
    for (std::size_t slot = 1; slot <= coverage_budget; ++slot) {
        const auto sample = std::clamp(
            slot * (sample_count - 1) / (coverage_budget + 1),
            std::size_t{1}, sample_count - 2);
        if (!chosen[sample]) {
            chosen[sample] = 1;
            selected.push_back(sample);
        }
    }

    struct Candidate {
        double score{};
        std::size_t sample{};
    };
    std::vector<Candidate> candidates;
    candidates.reserve(sample_count - selected.size());
    for (std::size_t sample = 1; sample + 1 < sample_count; ++sample) {
        if (!chosen[sample]) {
            candidates.push_back({prominence(sample), sample});
        }
    }
    const auto feature_budget = maximum_points - selected.size() - 1;
    const auto better = [](const Candidate& left, const Candidate& right) {
        if (left.score != right.score) return left.score > right.score;
        return left.sample < right.sample;
    };
    if (candidates.size() > feature_budget) {
        std::nth_element(
            candidates.begin(),
            candidates.begin() + static_cast<std::ptrdiff_t>(feature_budget),
            candidates.end(), better);
        candidates.resize(feature_budget);
    }
    for (const auto& candidate : candidates) {
        selected.push_back(candidate.sample);
    }
    selected.push_back(sample_count - 1);
    std::sort(selected.begin(), selected.end());
    selected.erase(
        std::unique(selected.begin(), selected.end()), selected.end());
    return selected;
}

}  // namespace racebox
