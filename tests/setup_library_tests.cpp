#include "racebox/setup_library.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::high_resolution_clock::now()
                                .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("racebox-setup-library-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void write_minimal_pdf(const std::filesystem::path& path) {
    // The setup library validates the PDF signature and hash. PDFium tests own
    // structural rendering/edit verification once that integration is enabled.
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "%PDF-1.4\n1 0 obj<</Type/Catalog>>endobj\n%%EOF\n";
}

}  // namespace

int main() {
    TemporaryDirectory temporary;
    const auto database = temporary.path() / "setup-library.db";
    const auto store = temporary.path() / "managed";
    const auto source = temporary.path() / "driver-base.pdf";
    write_minimal_pdf(source);

    racebox::setup::Library library;
    std::string error;
    require(library.open({database, store}, &error), error.c_str());
    require(library.schema_version() == racebox::setup::kSchemaVersion,
            "Setup-library schema version is wrong");
    require(!std::filesystem::exists(database.wstring() + L".pre-schema-1.bak"),
            "A brand-new library created an unnecessary migration backup");

    racebox::race_day::CarProfileSnapshot car{
        .id = "car-a800r",
        .name = "Awesomatix A800R",
        .brand = "Awesomatix",
        .model = "A800R",
        .chassis = "Carbon",
        .motor = "13.5T",
        .esc = "XR10 Pro",
        .servo = "PGS-XR",
        .receiver = "RX-493i",
        .radio = "M17",
        .gearing = "4.20",
        .notes = "Asphalt",
    };
    require(library.upsert_car_profile(car, &error), error.c_str());
    car.gearing = "4.10";
    require(library.upsert_car_profile(car, &error), error.c_str());
    const auto stored_car = library.car_profile(car.id, &error);
    require(stored_car && stored_car->gearing == "4.10",
            "Garage car profile did not round trip or update");

    const auto imported = library.import_pdf(source, "A800R driver base", car.id);
    require(imported.ok && !imported.duplicate &&
                imported.setup_template.source_sha256.size() == 64 &&
                std::filesystem::is_regular_file(
                    imported.setup_template.managed_source_path),
            imported.error.c_str());
    require(imported.base_revision.parent_revision_id.empty() &&
                imported.base_revision.car_profile_id == car.id,
            "Imported sheet did not become the immutable physical base");

    const auto duplicate = library.import_pdf(source, "Renamed duplicate", car.id);
    require(duplicate.ok && duplicate.duplicate &&
                duplicate.setup_template.id == imported.setup_template.id &&
                duplicate.base_revision.id == imported.base_revision.id &&
                library.templates(&error).size() == 1,
            "Duplicate PDFs were not de-duplicated by content hash");

    racebox::setup::RevisionRecord changed = imported.base_revision;
    changed.id.clear();
    changed.parent_revision_id = imported.base_revision.id;
    changed.run_id = "qualifying-1";
    changed.display_name = "Q1 rear spring";
    changed.fields = {{
        .key = "rear-spring", .label = "Rear spring",
        .section = "Suspension", .value = "2.8", .unit = "lb/in",
        .page = 0, .left = 0.6, .top = 0.3, .right = 0.7,
        .bottom = 0.35, .confidence = 100, .confirmed = true,
    }};
    racebox::setup::RevisionRecord stored_revision;
    require(library.create_revision(changed, &stored_revision, &error), error.c_str());
    require(!stored_revision.id.empty() &&
                stored_revision.parent_revision_id == imported.base_revision.id,
            "A setup change did not create a child revision");
    require(!library.create_revision(stored_revision, nullptr, &error) &&
                error.find("immutable") != std::string::npos,
            "An existing setup revision was silently overwritten");

    racebox::setup::RevisionRecord notes_only;
    notes_only.parent_revision_id = stored_revision.id;
    notes_only.car_profile_id = car.id;
    notes_only.run_id = "qualifying-2";
    notes_only.display_name = "Q2 setup sheet off";
    notes_only.partially_known = true;
    notes_only.needs_reconciliation = true;
    notes_only.untracked_changes = "Moved rear shocks one hole; sheet was off.";
    require(library.create_revision(notes_only, &notes_only, &error), error.c_str());
    const auto notes_snapshot = racebox::setup::snapshot(notes_only);
    require(notes_snapshot.partially_known &&
                notes_snapshot.needs_reconciliation &&
                notes_snapshot.untracked_changes.find("rear shocks") != std::string::npos,
            "Setup OFF did not produce a bounded partially-known snapshot");
    require(library.revisions_for_car(car.id, &error).size() == 3,
            "Immutable setup history is incomplete");

    const auto managed_hash = racebox::setup::sha256_file(
        imported.setup_template.managed_source_path, &error);
    require(managed_hash == imported.setup_template.source_sha256,
            "Managed setup PDF does not match its source hash");

    library.close();
    require(library.open({database, store}, &error), error.c_str());
    require(library.revision(notes_only.id, &error).has_value(),
            "Setup history did not survive database reopen");
    library.close();

    const auto legacy_database = temporary.path() / "legacy-library.db";
    {
        std::ofstream empty(legacy_database, std::ios::binary | std::ios::trunc);
    }
    racebox::setup::Library migrated;
    require(migrated.open({legacy_database, temporary.path() / "legacy-store"}, &error),
            error.c_str());
    require(std::filesystem::is_regular_file(
                legacy_database.wstring() + L".pre-schema-1.bak"),
            "Existing schema-0 library was not backed up before migration");

    std::cout << "Setup library, managed PDF, and immutable revision tests passed\n";
    return 0;
}
