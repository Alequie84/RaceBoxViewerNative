#include "racebox/core.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace racebox {
namespace {

constexpr Timestamp kSecond = 1'000'000;

struct ParsedVbo {
    TelemetrySeries telemetry;
    std::optional<std::pair<PhysicalMarker, PhysicalMarker>> start_line;
};

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n'");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::vector<std::string> split(const std::string& line, char separator, bool preserve_empty = true) {
    std::vector<std::string> values;
    std::string value;
    std::istringstream stream(line);
    while (std::getline(stream, value, separator)) {
        if (preserve_empty || !value.empty()) values.push_back(trim(value));
    }
    if (preserve_empty && !line.empty() && line.back() == separator) values.emplace_back();
    return values;
}

double number(const std::string& value, double fallback = 0.0) {
    try {
        std::size_t used = 0;
        const auto parsed = std::stod(trim(value), &used);
        return used ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

Timestamp duration_us(const std::string& input) {
    const auto parts = split(trim(input), ':');
    if (parts.size() != 3) return -1;
    return static_cast<Timestamp>(std::llround((number(parts[0]) * 3600.0 + number(parts[1]) * 60.0 + number(parts[2])) * kSecond));
}

double clock_seconds(const std::string& token) {
    auto text = trim(token);
    if (text.empty()) return -1.0;
    const auto dot = text.find('.');
    auto whole = dot == std::string::npos ? text : text.substr(0, dot);
    if (!std::all_of(whole.begin(), whole.end(), [](unsigned char value) { return std::isdigit(value) != 0; })) return -1.0;
    if (whole.size() < 6) whole.insert(whole.begin(), 6 - whole.size(), '0');
    if (whole.size() > 6) return -1.0;
    const auto hours = std::stoi(whole.substr(0, 2));
    const auto minutes = std::stoi(whole.substr(2, 2));
    const auto seconds = std::stoi(whole.substr(4, 2));
    if (hours > 23 || minutes > 59 || seconds > 59) return -1.0;
    double fraction = 0.0;
    if (dot != std::string::npos) fraction = number("0" + text.substr(dot), 0.0);
    return static_cast<double>(hours * 3600 + minutes * 60 + seconds) + fraction;
}

double pearson(const std::vector<float>& left, const std::vector<float>& right) {
    const auto count = std::min(left.size(), right.size());
    if (count < 3) return 0.0;
    double left_sum = 0.0, right_sum = 0.0;
    for (std::size_t index = 0; index < count; ++index) { left_sum += left[index]; right_sum += right[index]; }
    const auto left_mean = left_sum / count;
    const auto right_mean = right_sum / count;
    double covariance = 0.0, left_energy = 0.0, right_energy = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto a = left[index] - left_mean;
        const auto b = right[index] - right_mean;
        covariance += a * b;
        left_energy += a * a;
        right_energy += b * b;
    }
    const auto denominator = std::sqrt(left_energy * right_energy);
    return denominator > 1e-12 ? covariance / denominator : 0.0;
}

std::size_t nearest_time(const std::vector<Timestamp>& times, Timestamp value) {
    const auto iterator = std::lower_bound(times.begin(), times.end(), value);
    if (iterator == times.begin()) return 0;
    if (iterator == times.end()) return times.size() - 1;
    const auto upper = static_cast<std::size_t>(std::distance(times.begin(), iterator));
    return value - times[upper - 1] <= times[upper] - value ? upper - 1 : upper;
}

std::int64_t iso_epoch_us(const std::string& input) {
    if (input.size() < 19) return 0;
    std::tm tm{};
    std::istringstream stream(input.substr(0, 19));
    stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) return 0;
    const auto epoch = _mkgmtime64(&tm);
    if (epoch < 0) return 0;
    std::int64_t fractional = 0;
    const auto dot = input.find('.', 19);
    if (dot != std::string::npos) {
        std::string digits;
        for (auto index = dot + 1; index < input.size() && std::isdigit(static_cast<unsigned char>(input[index])); ++index) digits.push_back(input[index]);
        while (digits.size() < 6) digits.push_back('0');
        if (digits.size() > 6) digits.resize(6);
        fractional = digits.empty() ? 0 : std::stoll(digits);
    }
    return epoch * kSecond + fractional;
}

