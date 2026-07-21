#include "racebox/core.hpp"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <span>
#include <stdexcept>

namespace racebox {
namespace {

using json = nlohmann::json;

template <typename T>
void append_value(std::vector<std::byte>& output, const T& value) {
    const auto* begin = reinterpret_cast<const std::byte*>(&value);
    output.insert(output.end(), begin, begin + sizeof(T));
}

template <typename T>
void append_vector(std::vector<std::byte>& output, const std::vector<T>& values) {
    const auto count = static_cast<std::uint64_t>(values.size());
    append_value(output, count);
    if (!values.empty()) {
        const auto* begin = reinterpret_cast<const std::byte*>(values.data());
        output.insert(output.end(), begin, begin + values.size() * sizeof(T));
    }
}

template <typename T>
bool read_value(std::span<const std::byte>& input, T& value) {
    if (input.size() < sizeof(T)) return false;
    std::memcpy(&value, input.data(), sizeof(T));
    input = input.subspan(sizeof(T));
    return true;
}

template <typename T>
bool read_vector(std::span<const std::byte>& input, std::vector<T>& values) {
    std::uint64_t count = 0;
    if (!read_value(input, count) || count > 100'000'000 || input.size() < count * sizeof(T)) return false;
    values.resize(static_cast<std::size_t>(count));
    if (count) std::memcpy(values.data(), input.data(), static_cast<std::size_t>(count) * sizeof(T));
    input = input.subspan(static_cast<std::size_t>(count) * sizeof(T));
    return true;
}

std::vector<std::byte> encode_telemetry(const TelemetrySeries& telemetry) {
    std::vector<std::byte> output;
    output.reserve(telemetry.size() * 64);
    append_value(output, std::uint32_t{0x44584252});
    append_value(output, std::uint32_t{1});
    append_vector(output, telemetry.time_us); append_vector(output, telemetry.absolute_time_us);
    append_vector(output, telemetry.latitude); append_vector(output, telemetry.longitude);
    append_vector(output, telemetry.speed_kmh); append_vector(output, telemetry.heading_deg);
    append_vector(output, telemetry.altitude_m); append_vector(output, telemetry.longitudinal_g);
    append_vector(output, telemetry.lateral_g); append_vector(output, telemetry.satellites); append_vector(output, telemetry.raw_lap);
    return output;
}

bool decode_telemetry(std::span<const std::byte> input, TelemetrySeries& telemetry) {
    std::uint32_t magic = 0, version = 0;
    if (!read_value(input, magic) || !read_value(input, version) || magic != 0x44584252 || version != 1) return false;
    return read_vector(input, telemetry.time_us) && read_vector(input, telemetry.absolute_time_us) &&
           read_vector(input, telemetry.latitude) && read_vector(input, telemetry.longitude) &&
           read_vector(input, telemetry.speed_kmh) && read_vector(input, telemetry.heading_deg) &&
           read_vector(input, telemetry.altitude_m) && read_vector(input, telemetry.longitudinal_g) &&
           read_vector(input, telemetry.lateral_g) && read_vector(input, telemetry.satellites) && read_vector(input, telemetry.raw_lap);
}

std::vector<std::byte> encode_radio(const RadioSeries& radio) {
    std::vector<std::byte> output;
    append_value(output, std::uint32_t{0x52584252});
    append_value(output, std::uint32_t{1});
    append_vector(output, radio.elapsed_us); append_vector(output, radio.steering_percent);
    append_vector(output, radio.trigger_percent); append_vector(output, radio.voltage);
    return output;
}

bool decode_radio(std::span<const std::byte> input, RadioSeries& radio) {
    std::uint32_t magic = 0, version = 0;
    return read_value(input, magic) && read_value(input, version) && magic == 0x52584252 && version == 1 &&
           read_vector(input, radio.elapsed_us) && read_vector(input, radio.steering_percent) &&
           read_vector(input, radio.trigger_percent) && read_vector(input, radio.voltage);
}

json manifest_for(const Session& session, std::string_view format) {
    json manifest = {
        {"format", format}, {"version", 1}, {"name", session.name},
        {"workspace_state_json", session.workspace_state_json},
        {"alignment", {
            {"compatible", session.alignment.compatible}, {"anchor_us", session.alignment.radio_anchor_us},
            {"correction_us", session.alignment.fine_correction_us}, {"trigger_sign", session.alignment.trigger_sign},
            {"steering_sign", session.alignment.steering_sign}, {"steering_response_us", session.alignment.steering_response_us},
            {"throttle_response_us", session.alignment.throttle_response_us},
            {"merge_confidence", session.alignment.merge_confidence},
            {"trigger_correlation", session.alignment.trigger_correlation},
            {"direction_agreement", session.alignment.direction_agreement},
            {"steering_yaw_correlation", session.alignment.steering_yaw_correlation},
            {"lap_steering_correlation", session.alignment.lap_steering_correlation},
            {"confidence", session.alignment.confidence}, {"reason", session.alignment.reason}}},
        {"map", {{"offset_x", session.map_background.offset_x}, {"offset_y", session.map_background.offset_y},
                 {"scale_x", session.map_background.scale_x}, {"scale_y", session.map_background.scale_y},
                 {"opacity", session.map_background.opacity},
                 {"locked", session.map_background.locked},
                 {"georeferenced", session.map_background.georeferenced},
                 {"offset_east_m", session.map_background.offset_east_m},
                 {"offset_north_m", session.map_background.offset_north_m},
                 {"reference_latitude", session.map_background.reference_latitude},
                 {"reference_longitude", session.map_background.reference_longitude},
                 {"reference_pixel_x", session.map_background.reference_pixel_x},
                 {"reference_pixel_y", session.map_background.reference_pixel_y},
                 {"metres_per_pixel", session.map_background.metres_per_pixel},
                 {"rotation_degrees", session.map_background.rotation_degrees},
                 {"source_crop_left_px", session.map_background.source_crop_left_px},
                 {"source_crop_top_px", session.map_background.source_crop_top_px},
                 {"source_crop_right_px", session.map_background.source_crop_right_px},
                 {"source_crop_bottom_px", session.map_background.source_crop_bottom_px}}}
    };
    if (session.start_finish_line) {
        const auto& line = *session.start_finish_line;
        manifest["start_finish_line"] = {
            {"a", {line.a.latitude, line.a.longitude}},
            {"b", {line.b.latitude, line.b.longitude}}
        };
    }
    for (const auto& lap : session.laps) manifest["laps"].push_back({lap.raw_lap, lap.race_lap, lap.begin_index, lap.end_index, lap.duration_us, static_cast<int>(lap.phase)});
    for (const auto& marker : session.sector_markers) manifest["markers"].push_back({marker.latitude, marker.longitude, marker.reference_fraction});
    return manifest;
}

bool add_file(mz_zip_archive& archive, std::string_view name, std::span<const std::byte> data) {
    return mz_zip_writer_add_mem(&archive, std::string(name).c_str(), data.data(), data.size(), MZ_BEST_SPEED) != 0;
}

std::vector<std::byte> archive_file(mz_zip_archive& archive, const char* name) {
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&archive, name, &size, 0);
    if (!data) return {};
    std::vector<std::byte> output(size);
    std::memcpy(output.data(), data, size);
    mz_free(data);
    return output;
}

}  // namespace

