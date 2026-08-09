#include "racebox/setup_library.hpp"

#include "racebox/core.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winsqlite/winsqlite3.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <span>
#include <system_error>
#include <utility>

namespace racebox::setup {
namespace {

using nlohmann::json;

constexpr std::uintmax_t kMaximumPdfBytes = 64ULL * 1024ULL * 1024ULL;

std::string sqlite_error(sqlite3* database, std::string_view operation) {
    std::string result(operation);
    result += ": ";
    result += database ? sqlite3_errmsg(database) : "database is not open";
    return result;
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const char character : value) {
        utf8.push_back(static_cast<char8_t>(character));
    }
    return std::filesystem::path(utf8);
}

std::string bounded(std::string value, std::size_t maximum) {
    if (value.size() > maximum) value.resize(maximum);
    return value;
}

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_s(&utc, &seconds);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string random_id(std::string_view prefix) {
    std::array<unsigned char, 16> bytes{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(
            nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        const auto ticks = std::chrono::high_resolution_clock::now()
                               .time_since_epoch().count();
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<unsigned char>(
                static_cast<std::uint64_t>(ticks) >> ((index % 8) * 8));
        }
    }
    std::ostringstream output;
    output << prefix << '-';
    for (const auto byte : bytes) {
        output << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<int>(byte);
    }
    return output.str();
}

json field_json(const race_day::SetupFieldValue& field) {
    return {
        {"key", bounded(field.key, 120)},
        {"label", bounded(field.label, 160)},
        {"section", bounded(field.section, 120)},
        {"value", bounded(field.value, 500)},
        {"unit", bounded(field.unit, 40)},
        {"page", std::max(0, field.page)},
        {"left", std::clamp(field.left, 0.0, 1.0)},
        {"top", std::clamp(field.top, 0.0, 1.0)},
        {"right", std::clamp(field.right, 0.0, 1.0)},
        {"bottom", std::clamp(field.bottom, 0.0, 1.0)},
        {"confidence", std::clamp(field.confidence, 0, 100)},
        {"confirmed", field.confirmed},
    };
}

race_day::SetupFieldValue read_field(const json& value) {
    race_day::SetupFieldValue field;
    field.key = bounded(value.value("key", std::string{}), 120);
    field.label = bounded(value.value("label", std::string{}), 160);
    field.section = bounded(value.value("section", std::string{}), 120);
    field.value = bounded(value.value("value", std::string{}), 500);
    field.unit = bounded(value.value("unit", std::string{}), 40);
    field.page = std::max(0, value.value("page", 0));
    field.left = std::clamp(value.value("left", 0.0), 0.0, 1.0);
    field.top = std::clamp(value.value("top", 0.0), 0.0, 1.0);
    field.right = std::clamp(value.value("right", 0.0), 0.0, 1.0);
    field.bottom = std::clamp(value.value("bottom", 0.0), 0.0, 1.0);
    field.confidence = std::clamp(value.value("confidence", 0), 0, 100);
    field.confirmed = value.value("confirmed", false);
    return field;
}

std::string fields_json(std::span<const race_day::SetupFieldValue> fields) {
    json values = json::array();
    for (std::size_t index = 0; index < fields.size() && index < 500; ++index) {
        values.push_back(field_json(fields[index]));
    }
    return values.dump();
}

std::vector<race_day::SetupFieldValue> read_fields(const char* text) {
    std::vector<race_day::SetupFieldValue> result;
    if (!text || !*text) return result;
    const auto values = json::parse(text, nullptr, false);
    if (!values.is_array()) return result;
    result.reserve(std::min<std::size_t>(values.size(), 500));
    for (std::size_t index = 0; index < values.size() && index < 500; ++index) {
        if (values[index].is_object()) result.push_back(read_field(values[index]));
    }
    return result;
}

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database) {
        status_ = sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr);
    }
    ~Statement() {
        if (statement_) sqlite3_finalize(statement_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    [[nodiscard]] bool ok() const noexcept { return status_ == SQLITE_OK; }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }
    [[nodiscard]] int step() noexcept { return sqlite3_step(statement_); }
    bool text(int index, std::string_view value) {
        return sqlite3_bind_text(statement_, index, value.data(),
                                 static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
    }
    bool integer(int index, int value) {
        return sqlite3_bind_int(statement_, index, value) == SQLITE_OK;
    }

private:
    sqlite3* database_{};
    sqlite3_stmt* statement_{};
    int status_{SQLITE_ERROR};
};

std::string column_text(sqlite3_stmt* statement, int index) {
    const auto* value = sqlite3_column_text(statement, index);
    if (!value) return {};
    return reinterpret_cast<const char*>(value);
}

bool execute(sqlite3* database, const char* sql, std::string* error = nullptr) {
    char* raw_error = nullptr;
    const int status = sqlite3_exec(database, sql, nullptr, nullptr, &raw_error);
    if (status == SQLITE_OK) return true;
    if (error) {
        *error = raw_error ? raw_error : sqlite_error(database, "SQLite command failed");
    }
    if (raw_error) sqlite3_free(raw_error);
    return false;
}

int user_version(sqlite3* database) {
    Statement statement(database, "PRAGMA user_version");
    if (!statement.ok() || statement.step() != SQLITE_ROW) return -1;
    return sqlite3_column_int(statement.get(), 0);
}

bool looks_like_pdf(const std::filesystem::path& path, std::string* error) {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(path, filesystem_error)) {
        if (error) *error = "The selected setup sheet is not a readable file.";
        return false;
    }
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size < 8 || size > kMaximumPdfBytes) {
        if (error) *error = "The setup PDF must be between 8 bytes and 64 MB.";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    std::array<char, 5> signature{};
    input.read(signature.data(), static_cast<std::streamsize>(signature.size()));
    if (!input || std::string_view(signature.data(), signature.size()) != "%PDF-") {
        if (error) *error = "The selected file does not have a PDF signature.";
        return false;
    }
    return true;
}