std::optional<std::string> xml_value(std::string_view block, std::string_view name) {
    const auto open = std::string("<") + std::string(name) + ">";
    const auto close = std::string("</") + std::string(name) + ">";
    const auto begin = block.find(open);
    if (begin == std::string_view::npos) return std::nullopt;
    const auto value_begin = begin + open.size();
    const auto end = block.find(close, value_begin);
    if (end == std::string_view::npos) return std::nullopt;
    return trim(std::string(block.substr(value_begin, end - value_begin)));
}

std::optional<double> xml_attribute(std::string_view tag, std::string_view name) {
    const auto prefix = std::string(name) + "=\"";
    const auto begin = tag.find(prefix);
    if (begin == std::string_view::npos) return std::nullopt;
    const auto value_begin = begin + prefix.size();
    const auto end = tag.find('"', value_begin);
    if (end == std::string_view::npos) return std::nullopt;
    return number(std::string(tag.substr(value_begin, end - value_begin)));
}

double distance_m(double lat_a, double lon_a, double lat_b, double lon_b) {
    constexpr double radius = 6'371'000.0;
    constexpr double radians = 3.14159265358979323846 / 180.0;
    const auto dlat = (lat_b - lat_a) * radians;
    const auto dlon = (lon_b - lon_a) * radians;
    const auto a = std::sin(dlat * 0.5) * std::sin(dlat * 0.5) +
        std::cos(lat_a * radians) * std::cos(lat_b * radians) *
        std::sin(dlon * 0.5) * std::sin(dlon * 0.5);
    return radius * 2.0 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
}

double bearing_deg(double lat_a, double lon_a, double lat_b, double lon_b) {
    constexpr double radians = 3.14159265358979323846 / 180.0;
    const auto a = lat_a * radians, b = lat_b * radians, delta = (lon_b - lon_a) * radians;
    const auto y = std::sin(delta) * std::cos(b);
    const auto x = std::cos(a) * std::sin(b) - std::sin(a) * std::cos(b) * std::cos(delta);
    auto heading = std::atan2(y, x) / radians;
    if (heading < 0.0) heading += 360.0;
    return heading;
}

std::optional<std::int64_t> filename_epoch_us(const std::filesystem::path& path) {
    const auto name = path.stem().string();
    for (std::size_t start = 0; start + 12 <= name.size(); ++start) {
        const auto digits = name.substr(start, 12);
        if (!std::all_of(digits.begin(), digits.end(), [](unsigned char value) { return std::isdigit(value) != 0; })) continue;
        std::tm tm{};
        tm.tm_year = 100 + std::stoi(digits.substr(0, 2));
        tm.tm_mon = std::stoi(digits.substr(2, 2)) - 1;
        tm.tm_mday = std::stoi(digits.substr(4, 2));
        tm.tm_hour = std::stoi(digits.substr(6, 2));
        tm.tm_min = std::stoi(digits.substr(8, 2));
        tm.tm_sec = std::stoi(digits.substr(10, 2));
        const auto epoch = _mkgmtime64(&tm);
        if (epoch > 0) return epoch * kSecond;
    }
    return std::nullopt;
}

