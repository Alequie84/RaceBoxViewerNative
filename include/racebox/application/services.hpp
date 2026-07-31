#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace racebox::application {

using ServiceRequestId = std::uint64_t;

struct FileDialogFilter {
    std::string label;
    std::vector<std::string> extensions;
};

struct FileDialogRequest {
    ServiceRequestId id{};
    std::string title;
    std::vector<FileDialogFilter> filters;
    std::filesystem::path initial_directory;
    std::string suggested_filename;
    bool allow_multiple{};
    bool save{};
};

enum class FileDialogStatus {
    Selected,
    Cancelled,
    Failed,
};

struct FileDialogResult {
    ServiceRequestId id{};
    FileDialogStatus status{FileDialogStatus::Cancelled};
    std::vector<std::filesystem::path> paths;
    std::string error;
};

// Dialog backends may complete on another thread (as SDL3 does on macOS).
// Results are therefore polled and applied by the application's main thread.
class FileDialogService {
public:
    virtual ~FileDialogService() = default;
    virtual void request(FileDialogRequest request) = 0;
    [[nodiscard]] virtual std::vector<FileDialogResult> take_completed() = 0;
};

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    std::string method{"GET"};
    std::string url;
    std::vector<HttpHeader> headers;
    std::string body;
    std::chrono::milliseconds connect_timeout{10'000};
    std::chrono::milliseconds operation_timeout{180'000};
    std::size_t maximum_response_bytes{2'000'000};
};

struct HttpResponse {
    int status{};
    std::vector<HttpHeader> headers;
    std::string body;
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty() && status >= 200 && status < 300;
    }
};

// Blocking by design: the application schedules transports on a worker and
// uses the stop token for cancellation.
class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    [[nodiscard]] virtual HttpResponse perform(
        const HttpRequest& request,
        std::stop_token stop_token) = 0;
};

class AppDirectories {
public:
    virtual ~AppDirectories() = default;
    [[nodiscard]] virtual std::filesystem::path settings() const = 0;
    [[nodiscard]] virtual std::filesystem::path cache() const = 0;
    [[nodiscard]] virtual std::filesystem::path logs() const = 0;
    [[nodiscard]] virtual std::filesystem::path recovery() const = 0;
};

class AtomicFileWriter {
public:
    virtual ~AtomicFileWriter() = default;
    [[nodiscard]] virtual bool write(
        const std::filesystem::path& destination,
        std::span<const std::byte> bytes,
        std::string& error) = 0;
};

class AssetLocator {
public:
    virtual ~AssetLocator() = default;
    [[nodiscard]] virtual std::optional<std::filesystem::path> find(
        std::string_view asset_id) const = 0;
};

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(std::string_view message) = 0;
};

class SecretProvider {
public:
    virtual ~SecretProvider() = default;
    [[nodiscard]] virtual std::optional<std::string> read(
        std::string_view reference_name) const = 0;
};

enum class SystemTheme {
    Dark,
    Light,
};

class SystemAppearance {
public:
    virtual ~SystemAppearance() = default;
    [[nodiscard]] virtual SystemTheme theme() const noexcept = 0;
    [[nodiscard]] virtual float scale() const noexcept = 0;
};

// Offline services are required. Network and secret access remain optional so
// the viewer can always load, analyze, and save telemetry without a connection.
struct PlatformServices {
    FileDialogService* file_dialogs{};
    AppDirectories* directories{};
    AtomicFileWriter* files{};
    AssetLocator* assets{};
    LogSink* log{};
    SystemAppearance* appearance{};
    HttpTransport* http{};
    SecretProvider* secrets{};

    [[nodiscard]] bool offline_ready() const noexcept {
        return file_dialogs && directories && files && assets && log && appearance;
    }
};

}  // namespace racebox::application
