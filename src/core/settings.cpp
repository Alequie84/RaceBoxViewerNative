#include "racebox/core.hpp"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>

namespace racebox {

std::filesystem::path settings_directory() {
    PWSTR raw_path = nullptr;
    std::filesystem::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw_path))) {
        path = std::filesystem::path(raw_path) / L"RaceBoxViewerNative";
        CoTaskMemFree(raw_path);
    } else {
        path = std::filesystem::temp_directory_path() / L"RaceBoxViewerNative";
    }
    std::filesystem::create_directories(path);
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
    const auto path = log_directory() / L"RaceBoxViewer.log";
    std::error_code error;
    if (std::filesystem::exists(path, error) && std::filesystem::file_size(path, error) > 2 * 1024 * 1024) {
        const auto previous = log_directory() / L"RaceBoxViewer.1.log";
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