ParsedVbo parse_vbo_full(const std::filesystem::path& path, std::vector<std::string>& diagnostics) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Could not open VBO file: " + path.string());

    ParsedVbo parsed;
    std::string line;
    bool in_data = false;
    double first_clock = -1.0;
    double previous_clock = -1.0;
    double day_offset = 0.0;
    while (std::getline(file, line)) {
        const auto clean = trim(line);
        if (clean == "[data]") {
            in_data = true;
            continue;
        }
        if (!in_data && clean.rfind("Start", 0) == 0) {
            std::istringstream stream(clean.substr(5));
            std::array<double, 4> coordinate{};
            if (stream >> coordinate[0] >> coordinate[1] >> coordinate[2] >> coordinate[3]) {
                parsed.start_line = std::pair{
                    PhysicalMarker{coordinate[0] / 60.0, coordinate[1] / 60.0, 0.0},
                    PhysicalMarker{coordinate[2] / 60.0, coordinate[3] / 60.0, 0.0}};
            }
            continue;
        }
        if (!in_data || clean.empty() || clean.front() == '[') continue;

        std::istringstream stream(clean);
        std::array<double, 13> field{};
        std::string clock_token;
        bool valid = static_cast<bool>(stream >> clock_token);
        for (std::size_t index = 1; valid && index < field.size(); ++index) {
            auto& value = field[index];
            if (!(stream >> value)) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;
        auto clock = clock_seconds(clock_token);
        if (clock < 0.0) continue;
        if (previous_clock >= 0.0 && clock + day_offset < previous_clock - 12.0 * 3600.0) day_offset += 24.0 * 3600.0;
        clock += day_offset;
        if (first_clock < 0.0) first_clock = clock;
        previous_clock = clock;
        parsed.telemetry.time_us.push_back(static_cast<Timestamp>(std::llround((clock - first_clock) * kSecond)));
        parsed.telemetry.absolute_time_us.push_back(0);
        parsed.telemetry.latitude.push_back(field[1] / 60.0);
        parsed.telemetry.longitude.push_back(field[2] / 60.0);
        parsed.telemetry.speed_kmh.push_back(static_cast<float>(field[3]));
        parsed.telemetry.heading_deg.push_back(static_cast<float>(field[4]));
        parsed.telemetry.altitude_m.push_back(static_cast<float>(field[5]));
        parsed.telemetry.longitudinal_g.push_back(static_cast<float>(field[6]));
        parsed.telemetry.lateral_g.push_back(static_cast<float>(field[7]));
        parsed.telemetry.vertical_g.push_back(static_cast<float>(field[8]));
        parsed.telemetry.gyro_x_dps.push_back(static_cast<float>(field[9]));
        parsed.telemetry.gyro_y_dps.push_back(static_cast<float>(field[10]));
        parsed.telemetry.gyro_z_dps.push_back(static_cast<float>(field[11]));
        parsed.telemetry.satellites.push_back(static_cast<std::uint8_t>(std::clamp(field[12], 0.0, 255.0)));
        parsed.telemetry.raw_lap.push_back(0);
    }
    parsed.telemetry.validate();
    diagnostics.push_back("Parsed " + std::to_string(parsed.telemetry.size()) + " VBO rows");
    return parsed;
}

double side_of_line(double lat, double lon, const PhysicalMarker& a, const PhysicalMarker& b) {
    return (lon - a.longitude) * (b.latitude - a.latitude) - (lat - a.latitude) * (b.longitude - a.longitude);
}

bool segments_intersect(double p_lat, double p_lon, double q_lat, double q_lon,
                        const PhysicalMarker& a, const PhysicalMarker& b) {
    const auto orient = [](double ax, double ay, double bx, double by, double cx, double cy) {
        return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    };
    const auto o1 = orient(p_lon, p_lat, q_lon, q_lat, a.longitude, a.latitude);
    const auto o2 = orient(p_lon, p_lat, q_lon, q_lat, b.longitude, b.latitude);
    const auto o3 = orient(a.longitude, a.latitude, b.longitude, b.latitude, p_lon, p_lat);
    const auto o4 = orient(a.longitude, a.latitude, b.longitude, b.latitude, q_lon, q_lat);
    return ((o1 <= 0.0 && o2 >= 0.0) || (o1 >= 0.0 && o2 <= 0.0)) &&
           ((o3 <= 0.0 && o4 >= 0.0) || (o3 >= 0.0 && o4 <= 0.0));
}

}  // namespace

