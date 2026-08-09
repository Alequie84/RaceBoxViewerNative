#pragma once

#include "racebox/race_day.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace racebox::setup {

inline constexpr int kSchemaVersion = 1;

struct TemplateRecord {
    std::string id;
    std::string display_name;
    std::string source_sha256;
    std::filesystem::path managed_source_path;
    std::vector<race_day::SetupFieldValue> fields;
    std::string created_at_utc;
};

struct RevisionRecord {
    std::string id;
    std::string template_id;
    std::string parent_revision_id;
    std::string car_profile_id;
    std::string run_id;
    std::string display_name;
    std::string source_sha256;
    std::string rendered_sha256;
    std::filesystem::path managed_source_path;
    std::filesystem::path managed_rendered_path;
    bool partially_known{};
    bool needs_reconciliation{};
    std::string untracked_changes;
    std::vector<race_day::SetupFieldValue> fields;
    std::string created_at_utc;
};

struct ImportResult {
    bool ok{};
    bool duplicate{};
    TemplateRecord setup_template;
    RevisionRecord base_revision;
    std::string error;
};

struct OpenOptions {
    std::filesystem::path database_path;
    std::filesystem::path managed_store_path;
};

// Windows-local setup history. Revisions are insert-only: callers create a
// child revision instead of modifying one that may already be linked to a run.
class Library {
public:
    Library();
    ~Library();
    Library(Library&&) noexcept;
    Library& operator=(Library&&) noexcept;
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    bool open(OpenOptions options = {}, std::string* error = nullptr);
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] int schema_version() const noexcept;
    [[nodiscard]] const std::filesystem::path& database_path() const noexcept;
    [[nodiscard]] const std::filesystem::path& managed_store_path() const noexcept;

    bool upsert_car_profile(const race_day::CarProfileSnapshot& profile,
                            std::string* error = nullptr);
    [[nodiscard]] std::vector<race_day::CarProfileSnapshot> car_profiles(
        std::string* error = nullptr) const;
    [[nodiscard]] std::optional<race_day::CarProfileSnapshot> car_profile(
        const std::string& id, std::string* error = nullptr) const;

    ImportResult import_pdf(const std::filesystem::path& source,
                            std::string display_name,
                            const std::string& car_profile_id = {});
    [[nodiscard]] std::optional<TemplateRecord> setup_template(
        const std::string& id, std::string* error = nullptr) const;
    [[nodiscard]] std::vector<TemplateRecord> templates(
        std::string* error = nullptr) const;
    bool update_template_field_map(
        const std::string& template_id,
        const std::vector<race_day::SetupFieldValue>& fields,
        std::string* error = nullptr);

    bool create_revision(RevisionRecord revision,
                         RevisionRecord* stored = nullptr,
                         std::string* error = nullptr);
    [[nodiscard]] std::optional<RevisionRecord> revision(
        const std::string& id, std::string* error = nullptr) const;
    [[nodiscard]] std::vector<RevisionRecord> revisions_for_car(
        const std::string& car_profile_id,
        std::string* error = nullptr) const;

    // Copies a generated PDF into the managed store and verifies its digest.
    bool manage_rendered_pdf(const std::filesystem::path& source,
                             std::filesystem::path& managed_path,
                             std::string& sha256,
                             std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string sha256_file(const std::filesystem::path& path,
                                      std::string* error = nullptr);
[[nodiscard]] race_day::SetupSheetSnapshot snapshot(const RevisionRecord& revision);

}  // namespace racebox::setup
