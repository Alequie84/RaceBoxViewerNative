#include "racebox/source_identity.hpp"

#include <algorithm>
#include <iterator>
#include <limits>

namespace racebox::source_identity {
namespace {

constexpr int kFingerprintScore = 100;
constexpr int kFilenameScore = 30;
constexpr int kSizeScore = 35;
constexpr int kModifiedTimeScore = 25;

std::string ascii_lower(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(character >= 'A' && character <= 'Z'
            ? static_cast<char>(character - 'A' + 'a')
            : character);
    }
    return result;
}

bool is_ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' ||
           value == '\r' || value == '\f' || value == '\v';
}

std::string trim_ascii(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && is_ascii_space(value[begin])) ++begin;
    std::size_t end = value.size();
    while (end > begin && is_ascii_space(value[end - 1])) --end;
    return std::string(value.substr(begin, end - begin));
}

std::optional<std::string> normalized_fingerprint(const std::optional<std::string>& value) {
    if (!value) return std::nullopt;
    auto normalized = ascii_lower(trim_ascii(*value));
    if (normalized.empty()) return std::nullopt;
    return normalized;
}

PathCaseMode resolved_case_mode(PathCaseMode mode) noexcept {
    return mode == PathCaseMode::PlatformDefault ? platform_default_path_case() : mode;
}

bool filename_equal(std::string_view left, std::string_view right, PathCaseMode mode) {
    if (resolved_case_mode(mode) == PathCaseMode::Insensitive) {
        return ascii_lower(left) == ascii_lower(right);
    }
    return left == right;
}

bool modified_time_equal(
    const std::optional<ModifiedTimeUnixNs>& left,
    const std::optional<ModifiedTimeUnixNs>& right,
    ModifiedTimeUnixNs tolerance) noexcept {
    if (!left || !right) return false;
    const auto safe_tolerance = std::max<ModifiedTimeUnixNs>(0, tolerance);
    const auto magnitude = [](ModifiedTimeUnixNs value) noexcept {
        return value >= 0
            ? static_cast<std::uint64_t>(value)
            : static_cast<std::uint64_t>(-(value + 1)) + 1;
    };
    std::uint64_t difference{};
    if ((*left < 0) == (*right < 0)) {
        difference = *left >= *right
            ? static_cast<std::uint64_t>(*left - *right)
            : static_cast<std::uint64_t>(*right - *left);
    } else {
        const auto left_magnitude = magnitude(*left);
        const auto right_magnitude = magnitude(*right);
        difference = left_magnitude > std::numeric_limits<std::uint64_t>::max() - right_magnitude
            ? std::numeric_limits<std::uint64_t>::max()
            : left_magnitude + right_magnitude;
    }
    return difference <= static_cast<std::uint64_t>(safe_tolerance);
}

std::string sort_key(std::string_view value, PathCaseMode mode) {
    return resolved_case_mode(mode) == PathCaseMode::Insensitive
        ? ascii_lower(value)
        : std::string(value);
}

struct ScoredCandidate {
    MatchSuggestion suggestion;
    std::string filename_key;
    std::string id_key;
    bool exact_fingerprint{};
};

ScoredCandidate score_candidate(
    const SourceIdentity& expected,
    const Candidate& candidate,
    const MatchOptions& options) {
    ScoredCandidate scored;
    scored.suggestion.candidate_id = candidate.id;
    scored.suggestion.filename_match = filename_equal(
        expected.canonical_filename, candidate.identity.canonical_filename, options.path_case);
    scored.suggestion.size_match = expected.size_bytes == candidate.identity.size_bytes;
    scored.suggestion.modified_time_match = modified_time_equal(
        expected.modified_time_unix_ns,
        candidate.identity.modified_time_unix_ns,
        options.modified_time_tolerance_ns);

    const auto expected_fingerprint = normalized_fingerprint(expected.content_fingerprint);
    const auto candidate_fingerprint = normalized_fingerprint(candidate.identity.content_fingerprint);
    scored.suggestion.fingerprint_match =
        expected_fingerprint && candidate_fingerprint && *expected_fingerprint == *candidate_fingerprint;

    const auto fingerprint_conflict =
        expected_fingerprint && candidate_fingerprint && *expected_fingerprint != *candidate_fingerprint;
    const auto impossible_fingerprint_size =
        scored.suggestion.fingerprint_match && !scored.suggestion.size_match;

    if (!fingerprint_conflict && !impossible_fingerprint_size) {
        if (scored.suggestion.fingerprint_match) {
            scored.suggestion.score = kFingerprintScore;
            scored.exact_fingerprint = true;
        } else {
            if (scored.suggestion.filename_match) scored.suggestion.score += kFilenameScore;
            if (scored.suggestion.size_match) scored.suggestion.score += kSizeScore;
            if (scored.suggestion.modified_time_match) scored.suggestion.score += kModifiedTimeScore;
        }
    }

    const auto probable_threshold = std::clamp(
        options.probable_score_threshold, 1, kFingerprintScore);
    scored.suggestion.eligible =
        scored.exact_fingerprint || scored.suggestion.score >= probable_threshold;
    scored.filename_key = sort_key(candidate.identity.canonical_filename, options.path_case);
    scored.id_key = sort_key(candidate.id, options.path_case);
    return scored;
}

}  // namespace