void TelemetrySeries::reserve(std::size_t count) {
    time_us.reserve(count); absolute_time_us.reserve(count); latitude.reserve(count); longitude.reserve(count);
    speed_kmh.reserve(count); heading_deg.reserve(count); altitude_m.reserve(count); longitudinal_g.reserve(count);
    lateral_g.reserve(count); vertical_g.reserve(count); gyro_x_dps.reserve(count); gyro_y_dps.reserve(count);
    gyro_z_dps.reserve(count); satellites.reserve(count); raw_lap.reserve(count);
}

void TelemetrySeries::validate() const {
    const auto count = size();
    if (absolute_time_us.size() != count || latitude.size() != count || longitude.size() != count ||
        speed_kmh.size() != count || heading_deg.size() != count || altitude_m.size() != count ||
        longitudinal_g.size() != count || lateral_g.size() != count || vertical_g.size() != count ||
        gyro_x_dps.size() != count || gyro_y_dps.size() != count || gyro_z_dps.size() != count ||
        satellites.size() != count || raw_lap.size() != count) {
        throw std::runtime_error("Telemetry columns have inconsistent lengths");
    }
}

void RadioSeries::reserve(std::size_t count) {
    elapsed_us.reserve(count); steering_percent.reserve(count); trigger_percent.reserve(count); voltage.reserve(count);
}

void RadioSeries::validate() const {
    const auto count = size();
    if (steering_percent.size() != count || trigger_percent.size() != count || voltage.size() != count) {
        throw std::runtime_error("Radio columns have inconsistent lengths");
    }
}

Timestamp RadioSeries::duration_us() const noexcept {
    return elapsed_us.empty() ? 0 : elapsed_us.back() - elapsed_us.front();
}

TelemetrySeries parse_vbo(const std::filesystem::path& path, std::vector<std::string>& diagnostics) {
    return parse_vbo_full(path, diagnostics).telemetry;
}

TelemetrySeries parse_racebox_csv(const std::filesystem::path& path, std::vector<std::string>& diagnostics) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Could not open RaceBox CSV: " + path.string());
    std::string line;
    if (!std::getline(file, line)) throw std::runtime_error("RaceBox CSV is empty");
    const auto header = split(line, ',');
    const auto column = [&](std::string_view name) {
        for (std::size_t index = 0; index < header.size(); ++index) if (header[index] == name) return static_cast<int>(index);
        return -1;
    };
    const int time_col = column("Time"), lat_col = column("Latitude"), lon_col = column("Longitude");
    if (time_col < 0 || lat_col < 0 || lon_col < 0) throw std::runtime_error("RaceBox CSV is missing Time/Latitude/Longitude");
    const int alt_col = column("Altitude"), speed_col = column("Speed"), gx_col = column("GForceX"),
              gy_col = column("GForceY"), gz_col = column("GForceZ"), gyro_x_col = column("GyroX"),
              gyro_y_col = column("GyroY"), gyro_z_col = column("GyroZ"), lap_col = column("Lap");

    TelemetrySeries result;
    std::int64_t first_absolute = 0;
    while (std::getline(file, line)) {
        const auto fields = split(line, ',');
        if (fields.size() < header.size()) continue;
        const auto absolute = iso_epoch_us(fields[time_col]);
        if (!absolute) continue;
        if (!first_absolute) first_absolute = absolute;
        result.time_us.push_back(absolute - first_absolute);
        result.absolute_time_us.push_back(absolute);
        result.latitude.push_back(number(fields[lat_col]));
        result.longitude.push_back(number(fields[lon_col]));
        result.speed_kmh.push_back(static_cast<float>(speed_col >= 0 ? number(fields[speed_col]) : 0.0));
        result.heading_deg.push_back(0.0F);
        result.altitude_m.push_back(static_cast<float>(alt_col >= 0 ? number(fields[alt_col]) : 0.0));
        result.longitudinal_g.push_back(static_cast<float>(gx_col >= 0 ? number(fields[gx_col]) : 0.0));
        result.lateral_g.push_back(static_cast<float>(gy_col >= 0 ? number(fields[gy_col]) : 0.0));
        result.vertical_g.push_back(static_cast<float>(gz_col >= 0 ? number(fields[gz_col]) : 0.0));
        result.gyro_x_dps.push_back(static_cast<float>(gyro_x_col >= 0 ? number(fields[gyro_x_col]) : 0.0));
        result.gyro_y_dps.push_back(static_cast<float>(gyro_y_col >= 0 ? number(fields[gyro_y_col]) : 0.0));
        result.gyro_z_dps.push_back(static_cast<float>(gyro_z_col >= 0 ? number(fields[gyro_z_col]) : 0.0));
        result.satellites.push_back(0);
        result.raw_lap.push_back(static_cast<std::int32_t>(lap_col >= 0 ? number(fields[lap_col]) : 0.0));
    }
    result.validate();
    diagnostics.push_back("Parsed " + std::to_string(result.size()) + " RaceBox CSV rows");
    return result;
}

