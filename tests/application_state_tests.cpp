#include "racebox/application/state.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <version>

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
#include "racebox/application/services.hpp"
#define RACEBOX_TEST_STOP_TOKEN_CONTRACT 1
#endif

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

racebox::LapInfo lap(racebox::Timestamp duration, racebox::LapPhase phase) {
    racebox::LapInfo value;
    value.duration_us = duration;
    value.phase = phase;
    return value;
}

#if defined(RACEBOX_TEST_STOP_TOKEN_CONTRACT)
class FakeHttpTransport final : public racebox::application::HttpTransport {
public:
    racebox::application::HttpResponse perform(
        const racebox::application::HttpRequest& request,
        std::stop_token stop_token) override {
        saw_stop_token = stop_token.stop_possible();
        return {request.url == "https://test.invalid" ? 204 : 400, {}, {}, {}};
    }

    bool saw_stop_token{};
};
#endif

}  // namespace

int main() {
    try {
        using namespace racebox;
        using namespace racebox::application;

        require(workspace_key(Workspace::RaceDay) == "race_day", "Race Day workspace key changed");
        require(workspace_key(Workspace::Session) == "session", "Session workspace key changed");
        require(workspace_key(Workspace::Compare) == "compare", "Compare workspace key changed");
        require(workspace_key(Workspace::CrewChief) == "crew_chief", "Crew Chief workspace key changed");
        require(workspace_key(Workspace::Reports) == "reports", "Reports workspace key changed");

        const std::array laps{
            lap(18'000'000, LapPhase::Complete),
            lap(16'280'000, LapPhase::Complete),
            lap(16'580'000, LapPhase::Complete),
            lap(4'000'000, LapPhase::OutLap),
        };

        LapRoleSelection requested;
        requested.reference_mode = ReferenceMode::FastestComplete;
        requested.reference = 0;
        requested.compare_a = 2;
        requested.playback = 3;
        const auto roles = resolve_lap_roles(laps, requested);
        require(roles.reference_ready, "Fastest complete reference was not resolved");
        require(roles.comparison_ready, "Reference plus Compare A should be ready");
        require(roles.selection.reference == 1, "Fastest complete reference was not selected");
        require(roles.selection.compare_a == 2, "Valid Compare A was not retained");
        require(!roles.selection.compare_b, "Optional Compare B was filled without a request");
        require(roles.selection.playback == 3, "Independent Playback selection was not retained");

        requested.reference_mode = ReferenceMode::Manual;
        requested.reference = 1;
        requested.compare_a = 1;
        requested.compare_b = 1;
        requested.playback = 99;
        const auto unique_roles = resolve_lap_roles(laps, requested);
        require(unique_roles.selection.reference == 1, "Manual complete reference was not retained");
        require(unique_roles.selection.compare_a == 2, "Duplicate Compare A was not replaced deterministically");
        require(unique_roles.selection.compare_b == 0, "Duplicate Compare B was not replaced deterministically");
        require(unique_roles.selection.playback == 1, "Invalid Playback did not fall back to Reference");

        const std::array insufficient{
            lap(16'900'000, LapPhase::Complete),
            lap(3'000'000, LapPhase::InLap),
        };
        const auto limited = resolve_lap_roles(insufficient, {});
        require(limited.reference_ready, "Single complete lap should still be a valid reference");
        require(!limited.comparison_ready, "Single complete lap must not claim comparison readiness");
        require(!limited.selection.compare_a, "Compare A should be empty when no second complete lap exists");

        PlaybackState playback;
        playback.speed = std::numeric_limits<double>::infinity();
        normalise_playback(playback);
        require(playback.speed == 1.0, "Non-finite playback speed did not recover to 1x");
        playback.speed = 0.01;
        normalise_playback(playback);
        require(playback.speed == 0.25, "Playback speed lower bound changed");
        playback.speed = 9.0;
        normalise_playback(playback);
        require(playback.speed == 2.0, "Playback speed upper bound changed");

        AppState state;
        state.workspace = Workspace::CrewChief;
        state.document.kind = DocumentKind::Session;
        state.document.dirty = true;
        state.selection.corner = 3;
        state.notifications.push_back({1, NotificationSeverity::Warning, "Test", {}, {}});
        reset_document(state);
        require(state.workspace == Workspace::CrewChief, "Document reset changed the active workspace");
        require(state.document.kind == DocumentKind::None, "Document reset retained the old document");
        require(!state.selection.corner, "Document reset retained the old selection");
        require(state.notifications.empty(), "Document reset retained document-specific notifications");

#if defined(RACEBOX_TEST_STOP_TOKEN_CONTRACT)
        FakeHttpTransport transport;
        std::stop_source stop_source;
        HttpRequest request;
        request.url = "https://test.invalid";
        const auto response = transport.perform(request, stop_source.get_token());
        require(response.ok(), "Platform-neutral HTTP response success rule failed");
        require(transport.saw_stop_token, "HTTP transport did not receive a cancellable stop token");

        PlatformServices services;
        services.http = &transport;
        require(!services.offline_ready(), "Network-only services must not claim offline readiness");
#endif

        std::cout << "Application state/service contract tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Application state/service contract tests failed: " << exception.what() << '\n';
        return 1;
    }
}
