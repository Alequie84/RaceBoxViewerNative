#include "racebox/core.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void close_to(double actual, double expected, double tolerance, const char* message) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
        throw std::runtime_error(message);
    }
}

double seconds(racebox::Timestamp value) { return static_cast<double>(value) / 1'000'000.0; }

}  // namespace

int main() {
    try {
        racebox::MapBackground map_calibration;
        map_calibration.georeferenced = true;
        map_calibration.reference_latitude = 49.18407486000555;
        map_calibration.reference_longitude = -123.14511168306714;
        map_calibration.reference_pixel_x = 494.5F;
        map_calibration.reference_pixel_y = 256.5F;
        map_calibration.metres_per_pixel = 0.09748007033810534F;
        map_calibration.rotation_degrees = 0.09155874851718505F;
        map_calibration.source_crop_left_px = 138.0F;
        map_calibration.source_crop_top_px = 94.0F;
        map_calibration.source_crop_right_px = 837.0F;
        map_calibration.source_crop_bottom_px = 409.0F;
        {
            const auto anchor_a = racebox::map_coordinate_to_pixel(map_calibration, 49.184236, -123.145111);
            const auto anchor_b = racebox::map_coordinate_to_pixel(map_calibration, 49.184060, -123.145634);
            const auto anchor_c = racebox::map_coordinate_to_pixel(map_calibration, 49.184003, -123.144682);
            close_to(anchor_a.x, 495.0, 1.0, "Google map anchor A shifted");
            close_to(anchor_a.y, 73.0, 1.0, "Google map anchor A shifted vertically");
            close_to(anchor_b.x, 105.0, 1.0, "Google map anchor B shifted");
            close_to(anchor_b.y, 273.0, 1.0, "Google map anchor B shifted vertically");
            close_to(anchor_c.x, 815.0, 1.0, "Google map anchor C shifted");
            close_to(anchor_c.y, 339.0, 1.0, "Google map anchor C shifted vertically");

            const auto right = racebox::map_pixel_to_coordinate(map_calibration, 594.5, 256.5);
            const auto down = racebox::map_pixel_to_coordinate(map_calibration, 494.5, 356.5);
            const auto longitude_metres = 111'320.0 * std::cos(map_calibration.reference_latitude * 3.14159265358979323846 / 180.0);
            const auto distance_from_reference = [&](const racebox::MapCoordinate& coordinate) {
                const auto east = (coordinate.longitude - map_calibration.reference_longitude) * longitude_metres;
                const auto north = (coordinate.latitude - map_calibration.reference_latitude) * 110'540.0;
                return std::hypot(east, north);
            };
            close_to(distance_from_reference(right), 100.0 * map_calibration.metres_per_pixel, 0.0001,
                     "Map horizontal scale is not metric");
            close_to(distance_from_reference(down), 100.0 * map_calibration.metres_per_pixel, 0.0001,
                     "Map vertical scale is not metric");

            const auto crop = racebox::resolve_map_image_crop(map_calibration, 989.0, 513.0);
            close_to(crop.left, 138.0, 0.001, "Map display crop changed at left edge");
            close_to(crop.top, 94.0, 0.001, "Map display crop changed at top edge");
            close_to(crop.right, 837.0, 0.001, "Map display crop changed at right edge");
            close_to(crop.bottom, 409.0, 0.001, "Map display crop changed at bottom edge");
            close_to(crop.right - crop.left, 699.0, 0.001, "Map display crop width changed");
            close_to(crop.bottom - crop.top, 315.0, 0.001, "Map display crop height changed");

            racebox::MapBackground uncropped;
            const auto full = racebox::resolve_map_image_crop(uncropped, 989.0, 513.0);
            close_to(full.left, 0.0, 0.001, "Unspecified crop did not retain full image");
            close_to(full.top, 0.0, 0.001, "Unspecified crop did not retain full image vertically");
            close_to(full.right, 989.0, 0.001, "Unspecified crop changed full image width");
            close_to(full.bottom, 513.0, 0.001, "Unspecified crop changed full image height");

            auto invalid_crop = map_calibration;
            invalid_crop.source_crop_left_px = 900.0F;
            invalid_crop.source_crop_right_px = 100.0F;
            const auto invalid = racebox::resolve_map_image_crop(invalid_crop, 989.0, 513.0);
            close_to(invalid.right, 989.0, 0.001, "Invalid crop did not fail safely to the full image");
        }
        {
            const auto temporary = std::filesystem::temp_directory_path() / L"racebox-leading-zero-clock.vbo";
            std::ofstream file(temporary);
            file << "[data]\n002559.90 1 2 3 4 5 6 7 0 0 0 0 8\n002600.10 1 2 3 4 5 6 7 0 0 0 0 8\n";
            file.close();
            std::vector<std::string> diagnostics;
            const auto parsed = racebox::parse_vbo(temporary, diagnostics);
            require(parsed.size() == 2, "Leading-zero VBO clock fixture did not parse");
            close_to(seconds(parsed.time_us[1]), 0.2, 0.001, "VBO minute rollover created a false gap");
            std::filesystem::remove(temporary);
        }
        {
            const auto temporary = std::filesystem::temp_directory_path() / L"racebox-midnight-clock.vbo";
            std::ofstream file(temporary);
            file << "[data]\n235959.90 1 2 3 4 5 6 7 0 0 0 0 8\n000000.10 1 2 3 4 5 6 7 0 0 0 0 8\n";
            file.close();
            std::vector<std::string> diagnostics;
            const auto parsed = racebox::parse_vbo(temporary, diagnostics);
            require(parsed.size() == 2, "Midnight VBO fixture did not parse");
            close_to(seconds(parsed.time_us[1]), 0.2, 0.001, "VBO midnight rollover created a false gap");
            std::filesystem::remove(temporary);
        }
        {
            const auto temporary = std::filesystem::temp_directory_path() / L"racebox-native-test.gpx";
            std::ofstream file(temporary);
            file << "<gpx><trk><trkseg>"
                 << "<trkpt lat=\"49.0\" lon=\"-123.0\"><ele>2.0</ele><time>2026-07-11T16:00:00.000Z</time></trkpt>"
                 << "<trkpt lat=\"49.00001\" lon=\"-123.0\"><ele>2.1</ele><time>2026-07-11T16:00:00.100Z</time></trkpt>"
                 << "</trkseg></trk></gpx>";
            file.close();
            std::vector<std::string> diagnostics;
            const auto parsed = racebox::parse_gpx(temporary, diagnostics);
            require(parsed.size() == 2, "GPX parser did not retain track points");
            require(parsed.speed_kmh[1] > 1.0F, "GPX speed derivation failed");
            std::filesystem::remove(temporary);
        }
        const std::filesystem::path golden = GOLDEN_DIR;
        const auto loaded = racebox::load_session({golden / L"session.vbo", golden / L"session.csv", golden / L"sanwa.csv"});
        const auto& session = loaded.session;
        require(session.telemetry.size() == 13'863, "RaceBox sample count changed");
        require(*std::max_element(session.telemetry.vertical_g.begin(), session.telemetry.vertical_g.end()) > 1.3F,
                "VBO vertical acceleration was not retained");
        require(*std::max_element(session.telemetry.gyro_z_dps.begin(), session.telemetry.gyro_z_dps.end()) > 200.0F,
                "VBO gyroscope data was not retained");
        require(loaded.imu_analysis.available, "Golden RaceBox IMU was not detected");
        require(loaded.imu_analysis.initial_stationary_zero_used, "Golden initial stationary zero was not found");
        require(loaded.imu_analysis.zero_begin_index == 104, "Golden stationary calibration start changed");
        close_to(loaded.imu_analysis.acceleration_zero_g[0], -0.017, 0.003,
                 "Golden longitudinal zero changed");
        close_to(loaded.imu_analysis.acceleration_zero_g[1], 0.051, 0.003,
                 "Golden lateral zero changed");
        close_to(loaded.imu_analysis.acceleration_zero_g[2], 0.994, 0.003,
                 "Golden vertical zero changed");
        require(loaded.imu_analysis.calibration.valid, "Golden vehicle-yaw calibration is not reliable");
        require(loaded.imu_analysis.calibration.heading_correlation > 0.75,
                "Golden vehicle-yaw calibration correlation is too weak");
        require(session.radio.size() == 43'199, "Sanwa sample count changed");
        require(session.laps.size() == 20, "Lap count changed");
        require(session.start_finish_line.has_value(), "VBO start/finish line was not retained");
        close_to(seconds(session.alignment.radio_anchor_us), 123.780, 0.002, "Radio anchor changed");
        close_to(static_cast<double>(session.alignment.fine_correction_us) / 1000.0, -399.0, 3.0, "Fine correction changed");
        require(session.alignment.trigger_correlation > 0.32, "Trigger correlation is too weak");
        require(std::abs(session.alignment.steering_yaw_correlation) > 0.55, "Steering/yaw correlation is too weak");
        require(session.alignment.lap_steering_correlation > 0.70, "Lap steering correlation is too weak");
        const auto lap_17 = std::find_if(session.laps.begin(), session.laps.end(), [](const racebox::LapInfo& lap) { return lap.raw_lap == 17; });
        require(lap_17 != session.laps.end(), "Raw lap 17 missing");
        close_to(seconds(lap_17->duration_us), 16.280, 0.002, "16.280 lap changed");
        require(session.theoretical_best.duration_us > 0, "Theoretical best was not calculated");
        auto reapplied = session;
        std::vector<std::string> placement_diagnostics;
        require(racebox::apply_start_finish_line(reapplied, session.start_finish_line->a,
                    session.start_finish_line->b, placement_diagnostics),
                "Reapplying the known start/finish line failed");
        require(reapplied.laps.size() == session.laps.size(), "Known start/finish line changed the lap count");
        const auto raw_laps_before_rejection = reapplied.telemetry.raw_lap;
        require(!racebox::apply_start_finish_line(reapplied, session.start_finish_line->a,
                    session.start_finish_line->a, placement_diagnostics),
                "A zero-length start/finish line was accepted");
        require(reapplied.telemetry.raw_lap == raw_laps_before_rejection,
                "Rejected start/finish placement changed lap assignments");
        const auto racebox_only = racebox::load_session({golden / L"session.vbo", golden / L"session.csv", {}});
        require(racebox_only.session.telemetry.size() == 13'863, "RaceBox-only load failed");
        require(racebox_only.session.radio.empty(), "RaceBox-only load invented radio samples");

        const auto archive = std::filesystem::temp_directory_path() /
            L"racebox-native-\u03a9-roundtrip.rbxsession";
        std::string error;
        auto archive_session = session;
        archive_session.map_background = map_calibration;
        archive_session.map_background.locked = true;
        archive_session.map_background.offset_east_m = 2.5F;
        archive_session.map_background.offset_north_m = -1.25F;
        archive_session.workspace_state_json = R"({"format":"racebox-native-workspace","version":2,"session_notes":"round trip"})";
        require(racebox::save_session_archive(archive_session, archive, error), error.c_str());
        auto archive_temporary = archive;
        archive_temporary += L".writing";
        require(!std::filesystem::exists(archive_temporary),
                "Successful archive save left its temporary file behind");
        archive_session.workspace_state_json =
            R"({"format":"racebox-native-workspace","version":4,"session_notes":"atomic overwrite"})";
        require(racebox::save_session_archive(archive_session, archive, error),
                "Existing archive could not be replaced atomically");
        racebox::Session restored;
        require(racebox::load_session_archive(archive, restored, error), error.c_str());
        require(restored.telemetry.size() == session.telemetry.size(), "Archive telemetry round trip failed");
        close_to(restored.telemetry.vertical_g[500], session.telemetry.vertical_g[500], 1e-6,
                 "Archive lost vertical acceleration");
        close_to(restored.telemetry.gyro_z_dps[500], session.telemetry.gyro_z_dps[500], 1e-6,
                 "Archive lost gyroscope data");
        require(restored.radio.size() == session.radio.size(), "Archive radio round trip failed");
        require(restored.map_background.georeferenced, "Archive lost the map georeference");
        require(restored.start_finish_line.has_value(), "Archive lost the start/finish line");
        close_to(restored.start_finish_line->a.latitude, archive_session.start_finish_line->a.latitude, 1e-10,
                 "Archive changed start/finish latitude");
        require(restored.workspace_state_json == archive_session.workspace_state_json,
                "Version-2 archive lost its separate workspace document");
        close_to(restored.map_background.reference_latitude, map_calibration.reference_latitude, 1e-10,
                 "Archive changed map latitude");
        close_to(restored.map_background.metres_per_pixel, map_calibration.metres_per_pixel, 1e-8,
                 "Archive changed map scale");
        close_to(restored.map_background.source_crop_left_px, 138.0, 1e-6,
                 "Archive changed map display crop left edge");
        close_to(restored.map_background.source_crop_top_px, 94.0, 1e-6,
                 "Archive changed map display crop top edge");
        close_to(restored.map_background.source_crop_right_px, 837.0, 1e-6,
                 "Archive changed map display crop right edge");
        close_to(restored.map_background.source_crop_bottom_px, 409.0, 1e-6,
                 "Archive changed map display crop bottom edge");
        require(restored.map_background.locked, "Archive lost the map background lock");
        close_to(restored.map_background.offset_east_m, 2.5, 1e-6, "Archive changed map east offset");
        close_to(restored.map_background.offset_north_m, -1.25, 1e-6, "Archive changed map north offset");
        std::filesystem::remove(archive);
        const auto invalid_lap_archive =
            std::filesystem::temp_directory_path() /
            L"racebox-native-invalid-lap.rbxsession";
        auto invalid_lap_session = session;
        invalid_lap_session.map_background = {};
        invalid_lap_session.workspace_state_json.clear();
        invalid_lap_session.laps.front().end_index =
            invalid_lap_session.telemetry.size();
        require(racebox::save_session_archive(
                    invalid_lap_session, invalid_lap_archive, error),
                "Could not create invalid-lap archive fixture");
        racebox::Session rejected_archive;
        require(!racebox::load_session_archive(
                    invalid_lap_archive, rejected_archive, error),
                "Archive with an out-of-range lap was accepted");
        std::filesystem::remove(invalid_lap_archive);
        const auto mismatched_archive =
            std::filesystem::temp_directory_path() /
            L"racebox-native-mismatched-columns.rbxsession";
        auto mismatched_session = session;
        mismatched_session.map_background = {};
        mismatched_session.workspace_state_json.clear();
        mismatched_session.telemetry.speed_kmh.pop_back();
        require(racebox::save_session_archive(
                    mismatched_session, mismatched_archive, error),
                "Could not create mismatched-column archive fixture");
        require(!racebox::load_session_archive(
                    mismatched_archive, rejected_archive, error),
                "Archive with mismatched telemetry columns was accepted");
        std::filesystem::remove(mismatched_archive);
        const auto lap_archive = std::filesystem::temp_directory_path() / L"racebox-native-lap-roundtrip.rbxlap";
        require(racebox::save_lap_archive(session, *lap_17, lap_archive, error), error.c_str());
        racebox::Session restored_lap;
        require(racebox::load_session_archive(lap_archive, restored_lap, error), error.c_str());
        require(restored_lap.laps.size() == 1, "Lap archive contains the wrong lap count");
        close_to(seconds(restored_lap.laps.front().duration_us), 16.280, 0.002, "Lap archive duration changed");
        require(restored_lap.radio.size() > 1'000, "Lap archive did not retain native-rate Sanwa controls");
        require(restored_lap.start_finish_line.has_value(), "Lap archive lost the start/finish line");
        std::filesystem::remove(lap_archive);
        const auto corrupt = std::filesystem::temp_directory_path() / L"racebox-native-corrupt.rbxsession";
        { std::ofstream output(corrupt); output << "not a zip archive"; }
        racebox::Session corrupt_session;
        require(!racebox::load_session_archive(corrupt, corrupt_session, error), "Corrupt archive was accepted");
        std::filesystem::remove(corrupt);
        std::cout << "All RaceBox native golden checks passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failure: " << exception.what() << '\n';
        return 1;
    }
}
