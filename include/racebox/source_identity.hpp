#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace racebox::source_identity {

// The model stores a normalized UTC value rather than
// std::filesystem::file_time_type, whose epoch varies by standard library.
using ModifiedTimeUnixNs = std::int64_t;

enum class PathCaseMode {
    PlatformDefault,
    Sensitive,
    Insensitive,
};

enum class MatchStatus {
    Exact,
    Probable,
    Ambiguous,
    Missing,
};

struct SourceIdentity {
    // Basename only. Full source paths and file contents are deliberately not
    // part of this persistence-safe identity.
    std::string canonical_filename;
    std::uint64_t size_bytes{};
    std::optional<ModifiedTimeUnixNs> modified_time_unix_ns;
    // Caller-calculated, algorithm-tagged value such as "sha256:abcd...".
    // The engine normalizes ASCII case and surrounding whitespace but never
    // opens a source file or calculates a fingerprint itself.
    std::optional<std::string> content_fingerprint;

    [[nodiscard]] std::string redacted_display_name() const;
};

struct Candidate {
    // Opaque caller-owned identifier. It may index a path held outside this
    // privacy-bounded model.
    std::string id;
    SourceIdentity identity;
};

struct MatchSuggestion {
    std::string candidate_id;
    int score{};
    bool eligible{};
    bool fingerprint_match{};
    bool filename_match{};
    bool size_match{};
    bool modified_time_match{};
};

struct MatchOptions {
    PathCaseMode path_case{PathCaseMode::PlatformDefault};
    // Accommodates timestamps rounded by common archive and filesystem tools.
    ModifiedTimeUnixNs modified_time_tolerance_ns{2'000'000'000};
    int probable_score_threshold{60};
    int ambiguity_score_margin{10};
};

struct MatchResult {
    MatchStatus status{MatchStatus::Missing};
    // Set for the unique top exact/probable candidate. Probable matches remain
    // suggestions: callers must honor requires_user_confirmation before
    // replacing a stored source reference.
    std::optional<std::string> recommended_candidate_id;
    bool requires_user_confirmation{true};
    std::vector<MatchSuggestion> suggestions;
};

[[nodiscard]] PathCaseMode platform_default_path_case() noexcept;
[[nodiscard]] std::string canonical_filename(std::string_view path_or_filename);
[[nodiscard]] SourceIdentity make_source_identity(
    std::string_view path_or_filename,
    std::uint64_t size_bytes,
    std::optional<ModifiedTimeUnixNs> modified_time_unix_ns = std::nullopt,
    std::optional<std::string_view> content_fingerprint = std::nullopt);

[[nodiscard]] MatchResult match_source(
    const SourceIdentity& expected,
    std::span<const Candidate> candidates,
    const MatchOptions& options = {});

[[nodiscard]] std::string_view match_status_name(MatchStatus status) noexcept;

}  // namespace racebox::source_identity
