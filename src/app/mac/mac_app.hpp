#pragma once

#include "racebox/domain.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace racebox::app::mac {

class MacApp {
public:
    explicit MacApp(std::filesystem::path resource_root);

    void begin_drop();
    void add_dropped_file(std::filesystem::path path);
    void finish_drop();
    void load_paths(const std::vector<std::filesystem::path>& paths);
    void draw();

private:
    void load_demo();
    void rebuild_plot_cache();
    void draw_overview();
    void draw_track_map();
    void draw_telemetry();
    void draw_laps();
    void draw_imu();
    void select_lap(std::size_t index);

    std::filesystem::path resource_root_;
    std::optional<LoadResult> loaded_;
    std::vector<std::filesystem::path> drop_paths_;
    std::size_t selected_lap_{};
    bool light_mode_{};
    std::string status_;

    std::vector<double> time_s_;
    std::vector<double> east_m_;
    std::vector<double> north_m_;
    std::vector<double> speed_kmh_;
    std::vector<double> lateral_g_;
    std::vector<double> longitudinal_g_;
    std::vector<double> vertical_g_;
    std::vector<double> yaw_rate_dps_;
    std::vector<double> throttle_;
    std::vector<double> brake_;
    std::vector<double> steering_;
};

}  // namespace racebox::app::mac