TelemetrySeries parse_gpx(const std::filesystem::path& path, std::vector<std::string>& diagnostics) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Could not open GPX file: " + path.string());
    const std::string document((std::istreambuf_iterator<char>(file)), {});
    TelemetrySeries result;
    std::size_t cursor = 0;
    std::int64_t first_absolute = 0;
    while ((cursor = document.find("<trkpt", cursor)) != std::string::npos) {
        const auto tag_end = document.find('>', cursor);
        const auto block_end = document.find("</trkpt>", tag_end);
        if (tag_end == std::string::npos || block_end == std::string::npos) break;
        const std::string_view tag(document.data() + cursor, tag_end - cursor + 1);
        const std::string_view block(document.data() + tag_end + 1, block_end - tag_end - 1);
        const auto latitude = xml_attribute(tag, "lat");
        const auto longitude = xml_attribute(tag, "lon");
        const auto time_text = xml_value(block, "time");
        if (latitude && longitude && time_text) {
            const auto absolute = iso_epoch_us(*time_text);
            if (absolute) {
                if (!first_absolute) first_absolute = absolute;
                result.time_us.push_back(absolute - first_absolute);
                result.absolute_time_us.push_back(absolute);
                result.latitude.push_back(*latitude);
                result.longitude.push_back(*longitude);
                result.speed_kmh.push_back(0.0F);
                result.heading_deg.push_back(0.0F);
                result.altitude_m.push_back(static_cast<float>(xml_value(block, "ele").has_value() ? number(*xml_value(block, "ele")) : 0.0));
                result.longitudinal_g.push_back(0.0F);
                result.lateral_g.push_back(0.0F);
                result.vertical_g.push_back(0.0F);
                result.gyro_x_dps.push_back(0.0F);
                result.gyro_y_dps.push_back(0.0F);
                result.gyro_z_dps.push_back(0.0F);
                result.satellites.push_back(0);
                result.raw_lap.push_back(0);
            }
        }
        cursor = block_end + 8;
    }
    for (std::size_t index = 1; index < result.size(); ++index) {
        const auto elapsed = static_cast<double>(result.time_us[index] - result.time_us[index - 1]) / kSecond;
        if (elapsed > 0.0) result.speed_kmh[index] = static_cast<float>(distance_m(
            result.latitude[index - 1], result.longitude[index - 1], result.latitude[index], result.longitude[index]) / elapsed * 3.6);
        result.heading_deg[index] = static_cast<float>(bearing_deg(
            result.latitude[index - 1], result.longitude[index - 1], result.latitude[index], result.longitude[index]));
    }
    if (result.size() > 1) { result.speed_kmh[0] = result.speed_kmh[1]; result.heading_deg[0] = result.heading_deg[1]; }
    result.validate();
    diagnostics.push_back("Parsed " + std::to_string(result.size()) + " GPX track points");
    return result;
}

