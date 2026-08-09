#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace racebox::app {

enum class TelemetryPlotId {
    RelativeTime,
    Speed,
    LateralG,
    LongitudinalG,
    Controls,
    Steering,
};

inline constexpr std::array<TelemetryPlotId, 6> kAllTelemetryPlotIds{
    TelemetryPlotId::RelativeTime,
    TelemetryPlotId::Speed,
    TelemetryPlotId::LateralG,
    TelemetryPlotId::LongitudinalG,
    TelemetryPlotId::Controls,
    TelemetryPlotId::Steering,
};

inline constexpr std::array<TelemetryPlotId, 4> kPairedTelemetryPlotOrder{
    TelemetryPlotId::RelativeTime,
    TelemetryPlotId::Speed,
    TelemetryPlotId::LateralG,
    TelemetryPlotId::LongitudinalG,
};

inline constexpr std::array<TelemetryPlotId, 6> kAllSeparateTelemetryPlotOrder{
    TelemetryPlotId::RelativeTime,
    TelemetryPlotId::Speed,
    TelemetryPlotId::LateralG,
    TelemetryPlotId::Steering,
    TelemetryPlotId::LongitudinalG,
    TelemetryPlotId::Controls,
};

constexpr std::string_view telemetry_plot_key(TelemetryPlotId id) {
    switch (id) {
        case TelemetryPlotId::RelativeTime: return "relative_time";
        case TelemetryPlotId::Speed: return "speed";
        case TelemetryPlotId::LateralG: return "lateral_g";
        case TelemetryPlotId::LongitudinalG: return "longitudinal_g";
        case TelemetryPlotId::Controls: return "controls";
        case TelemetryPlotId::Steering: return "steering";
    }
    return "speed";
}

constexpr std::string_view telemetry_plot_name(TelemetryPlotId id, bool paired = false) {
    switch (id) {
        case TelemetryPlotId::RelativeTime: return "Relative time";
        case TelemetryPlotId::Speed: return "Speed";
        case TelemetryPlotId::LateralG:
            return paired ? "Lateral G + Steering" : "Lateral G";
        case TelemetryPlotId::LongitudinalG:
            return paired ? "Longitudinal G + Throttle / Brake" : "Longitudinal G";
        case TelemetryPlotId::Controls: return "Throttle / Brake";
        case TelemetryPlotId::Steering: return "Steering";
    }
    return "Speed";
}

constexpr std::optional<TelemetryPlotId> telemetry_plot_id(std::string_view key) {
    for (const auto id : kAllTelemetryPlotIds) {
        if (telemetry_plot_key(id) == key) return id;
    }
    return std::nullopt;
}

inline std::vector<TelemetryPlotId> normalize_telemetry_plot_order(
    std::span<const std::string> keys, bool all_separate = false) {
    std::vector<TelemetryPlotId> result;
    result.reserve(kAllTelemetryPlotIds.size());
    for (const auto& key : keys) {
        const auto id = telemetry_plot_id(key);
        if (id && std::find(result.begin(), result.end(), *id) == result.end()) result.push_back(*id);
    }
    const auto required = all_separate
        ? std::span<const TelemetryPlotId>{kAllSeparateTelemetryPlotOrder}
        : std::span<const TelemetryPlotId>{kPairedTelemetryPlotOrder};
    for (const auto id : required) {
        if (std::find(result.begin(), result.end(), id) == result.end()) result.push_back(id);
    }
    return result;
}

inline std::vector<TelemetryPlotId> default_telemetry_plot_order(bool all_separate = true) {
    if (all_separate) {
        return {kAllSeparateTelemetryPlotOrder.begin(), kAllSeparateTelemetryPlotOrder.end()};
    }
    return {kPairedTelemetryPlotOrder.begin(), kPairedTelemetryPlotOrder.end()};
}

// target_visible_gap is a gap in the currently visible list: zero is before the
// first graph and visible_count is after the last. When relative time is hidden,
// its stored slot is deliberately retained while the other graphs move around it.
inline bool move_telemetry_plot(std::vector<TelemetryPlotId>& order, TelemetryPlotId dragged,
                                std::size_t target_visible_gap, bool relative_time_visible) {
    std::vector<TelemetryPlotId> visible;
    visible.reserve(order.size());
    for (const auto id : order) {
        if (relative_time_visible || id != TelemetryPlotId::RelativeTime) visible.push_back(id);
    }

    const auto dragged_iterator = std::find(visible.begin(), visible.end(), dragged);
    if (dragged_iterator == visible.end()) return false;
    const auto old_index = static_cast<std::size_t>(std::distance(visible.begin(), dragged_iterator));
    auto insertion_index = std::min(target_visible_gap, visible.size());
    visible.erase(dragged_iterator);
    if (old_index < insertion_index) --insertion_index;
    insertion_index = std::min(insertion_index, visible.size());
    visible.insert(visible.begin() + static_cast<std::ptrdiff_t>(insertion_index), dragged);

    const auto old_order = order;
    if (relative_time_visible) {
        order = std::move(visible);
    } else {
        auto next_visible = visible.begin();
        for (auto& id : order) {
            if (id == TelemetryPlotId::RelativeTime) continue;
            id = *next_visible++;
        }
    }
    return order != old_order;
}

}  // namespace racebox::app
