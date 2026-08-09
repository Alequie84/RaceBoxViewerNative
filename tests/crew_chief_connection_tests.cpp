#include "racebox/credential_store.hpp"
#include "racebox/crew_chief_connection.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace racebox;

int main() {
    using crew_chief_connection::Mode;

    const auto https = crew_chief_connection::validate_gateway_url(" https://gateway.example.test/ ");
    assert(https.ok);
    assert(https.normalized_url == "https://gateway.example.test");
    assert(crew_chief_connection::chat_endpoint(https.normalized_url) ==
           "https://gateway.example.test/v1/crew-chief/chat");

    const auto tailscale = crew_chief_connection::validate_gateway_url("http://100.64.10.20:18804");
    assert(tailscale.ok);
    assert(crew_chief_connection::validate_gateway_url("http://100.127.255.255:18804").ok);
    assert(!crew_chief_connection::validate_gateway_url("http://100.128.0.1:18804").ok);
    assert(!crew_chief_connection::validate_gateway_url("http://192.168.1.20:18804").ok);
    assert(!crew_chief_connection::validate_gateway_url("https://user:secret@example.test").ok);
    assert(!crew_chief_connection::validate_gateway_url("https://example.test/v1/crew-chief/chat").ok);

    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path() /
        ("racebox-connection-test-" + unique);
    const auto path = directory / "connection.json";
    crew_chief_connection::Settings settings;
    settings.mode = Mode::UserGateway;
    settings.gateway_url = tailscale.normalized_url;
    std::string error;
    assert(crew_chief_connection::save_settings(path, settings, &error));
    const auto loaded = crew_chief_connection::load_settings(path, &error);
    assert(loaded.mode == Mode::UserGateway);
    assert(loaded.gateway_url == tailscale.normalized_url);
    std::ifstream input(path, std::ios::binary);
    const std::string saved((std::istreambuf_iterator<char>(input)), {});
    assert(saved.find("racebox-crew-chief-connection-v1") != std::string::npos);
    assert(saved.find("prompt_revision") != std::string::npos);
    assert(saved.find("raw_token") == std::string::npos);
    assert(saved.find("test-client-secret") == std::string::npos);

    settings.mode = Mode::OfflineDemo;
    settings.gateway_url = "https://must-not-be-saved.example.test";
    assert(crew_chief_connection::save_settings(path, settings, &error));
    const auto demo_settings = crew_chief_connection::load_settings(path, &error);
    assert(demo_settings.mode == Mode::OfflineDemo);
    assert(demo_settings.gateway_url.empty());
    const auto demo = crew_chief_connection::scripted_demo_response("What changed?");
    assert(demo.ok);
    assert(demo.report.route == "offline-demo");
    assert(demo.report.summary.find("OFFLINE DEMO") != std::string::npos);

    const auto credential_reference = "RaceBox/CrewChiefGatewayToken-Test-" + unique;
    assert(credential_store::write(credential_reference, "test-client-secret", &error));
    const auto stored = credential_store::read(credential_reference, &error);
    assert(stored && *stored == "test-client-secret");
    assert(credential_store::erase(credential_reference, &error));
    assert(!credential_store::read(credential_reference, nullptr));

    std::filesystem::remove_all(directory);
    std::cout << "Crew Chief connection, demo, URL-policy, and credential-storage tests passed\n";
    return 0;
}