RadioSeries parse_sanwa_csv(const std::filesystem::path& path, std::vector<std::string>& diagnostics) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Could not open Sanwa CSV: " + path.string());
    std::string line;
    std::vector<std::string> header;
    while (std::getline(file, line)) {
        header = split(line, ',');
        if (std::find(header.begin(), header.end(), "REC TIME") != header.end() &&
            std::find(header.begin(), header.end(), "ST(%)") != header.end() &&
            std::find(header.begin(), header.end(), "TH(%)") != header.end()) break;
        header.clear();
    }
    if (header.empty()) throw std::runtime_error("Sanwa CSV is missing REC TIME/ST(%)/TH(%)");
    const auto column = [&](std::string_view name) {
        for (std::size_t index = 0; index < header.size(); ++index) if (header[index] == name) return static_cast<int>(index);
        return -1;
    };
    const int time_col = column("REC TIME"), steering_col = column("ST(%)"), trigger_col = column("TH(%)"), voltage_col = column("VOLT(V)");
    RadioSeries result;
    result.filename = path.filename().string();
    result.filename_time_us = filename_epoch_us(path);
    while (std::getline(file, line)) {
        const auto fields = split(line, ',');
        if (time_col >= static_cast<int>(fields.size()) || steering_col >= static_cast<int>(fields.size()) || trigger_col >= static_cast<int>(fields.size())) continue;
        const auto elapsed = duration_us(fields[time_col]);
        if (elapsed < 0) continue;
        result.elapsed_us.push_back(elapsed);
        result.steering_percent.push_back(static_cast<float>(std::clamp(number(fields[steering_col]), -100.0, 100.0)));
        result.trigger_percent.push_back(static_cast<float>(std::clamp(number(fields[trigger_col]), -100.0, 100.0)));
        result.voltage.push_back(static_cast<float>(voltage_col >= 0 && voltage_col < static_cast<int>(fields.size()) ? number(fields[voltage_col]) : 0.0));
    }
    result.validate();
    diagnostics.push_back("Parsed " + std::to_string(result.size()) + " Sanwa rows at native rate");
    return result;
}

void assign_directed_laps(TelemetrySeries& telemetry, const PhysicalMarker& line_a, const PhysicalMarker& line_b,
                          std::vector<std::string>& diagnostics) {
    struct Crossing { std::size_t index; Timestamp time; int direction; float speed; };
    std::vector<Crossing> events;
    for (std::size_t index = 1; index < telemetry.size(); ++index) {
        const auto before = side_of_line(telemetry.latitude[index - 1], telemetry.longitude[index - 1], line_a, line_b);
        const auto after = side_of_line(telemetry.latitude[index], telemetry.longitude[index], line_a, line_b);
        if ((before < 0.0 && after >= 0.0) || (before > 0.0 && after <= 0.0)) {
            if (segments_intersect(telemetry.latitude[index - 1], telemetry.longitude[index - 1], telemetry.latitude[index], telemetry.longitude[index], line_a, line_b)) {
                events.push_back({index, telemetry.time_us[index], after > before ? 1 : -1, telemetry.speed_kmh[index]});
            }
        }
    }
    int direction_sum = 0;
    for (const auto& event : events) if (event.speed >= 5.0F) direction_sum += event.direction;
    const int forward = direction_sum >= 0 ? 1 : -1;
    std::vector<Crossing> accepted;
    for (const auto& event : events) {
        if (event.speed < 5.0F || event.direction != forward) continue;
        if (!accepted.empty() && event.time - accepted.back().time < 8 * kSecond) continue;
        accepted.push_back(event);
    }
    std::fill(telemetry.raw_lap.begin(), telemetry.raw_lap.end(), 0);
    std::size_t crossing_index = 0;
    int lap = 0;
    for (std::size_t index = 0; index < telemetry.size(); ++index) {
        while (crossing_index < accepted.size() && index >= accepted[crossing_index].index) {
            ++lap;
            ++crossing_index;
        }
        telemetry.raw_lap[index] = lap;
    }
    diagnostics.push_back("Directed start/finish crossings accepted " + std::to_string(accepted.size()) + " of " + std::to_string(events.size()));
}