bool save_archive(const Session& session, const std::filesystem::path& destination, std::string_view format, std::string& error) {
    try {
        const auto telemetry = encode_telemetry(session.telemetry);
        const auto radio = encode_radio(session.radio);
        auto manifest = manifest_for(session, format);
        if (!session.map_background.image_path.empty()) {
            manifest["map"]["image_entry"] = "background" + session.map_background.image_path.extension().string();
        }
        const auto manifest_text = manifest.dump(2);
        mz_zip_archive archive{};
        if (!mz_zip_writer_init_file(&archive, destination.string().c_str(), 0)) throw std::runtime_error("Could not create archive");
        const auto manifest_bytes = std::as_bytes(std::span(manifest_text));
        bool ok = add_file(archive, "manifest.json", manifest_bytes) && add_file(archive, "telemetry.bin", telemetry) && add_file(archive, "radio.bin", radio);
        if (ok && !session.map_background.image_path.empty() && std::filesystem::exists(session.map_background.image_path)) {
            std::ifstream image(session.map_background.image_path, std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(image)), {});
            const auto extension = session.map_background.image_path.extension().string();
            ok = add_file(archive, "background" + extension, std::as_bytes(std::span(bytes)));
        }
        ok = ok && mz_zip_writer_finalize_archive(&archive) != 0;
        mz_zip_writer_end(&archive);
        if (!ok) throw std::runtime_error("Archive write failed");
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool save_session_archive(const Session& session, const std::filesystem::path& destination, std::string& error) {
    return save_archive(session, destination, "racebox-session", error);
}

bool save_lap_archive(const Session& session, const LapInfo& lap, const std::filesystem::path& destination, std::string& error) {
    if (lap.end_index < lap.begin_index || lap.end_index >= session.telemetry.size()) {
        error = "Lap range is invalid";
        return false;
    }
    Session output;
    output.name = session.name + " - lap " + std::to_string(lap.raw_lap);
    output.alignment = session.alignment;
    output.map_background = session.map_background;
    output.start_finish_line = session.start_finish_line;
    output.workspace_state_json = session.workspace_state_json;
    output.sector_markers = session.sector_markers;
    const auto begin = lap.begin_index;
    const auto end = lap.end_index + 1;
    const auto copy = [begin, end](auto& destination_values, const auto& source_values) {
        destination_values.assign(source_values.begin() + static_cast<std::ptrdiff_t>(begin),
                                  source_values.begin() + static_cast<std::ptrdiff_t>(end));
    };
    copy(output.telemetry.time_us, session.telemetry.time_us);
    copy(output.telemetry.absolute_time_us, session.telemetry.absolute_time_us);
    copy(output.telemetry.latitude, session.telemetry.latitude);
    copy(output.telemetry.longitude, session.telemetry.longitude);
    copy(output.telemetry.speed_kmh, session.telemetry.speed_kmh);
    copy(output.telemetry.heading_deg, session.telemetry.heading_deg);
    copy(output.telemetry.altitude_m, session.telemetry.altitude_m);
    copy(output.telemetry.longitudinal_g, session.telemetry.longitudinal_g);
    copy(output.telemetry.lateral_g, session.telemetry.lateral_g);
    copy(output.telemetry.satellites, session.telemetry.satellites);
    copy(output.telemetry.raw_lap, session.telemetry.raw_lap);
    output.laps.push_back({lap.raw_lap, lap.race_lap, 0, output.telemetry.size() - 1, lap.duration_us, LapPhase::Complete});

    const auto radio_begin_time = session.telemetry.time_us[lap.begin_index] - session.alignment.radio_anchor_us - session.alignment.fine_correction_us;
    const auto radio_end_time = session.telemetry.time_us[lap.end_index] - session.alignment.radio_anchor_us - session.alignment.fine_correction_us;
    const auto radio_begin = std::lower_bound(session.radio.elapsed_us.begin(), session.radio.elapsed_us.end(), radio_begin_time);
    const auto radio_end = std::upper_bound(session.radio.elapsed_us.begin(), session.radio.elapsed_us.end(), radio_end_time);
    const auto radio_first = static_cast<std::size_t>(std::distance(session.radio.elapsed_us.begin(), radio_begin));
    const auto radio_last = static_cast<std::size_t>(std::distance(session.radio.elapsed_us.begin(), radio_end));
    if (radio_first < radio_last) {
        output.radio.elapsed_us.assign(radio_begin, radio_end);
        output.radio.steering_percent.assign(session.radio.steering_percent.begin() + static_cast<std::ptrdiff_t>(radio_first), session.radio.steering_percent.begin() + static_cast<std::ptrdiff_t>(radio_last));
        output.radio.trigger_percent.assign(session.radio.trigger_percent.begin() + static_cast<std::ptrdiff_t>(radio_first), session.radio.trigger_percent.begin() + static_cast<std::ptrdiff_t>(radio_last));
        output.radio.voltage.assign(session.radio.voltage.begin() + static_cast<std::ptrdiff_t>(radio_first), session.radio.voltage.begin() + static_cast<std::ptrdiff_t>(radio_last));
    }
    output.theoretical_best = calculate_theoretical_best(output);
    return save_archive(output, destination, "racebox-lap", error);
}

bool load_session_archive(const std::filesystem::path& source, Session& session, std::string& error) {
    try {
        mz_zip_archive archive{};
        if (!mz_zip_reader_init_file(&archive, source.string().c_str(), 0)) throw std::runtime_error("Could not open archive");
        const auto manifest_bytes = archive_file(archive, "manifest.json");
        const auto telemetry_bytes = archive_file(archive, "telemetry.bin");
        const auto radio_bytes = archive_file(archive, "radio.bin");
        if (manifest_bytes.empty() || telemetry_bytes.empty()) {
            mz_zip_reader_end(&archive);
            throw std::runtime_error("Archive is missing required files");
        }
        const std::string manifest_text(reinterpret_cast<const char*>(manifest_bytes.data()), manifest_bytes.size());
        const auto manifest = json::parse(manifest_text);
        const auto format = manifest.value("format", "");
        if ((format != "racebox-session" && format != "racebox-lap") || manifest.value("version", 0) != 1) throw std::runtime_error("Unsupported archive version");
        session = {};
        session.name = manifest.value("name", "Saved session");
        session.workspace_state_json = manifest.value("workspace_state_json", std::string{});
        if (!decode_telemetry(telemetry_bytes, session.telemetry)) throw std::runtime_error("Telemetry payload is corrupt");
        if (!radio_bytes.empty() && !decode_radio(radio_bytes, session.radio)) throw std::runtime_error("Radio payload is corrupt");
        for (const auto& value : manifest.value("laps", json::array())) {
            session.laps.push_back({value[0], value[1], value[2], value[3], value[4], static_cast<LapPhase>(value[5].get<int>())});
        }
        for (const auto& value : manifest.value("markers", json::array())) session.sector_markers.push_back({value[0], value[1], value[2]});
        if (const auto line = manifest.find("start_finish_line"); line != manifest.end() && line->is_object()) {
            const auto a = line->value("a", json::array());
            const auto b = line->value("b", json::array());
            if (a.size() >= 2 && b.size() >= 2) {
                session.start_finish_line = PhysicalLine{
                    PhysicalMarker{a[0].get<double>(), a[1].get<double>(), 0.0},
                    PhysicalMarker{b[0].get<double>(), b[1].get<double>(), 0.0}};
            }
        }
        const auto alignment = manifest.at("alignment");
        session.alignment.compatible = alignment.value("compatible", false);
        session.alignment.radio_anchor_us = alignment.value("anchor_us", 0LL);
        session.alignment.fine_correction_us = alignment.value("correction_us", 0LL);
        session.alignment.trigger_sign = alignment.value("trigger_sign", 1);
        session.alignment.steering_sign = alignment.value("steering_sign", 1);
        session.alignment.throttle_response_us = alignment.value("throttle_response_us", 180000LL);
        session.alignment.steering_response_us = alignment.value("steering_response_us", 220000LL);
        session.alignment.merge_confidence = alignment.value("merge_confidence", 0.0);
        session.alignment.trigger_correlation = alignment.value("trigger_correlation", 0.0);
        session.alignment.direction_agreement = alignment.value("direction_agreement", 0.0);
        session.alignment.steering_yaw_correlation = alignment.value("steering_yaw_correlation", 0.0);
        session.alignment.lap_steering_correlation = alignment.value("lap_steering_correlation", 0.0);
        session.alignment.confidence = alignment.value("confidence", "none");
        session.alignment.reason = alignment.value("reason", "Loaded archive");
        if (manifest.contains("map")) {
            const auto map = manifest.at("map");
            session.map_background.offset_x = map.value("offset_x", 0.0F);
            session.map_background.offset_y = map.value("offset_y", 0.0F);
            session.map_background.scale_x = map.value("scale_x", 1.0F);
            session.map_background.scale_y = map.value("scale_y", 1.0F);
            session.map_background.opacity = map.value("opacity", 0.72F);
            session.map_background.locked = map.value("locked", false);
            session.map_background.georeferenced = map.value("georeferenced", false);
            session.map_background.offset_east_m = map.value("offset_east_m", 0.0F);
            session.map_background.offset_north_m = map.value("offset_north_m", 0.0F);
            session.map_background.reference_latitude = map.value("reference_latitude", 0.0);
            session.map_background.reference_longitude = map.value("reference_longitude", 0.0);
            session.map_background.reference_pixel_x = map.value("reference_pixel_x", 0.0F);
            session.map_background.reference_pixel_y = map.value("reference_pixel_y", 0.0F);
            session.map_background.metres_per_pixel = map.value("metres_per_pixel", 0.0F);
            session.map_background.rotation_degrees = map.value("rotation_degrees", 0.0F);
            session.map_background.source_crop_left_px = map.value("source_crop_left_px", 0.0F);
            session.map_background.source_crop_top_px = map.value("source_crop_top_px", 0.0F);
            session.map_background.source_crop_right_px = map.value("source_crop_right_px", 0.0F);
            session.map_background.source_crop_bottom_px = map.value("source_crop_bottom_px", 0.0F);
            if (session.map_background.georeferenced) {
                session.map_background.scale_x = 1.0F;
                session.map_background.scale_y = 1.0F;
            } else {
                const auto uniform_scale = std::sqrt(std::max(0.01F,
                    session.map_background.scale_x * session.map_background.scale_y));
                session.map_background.scale_x = uniform_scale;
                session.map_background.scale_y = uniform_scale;
            }
            const auto entry = map.value("image_entry", "");
            if (!entry.empty()) {
                const auto bytes = archive_file(archive, entry.c_str());
                if (!bytes.empty()) {
                    const auto cache = settings_directory() / L"map-cache";
                    std::filesystem::create_directories(cache);
                    const auto extension = std::filesystem::path(entry).extension();
                    const auto filename = std::to_wstring(std::hash<std::wstring>{}(source.wstring())) + extension.wstring();
                    const auto image_path = cache / filename;
                    std::ofstream image(image_path, std::ios::binary);
                    image.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    session.map_background.image_path = image_path;
                }
            }
        }
        session.theoretical_best = calculate_theoretical_best(session);
        mz_zip_reader_end(&archive);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool export_lap_csv(const Session& session, const LapInfo& lap, const std::filesystem::path& destination, std::string& error) {
    std::ofstream output(destination);
    if (!output) {
        error = "Could not create CSV";
        return false;
    }
    output << "TimeSeconds,Latitude,Longitude,SpeedKmh,LateralG,LongitudinalG,Throttle,Brake,Steering\n";
    output << std::fixed << std::setprecision(6);
    for (auto index = lap.begin_index; index <= lap.end_index; ++index) {
        const auto radio = sample_radio(session, session.telemetry.time_us[index]);
        output << static_cast<double>(session.telemetry.time_us[index] - session.telemetry.time_us[lap.begin_index]) / 1'000'000.0 << ','
               << session.telemetry.latitude[index] << ',' << session.telemetry.longitude[index] << ','
               << session.telemetry.speed_kmh[index] << ',' << session.telemetry.lateral_g[index] << ','
               << session.telemetry.longitudinal_g[index] << ',' << radio.throttle << ',' << radio.brake << ',' << radio.steering << '\n';
    }
    return true;
}

}  // namespace racebox