PathCaseMode platform_default_path_case() noexcept {
#if defined(_WIN32)
    return PathCaseMode::Insensitive;
#else
    return PathCaseMode::Sensitive;
#endif
}

std::string canonical_filename(std::string_view path_or_filename) {
    while (!path_or_filename.empty() &&
           (path_or_filename.back() == '/' || path_or_filename.back() == '\\')) {
        path_or_filename.remove_suffix(1);
    }
    const auto separator = path_or_filename.find_last_of("/\\");
    return std::string(separator == std::string_view::npos
        ? path_or_filename
        : path_or_filename.substr(separator + 1));
}

SourceIdentity make_source_identity(
    std::string_view path_or_filename,
    std::uint64_t size_bytes,
    std::optional<ModifiedTimeUnixNs> modified_time_unix_ns,
    std::optional<std::string_view> content_fingerprint) {
    SourceIdentity result;
    result.canonical_filename = canonical_filename(path_or_filename);
    result.size_bytes = size_bytes;
    result.modified_time_unix_ns = modified_time_unix_ns;
    if (content_fingerprint) {
        auto normalized = ascii_lower(trim_ascii(*content_fingerprint));
        if (!normalized.empty()) result.content_fingerprint = std::move(normalized);
    }
    return result;
}

std::string SourceIdentity::redacted_display_name() const {
    const auto dot = canonical_filename.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= canonical_filename.size()) {
        return "Telemetry source";
    }
    return "Telemetry source (." + ascii_lower(std::string_view(canonical_filename).substr(dot + 1)) + ')';
}

MatchResult match_source(
    const SourceIdentity& expected,
    std::span<const Candidate> candidates,
    const MatchOptions& options) {
    MatchResult result;
    std::vector<ScoredCandidate> scored;
    scored.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        scored.push_back(score_candidate(expected, candidate, options));
    }

    std::sort(scored.begin(), scored.end(), [](const ScoredCandidate& left, const ScoredCandidate& right) {
        if (left.suggestion.score != right.suggestion.score) {
            return left.suggestion.score > right.suggestion.score;
        }
        if (left.suggestion.fingerprint_match != right.suggestion.fingerprint_match) {
            return left.suggestion.fingerprint_match;
        }
        if (left.suggestion.filename_match != right.suggestion.filename_match) {
            return left.suggestion.filename_match;
        }
        if (left.suggestion.modified_time_match != right.suggestion.modified_time_match) {
            return left.suggestion.modified_time_match;
        }
        if (left.filename_key != right.filename_key) return left.filename_key < right.filename_key;
        if (left.id_key != right.id_key) return left.id_key < right.id_key;
        return left.suggestion.candidate_id < right.suggestion.candidate_id;
    });

    result.suggestions.reserve(scored.size());
    for (const auto& entry : scored) result.suggestions.push_back(entry.suggestion);

    const auto exact_count = static_cast<std::size_t>(std::count_if(
        scored.begin(), scored.end(), [](const ScoredCandidate& entry) { return entry.exact_fingerprint; }));
    if (exact_count == 1) {
        const auto exact = std::find_if(
            scored.begin(), scored.end(), [](const ScoredCandidate& entry) { return entry.exact_fingerprint; });
        result.status = MatchStatus::Exact;
        result.recommended_candidate_id = exact->suggestion.candidate_id;
        result.requires_user_confirmation = false;
        return result;
    }
    if (exact_count > 1) {
        result.status = MatchStatus::Ambiguous;
        return result;
    }

    const auto eligible = std::count_if(scored.begin(), scored.end(), [](const ScoredCandidate& entry) {
        return entry.suggestion.eligible;
    });
    if (eligible == 0) {
        result.status = MatchStatus::Missing;
        return result;
    }

    const auto& best = scored.front();
    if (eligible > 1) {
        const auto runner_up = std::find_if(std::next(scored.begin()), scored.end(), [](const ScoredCandidate& entry) {
            return entry.suggestion.eligible;
        });
        if (runner_up != scored.end() &&
            best.suggestion.score - runner_up->suggestion.score <=
                std::clamp(options.ambiguity_score_margin, 0, kFingerprintScore)) {
            result.status = MatchStatus::Ambiguous;
            return result;
        }
    }

    result.status = MatchStatus::Probable;
    result.recommended_candidate_id = best.suggestion.candidate_id;
    return result;
}

std::string_view match_status_name(MatchStatus status) noexcept {
    switch (status) {
        case MatchStatus::Exact: return "exact";
        case MatchStatus::Probable: return "probable";
        case MatchStatus::Ambiguous: return "ambiguous";
        case MatchStatus::Missing: return "missing";
    }
    return "missing";
}

}  // namespace racebox::source_identity