bool copy_to_store(const std::filesystem::path& source,
                   const std::filesystem::path& store_root,
                   const std::string& digest,
                   std::filesystem::path& destination,
                   std::string* error) {
    if (digest.size() != 64) {
        if (error) *error = "A valid SHA-256 digest was not available.";
        return false;
    }
    destination = store_root / L"pdf" /
                  path_from_utf8(digest.substr(0, 2)) /
                  path_from_utf8(digest + ".pdf");
    std::error_code filesystem_error;
    std::filesystem::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error) {
        if (error) *error = "Could not create the managed setup-PDF store: " + filesystem_error.message();
        return false;
    }
    if (std::filesystem::exists(destination, filesystem_error)) {
        std::string verify_error;
        if (sha256_file(destination, &verify_error) == digest) return true;
        if (error) *error = "The managed setup-PDF store contains a hash mismatch.";
        return false;
    }

    auto temporary = destination;
    temporary += path_from_utf8("." + random_id("writing"));
    std::filesystem::copy_file(source, temporary,
                               std::filesystem::copy_options::none,
                               filesystem_error);
    if (filesystem_error) {
        if (error) *error = "Could not copy the PDF into managed storage: " + filesystem_error.message();
        return false;
    }
    const auto copied_digest = sha256_file(temporary, error);
    if (copied_digest != digest) {
        std::filesystem::remove(temporary, filesystem_error);
        if (error && error->empty()) *error = "The managed PDF copy did not match its source hash.";
        return false;
    }
    std::filesystem::rename(temporary, destination, filesystem_error);
    if (filesystem_error) {
        if (std::filesystem::exists(destination)) {
            std::filesystem::remove(temporary, filesystem_error);
            return sha256_file(destination, error) == digest;
        }
        std::filesystem::remove(temporary, filesystem_error);
        if (error) *error = "Could not finalize the managed PDF copy: " + filesystem_error.message();
        return false;
    }
    return true;
}

