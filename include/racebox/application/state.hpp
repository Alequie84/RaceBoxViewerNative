#pragma once

#include "racebox/session.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace racebox::application {

enum class Workspace {
    RaceDay,
    Session,
    Compare,
    CrewChief,
    Reports,
};

[[nodiscard]] std::string_view workspace_key(Workspace workspace) noexcept;

enum class DocumentKind {
    None,
    Session,
    RaceDay,
};

struct DocumentState {
    DocumentKind kind{DocumentKind::None};
    std::filesystem::path source;
    bool dirty{};
    std::uint64_t generation{};
};

enum class ReferenceMode {
    Manual,
    FastestComplete,
};

struct LapRoleSelection {
    ReferenceMode reference_mode{ReferenceMode::FastestComplete};
    std::optional<std::size_t> reference;
    std::optional<std::size_t> compare_a;
    std::optional<std::size_t> compare_b;
    std::optional<std::size_t> playback;
};

struct ResolvedLapRoles {
    LapRoleSelection selection;
    bool reference_ready{};
    bool comparison_ready{};
};

// Resolves invalid or duplicate comparison roles deterministically. Reference,
// Compare A, and Compare B must be distinct complete laps. Playback is
// intentionally independent and may point to any lap, including the reference.
[[nodiscard]] ResolvedLapRoles resolve_lap_roles(
    std::span<const LapInfo> laps,
    const LapRoleSelection& requested);

struct SelectionContext {
    std::optional<std::size_t> run;
    std::optional<std::size_t> lap;
    std::optional<std::size_t> corner;
    std::optional<std::size_t> event;
    std::optional<std::size_t> sector;
    std::optional<std::size_t> insight;
    Timestamp cursor_us{};
};

struct PlaybackState {
    bool playing{};
    bool repeat{};
    double speed{1.0};
};

void normalise_playback(PlaybackState& playback) noexcept;

enum class JobStatus {
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
};

struct JobState {
    std::uint64_t id{};
    JobStatus status{JobStatus::Queued};
    std::string label;
    std::string stage;
    float progress{};
    bool cancellable{};
};

enum class NotificationSeverity {
    Information,
    Warning,
    Error,
};

struct Notification {
    std::uint64_t id{};
    NotificationSeverity severity{NotificationSeverity::Information};
    std::string summary;
    std::string detail;
    std::string action_command;
};

struct AppState {
    Workspace workspace{Workspace::Session};
    DocumentState document;
    LapRoleSelection lap_roles;
    SelectionContext selection;
    PlaybackState playback;
    std::vector<JobState> jobs;
    std::vector<Notification> notifications;
};

// Clears document-specific state while retaining the user's active workspace.
void reset_document(AppState& state) noexcept;

}  // namespace racebox::application
