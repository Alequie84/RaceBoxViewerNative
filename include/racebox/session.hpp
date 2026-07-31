#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace racebox {

using Timestamp = std::int64_t;

enum class LapPhase { Complete, OutLap, InLap, Invalid };
enum class ViewMode { SingleLap, Continuous, Compare };

struct TelemetrySeries {
    std::vector<Timestamp> time_us;
    std::vector<std::int64_t> absolute_time_us;
    std::vector<double> latitude;
    std::vector<double> longitude;
    std::vector<float> speed_kmh;
    std::vector<float> heading_deg;
    std::vector<float> altitude_m;
    std::vector<float> longitudinal_g;
    std::vector<float> lateral_g;
    std::vector<float> vertical_g;
    std::vector<float> gyro_x_dps;
    std::vector<float> gyro_y_dps;
    std::vector<float> gyro_z_dps;
    std::vector<std::uint8_t> satellites;
    std::vector<std::int32_t> raw_lap;

    [[nodiscard]] std::size_t size() const noexcept { return time_us.size(); }
    [[nodiscard]] bool empty() const noexcept { return time_us.empty(); }
    void reserve(std::size_t count);
    void validate() const;
};

struct RadioSeries {
    std::vector<Timestamp> elapsed_us;
    std::vector<float> steering_percent;
    std::vector<float> trigger_percent;
    std::vector<float> voltage;
    std::string filename;
    std::optional<std::int64_t> filename_time_us;

    [[nodiscard]] std::size_t size() const noexcept { return elapsed_us.size(); }
    [[nodiscard]] bool empty() const noexcept { return elapsed_us.empty(); }
    [[nodiscard]] Timestamp duration_us() const noexcept;
    void reserve(std::size_t count);
    void validate() const;
};

struct LapInfo {
    std::int32_t raw_lap{};
    std::int32_t race_lap{};
    std::size_t begin_index{};
    std::size_t end_index{};
    Timestamp duration_us{};
    LapPhase phase{LapPhase::Invalid};
};

struct PhysicalMarker {
    double latitude{};
    double longitude{};
    double reference_fraction{};
};

struct PhysicalLine {
    PhysicalMarker a;
    PhysicalMarker b;
};

struct SectorResult {
    std::int32_t sector{};
    Timestamp duration_us{};
    std::int32_t source_raw_lap{};
    std::size_t begin_index{};
    std::size_t end_index{};
};

struct TheoreticalBest {
    Timestamp duration_us{};
    std::vector<SectorResult> sectors;
};

struct AlignmentResult {
    bool compatible{};
    bool used_end_anchor{};
    Timestamp radio_anchor_us{};
    Timestamp fine_correction_us{};
    Timestamp throttle_response_us{180000};
    Timestamp steering_response_us{220000};
    int trigger_sign{1};
    int steering_sign{1};
    double merge_confidence{};
    double trigger_correlation{};
    double direction_agreement{};
    double steering_yaw_correlation{};
    double lap_steering_correlation{};
    std::string confidence{"none"};
    std::string reason;
};

struct MapBackground {
    std::filesystem::path image_path;
    // Legacy panel-relative placement for uncalibrated images only.
    float offset_x{};
    float offset_y{};
    float scale_x{1.0F};
    float scale_y{1.0F};
    float opacity{0.72F};
    bool locked{};
    bool georeferenced{};
    // Calibrated-image translation in the same east/north metre space as GPS.
    float offset_east_m{};
    float offset_north_m{};
    double reference_latitude{};
    double reference_longitude{};
    float reference_pixel_x{};
    float reference_pixel_y{};
    float metres_per_pixel{};
    float rotation_degrees{};
    // Optional display-only source rectangle in full-image pixels. Zero or an
    // invalid rectangle means the whole image, preserving old archives and
    // generic uncalibrated imports.
    float source_crop_left_px{};
    float source_crop_top_px{};
    float source_crop_right_px{};
    float source_crop_bottom_px{};
};

struct Session {
    std::string name;
    std::filesystem::path vbo_path;
    std::filesystem::path csv_path;
    std::filesystem::path sanwa_path;
    TelemetrySeries telemetry;
    RadioSeries radio;
    std::vector<LapInfo> laps;
    std::optional<PhysicalLine> start_finish_line;
    std::vector<PhysicalMarker> sector_markers;
    TheoreticalBest theoretical_best;
    AlignmentResult alignment;
    MapBackground map_background;
    // Native workspace state (lap roles, driver-analysis rules/corners, notes and
    // annotations). Kept as versioned JSON so archives remain forward compatible.
    std::string workspace_state_json;
};

struct RadioSample {
    bool valid{};
    float throttle{};
    float brake{};
    float steering{};
};

}  // namespace racebox
