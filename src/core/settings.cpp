#include "racebox/core.hpp"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>

namespace racebox {
namespace {

std::filesystem::path local_app_data_root() {
    PWSTR raw_path = nullptr;
    std::filesystem::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw_path))) {
        path = raw_path;
        CoTaskMemFree(raw_path);
        return path;
    }
    return std::filesystem::temp_directory_path();
}

void migrate_legacy_settings_once(
    const std::filesystem::path& root,
    const std::filesystem::path& destination) {
    const auto marker = destination / L".migrated-from-racebox-viewer-native";
    std::error_code error;
    if (std::filesystem::exists(marker, error)) return;

    const auto legacy = root / L"RaceBoxViewerNative";
    bool migration_succeeded = true;
    for (const auto* filename : {L"preferences.json", L"layout.ini"}) {
        const auto source = legacy / filename;
        const auto target = destination / filename;
        if (!std::filesystem::exists(target, error) &&
            std::filesystem::is_regular_file(source, error)) {
            error.clear();
            std::filesystem::copy_file(
                source, target,
                std::filesystem::copy_options::none, error);
            if (error) migration_succeeded = false;
        }
        if (error) migration_succeeded = false;
        error.clear();
    }

    if (!migration_succeeded) return;
    std::ofstream complete(marker, std::ios::binary | std::ios::trunc);
    complete << "RaceBox Telemetry Viewer 2 settings migration complete\n";
}

}  // namespace

std::filesystem::path settings_directory() {
    static std::once_flag initialize;
    static std::filesystem::path path;
    std::call_once(initialize, [] {
        const auto root = local_app_data_root();
        path = root / L"RaceBoxTelemetryViewer" / L"2";
        std::filesystem::create_directories(path);
        migrate_legacy_settings_once(root, path);
    });
    return path;
}

std::filesystem::path log_directory() {
    const auto path = settings_directory() / L"logs";
    std::filesystem::create_directories(path);
    return path;
}

void write_log(std::string_view message) {
    static std::mutex mutex;
    std::scoped_lock lock(mutex);
    const auto path = log_directory() / L"RaceBoxTelemetryViewer.log";
    std::error_code error;
    if (std::filesystem::exists(path, error) && std::filesystem::file_size(path, error) > 2 * 1024 * 1024) {
        const auto previous =
            log_directory() / L"RaceBoxTelemetryViewer.1.log";
        std::filesystem::remove(previous, error);
        std::filesystem::rename(path, previous, error);
    }
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &now);
    std::ofstream file(path, std::ios::app);
    file << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << " " << message << '\n';
}

}  // namespace racebox