TemplateRecord read_template_row(sqlite3_stmt* statement) {
    TemplateRecord record;
    record.id = column_text(statement, 0);
    record.display_name = column_text(statement, 1);
    record.source_sha256 = column_text(statement, 2);
    record.managed_source_path = path_from_utf8(column_text(statement, 3));
    record.fields = read_fields(reinterpret_cast<const char*>(sqlite3_column_text(statement, 4)));
    record.created_at_utc = column_text(statement, 5);
    return record;
}

RevisionRecord read_revision_row(sqlite3_stmt* statement) {
    RevisionRecord record;
    record.id = column_text(statement, 0);
    record.template_id = column_text(statement, 1);
    record.parent_revision_id = column_text(statement, 2);
    record.car_profile_id = column_text(statement, 3);
    record.run_id = column_text(statement, 4);
    record.display_name = column_text(statement, 5);
    record.source_sha256 = column_text(statement, 6);
    record.rendered_sha256 = column_text(statement, 7);
    record.managed_source_path = path_from_utf8(column_text(statement, 8));
    record.managed_rendered_path = path_from_utf8(column_text(statement, 9));
    record.partially_known = sqlite3_column_int(statement, 10) != 0;
    record.needs_reconciliation = sqlite3_column_int(statement, 11) != 0;
    record.untracked_changes = column_text(statement, 12);
    record.fields = read_fields(reinterpret_cast<const char*>(sqlite3_column_text(statement, 13)));
    record.created_at_utc = column_text(statement, 14);
    return record;
}

constexpr const char* kTemplateColumns =
    "id,display_name,source_sha256,managed_source_path,fields_json,created_at_utc";
constexpr const char* kRevisionColumns =
    "id,template_id,parent_revision_id,car_profile_id,run_id,display_name,"
    "source_sha256,rendered_sha256,managed_source_path,managed_rendered_path,"
    "partially_known,needs_reconciliation,untracked_changes,fields_json,created_at_utc";

}  // namespace

struct Library::Impl {
    sqlite3* database{};
    std::filesystem::path database_path;
    std::filesystem::path store_path;
    int schema_version{};
};

Library::Library() : impl_(std::make_unique<Impl>()) {}
Library::~Library() { close(); }
Library::Library(Library&&) noexcept = default;
Library& Library::operator=(Library&&) noexcept = default;

