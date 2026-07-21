#pragma once

#include "racebox/session.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace racebox {

struct LoadRequest {
    std::filesystem::path vbo;
    std::filesystem::path racebox_csv;
    std::filesystem::path sanwa_csv;
    std::filesystem::path gpx;
};

struct LoadResult {
    Session session;
    std::vector<std::string> diagnostics;
};

struct MapPixel {
    double x{};
    double y{};
};

struct MapCoordinate {
    double latitude{};
    double longitude{};
};

struct MapPixelBounds {
    double left{};
    double top{};
    double right{};
    double bottom{};
};

TelemetrySeries parse_vbo(const std::filesystem::path& path, std::vector<std::string>& diagnostics);
TelemetrySeries parse_racebox_csv(const std::filesystem::path& path, std::vector<std::string>& diagnostics);
TelemetrySeries parse_gpx(const std::filesystem::path& path, std::vector<std::string>& diagnostics);
RadioSeries parse_sanwa_csv(const std::filesystem::path& path, std::vector<std::string>& diagnostics);
LoadResult load_session(const LoadRequest& request);

void assign_directed_laps(TelemetrySeries& telemetry, const PhysicalMarker& line_a, const PhysicalMarker& line_b,
                          std::vector<std::string>& diagnostics);
bool apply_start_finish_line(Session& session, const PhysicalMarker& line_a, const PhysicalMarker& line_b,
                             std::vector<std::string>& diagnostics);
std::vector<LapInfo> build_lap_index(const TelemetrySeries& telemetry);
std::vector<PhysicalMarker> default_sector_markers(const Session& session, int sector_count = 3);
bool move_sector_marker(Session& session, std::size_t marker_index, double reference_fraction);
TheoreticalBest calculate_theoretical_best(const Session& session);
AlignmentResult align_radio(const TelemetrySeries& telemetry, const RadioSeries& radio);
RadioSample sample_radio(const Session& session, Timestamp telemetry_time_us, float deadband = 2.0F);
MapPixel map_coordinate_to_pixel(const MapBackground& background, double latitude, double longitude);
MapCoordinate map_pixel_to_coordinate(const MapBackground& background, double pixel_x, double pixel_y);
MapPixelBounds resolve_map_image_crop(const MapBackground& background, double image_width,
                                      double image_height) noexcept;

bool save_session_archive(const Session& session, const std::filesystem::path& destination, std::string& error);
bool save_lap_archive(const Session& session, const LapInfo& lap, const std::filesystem::path& destination, std::string& error);
bool load_session_archive(const std::filesystem::path& source, Session& session, std::string& error);
bool export_lap_csv(const Session& session, const LapInfo& lap, const std::filesystem::path& destination, std::string& error);

std::filesystem::path settings_directory();
std::filesystem::path log_directory();
void write_log(std::string_view message);

}  // namespace racebox
