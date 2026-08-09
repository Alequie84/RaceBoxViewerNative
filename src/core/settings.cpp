#include "racebox/core.hpp"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>

namespace racebox {
namespace {

struct SettingsProfileState {
    std::mutex mutex;
    bool initialized{};
    bool demo{};
    std::filesystem::path directory;
};

SettingsProfileState& settings_profile_state() {
    static SettingsProfileState state;
    return state;
}

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

bool configure_settings_profile(bool demo_profile) noexcept {
    auto& state = settings_profile_state();
    std::scoped_lock lock(state.mutex);
    if (state.initialized) return state.demo == demo_profile;
    state.demo = demo_profile;
    return true;
}

bool demo_settings_profile() noexcept {
    auto& state = settings_profile_state();
    std::scoped_lock lock(state.mutex);
    return state.demo;
}

std::filesystem::path settings_directory() {
    auto& state = settings_profile_state();
    std::scoped_lock lock(state.mutex);
    if (!state.initialized) {
        const auto root = local_app_data_root();
        state.directory = root / L"RaceBoxTelemetryViewer" /
            (state.demo ? L"2-demo" : L"2");
        std::filesystem::create_directories(state.directory);
        if (!state.demo) migrate_legacy_settings_once(root, state.directory);
        state.initialized = true;
    }
    return state.directory;
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
