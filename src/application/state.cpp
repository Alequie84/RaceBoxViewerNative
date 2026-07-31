#include "racebox/application/state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace racebox::application {
namespace {

bool is_complete(std::span<const LapInfo> laps, std::size_t index) {
    return index < laps.size() && laps[index].phase == LapPhase::Complete;
}

std::vector<std::size_t> complete_laps_by_time(std::span<const LapInfo> laps) {
    std::vector<std::size_t> complete;
    complete.reserve(laps.size());
    for (std::size_t index = 0; index < laps.size(); ++index) {
        if (laps[index].phase == LapPhase::Complete) complete.push_back(index);
    }
    std::stable_sort(complete.begin(), complete.end(), [&](const auto left, const auto right) {
        return laps[left].duration_us < laps[right].duration_us;
    });
    return complete;
}

std::optional<std::size_t> first_unused(
    std::span<const std::size_t> ordered,
    std::span<const std::size_t> used) {
    const auto match = std::find_if(ordered.begin(), ordered.end(), [&](const auto candidate) {
        return std::find(used.begin(), used.end(), candidate) == used.end();
    });
    return match == ordered.end() ? std::nullopt : std::optional<std::size_t>(*match);
}

}  // namespace

std::string_view workspace_key(Workspace workspace) noexcept {
    switch (workspace) {
        case Workspace::RaceDay: return "race_day";
        case Workspace::Session: return "session";
        case Workspace::Compare: return "compare";
        case Workspace::CrewChief: return "crew_chief";
        case Workspace::Reports: return "reports";
    }
    return "session";
}

ResolvedLapRoles resolve_lap_roles(
    std::span<const LapInfo> laps,
    const LapRoleSelection& requested) {
    ResolvedLapRoles result;
    result.selection = requested;
    const auto ordered = complete_laps_by_time(laps);
    if (ordered.empty()) {
        result.selection.reference.reset();
        result.selection.compare_a.reset();
        result.selection.compare_b.reset();
        result.selection.playback = laps.empty() ? std::nullopt : std::optional<std::size_t>(0);
        return result;
    }

    if (requested.reference_mode == ReferenceMode::FastestComplete ||
        !requested.reference || !is_complete(laps, *requested.reference)) {
        result.selection.reference = ordered.front();
    }
    result.reference_ready = result.selection.reference.has_value();

    std::vector<std::size_t> used{*result.selection.reference};
    const auto resolve_comparison = [&](const std::optional<std::size_t> preferred,
                                        bool fill_when_missing) -> std::optional<std::size_t> {
        if (preferred && is_complete(laps, *preferred) &&
            std::find(used.begin(), used.end(), *preferred) == used.end()) {
            used.push_back(*preferred);
            return preferred;
        }
        if (!preferred && !fill_when_missing) return std::nullopt;
        const auto replacement = first_unused(ordered, used);
        if (replacement) used.push_back(*replacement);
        return replacement;
    };

    // Compare A is the minimum useful comparison and is filled when possible.
    result.selection.compare_a = resolve_comparison(requested.compare_a, true);
    // Compare B is explicitly optional; an absent request stays absent.
    result.selection.compare_b = resolve_comparison(requested.compare_b, false);
    result.comparison_ready = result.selection.reference.has_value() &&
                              result.selection.compare_a.has_value();

    if (!requested.playback || *requested.playback >= laps.size()) {
        result.selection.playback = result.selection.reference;
    }
    return result;
}

void normalise_playback(PlaybackState& playback) noexcept {
    if (!std::isfinite(playback.speed)) playback.speed = 1.0;
    playback.speed = std::clamp(playback.speed, 0.25, 2.0);
}

void reset_document(AppState& state) noexcept {
    const auto workspace = state.workspace;
    state = {};
    state.workspace = workspace;
}

}  // namespace racebox::application