bool apply_start_finish_line(Session& session, const PhysicalMarker& line_a, const PhysicalMarker& line_b,
                             std::vector<std::string>& diagnostics) {
    if (session.telemetry.empty()) {
        diagnostics.emplace_back("Start/finish placement rejected: telemetry is empty");
        return false;
    }
    const auto centre_latitude = (line_a.latitude + line_b.latitude) * 0.5;
    const auto longitude_metres = 111'320.0 * std::cos(centre_latitude * 3.14159265358979323846 / 180.0);
    const auto line_length_m = std::hypot((line_b.longitude - line_a.longitude) * longitude_metres,
                                          (line_b.latitude - line_a.latitude) * 110'540.0);
    if (!std::isfinite(line_length_m) || line_length_m < 0.5) {
        diagnostics.emplace_back("Start/finish placement rejected: line is too short");
        return false;
    }

    const auto previous_raw_lap = session.telemetry.raw_lap;
    const auto previous_laps = session.laps;
    const auto previous_line = session.start_finish_line;
    const auto previous_markers = session.sector_markers;
    const auto previous_theoretical = session.theoretical_best;

    assign_directed_laps(session.telemetry, line_a, line_b, diagnostics);
    auto rebuilt_laps = build_lap_index(session.telemetry);
    const auto complete_count = static_cast<std::size_t>(std::count_if(
        rebuilt_laps.begin(), rebuilt_laps.end(), [](const LapInfo& lap) { return lap.phase == LapPhase::Complete; }));
    if (complete_count < 2) {
        session.telemetry.raw_lap = previous_raw_lap;
        session.laps = previous_laps;
        session.start_finish_line = previous_line;
        session.sector_markers = previous_markers;
        session.theoretical_best = previous_theoretical;
        diagnostics.emplace_back("Start/finish placement rejected: fewer than two complete laps were found");
        return false;
    }

    session.laps = std::move(rebuilt_laps);
    session.start_finish_line = PhysicalLine{line_a, line_b};
    session.sector_markers = default_sector_markers(session);
    session.theoretical_best = calculate_theoretical_best(session);
    diagnostics.push_back("Start/finish line applied; rebuilt " + std::to_string(session.laps.size()) +
                          " lap segments (" + std::to_string(complete_count) + " complete)");
    return true;
}

LoadResult load_session(const LoadRequest& request) {
    LoadResult result;
    auto vbo = parse_vbo_full(request.vbo, result.diagnostics);
    auto csv = parse_racebox_csv(request.racebox_csv, result.diagnostics);
    if (!request.sanwa_csv.empty()) result.session.radio = parse_sanwa_csv(request.sanwa_csv, result.diagnostics);
    else result.diagnostics.emplace_back("Sanwa source not supplied; controls remain unavailable");
    if (vbo.telemetry.empty() || csv.empty()) throw std::runtime_error("RaceBox inputs contain no samples");

    TelemetrySeries merged;
    merged.reserve(csv.size());
    const auto longitude_sign = std::accumulate(csv.longitude.begin(), csv.longitude.end(), 0.0) < 0.0 ? -1.0 : 1.0;
    std::vector<float> aligned_vbo_speed;
    aligned_vbo_speed.reserve(csv.size());
    double time_error_us = 0.0;
    const bool direct = vbo.telemetry.size() == csv.size();
    for (std::size_t index = 0; index < csv.size(); ++index) {
        const auto source = direct ? index : nearest_time(vbo.telemetry.time_us, csv.time_us[index]);
        time_error_us += std::abs(static_cast<double>(vbo.telemetry.time_us[source] - csv.time_us[index]));
        aligned_vbo_speed.push_back(vbo.telemetry.speed_kmh[source]);
        merged.time_us.push_back(csv.time_us[index]);
        merged.absolute_time_us.push_back(csv.absolute_time_us[index]);
        merged.latitude.push_back(vbo.telemetry.latitude[source]);
        merged.longitude.push_back(std::abs(vbo.telemetry.longitude[source]) * longitude_sign);
        merged.speed_kmh.push_back(csv.speed_kmh[index]);
        merged.heading_deg.push_back(vbo.telemetry.heading_deg[source]);
        merged.altitude_m.push_back(vbo.telemetry.altitude_m[source]);
        merged.longitudinal_g.push_back(vbo.telemetry.longitudinal_g[source]);
        merged.lateral_g.push_back(vbo.telemetry.lateral_g[source]);
        merged.vertical_g.push_back(vbo.telemetry.vertical_g[source]);
        merged.gyro_x_dps.push_back(vbo.telemetry.gyro_x_dps[source]);
        merged.gyro_y_dps.push_back(vbo.telemetry.gyro_y_dps[source]);
        merged.gyro_z_dps.push_back(vbo.telemetry.gyro_z_dps[source]);
        merged.satellites.push_back(vbo.telemetry.satellites[source]);
        merged.raw_lap.push_back(csv.raw_lap[index]);
    }
    merged.validate();
    const auto speed_correlation = std::max(0.0, pearson(aligned_vbo_speed, csv.speed_kmh));
    const auto mean_time_error_ms = time_error_us / std::max<std::size_t>(1, csv.size()) / 1000.0;
    const auto sample_ratio = static_cast<double>(std::min(vbo.telemetry.size(), csv.size())) /
                              static_cast<double>(std::max(vbo.telemetry.size(), csv.size()));
    const auto timing_score = std::clamp(1.0 - mean_time_error_ms / 200.0, 0.0, 1.0);
    const auto merge_confidence = direct ? 1.0 : 0.45 * speed_correlation + 0.35 * timing_score + 0.20 * sample_ratio;
    std::optional<PhysicalLine> start_finish_line;
    if (vbo.start_line) {
        auto [a, b] = *vbo.start_line;
        a.longitude = std::abs(a.longitude) * longitude_sign;
        b.longitude = std::abs(b.longitude) * longitude_sign;
        assign_directed_laps(merged, a, b, result.diagnostics);
        start_finish_line = PhysicalLine{a, b};
    } else {
        merged.raw_lap = csv.raw_lap;
        result.diagnostics.emplace_back("VBO start line missing; CSV laps used");
    }

    result.session.name = request.vbo.stem().string();
    result.session.vbo_path = request.vbo;
    result.session.csv_path = request.racebox_csv;
    result.session.sanwa_path = request.sanwa_csv;
    result.session.telemetry = std::move(merged);
    result.session.laps = build_lap_index(result.session.telemetry);
    result.session.start_finish_line = start_finish_line;
    result.session.alignment = align_radio(result.session.telemetry, result.session.radio);
    result.session.sector_markers = default_sector_markers(result.session);
    result.session.theoretical_best = calculate_theoretical_best(result.session);
    result.session.alignment.merge_confidence = merge_confidence;
    if (direct) {
        result.diagnostics.emplace_back("Matched every RaceBox CSV row to VBO by direct row index");
    } else {
        result.diagnostics.push_back("Aligned unequal VBO/CSV rows by normalized time; speed r=" +
            std::to_string(speed_correlation) + ", mean time error=" + std::to_string(mean_time_error_ms) + " ms");
        if (merge_confidence < 0.55) result.diagnostics.emplace_back("WARNING: VBO/CSV merge confidence is weak; inspect alignment before analysis");
    }
    result.imu_analysis = imu::analyze(result.session.telemetry);
    return result;
}

}  // namespace racebox
