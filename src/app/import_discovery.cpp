#include "import_discovery.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <format>
#include <fstream>
#include <system_error>

namespace racebox::app {
namespace {

std::string uppercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    return value;
}

race_day::TelemetrySourceKind discover_kind(
    const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    if (extension == L".vbo") return race_day::TelemetrySourceKind::Vbo;
    if (extension == L".gpx") return race_day::TelemetrySourceKind::Gpx;
    if (extension == L".rbxsession" || extension == L".rbxlap") {
        return race_day::TelemetrySourceKind::NativeArchive;
    }
    if (extension != L".csv") {
        return race_day::TelemetrySourceKind::Unsupported;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) return race_day::TelemetrySourceKind::Unsupported;
    std::string header;
    std::string line;
    for (int index = 0; index < 12 && std::getline(input, line); ++index) {
        header += line;
        header.push_back('\n');
        if (header.size() >= 16 * 1024) break;
    }
    header = uppercase_ascii(std::move(header));
    if (header.find("REC TIME") != std::string::npos &&
        header.find("ST(%)") != std::string::npos &&
        header.find("TH(%)") != std::string::npos) {
        return race_day::TelemetrySourceKind::SanwaCsv;
    }
    if (header.find("LATITUDE") != std::string::npos &&
        header.find("LONGITUDE") != std::string::npos &&
        header.find("SPEED") != std::string::npos) {
        return race_day::TelemetrySourceKind::RaceBoxCsv;
    }
    return race_day::TelemetrySourceKind::Unsupported;
}

void consider_file(
    TelemetryFolderScan& result,
    const std::filesystem::path& path,
    bool sanwa_only) {
    const auto kind = discover_kind(path);
    if (kind == race_day::TelemetrySourceKind::Unsupported ||
        (sanwa_only && kind != race_day::TelemetrySourceKind::SanwaCsv)) {
        return;
    }

    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0) return;
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error) return;
    result.files.push_back({path, kind, size, modified});
}

}  // namespace

TelemetryFolderScan scan_telemetry_folder(
    const std::filesystem::path& root,
    bool sanwa_only,
    std::size_t max_files_examined,
    std::size_t max_depth) noexcept {
    TelemetryFolderScan result;
    result.root = root;
    try {
        std::error_code error;
        if (root.empty() || !std::filesystem::is_directory(root, error)) {
            result.error = "The selected import folder is not available.";
            return result;
        }

        std::filesystem::recursive_directory_iterator iterator(
            root, std::filesystem::directory_options::skip_permission_denied,
            error);
        const std::filesystem::recursive_directory_iterator end;
        while (iterator != end) {
            if (error) {
                error.clear();
                iterator.increment(error);
                continue;
            }
            if (iterator.depth() >= static_cast<int>(max_depth) &&
                iterator->is_directory(error)) {
                iterator.disable_recursion_pending();
            }
            if (iterator->is_regular_file(error)) {
                ++result.files_examined;
                if (result.files_examined > max_files_examined) {
                    result.truncated = true;
                    break;
                }
                consider_file(result, iterator->path(), sanwa_only);
            }
            iterator.increment(error);
        }
        std::ranges::sort(result.files, [](const auto& left, const auto& right) {
            if (left.modified_at != right.modified_at) {
                return left.modified_at > right.modified_at;
            }
            return left.path.native() < right.path.native();
        });
        return result;
    } catch (const std::exception& exception) {
        result.error = std::string("Could not scan the import folder: ") +
            exception.what();
        return result;
    } catch (...) {
        result.error = "Could not scan the import folder.";
        return result;
    }
}

std::vector<std::filesystem::path> removable_drive_roots() noexcept {
    std::vector<std::filesystem::path> result;
    const auto mask = GetLogicalDrives();
    if (mask == 0) return result;
    for (unsigned index = 0; index < 26; ++index) {
        if ((mask & (1UL << index)) == 0) continue;
        wchar_t root[]{static_cast<wchar_t>(L'A' + index), L':', L'\\', L'\0'};
        if (GetDriveTypeW(root) == DRIVE_REMOVABLE) {
            result.emplace_back(root);
        }
    }
    return result;
}

std::string discovered_file_time_label(
    std::filesystem::file_time_type modified_at) {
    const auto system_time = std::chrono::time_point_cast<
        std::chrono::system_clock::duration>(
        modified_at - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    const auto time = std::chrono::system_clock::to_time_t(system_time);
    std::tm local{};
    localtime_s(&local, &time);
    return std::format(
        "{:04}-{:02}-{:02} {:02}:{:02}",
        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
        local.tm_hour, local.tm_min);
}

}  // namespace racebox::app
