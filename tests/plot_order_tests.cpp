#include "racebox/telemetry_plot_order.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using racebox::app::TelemetryPlotId;

namespace {

bool expect(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAILED: " << message << '\n';
    return false;
}

}  // namespace

int main() {
    using namespace racebox::app;
    auto passed = true;

    const std::vector<std::string> invalid_preferences{
        "steering", "unknown", "speed", "steering", "relative_time"};
    const auto normalized = normalize_telemetry_plot_order(invalid_preferences);
    passed &= expect(normalized.size() == 5,
                     "paired normalization must retain explicitly saved separate graphs");
    passed &= expect(normalized[0] == TelemetryPlotId::Steering &&
                     normalized[1] == TelemetryPlotId::Speed &&
                     normalized[2] == TelemetryPlotId::RelativeTime,
                     "normalization must retain valid first-class graph IDs and remove duplicates");
    passed &= expect(normalized[3] == TelemetryPlotId::LateralG &&
                     normalized[4] == TelemetryPlotId::LongitudinalG,
                     "normalization must append the two combined response/input graphs");

    passed &= expect(default_telemetry_plot_order() == default_telemetry_plot_order(true),
                     "the default graph layout must keep every telemetry channel separate");
    passed &= expect(telemetry_plot_name(TelemetryPlotId::LateralG) == "Lateral G" &&
                         telemetry_plot_name(TelemetryPlotId::LateralG, true) ==
                             "Lateral G + Steering",
                     "graph headers must describe separate and paired layouts accurately");

    auto compare_order = default_telemetry_plot_order(false);
    passed &= expect(move_telemetry_plot(compare_order, TelemetryPlotId::LateralG, 0, true),
                     "moving a compare-mode graph should report a change");
    passed &= expect(compare_order.front() == TelemetryPlotId::LateralG &&
                     compare_order[1] == TelemetryPlotId::RelativeTime,
                     "compare-mode move must use the requested visible gap");

    auto single_order = default_telemetry_plot_order(false);
    passed &= expect(move_telemetry_plot(single_order, TelemetryPlotId::LateralG, 0, false),
                     "moving a visible single-mode graph should report a change");
    passed &= expect(single_order[0] == TelemetryPlotId::RelativeTime &&
                     single_order[1] == TelemetryPlotId::LateralG &&
                     single_order[2] == TelemetryPlotId::Speed,
                     "hidden relative-time graph must retain its stored slot");

    const auto unchanged = single_order;
    passed &= expect(!move_telemetry_plot(single_order, TelemetryPlotId::RelativeTime, 0, false),
                     "a hidden graph must not be draggable");
    passed &= expect(single_order == unchanged, "a rejected move must not change stored order");

    // Exercise every stable graph identifier, not just one representative move.
    for (const auto id : kAllSeparateTelemetryPlotOrder) {
        auto order = default_telemetry_plot_order(true);
        const auto old_position = static_cast<std::size_t>(std::distance(
            order.begin(), std::find(order.begin(), order.end(), id)));
        const auto target_gap = old_position == 0 ? order.size() : 0;
        passed &= expect(move_telemetry_plot(order, id, target_gap, true),
                         "every visible graph must be reorderable");
        passed &= expect((target_gap == 0 && order.front() == id) ||
                         (target_gap != 0 && order.back() == id),
                         "the requested graph did not reach its drop position");
        const auto persisted_keys = [&] {
            std::vector<std::string> keys;
            for (const auto value : order) keys.emplace_back(telemetry_plot_key(value));
            return keys;
        }();
        passed &= expect(normalize_telemetry_plot_order(persisted_keys, true) == order,
                         "a valid saved graph order must survive preference normalization");
    }

    auto mode_switch_order = default_telemetry_plot_order(false);
    const auto hidden_slot = static_cast<std::size_t>(std::distance(
        mode_switch_order.begin(), std::find(mode_switch_order.begin(), mode_switch_order.end(),
                                              TelemetryPlotId::RelativeTime)));
    passed &= expect(move_telemetry_plot(mode_switch_order, TelemetryPlotId::LongitudinalG, 0, false),
                     "visible graphs must remain reorderable while relative time is hidden");
    const auto hidden_slot_after = static_cast<std::size_t>(std::distance(
        mode_switch_order.begin(), std::find(mode_switch_order.begin(), mode_switch_order.end(),
                                              TelemetryPlotId::RelativeTime)));
    passed &= expect(hidden_slot_after == hidden_slot,
                     "switching out of Compare mode must preserve relative time's stored slot");
    passed &= expect(std::all_of(kPairedTelemetryPlotOrder.begin(), kPairedTelemetryPlotOrder.end(),
        [&](TelemetryPlotId id) { return std::count(mode_switch_order.begin(), mode_switch_order.end(), id) == 1; }),
        "mode changes must retain every stable graph exactly once");

    if (passed) std::cout << "Telemetry plot order tests passed\n";
    return passed ? 0 : 1;
}
