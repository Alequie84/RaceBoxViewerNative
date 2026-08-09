#pragma once

#include "racebox/crew_chief.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace racebox::crew_chief_connection {

inline constexpr std::string_view kContract{"racebox-crew-chief-connection-v1"};
inline constexpr std::string_view kHealthContract{"racebox-crew-chief-v14"};
inline constexpr std::string_view kPromptRevision{"racebox-crew-chief-behavior-v14-public-1"};
inline constexpr std::string_view kCredentialReference{"RaceBox/CrewChiefGatewayToken"};

enum class Mode {
    OfflineDemo,
    UserGateway,
};

struct Settings {
    Mode mode{Mode::OfflineDemo};
    std::string gateway_url;
    std::string credential_reference{std::string(kCredentialReference)};
};

struct ValidationResult {
    bool ok{};
    std::string normalized_url;
    std::string error;
};

[[nodiscard]] ValidationResult validate_gateway_url(std::string_view url);
[[nodiscard]] std::string chat_endpoint(std::string_view normalized_gateway_url);
[[nodiscard]] std::string health_endpoint(std::string_view normalized_gateway_url);

[[nodiscard]] Settings load_settings(
    const std::filesystem::path& path,
    std::string* warning = nullptr);
[[nodiscard]] bool save_settings(
    const std::filesystem::path& path,
    const Settings& settings,
    std::string* error = nullptr);

// A deterministic, clearly labelled response for judges and users who do not
// have an OpenClaw gateway. No network or model call is made.
[[nodiscard]] crew_chief::Response scripted_demo_response(
    std::string_view question,
    std::string_view scope = "Richmond telemetry");

}  // namespace racebox::crew_chief_connection
