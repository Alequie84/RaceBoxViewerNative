#include "racebox/crew_chief_connection.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace racebox::crew_chief_connection {
namespace {

using nlohmann::json;

std::string trim(std::string_view value) {
    auto first = value.begin();
    auto last = value.end();
    while (first != last && std::isspace(static_cast<unsigned char>(*first))) ++first;
    while (last != first && std::isspace(static_cast<unsigned char>(*(last - 1)))) --last;
    return {first, last};
}

bool parse_ipv4(std::string_view host, std::array<int, 4>& octets) {
    std::size_t start = 0;
    for (std::size_t index = 0; index < octets.size(); ++index) {
        const auto end = index == octets.size() - 1 ? host.size() : host.find('.', start);
        if (end == std::string_view::npos || end == start || end - start > 3) return false;
        int value = 0;
        for (auto position = start; position < end; ++position) {
            if (!std::isdigit(static_cast<unsigned char>(host[position]))) return false;
            value = value * 10 + (host[position] - '0');
        }
        if (value > 255) return false;
        octets[index] = value;
        start = end + 1;
    }
    return start == host.size() + 1;
}

bool safe_cleartext_host(std::string_view host) {
    std::string lowered(host);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (lowered == "localhost" || lowered == "[::1]") return true;
    std::array<int, 4> octets{};
    if (!parse_ipv4(lowered, octets)) return false;
    if (octets[0] == 127) return true;
    // Tailscale IPv4 addresses are allocated from 100.64.0.0/10.
    return octets[0] == 100 && octets[1] >= 64 && octets[1] <= 127;
}

}  // namespace

ValidationResult validate_gateway_url(std::string_view value) {
    ValidationResult result;
    auto url = trim(value);
    while (url.size() > 1 && url.back() == '/') url.pop_back();
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        result.error = "Enter a complete gateway URL beginning with https:// or http://";
        return result;
    }
    std::string scheme = url.substr(0, scheme_end);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
        [](unsigned char current) { return static_cast<char>(std::tolower(current)); });
    if (scheme != "https" && scheme != "http") {
        result.error = "The gateway URL must use HTTPS, or HTTP on Tailscale/loopback";
        return result;
    }
    const auto authority_start = scheme_end + 3;
    const auto path_start = url.find_first_of("/?#", authority_start);
    const auto authority = url.substr(
        authority_start,
        path_start == std::string::npos ? std::string::npos : path_start - authority_start);
    if (authority.empty() || authority.find('@') != std::string::npos) {
        result.error = "The gateway URL has an invalid host or embedded credentials";
        return result;
    }
    if (path_start != std::string::npos && url[path_start] != '/') {
        result.error = "The gateway URL must not contain a query or fragment";
        return result;
    }
    if (path_start != std::string::npos && url.substr(path_start) != "/") {
        result.error = "Enter the gateway base URL without an API path, query, or fragment";
        return result;
    }
    std::string host;
    if (authority.front() == '[') {
        const auto bracket = authority.find(']');
        if (bracket == std::string::npos) {
            result.error = "The gateway IPv6 host is invalid";
            return result;
        }
        host = authority.substr(0, bracket + 1);
        if (bracket + 1 < authority.size() && authority[bracket + 1] != ':') {
            result.error = "The gateway port is invalid";
            return result;
        }
    } else {
        const auto colon = authority.find(':');
        host = authority.substr(0, colon);
    }
    if (host.empty()) {
        result.error = "The gateway host is missing";
        return result;
    }
    if (scheme == "http" && !safe_cleartext_host(host)) {
        result.error = "Cleartext HTTP is allowed only for Tailscale 100.64.0.0/10 or loopback; use HTTPS for other hosts";
        return result;
    }
    result.ok = true;
    result.normalized_url = scheme + url.substr(scheme_end);
    return result;
}

std::string chat_endpoint(std::string_view base) {
    return std::string(base) + "/v1/crew-chief/chat";
}