bool Library::open(OpenOptions options, std::string* error) {
    close();
    if (error) error->clear();
    if (options.database_path.empty()) {
        options.database_path = settings_directory() / L"setup-library.db";
    }
    if (options.managed_store_path.empty()) {
        options.managed_store_path = options.database_path.parent_path() / L"setup-library-store";
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(options.database_path.parent_path(), filesystem_error);
    std::filesystem::create_directories(options.managed_store_path, filesystem_error);
    if (filesystem_error) {
        if (error) *error = "Could not create the setup-library folder: " + filesystem_error.message();
        return false;
    }

    const bool existed = std::filesystem::is_regular_file(options.database_path, filesystem_error);
    sqlite3* database = nullptr;
    if (sqlite3_open16(options.database_path.c_str(), &database) != SQLITE_OK) {
        if (error) *error = sqlite_error(database, "Could not open the setup library");
        if (database) sqlite3_close(database);
        return false;
    }
    sqlite3_busy_timeout(database, 5'000);
    const int existing_version = user_version(database);
    if (existing_version < 0 || existing_version > kSchemaVersion) {
        if (error) {
            *error = existing_version > kSchemaVersion
                ? "This setup library was created by a newer application version."
                : sqlite_error(database, "Could not read the setup-library schema");
        }
        sqlite3_close(database);
        return false;
    }

    if (existed && existing_version < kSchemaVersion) {
        sqlite3_close(database);
        database = nullptr;
        auto backup = options.database_path;
        backup += L".pre-schema-1.bak";
        if (!std::filesystem::exists(backup, filesystem_error)) {
            std::filesystem::copy_file(options.database_path, backup,
                                       std::filesystem::copy_options::none,
                                       filesystem_error);
            if (filesystem_error) {
                if (error) *error = "Could not back up the setup library before migration: " + filesystem_error.message();
                return false;
            }
        }
        if (sqlite3_open16(options.database_path.c_str(), &database) != SQLITE_OK) {
            if (error) *error = sqlite_error(database, "Could not reopen the setup library");
            if (database) sqlite3_close(database);
            return false;
        }
        sqlite3_busy_timeout(database, 5'000);
    }

    if (!execute(database, "PRAGMA foreign_keys=ON", error) ||
        !execute(database, "PRAGMA journal_mode=WAL", error)) {
        sqlite3_close(database);
        return false;
    }
    if (existing_version < 1) {
        constexpr const char* migration = R"SQL(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS car_profiles(
  id TEXT PRIMARY KEY NOT NULL,
  name TEXT NOT NULL, brand TEXT NOT NULL, model TEXT NOT NULL,
  chassis TEXT NOT NULL, motor TEXT NOT NULL, esc TEXT NOT NULL,
  servo TEXT NOT NULL, receiver TEXT NOT NULL, radio TEXT NOT NULL,
  gearing TEXT NOT NULL, notes TEXT NOT NULL, updated_at_utc TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS pdf_templates(
  id TEXT PRIMARY KEY NOT NULL,
  display_name TEXT NOT NULL,
  source_sha256 TEXT NOT NULL UNIQUE,
  managed_source_path TEXT NOT NULL,
  fields_json TEXT NOT NULL,
  created_at_utc TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS setup_revisions(
  id TEXT PRIMARY KEY NOT NULL,
  template_id TEXT NOT NULL,
  parent_revision_id TEXT NOT NULL,
  car_profile_id TEXT NOT NULL,
  run_id TEXT NOT NULL,
  display_name TEXT NOT NULL,
  source_sha256 TEXT NOT NULL,
  rendered_sha256 TEXT NOT NULL,
  managed_source_path TEXT NOT NULL,
  managed_rendered_path TEXT NOT NULL,
  partially_known INTEGER NOT NULL CHECK(partially_known IN (0,1)),
  needs_reconciliation INTEGER NOT NULL CHECK(needs_reconciliation IN (0,1)),
  untracked_changes TEXT NOT NULL,
  fields_json TEXT NOT NULL,
  created_at_utc TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS setup_revisions_car_created
  ON setup_revisions(car_profile_id, created_at_utc);
CREATE INDEX IF NOT EXISTS setup_revisions_template
  ON setup_revisions(template_id, created_at_utc);
PRAGMA user_version=1;
COMMIT;
)SQL";
        if (!execute(database, migration, error)) {
            execute(database, "ROLLBACK", nullptr);
            sqlite3_close(database);
            return false;
        }
    }
    impl_->database = database;
    impl_->database_path = std::move(options.database_path);
    impl_->store_path = std::move(options.managed_store_path);
    impl_->schema_version = user_version(database);
    return true;
}

void Library::close() noexcept {
    if (impl_ && impl_->database) {
        sqlite3_close(impl_->database);
        impl_->database = nullptr;
    }
    if (impl_) impl_->schema_version = 0;
}

bool Library::is_open() const noexcept { return impl_ && impl_->database; }
int Library::schema_version() const noexcept { return impl_ ? impl_->schema_version : 0; }
const std::filesystem::path& Library::database_path() const noexcept { return impl_->database_path; }
const std::filesystem::path& Library::managed_store_path() const noexcept { return impl_->store_path; }

bool Library::upsert_car_profile(const race_day::CarProfileSnapshot& profile,
                                 std::string* error) {
    if (!is_open() || profile.id.empty()) {
        if (error) *error = "A setup library and car-profile ID are required.";
        return false;
    }
    constexpr const char* sql = R"SQL(
INSERT INTO car_profiles(id,name,brand,model,chassis,motor,esc,servo,receiver,radio,gearing,notes,updated_at_utc)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(id) DO UPDATE SET name=excluded.name,brand=excluded.brand,model=excluded.model,
chassis=excluded.chassis,motor=excluded.motor,esc=excluded.esc,servo=excluded.servo,
receiver=excluded.receiver,radio=excluded.radio,gearing=excluded.gearing,notes=excluded.notes,
updated_at_utc=excluded.updated_at_utc
)SQL";
    Statement statement(impl_->database, sql);
    const std::array values{
        bounded(profile.id, 120), bounded(profile.name, 160), bounded(profile.brand, 120),
        bounded(profile.model, 120), bounded(profile.chassis, 160), bounded(profile.motor, 160),
        bounded(profile.esc, 160), bounded(profile.servo, 160), bounded(profile.receiver, 160),
        bounded(profile.radio, 160), bounded(profile.gearing, 120), bounded(profile.notes, 2'000),
        utc_now(),
    };
    if (!statement.ok()) {
        if (error) *error = sqlite_error(impl_->database, "Could not prepare the car profile");
        return false;
    }
    for (std::size_t index = 0; index < values.size(); ++index) {
        statement.text(static_cast<int>(index + 1), values[index]);
    }
    if (statement.step() != SQLITE_DONE) {
        if (error) *error = sqlite_error(impl_->database, "Could not save the car profile");
        return false;
    }
    if (error) error->clear();
    return true;
}

std::vector<race_day::CarProfileSnapshot> Library::car_profiles(std::string* error) const {
    std::vector<race_day::CarProfileSnapshot> result;
    if (!is_open()) {
        if (error) *error = "The setup library is not open.";
        return result;
    }
    Statement statement(impl_->database,
        "SELECT id,name,brand,model,chassis,motor,esc,servo,receiver,radio,gearing,notes "
        "FROM car_profiles ORDER BY name COLLATE NOCASE,id");
    if (!statement.ok()) {
        if (error) *error = sqlite_error(impl_->database, "Could not list car profiles");
        return result;
    }
    while (statement.step() == SQLITE_ROW) {
        race_day::CarProfileSnapshot profile;
        profile.id = column_text(statement.get(), 0);
        profile.name = column_text(statement.get(), 1);
        profile.brand = column_text(statement.get(), 2);
        profile.model = column_text(statement.get(), 3);
        profile.chassis = column_text(statement.get(), 4);
        profile.motor = column_text(statement.get(), 5);
        profile.esc = column_text(statement.get(), 6);
        profile.servo = column_text(statement.get(), 7);
        profile.receiver = column_text(statement.get(), 8);
        profile.radio = column_text(statement.get(), 9);
        profile.gearing = column_text(statement.get(), 10);
        profile.notes = column_text(statement.get(), 11);
        result.push_back(std::move(profile));
    }
    if (error) error->clear();
    return result;
}

std::optional<race_day::CarProfileSnapshot> Library::car_profile(
    const std::string& id, std::string* error) const {
    const auto profiles = car_profiles(error);
    const auto found = std::find_if(profiles.begin(), profiles.end(),
        [&](const auto& profile) { return profile.id == id; });
    if (found == profiles.end()) return std::nullopt;
    return *found;
}

ImportResult Library::import_pdf(const std::filesystem::path& source,
                                 std::string display_name,
                                 const std::string& car_profile_id) {
    ImportResult result;
    if (!is_open()) {
        result.error = "The setup library is not open.";
        return result;
    }
    if (!looks_like_pdf(source, &result.error)) return result;
    const auto digest = sha256_file(source, &result.error);
    if (digest.empty()) return result;
    std::filesystem::path managed;
    if (!copy_to_store(source, impl_->store_path, digest, managed, &result.error)) return result;

    {
        Statement existing(impl_->database,
            "SELECT id,display_name,source_sha256,managed_source_path,fields_json,created_at_utc "
            "FROM pdf_templates WHERE source_sha256=?");
        existing.text(1, digest);
        if (existing.ok() && existing.step() == SQLITE_ROW) {
            result.duplicate = true;
            result.setup_template = read_template_row(existing.get());
            Statement base(impl_->database,
                "SELECT id,template_id,parent_revision_id,car_profile_id,run_id,display_name,"
                "source_sha256,rendered_sha256,managed_source_path,managed_rendered_path,"
                "partially_known,needs_reconciliation,untracked_changes,fields_json,created_at_utc "
                "FROM setup_revisions WHERE template_id=? ORDER BY rowid LIMIT 1");
            base.text(1, result.setup_template.id);
            if (base.ok() && base.step() == SQLITE_ROW) result.base_revision = read_revision_row(base.get());
            result.ok = true;
            return result;
        }
    }

    if (display_name.empty()) display_name = path_utf8(source.stem());
    display_name = bounded(std::move(display_name), 200);
    result.setup_template.id = "template-" + digest.substr(0, 24);
    result.setup_template.display_name = display_name;
    result.setup_template.source_sha256 = digest;
    result.setup_template.managed_source_path = managed;
    result.setup_template.created_at_utc = utc_now();
    result.base_revision.id = "revision-base-" + digest.substr(0, 24);
    result.base_revision.template_id = result.setup_template.id;
    result.base_revision.car_profile_id = bounded(car_profile_id, 120);
    result.base_revision.display_name = display_name;
    result.base_revision.source_sha256 = digest;
    result.base_revision.managed_source_path = managed;
    result.base_revision.created_at_utc = result.setup_template.created_at_utc;

    if (!execute(impl_->database, "BEGIN IMMEDIATE", &result.error)) return result;
    Statement insert_template(impl_->database,
        "INSERT INTO pdf_templates(id,display_name,source_sha256,managed_source_path,fields_json,created_at_utc) "
        "VALUES(?,?,?,?,?,?)");
    insert_template.text(1, result.setup_template.id);
    insert_template.text(2, display_name);
    insert_template.text(3, digest);
    insert_template.text(4, path_utf8(managed));
    insert_template.text(5, "[]");
    insert_template.text(6, result.setup_template.created_at_utc);
    const bool template_ok = insert_template.ok() && insert_template.step() == SQLITE_DONE;

    Statement insert_revision(impl_->database,
        "INSERT INTO setup_revisions(id,template_id,parent_revision_id,car_profile_id,run_id,display_name,"
        "source_sha256,rendered_sha256,managed_source_path,managed_rendered_path,partially_known,"
        "needs_reconciliation,untracked_changes,fields_json,created_at_utc) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    const std::array revision_values{
        result.base_revision.id, result.base_revision.template_id, std::string{},
        result.base_revision.car_profile_id, std::string{}, display_name, digest,
        std::string{}, path_utf8(managed), std::string{}, std::string{}, std::string{},
        std::string{}, std::string{}, result.base_revision.created_at_utc,
    };
    for (int index = 1; index <= 10; ++index) insert_revision.text(index, revision_values[static_cast<std::size_t>(index - 1)]);
    insert_revision.integer(11, 0);
    insert_revision.integer(12, 0);
    insert_revision.text(13, "");
    insert_revision.text(14, "[]");
    insert_revision.text(15, result.base_revision.created_at_utc);
    const bool revision_ok = insert_revision.ok() && insert_revision.step() == SQLITE_DONE;
    if (!template_ok || !revision_ok || !execute(impl_->database, "COMMIT", &result.error)) {
        if (result.error.empty()) result.error = sqlite_error(impl_->database, "Could not import the setup PDF");
        execute(impl_->database, "ROLLBACK", nullptr);
        return result;
    }
    result.ok = true;
    return result;
}

std::optional<TemplateRecord> Library::setup_template(
    const std::string& id, std::string* error) const {
    if (!is_open()) {
        if (error) *error = "The setup library is not open.";
        return std::nullopt;
    }
    const std::string sql = std::string("SELECT ") + kTemplateColumns +
                            " FROM pdf_templates WHERE id=?";
    Statement statement(impl_->database, sql.c_str());
    statement.text(1, id);
    if (!statement.ok() || statement.step() != SQLITE_ROW) return std::nullopt;
    if (error) error->clear();
    return read_template_row(statement.get());
}

std::vector<TemplateRecord> Library::templates(std::string* error) const {
    std::vector<TemplateRecord> result;
    if (!is_open()) {
        if (error) *error = "The setup library is not open.";
        return result;
    }
    const std::string sql = std::string("SELECT ") + kTemplateColumns +
                            " FROM pdf_templates ORDER BY created_at_utc DESC,rowid DESC";
    Statement statement(impl_->database, sql.c_str());
    if (!statement.ok()) {
        if (error) *error = sqlite_error(impl_->database, "Could not list setup templates");
        return result;
    }
    while (statement.step() == SQLITE_ROW) result.push_back(read_template_row(statement.get()));
    if (error) error->clear();
    return result;
}

bool Library::update_template_field_map(
    const std::string& template_id,
    const std::vector<race_day::SetupFieldValue>& fields,
    std::string* error) {
    if (!is_open() || template_id.empty()) {
        if (error) *error = "An open setup library and template ID are required.";
        return false;
    }
    Statement statement(impl_->database,
        "UPDATE pdf_templates SET fields_json=? WHERE id=?");
    statement.text(1, fields_json(fields));
    statement.text(2, template_id);
    if (!statement.ok() || statement.step() != SQLITE_DONE ||
        sqlite3_changes(impl_->database) != 1) {
        if (error) *error = sqlite_error(
            impl_->database, "Could not save the setup-sheet field map");
        return false;
    }
    if (error) error->clear();
    return true;
}

bool Library::create_revision(RevisionRecord revision, RevisionRecord* stored,
                              std::string* error) {
    if (!is_open()) {
        if (error) *error = "The setup library is not open.";
        return false;
    }
    if (revision.id.empty()) revision.id = random_id("revision");
    if (revision.created_at_utc.empty()) revision.created_at_utc = utc_now();
    if (!revision.parent_revision_id.empty() && !this->revision(revision.parent_revision_id, error)) {
        if (error && error->empty()) *error = "The parent setup revision does not exist.";
        return false;
    }
    if (!revision.template_id.empty() && !setup_template(revision.template_id, error)) {
        if (error && error->empty()) *error = "The setup-sheet template does not exist.";
        return false;
    }
    Statement statement(impl_->database,
        "INSERT INTO setup_revisions(id,template_id,parent_revision_id,car_profile_id,run_id,display_name,"
        "source_sha256,rendered_sha256,managed_source_path,managed_rendered_path,partially_known,"
        "needs_reconciliation,untracked_changes,fields_json,created_at_utc) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!statement.ok()) {
        if (error) *error = sqlite_error(impl_->database, "Could not prepare the setup revision");
        return false;
    }
    statement.text(1, bounded(revision.id, 120));
    statement.text(2, bounded(revision.template_id, 120));
    statement.text(3, bounded(revision.parent_revision_id, 120));
    statement.text(4, bounded(revision.car_profile_id, 120));
    statement.text(5, bounded(revision.run_id, 120));
    statement.text(6, bounded(revision.display_name, 200));
    statement.text(7, bounded(revision.source_sha256, 80));
    statement.text(8, bounded(revision.rendered_sha256, 80));
    statement.text(9, path_utf8(revision.managed_source_path));
    statement.text(10, path_utf8(revision.managed_rendered_path));
    statement.integer(11, revision.partially_known ? 1 : 0);
    statement.integer(12, revision.needs_reconciliation ? 1 : 0);
    statement.text(13, bounded(revision.untracked_changes, 4'000));
    statement.text(14, fields_json(revision.fields));
    statement.text(15, revision.created_at_utc);
    if (statement.step() != SQLITE_DONE) {
        if (error) *error = sqlite_error(impl_->database,
            "Could not create the immutable setup revision");
        return false;
    }
    if (stored) *stored = std::move(revision);
    if (error) error->clear();
    return true;
}

std::optional<RevisionRecord> Library::revision(
    const std::string& id, std::string* error) const {
    if (!is_open()) {
        if (error) *error = "The setup library is not open.";
        return std::nullopt;
    }
    const std::string sql = std::string("SELECT ") + kRevisionColumns +
                            " FROM setup_revisions WHERE id=?";
    Statement statement(impl_->database, sql.c_str());
    statement.text(1, id);
    if (!statement.ok()) {
        if (error) *error = sqlite_error(impl_->database, "Could not read the setup revision");
        return std::nullopt;
    }
    if (statement.step() != SQLITE_ROW) return std::nullopt;
    if (error) error->clear();
    return read_revision_row(statement.get());
}

std::vector<RevisionRecord> Library::revisions_for_car(
    const std::string& car_profile_id, std::string* error) const {
    std::vector<RevisionRecord> result;
    if (!is_open()) {
        if (error) *error = "The setup library is not open.";
        return result;
    }
    const std::string sql = std::string("SELECT ") + kRevisionColumns +
        " FROM setup_revisions WHERE car_profile_id=? ORDER BY created_at_utc DESC,rowid DESC";
    Statement statement(impl_->database, sql.c_str());
    statement.text(1, car_profile_id);
    if (!statement.ok()) {
        if (error) *error = sqlite_error(impl_->database, "Could not list setup revisions");
        return result;
    }
    while (statement.step() == SQLITE_ROW) result.push_back(read_revision_row(statement.get()));
    if (error) error->clear();
    return result;
}

bool Library::manage_rendered_pdf(const std::filesystem::path& source,
                                  std::filesystem::path& managed_path,
                                  std::string& sha256,
                                  std::string* error) {
    if (!is_open()) {
        if (error) *error = "The setup library is not open.";
        return false;
    }
    if (!looks_like_pdf(source, error)) return false;
    sha256 = sha256_file(source, error);
    if (sha256.empty()) return false;
    return copy_to_store(source, impl_->store_path, sha256, managed_path, error);
}

std::string sha256_file(const std::filesystem::path& path, std::string* error) {
    if (error) error->clear();
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_bytes = 0;
    DWORD hash_bytes = 0;
    DWORD returned = 0;
    auto fail = [&](std::string message) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        if (error) *error = std::move(message);
        return std::string{};
    };
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
        !BCRYPT_SUCCESS(BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes),
            &returned, 0)) ||
        !BCRYPT_SUCCESS(BCryptGetProperty(
            algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_bytes), sizeof(hash_bytes),
            &returned, 0))) {
        return fail("Windows could not initialize SHA-256.");
    }
    std::vector<unsigned char> object(object_bytes);
    std::vector<unsigned char> digest(hash_bytes);
    if (!BCRYPT_SUCCESS(BCryptCreateHash(
            algorithm, &hash, object.data(), object_bytes,
            nullptr, 0, 0))) {
        return fail("Windows could not create the SHA-256 operation.");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return fail("Could not read the selected PDF.");
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && !BCRYPT_SUCCESS(BCryptHashData(
                hash, reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(count), 0))) {
            return fail("Windows could not hash the selected PDF.");
        }
    }
    if (!input.eof() || !BCRYPT_SUCCESS(BCryptFinishHash(
            hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
        return fail("Windows could not finish hashing the selected PDF.");
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    hash = nullptr;
    algorithm = nullptr;
    std::ostringstream output;
    for (const auto byte : digest) {
        output << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<int>(byte);
    }
    return output.str();
}

race_day::SetupSheetSnapshot snapshot(const RevisionRecord& revision) {
    race_day::SetupSheetSnapshot result;
    result.template_id = revision.template_id;
    result.revision_id = revision.id;
    result.parent_revision_id = revision.parent_revision_id;
    result.display_name = revision.display_name;
    result.source_sha256 = revision.source_sha256;
    result.rendered_sha256 = revision.rendered_sha256;
    result.partially_known = revision.partially_known;
    result.needs_reconciliation = revision.needs_reconciliation;
    result.untracked_changes = revision.untracked_changes;
    result.fields = revision.fields;
    return result;
}

}  // namespace racebox::setup
