#include "racebox/source_identity.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using racebox::source_identity::Candidate;
using racebox::source_identity::MatchOptions;
using racebox::source_identity::MatchStatus;
using racebox::source_identity::PathCaseMode;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        throw std::runtime_error(message);
    }
}

Candidate candidate(
    std::string id,
    std::string filename,
    std::uint64_t size,
    std::int64_t modified,
    std::optional<std::string_view> fingerprint = std::nullopt) {
    return {
        std::move(id),
        racebox::source_identity::make_source_identity(filename, size, modified, fingerprint),
    };
}

}  // namespace

int main() {
    using namespace racebox::source_identity;

    const auto expected = make_source_identity(
        R"(C:\private\RaceBox Session.csv)",
        42'000,
        1'700'000'000'000'000'000LL,
        "SHA256:ABCDEF");
    require(expected.canonical_filename == "RaceBox Session.csv", "identity retains basename only");
    require(expected.content_fingerprint == "sha256:abcdef", "fingerprint is normalized");
    require(expected.redacted_display_name() == "Telemetry source (.csv)", "redacted display hides source name");

    {
        const std::vector<Candidate> candidates{
            candidate("wrong", "RaceBox Session.csv", 42'000, 1'700'000'000'000'000'000LL, "sha256:000000"),
            candidate("exact", "RaceBox Session.csv", 42'000, 1'700'000'000'000'000'000LL, "sha256:abcdef"),
        };
        const auto result = match_source(expected, candidates);
        require(result.status == MatchStatus::Exact, "matching fingerprint is exact");
        require(result.recommended_candidate_id == "exact", "unique exact fingerprint is recommended");
        require(!result.requires_user_confirmation, "exact fingerprint can relink without confirmation");
        require(result.suggestions.front().candidate_id == "exact" &&
                    result.suggestions.front().score == 100,
                "exact fingerprint ranks first");
    }

    {
        const auto renamed_expected = make_source_identity(
            "original-session.vbo", 99'000, 1'700'000'123'000'000'000LL);
        const std::vector<Candidate> candidates{
            candidate("renamed", "mains-session.vbo", 99'000, 1'700'000'123'500'000'000LL),
            candidate("name-only", "original-session.vbo", 10, 7),
        };
        const auto result = match_source(renamed_expected, candidates);
        require(result.status == MatchStatus::Probable, "renamed size/time match is probable");
        require(result.recommended_candidate_id == "renamed", "renamed candidate is suggested");
        require(result.requires_user_confirmation, "probable rename requires confirmation");
        require(result.suggestions.front().score == 60 &&
                    result.suggestions.front().size_match &&
                    result.suggestions.front().modified_time_match &&
                    !result.suggestions.front().filename_match,
                "renamed evidence is disclosed");
    }

    {
        const auto ambiguous_expected = make_source_identity("session.csv", 50'000, 90'000'000'000LL);
        const std::vector<Candidate> candidates{
            candidate("copy-b", "renamed-b.csv", 50'000, 90'000'000'000LL),
            candidate("copy-a", "renamed-a.csv", 50'000, 90'000'000'000LL),
        };
        const auto result = match_source(ambiguous_expected, candidates);
        require(result.status == MatchStatus::Ambiguous, "equally likely candidates are ambiguous");
        require(!result.recommended_candidate_id, "ambiguous match is never silently selected");
        require(result.requires_user_confirmation, "ambiguous candidates require user choice");
        require(result.suggestions[0].candidate_id == "copy-a" &&
                    result.suggestions[1].candidate_id == "copy-b",
                "ambiguous ranking is deterministic");
    }

    {
        const auto missing_expected = make_source_identity("lost.gpx", 123, 4'000'000'000LL);
        const std::vector<Candidate> candidates{
            candidate("unrelated", "different.csv", 999, 8'000'000'000LL),
        };
        const auto result = match_source(missing_expected, candidates);
        require(result.status == MatchStatus::Missing, "unrelated candidates produce missing");
        require(!result.recommended_candidate_id, "missing source has no selection");
        require(result.suggestions.size() == 1 && !result.suggestions.front().eligible,
                "missing result can still disclose a ranked rejection");
    }

    {
        const auto case_expected = make_source_identity("SESSION.CSV", 1'000, 20'000'000'000LL);
        const std::vector<Candidate> candidates{
            candidate("case", "session.csv", 1'000, 60'000'000'000LL),
        };

        MatchOptions windows_options;
        windows_options.path_case = PathCaseMode::Insensitive;
        const auto windows_result = match_source(case_expected, candidates, windows_options);
        require(windows_result.status == MatchStatus::Probable &&
                    windows_result.recommended_candidate_id == "case",
                "Windows-style matching is case insensitive");

        MatchOptions portable_sensitive_options;
        portable_sensitive_options.path_case = PathCaseMode::Sensitive;
        const auto sensitive_result = match_source(case_expected, candidates, portable_sensitive_options);
        require(sensitive_result.status == MatchStatus::Missing,
                "cross-platform model can use case-sensitive matching");

#if defined(_WIN32)
        const auto platform_result = match_source(case_expected, candidates);
        require(platform_result.status == MatchStatus::Probable,
                "Windows platform default is case insensitive");
#endif
    }

    {
        const auto duplicate_expected = make_source_identity(
            "session.csv", 1'000, 20'000'000'000LL, "sha256:duplicate");
        const std::vector<Candidate> candidates{
            candidate("duplicate-a", "session.csv", 1'000, 20'000'000'000LL, "sha256:duplicate"),
            candidate("duplicate-b", "copy.csv", 1'000, 20'000'000'000LL, "sha256:duplicate"),
        };
        const auto result = match_source(duplicate_expected, candidates);
        require(result.status == MatchStatus::Ambiguous,
                "duplicate exact fingerprints require an explicit choice");
        require(!result.recommended_candidate_id, "duplicate exact fingerprints are never selected silently");
    }

    {
        const auto pre_epoch = make_source_identity("old.vbo", 4'000, -5'000'000'000LL);
        const std::vector<Candidate> candidates{
            candidate("old-renamed", "renamed-old.vbo", 4'000, -4'000'000'000LL),
        };
        const auto result = match_source(pre_epoch, candidates);
        require(result.status == MatchStatus::Probable &&
                    result.suggestions.front().modified_time_match,
                "nearby pre-epoch timestamps compare without signed overflow");

        const auto extreme = make_source_identity(
            "extreme.vbo", 4'000, std::numeric_limits<std::int64_t>::min());
        const std::vector<Candidate> extreme_candidates{
            candidate(
                "opposite-extreme",
                "renamed-extreme.vbo",
                4'000,
                std::numeric_limits<std::int64_t>::max()),
        };
        MatchOptions maximum_tolerance;
        maximum_tolerance.modified_time_tolerance_ns = std::numeric_limits<std::int64_t>::max();
        const auto extreme_result = match_source(extreme, extreme_candidates, maximum_tolerance);
        require(extreme_result.status == MatchStatus::Missing &&
                    !extreme_result.suggestions.front().modified_time_match,
                "opposite timestamp extremes do not overflow into a match");
    }

    {
        const auto guarded = make_source_identity("guarded.csv", 100, 1'000);
        const std::vector<Candidate> zero_evidence{
            candidate("zero", "other.vbo", 999, 9'000'000'000LL),
        };
        MatchOptions pathological_threshold;
        pathological_threshold.probable_score_threshold = std::numeric_limits<int>::min();
        const auto guarded_result = match_source(guarded, zero_evidence, pathological_threshold);
        require(guarded_result.status == MatchStatus::Missing &&
                    !guarded_result.suggestions.front().eligible,
                "pathological threshold cannot make zero evidence eligible");

        const std::vector<Candidate> separated{
            candidate("best", "guarded.csv", 100, 1'000),
            candidate("runner-up", "renamed.csv", 100, 1'000),
        };
        MatchOptions negative_margin;
        negative_margin.ambiguity_score_margin = std::numeric_limits<int>::min();
        const auto separated_result = match_source(guarded, separated, negative_margin);
        require(separated_result.status == MatchStatus::Probable &&
                    separated_result.recommended_candidate_id == "best",
                "negative ambiguity margin clamps to zero");
    }

    std::cout << "Source identity and relink matching tests passed\n";
    return 0;
}