std::string health_endpoint(std::string_view base) {
    return std::string(base) + "/health";
}

Settings load_settings(const std::filesystem::path& path, std::string* warning) {
    Settings settings;
    try {
        if (!std::filesystem::exists(path)) return settings;
        std::ifstream input(path, std::ios::binary);
        const auto value = json::parse(input);
        if (value.value("contract", std::string{}) != kContract) {
            if (warning) *warning = "Unsupported Crew Chief connection settings were ignored";
            return settings;
        }
        settings.mode = value.value("mode", "offline_demo") == "user_gateway"
            ? Mode::UserGateway : Mode::OfflineDemo;
        settings.gateway_url = value.value("gateway_url", std::string{});
        settings.credential_reference = value.value(
            "credential_reference", std::string(kCredentialReference));
        if (settings.mode == Mode::UserGateway) {
            const auto validated = validate_gateway_url(settings.gateway_url);
            if (!validated.ok) {
                if (warning) *warning = "Saved Crew Chief gateway was invalid; Offline demo was selected";
                return {};
            }
            settings.gateway_url = validated.normalized_url;
        }
        return settings;
    } catch (const std::exception& exception) {
        if (warning) *warning = std::string("Crew Chief settings could not be read: ") + exception.what();
        return {};
    }
}

bool save_settings(
    const std::filesystem::path& path,
    const Settings& settings,
    std::string* error) {
    try {
        auto stored = settings;
        if (stored.mode == Mode::UserGateway) {
            const auto validated = validate_gateway_url(stored.gateway_url);
            if (!validated.ok) throw std::runtime_error(validated.error);
            stored.gateway_url = validated.normalized_url;
        } else {
            stored.gateway_url.clear();
        }
        const json value{
            {"contract", kContract},
            {"mode", stored.mode == Mode::UserGateway ? "user_gateway" : "offline_demo"},
            {"gateway_url", stored.gateway_url},
            {"credential_reference", stored.credential_reference},
            {"health_contract", kHealthContract},
            {"prompt_revision", kPromptRevision},
        };
        std::filesystem::create_directories(path.parent_path());
        const auto temporary = path.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("could not open the temporary settings file");
            output << value.dump(2) << '\n';
            if (!output) throw std::runtime_error("could not write the settings file");
        }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::rename(temporary, path);
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

crew_chief::Response scripted_demo_response(
    std::string_view question,
    std::string_view scope) {
    crew_chief::Response response;
    response.ok = true;
    response.http_status = 200;
    response.report.verdict = "demo";
    response.report.confidence = 72;
    response.report.summary =
        "OFFLINE DEMO — Richmond fixture: the quicker lap carries more speed through the flowing section, "
        "but one lap is not enough to prove the setup change caused the gain. Repeat the same change for "
        "three clean laps and compare the median, steering correction, and corner-exit speed.";
    if (!question.empty()) {
        response.report.summary += " Your question was answered from scripted local evidence only.";
    }
    response.report.observations.push_back({
        "Richmond", "The comparison lap is quicker in the scripted fixture",
        "Treat this as a test direction, not proof of causality", {"demo-richmond-telemetry"}});
    response.report.confounds = {
        "Offline demo uses a fixed Richmond recording",
        "Driver consistency and track conditions can explain part of the difference",
    };
    response.report.next_test =
        "Make one change, record at least three clean laps in each condition, and compare repeatable median pace.";
    response.report.causality_note =
        "This is a scripted demonstration; it does not claim the setting caused the measured change.";
    response.report.route = "offline-demo";
    response.report.model = "scripted-local-demo";
    response.report.thinking = "none";
    response.report.selected_lane = std::string(scope);
    response.report.evidence.analytics_contract = "racebox-crew-chief-evidence-v1";
    response.report.evidence.formula_version = 1;
    response.report.evidence.quality_confidence = 72;
    return response;
}

}  // namespace racebox::crew_chief_connection
