#include "racebox/core.hpp"

#include <windows.h>
#include <psapi.h>

#include <iostream>

int main() {
    constexpr std::size_t sample_count = 1'000'000;
    racebox::Session session;
    session.telemetry.reserve(sample_count);
    for (std::size_t index = 0; index < sample_count; ++index) {
        session.telemetry.time_us.push_back(static_cast<racebox::Timestamp>(index) * 40'000);
        session.telemetry.absolute_time_us.push_back(1'700'000'000'000'000LL + static_cast<std::int64_t>(index) * 40'000);
        session.telemetry.latitude.push_back(49.0 + static_cast<double>(index % 1000) * 1e-8);
        session.telemetry.longitude.push_back(-123.0 + static_cast<double>(index % 1000) * 1e-8);
        session.telemetry.speed_kmh.push_back(static_cast<float>(index % 110));
        session.telemetry.heading_deg.push_back(static_cast<float>(index % 360));
        session.telemetry.altitude_m.push_back(3.0F);
        session.telemetry.longitudinal_g.push_back(0.0F);
        session.telemetry.lateral_g.push_back(0.0F);
        session.telemetry.vertical_g.push_back(1.0F);
        session.telemetry.gyro_x_dps.push_back(0.0F);
        session.telemetry.gyro_y_dps.push_back(0.0F);
        session.telemetry.gyro_z_dps.push_back(0.0F);
        session.telemetry.satellites.push_back(18);
        session.telemetry.raw_lap.push_back(static_cast<std::int32_t>(index / 400));
    }
    session.telemetry.validate();
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) return 1;
    const auto megabytes = static_cast<double>(memory.WorkingSetSize) / (1024.0 * 1024.0);
    std::cout << "One-million-sample working memory: " << megabytes << " MB\n";
    if (megabytes > 250.0) {
        std::cerr << "Memory target exceeded\n";
        return 1;
    }
    return 0;
}
