#include "import_discovery.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix =
            std::chrono::high_resolution_clock::now()
                .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("racebox-import-discovery-" + std::to_string(suffix));
        std::filesystem::create_directories(path_ / "nested");
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void write(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output(path, std::ios::binary);
    output << content;
}

}  // namespace

int main() {
    using namespace racebox::app;
    using racebox::race_day::TelemetrySourceKind;
    bool passed = true;
    TemporaryDirectory temporary;

    write(temporary.path() / "racebox.csv",
        "Record,Time,Latitude,Longitude,Altitude,Speed,GForceX\n"
        "1,2026-07-11T21:49:38.720Z,49.1,-123.1,4,5,0\n");
    write(temporary.path() / "nested" / "sanwa.csv",
        "TOTAL LAP,'00:01:00.00\n"
        "LAP,LAP TIME,REC TIME,ST(%),TH(%),RPM\n"
        "L000,'00:00:00.00,'00:00:00.00,0,0,0\n");
    write(temporary.path() / "session.vbo", "[header]\n");
    write(temporary.path() / "unrelated.csv", "name,value\nx,1\n");
    write(temporary.path() / "empty.gpx", {});

    const auto all = scan_telemetry_folder(temporary.path());
    passed &= expect(all.error.empty(), "valid folder scan succeeds");
    passed &= expect(all.files.size() == 3,
        "scan includes VBO, RaceBox CSV, and nested Sanwa but rejects unrelated or empty files");
    const auto count_kind = [&](TelemetrySourceKind kind) {
        return std::count_if(all.files.begin(), all.files.end(),
            [&](const auto& file) { return file.kind == kind; });
    };
    passed &= expect(count_kind(TelemetrySourceKind::Vbo) == 1,
        "VBO is classified");
    passed &= expect(count_kind(TelemetrySourceKind::RaceBoxCsv) == 1,
        "RaceBox CSV header is classified");
    passed &= expect(count_kind(TelemetrySourceKind::SanwaCsv) == 1,
        "Sanwa header is classified");

    const auto sanwa = scan_telemetry_folder(
        temporary.path(), true);
    passed &= expect(sanwa.files.size() == 1 &&
        sanwa.files.front().kind == TelemetrySourceKind::SanwaCsv,
        "Sanwa-only USB scan excludes every other source");

    const auto bounded = scan_telemetry_folder(
        temporary.path(), false, 1);
    passed &= expect(bounded.truncated,
        "file examination is bounded");

    const auto missing = scan_telemetry_folder(
        temporary.path() / "missing");
    passed &= expect(!missing.error.empty() && missing.files.empty(),
        "missing folder reports a safe error");

    if (!passed) return 1;
    std::cout << "Telemetry folder and Sanwa USB discovery tests passed\n";
    return 0;
}
