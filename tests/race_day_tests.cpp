#include "racebox/race_day.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

racebox::Session synthetic_session() {
    racebox::Session session;
    auto& telemetry = session.telemetry;
    for (int index = 0; index < 8; ++index) {
        telemetry.time_us.push_back(index * 40'000);
        telemetry.absolute_time_us.push_back(1'700'000'000'000'000LL + index * 40'000);
        telemetry.latitude.push_back(49.184 + index * 0.000001);
        telemetry.longitude.push_back(-123.145 + index * 0.000001);
        telemetry.speed_kmh.push_back(10.0F + index);
        telemetry.heading_deg.push_back(0.0F);
        telemetry.altitude_m.push_back(2.0F);
        telemetry.longitudinal_g.push_back(0.1F);
        telemetry.lateral_g.push_back(0.2F);
        telemetry.vertical_g.push_back(1.0F);
        telemetry.gyro_x_dps.push_back(0.0F);
        telemetry.gyro_y_dps.push_back(0.0F);
        telemetry.gyro_z_dps.push_back(0.0F);
        telemetry.satellites.push_back(14);
        telemetry.raw_lap.push_back(1);
    }
    telemetry.validate();
    session.laps.push_back({1, 1, 0, 7, 280'000, racebox::LapPhase::Complete});
    session.alignment.compatible = false;
    return session;
}

racebox::Session synthetic_dynamics_session(
    const std::vector<float>& speeds,
    const std::vector<float>& triggers,
    const std::vector<float>& steering,
    const std::vector<float>& lateral_g,
    const std::vector<float>& longitudinal_g) {
    racebox::Session session;
    auto& telemetry = session.telemetry;
    const auto count = speeds.size();
    for (std::size_t index = 0; index < count; ++index) {
        telemetry.time_us.push_back(static_cast<racebox::Timestamp>(index) * 40'000);
        telemetry.absolute_time_us.push_back(1'700'000'000'000'000LL + static_cast<racebox::Timestamp>(index) * 40'000);
        telemetry.latitude.push_back(49.184 + static_cast<double>(index) * 0.000001);
        telemetry.longitude.push_back(-123.145 + static_cast<double>(index) * 0.000001);
        telemetry.speed_kmh.push_back(speeds[index]);
        telemetry.heading_deg.push_back(0.0F);
        telemetry.altitude_m.push_back(2.0F);
        telemetry.longitudinal_g.push_back(longitudinal_g[index]);
        telemetry.lateral_g.push_back(lateral_g[index]);
        telemetry.vertical_g.push_back(1.0F);
        telemetry.gyro_x_dps.push_back(0.0F);
        telemetry.gyro_y_dps.push_back(0.0F);
        telemetry.gyro_z_dps.push_back(0.0F);
        telemetry.satellites.push_back(14);
        telemetry.raw_lap.push_back(1);
        session.radio.elapsed_us.push_back(static_cast<racebox::Timestamp>(index) * 40'000);
        session.radio.trigger_percent.push_back(triggers[index]);
        session.radio.steering_percent.push_back(steering[index]);
        session.radio.voltage.push_back(6.0F);
    }
    telemetry.validate();
    session.radio.validate();
    session.laps.push_back({1, 1, 0, count - 1,
        static_cast<racebox::Timestamp>((count - 1) * 40'000), racebox::LapPhase::Complete});
    session.alignment.compatible = true;
    session.alignment.trigger_sign = 1;
    session.alignment.steering_sign = 1;
    return session;
}

racebox::Session synthetic_roll_session(float stopped_lateral_g, float corner_lateral_g, float corner_vertical_g) {
    racebox::Session session;
    auto& telemetry = session.telemetry;
    for (std::size_t index = 0; index < 80; ++index) {
        const auto stopped = index < 30;
        telemetry.time_us.push_back(static_cast<racebox::Timestamp>(index) * 40'000);
        telemetry.absolute_time_us.push_back(1'700'000'000'000'000LL + static_cast<racebox::Timestamp>(index) * 40'000);
        telemetry.latitude.push_back(49.184 + static_cast<double>(index) * 0.000001);
        telemetry.longitude.push_back(-123.145 + static_cast<double>(index) * 0.000001);
        telemetry.speed_kmh.push_back(stopped ? 0.0F : 34.0F);
        telemetry.heading_deg.push_back(0.0F);
        telemetry.altitude_m.push_back(2.0F);
        telemetry.longitudinal_g.push_back(0.0F);
        telemetry.lateral_g.push_back(stopped ? stopped_lateral_g : corner_lateral_g);
        telemetry.vertical_g.push_back(stopped ? 1.0F : corner_vertical_g);
        telemetry.gyro_x_dps.push_back(0.0F);
        telemetry.gyro_y_dps.push_back(0.0F);
        telemetry.gyro_z_dps.push_back(0.0F);
        telemetry.satellites.push_back(14);
        telemetry.raw_lap.push_back(1);
        session.radio.elapsed_us.push_back(static_cast<racebox::Timestamp>(index) * 40'000);
        session.radio.trigger_percent.push_back(0.0F);
        session.radio.steering_percent.push_back(stopped ? 0.0F : 45.0F);
        session.radio.voltage.push_back(6.0F);
    }
    telemetry.validate();
    session.radio.validate();
    session.laps.push_back({1, 1, 0, 79, 79 * 40'000, racebox::LapPhase::Complete});
    session.alignment.compatible = true;
    session.alignment.trigger_sign = 1;
    session.alignment.steering_sign = 1;
    return session;
}

racebox::Session synthetic_roll_rate_session(double amplitude_deg, double frequency_per_sample) {
    racebox::Session session;
    auto& telemetry = session.telemetry;
    constexpr double radians_per_degree = 3.14159265358979323846 / 180.0;
    for (std::size_t index = 0; index < 120; ++index) {
        const auto stopped = index < 30;
        const auto angle_deg = stopped
            ? 2.0
            : 28.0 + amplitude_deg * std::sin(static_cast<double>(index - 30) * frequency_per_sample);
        const auto vertical_g = 1.0F;
        const auto lateral_g = static_cast<float>(std::tan(angle_deg * radians_per_degree) * vertical_g);
        telemetry.time_us.push_back(static_cast<racebox::Timestamp>(index) * 40'000);
        telemetry.absolute_time_us.push_back(1'700'000'000'000'000LL + static_cast<racebox::Timestamp>(index) * 40'000);
        telemetry.latitude.push_back(49.184 + static_cast<double>(index) * 0.000001);
        telemetry.longitude.push_back(-123.145 + static_cast<double>(index) * 0.000001);
        telemetry.speed_kmh.push_back(stopped ? 0.0F : 34.0F);
        telemetry.heading_deg.push_back(0.0F);
        telemetry.altitude_m.push_back(2.0F);
        telemetry.longitudinal_g.push_back(0.0F);
        telemetry.lateral_g.push_back(lateral_g);
        telemetry.vertical_g.push_back(vertical_g);
        telemetry.gyro_x_dps.push_back(0.0F);
        telemetry.gyro_y_dps.push_back(0.0F);
        telemetry.gyro_z_dps.push_back(0.0F);
        telemetry.satellites.push_back(14);
        telemetry.raw_lap.push_back(1);
        session.radio.elapsed_us.push_back(static_cast<racebox::Timestamp>(index) * 40'000);
        session.radio.trigger_percent.push_back(0.0F);
        session.radio.steering_percent.push_back(stopped ? 0.0F : 45.0F);
        session.radio.voltage.push_back(6.0F);
    }
    telemetry.validate();
    session.radio.validate();
    session.laps.push_back({1, 1, 0, 119, 119 * 40'000, racebox::LapPhase::Complete});
    session.alignment.compatible = true;
    session.alignment.trigger_sign = 1;
    session.alignment.steering_sign = 1;
    return session;
}

}  // namespace

int main() {
    try {
        auto day = racebox::race_day::standard_day(true, true);
        day.event_name = "Club race";
        day.track_name = "RC Raceway";
        require(day.runs.size() == 10, "P1-3, Q1-4, and triple A Main were not created");
        require(day.runs[0].label == "Practice 1", "Practice naming changed");
        require(day.runs[3].label == "Q1", "Qualifying naming changed");
        require(day.runs[7].label == "A Main 1", "Triple-main naming changed");
        require(day.runs.front().checklist.size() >= 8, "Pre-run checklist was not populated");

        const auto added_b = racebox::race_day::add_main_group(day, 'B', 1);
        require(added_b.size() == 1 && day.runs[added_b.front()].label == "B Main",
                "Single B Main was not created");
        require(racebox::race_day::add_main_group(day, 'B', 1).empty(),
                "Duplicate main group was not rejected");
        const auto first_generic_race_id =
            racebox::race_day::add_race(day).id;
        const auto second_generic_race_id =
            racebox::race_day::add_race(day).id;
        require(first_generic_race_id != second_generic_race_id,
                "Repeated generic races received duplicate IDs");
        const auto first_generic_race = std::find_if(
            day.runs.begin(), day.runs.end(),
            [&](const auto& run) {
                return run.id == first_generic_race_id;
            });
        const auto second_generic_race = std::find_if(
            day.runs.begin(), day.runs.end(),
            [&](const auto& run) {
                return run.id == second_generic_race_id;
            });
        require(first_generic_race != day.runs.end() &&
                    second_generic_race != day.runs.end() &&
                    first_generic_race->kind ==
                        racebox::race_day::RunKind::Main &&
                    second_generic_race->kind ==
                        racebox::race_day::RunKind::Main &&
                    first_generic_race->main_leg == 0 &&
                    second_generic_race->main_leg == 0,
                "Generic race entries did not retain valid Main metadata");

        const auto temporary_root = std::filesystem::temp_directory_path();
        const auto managed_vbo_a =
            temporary_root / L"racebox-managed-source-a.vbo";
        const auto managed_vbo_b =
            temporary_root / L"racebox-managed-source-b.vbo";
        const auto managed_racebox_csv =
            temporary_root / L"racebox-managed-source.csv";
        const auto managed_sanwa_csv =
            temporary_root / L"racebox-managed-sanwa.csv";
        const auto managed_gpx =
            temporary_root / L"racebox-managed-source.gpx";
        const auto managed_archive =
            temporary_root / L"racebox-managed-source.rbxsession";
        const auto managed_unsupported =
            temporary_root / L"racebox-managed-source.txt";
        const auto managed_empty_csv =
            temporary_root / L"racebox-managed-empty.csv";
        const auto managed_missing =
            temporary_root / L"racebox-managed-missing.vbo";
        const auto write_fixture = [](const std::filesystem::path& path,
                                      std::string_view contents) {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << contents;
            if (!output) throw std::runtime_error(
                "Could not write managed-source fixture");
        };
        write_fixture(managed_vbo_a, "vbo recording A");
        write_fixture(managed_vbo_b, "different vbo recording B");
        write_fixture(
            managed_racebox_csv,
            "Timestamp,Lap,Speed,GForceX,GForceY\n");
        write_fixture(
            managed_sanwa_csv,
            "REC TIME,ST(%),TH(%),RPM\n");
        write_fixture(managed_gpx, "<gpx></gpx>");
        write_fixture(managed_archive, "native archive fixture");
        write_fixture(managed_unsupported, "unsupported fixture");
        write_fixture(managed_empty_csv, {});
        std::error_code fixture_cleanup_error;
        std::filesystem::remove(
            managed_missing, fixture_cleanup_error);

        using racebox::race_day::TelemetrySourceKind;
        require(racebox::race_day::telemetry_source_kind(managed_vbo_a) ==
                    TelemetrySourceKind::Vbo,
                "VBO source classification failed");
        require(racebox::race_day::telemetry_source_kind(
                    managed_racebox_csv) ==
                    TelemetrySourceKind::RaceBoxCsv,
                "RaceBox CSV source classification failed");
        require(racebox::race_day::telemetry_source_kind(
                    managed_sanwa_csv) ==
                    TelemetrySourceKind::SanwaCsv,
                "Sanwa CSV source classification failed");
        require(racebox::race_day::telemetry_source_kind(managed_gpx) ==
                    TelemetrySourceKind::Gpx,
                "GPX source classification failed");
        require(racebox::race_day::telemetry_source_kind(managed_archive) ==
                    TelemetrySourceKind::NativeArchive,
                "Native archive source classification failed");
        require(racebox::race_day::telemetry_source_kind(
                    managed_unsupported) ==
                    TelemetrySourceKind::Unsupported,
                "Unsupported source classification failed");

        racebox::race_day::Run sanwa_only;
        std::vector<std::filesystem::path> selected_sources{
            managed_sanwa_csv};
        auto attachment =
            racebox::race_day::attach_or_replace_telemetry_sources(
                sanwa_only, selected_sources);
        require(attachment.ok && attachment.added == 1 &&
                    attachment.replaced == 0,
                "Sanwa attachment failed");
        require(!racebox::race_day::has_primary_telemetry(sanwa_only),
                "Sanwa-only run was incorrectly treated as primary telemetry");
        const auto optional_sanwa_composition =
            racebox::race_day::validate_telemetry_source_composition(
                sanwa_only);
        const auto required_sanwa_composition =
            racebox::race_day::validate_telemetry_source_composition(
                sanwa_only, true);
        require(optional_sanwa_composition.ok &&
                    !optional_sanwa_composition.has_primary &&
                    !required_sanwa_composition.ok,
                "Source composition did not distinguish optional controls from required primary telemetry");

        racebox::race_day::Run managed_run;
        selected_sources = {
            managed_vbo_a, managed_racebox_csv, managed_sanwa_csv};
        attachment =
            racebox::race_day::attach_or_replace_telemetry_sources(
                managed_run, selected_sources);
        require(attachment.ok && attachment.added == 3 &&
                    attachment.replaced == 0,
                "Initial multi-source attachment failed");
        require(managed_run.telemetry_files.size() == 3 &&
                    managed_run.telemetry_source_identities.size() == 3,
                "Attached path/identity vectors are not parallel");
        require(racebox::race_day::has_primary_telemetry(managed_run) &&
                    racebox::race_day::has_racebox_csv(managed_run) &&
                    racebox::race_day::has_sanwa_csv(managed_run),
                "Attached telemetry roles were not detected");
        const auto complete_composition =
            racebox::race_day::validate_telemetry_source_composition(
                managed_run, true);
        require(complete_composition.ok &&
                    complete_composition.has_primary,
                "Valid VBO/RaceBox/Sanwa composition was rejected");
        const auto initial_vbo = std::find_if(
            managed_run.telemetry_files.begin(),
            managed_run.telemetry_files.end(),
            [](const auto& path) {
                return racebox::race_day::telemetry_source_kind(path) ==
                    TelemetrySourceKind::Vbo;
            });
        const auto initial_sanwa = std::find_if(
            managed_run.telemetry_files.begin(),
            managed_run.telemetry_files.end(),
            [](const auto& path) {
                return racebox::race_day::telemetry_source_kind(path) ==
                    TelemetrySourceKind::SanwaCsv;
            });
        require(initial_vbo != managed_run.telemetry_files.end() &&
                    initial_sanwa != managed_run.telemetry_files.end(),
                "Initial managed sources could not be located");
        const auto initial_vbo_index = static_cast<std::size_t>(
            std::distance(managed_run.telemetry_files.begin(), initial_vbo));
        const auto initial_sanwa_index = static_cast<std::size_t>(
            std::distance(managed_run.telemetry_files.begin(), initial_sanwa));
        const auto old_vbo_fingerprint =
            managed_run.telemetry_source_identities[initial_vbo_index]
                .content_fingerprint;
        const auto old_sanwa_fingerprint =
            managed_run.telemetry_source_identities[initial_sanwa_index]
                .content_fingerprint;

        selected_sources = {managed_vbo_b};
        attachment =
            racebox::race_day::attach_or_replace_telemetry_sources(
                managed_run, selected_sources);
        require(attachment.ok && attachment.added == 0 &&
                    attachment.replaced == 1,
                "Same-kind VBO replacement was not reported");
        const auto replacement_vbo = std::find_if(
            managed_run.telemetry_files.begin(),
            managed_run.telemetry_files.end(),
            [](const auto& path) {
                return racebox::race_day::telemetry_source_kind(path) ==
                    TelemetrySourceKind::Vbo;
            });
        require(replacement_vbo != managed_run.telemetry_files.end() &&
                    *replacement_vbo == managed_vbo_b,
                "Replacement VBO path was not installed");
        const auto replacement_vbo_index = static_cast<std::size_t>(
            std::distance(
                managed_run.telemetry_files.begin(), replacement_vbo));
        require(
            managed_run.telemetry_source_identities[replacement_vbo_index]
                    .content_fingerprint != old_vbo_fingerprint,
            "Replacement VBO retained the old source fingerprint");
        const auto replacement_sanwa = std::find_if(
            managed_run.telemetry_files.begin(),
            managed_run.telemetry_files.end(),
            [](const auto& path) {
                return racebox::race_day::telemetry_source_kind(path) ==
                    TelemetrySourceKind::SanwaCsv;
            });
        require(replacement_sanwa != managed_run.telemetry_files.end(),
                "Replacing VBO removed the Sanwa source");
        const auto replacement_sanwa_index = static_cast<std::size_t>(
            std::distance(
                managed_run.telemetry_files.begin(), replacement_sanwa));
        require(
            managed_run.telemetry_source_identities[replacement_sanwa_index]
                    .content_fingerprint == old_sanwa_fingerprint,
            "Replacing VBO changed an unrelated Sanwa identity");

        const auto racebox_source = std::find_if(
            managed_run.telemetry_files.begin(),
            managed_run.telemetry_files.end(),
            [](const auto& path) {
                return racebox::race_day::telemetry_source_kind(path) ==
                    TelemetrySourceKind::RaceBoxCsv;
            });
        require(racebox_source != managed_run.telemetry_files.end(),
                "RaceBox source could not be located for removal");
        const auto racebox_source_index = static_cast<std::size_t>(
            std::distance(
                managed_run.telemetry_files.begin(), racebox_source));
        require(racebox::race_day::remove_telemetry_source(
                    managed_run, racebox_source_index),
                "Removing one telemetry source failed");
        require(!racebox::race_day::has_racebox_csv(managed_run) &&
                    managed_run.telemetry_files.size() ==
                        managed_run.telemetry_source_identities.size(),
                "Removing one source broke source identity pairing");
        const auto before_invalid_remove = managed_run.telemetry_files;
        require(!racebox::race_day::remove_telemetry_source(
                    managed_run, 999),
                "Out-of-range source removal unexpectedly succeeded");
        require(managed_run.telemetry_files == before_invalid_remove,
                "Out-of-range removal changed the run");

        const auto before_atomic_rejection = managed_run.telemetry_files;
        selected_sources = {managed_empty_csv};
        attachment =
            racebox::race_day::attach_or_replace_telemetry_sources(
                managed_run, selected_sources);
        require(
            !attachment.ok &&
                attachment.error.find("empty (0 bytes)") !=
                    std::string::npos &&
                managed_run.telemetry_files == before_atomic_rejection,
            "Empty telemetry source was attached or lacked a clear error");
        selected_sources = {managed_gpx, managed_missing};
        attachment =
            racebox::race_day::attach_or_replace_telemetry_sources(
                managed_run, selected_sources);
        require(!attachment.ok &&
                    managed_run.telemetry_files == before_atomic_rejection,
                "Missing source partially changed the run");
        selected_sources = {managed_gpx, managed_unsupported};
        attachment =
            racebox::race_day::attach_or_replace_telemetry_sources(
                managed_run, selected_sources);
        require(!attachment.ok &&
                    managed_run.telemetry_files == before_atomic_rejection,
                "Unsupported selection partially changed the run");
        selected_sources = {managed_vbo_a, managed_vbo_b};
        attachment =
            racebox::race_day::attach_or_replace_telemetry_sources(
                managed_run, selected_sources);
        require(!attachment.ok &&
                    managed_run.telemetry_files == before_atomic_rejection,
                "Duplicate logical source kinds were not rejected atomically");
        selected_sources = {managed_gpx, managed_vbo_a};
        attachment =
            racebox::race_day::attach_or_replace_telemetry_sources(
                managed_run, selected_sources);
        require(!attachment.ok &&
                    managed_run.telemetry_files == before_atomic_rejection,
                "GPX/VBO mixing was not rejected atomically");
        selected_sources = {managed_archive, managed_sanwa_csv};
        attachment =
            racebox::race_day::attach_or_replace_telemetry_sources(
                managed_run, selected_sources);
        require(!attachment.ok &&
                    managed_run.telemetry_files == before_atomic_rejection,
                "Archive/raw mixing was not rejected atomically");

        racebox::race_day::Run duplicate_composition;
        duplicate_composition.telemetry_files = {
            managed_vbo_a, managed_vbo_b};
        require(!racebox::race_day::
                    validate_telemetry_source_composition(
                        duplicate_composition, true).ok,
                "Duplicate source kinds passed composition validation");
        racebox::race_day::Run archive_mixed_composition;
        archive_mixed_composition.telemetry_files = {
            managed_archive, managed_sanwa_csv};
        require(!racebox::race_day::
                    validate_telemetry_source_composition(
                        archive_mixed_composition, true).ok,
                "Archive/raw sources passed composition validation");
        racebox::race_day::Run gpx_mixed_composition;
        gpx_mixed_composition.telemetry_files = {
            managed_gpx, managed_vbo_a};
        require(!racebox::race_day::
                    validate_telemetry_source_composition(
                        gpx_mixed_composition, true).ok,
                "GPX/VBO sources passed composition validation");
        auto invalid_loader_rejected = false;
        try {
            (void)racebox::race_day::load_run_telemetry(
                gpx_mixed_composition);
        } catch (const std::runtime_error&) {
            invalid_loader_rejected = true;
        }
        require(invalid_loader_rejected,
                "Race Day loader did not reject an invalid source composition");

        selected_sources = {managed_archive};
        attachment =
            racebox::race_day::attach_or_replace_telemetry_sources(
                managed_run, selected_sources);
        require(attachment.ok && attachment.replaced == 1 &&
                    managed_run.telemetry_files.size() == 1 &&
                    racebox::race_day::telemetry_source_kind(
                        managed_run.telemetry_files.front()) ==
                         TelemetrySourceKind::NativeArchive,
                "Incoming native archive did not replace raw sources");
        const auto archive_only_sources =
            managed_run.telemetry_files;
        selected_sources = {managed_sanwa_csv};
        attachment =
            racebox::race_day::attach_or_replace_telemetry_sources(
                managed_run, selected_sources);
        require(!attachment.ok &&
                    managed_run.telemetry_files ==
                        archive_only_sources &&
                    racebox::race_day::has_primary_telemetry(
                        managed_run),
                "Supplemental-only input erased the native archive");
        selected_sources = {managed_vbo_a, managed_sanwa_csv};
        attachment =
            racebox::race_day::attach_or_replace_telemetry_sources(
                managed_run, selected_sources);
        require(attachment.ok && attachment.replaced == 1 &&
                    attachment.added == 1 &&
                    managed_run.telemetry_files.size() == 2 &&
                    racebox::race_day::has_primary_telemetry(managed_run) &&
                    racebox::race_day::has_sanwa_csv(managed_run),
                "Incoming raw sources did not replace the native archive");
        selected_sources = {managed_gpx};
        attachment =
            racebox::race_day::attach_or_replace_telemetry_sources(
                managed_run, selected_sources);
        require(attachment.ok &&
                    std::none_of(
                        managed_run.telemetry_files.begin(),
                        managed_run.telemetry_files.end(),
                        [](const auto& path) {
                            const auto kind =
                                racebox::race_day::telemetry_source_kind(
                                    path);
                            return kind == TelemetrySourceKind::Vbo ||
                                kind ==
                                    TelemetrySourceKind::RaceBoxCsv;
                        }) &&
                    racebox::race_day::has_sanwa_csv(managed_run),
                "Incoming GPX did not replace incompatible primary sources");
        selected_sources = {managed_vbo_a};
        attachment =
            racebox::race_day::attach_or_replace_telemetry_sources(
                managed_run, selected_sources);
        require(attachment.ok &&
                    std::none_of(
                        managed_run.telemetry_files.begin(),
                        managed_run.telemetry_files.end(),
                        [](const auto& path) {
                            return racebox::race_day::telemetry_source_kind(
                                       path) ==
                                TelemetrySourceKind::Gpx;
                        }),
                "Incoming VBO did not replace the GPX source");
        managed_run.recorded_at_utc =
            "2026-07-11T21:54:00.040Z";
        racebox::race_day::clear_telemetry_sources(managed_run);
        require(managed_run.telemetry_files.empty() &&
                    managed_run.telemetry_source_identities.empty() &&
                    managed_run.recorded_at_utc.empty(),
                "Clearing telemetry sources did not clear recording metadata");
        sanwa_only.recorded_at_utc =
            "2026-07-11T21:54:00.040Z";
        require(racebox::race_day::remove_telemetry_source(
                    sanwa_only, 0) &&
                    sanwa_only.telemetry_files.empty() &&
                    sanwa_only.recorded_at_utc.empty(),
                "Removing the final source retained a stale recording timestamp");

        auto& current = day.runs[1];
        day.car_profile_id = "car-awesomatix-a800r";
        day.car_profile = {
            .id = day.car_profile_id,
            .name = "Awesomatix asphalt car",
            .brand = "Awesomatix",
            .model = "A800R",
            .chassis = "A800R carbon",
            .motor = "13.5T",
            .esc = "Hobbywing XR10 Pro",
            .servo = "Sanwa PGS-XR",
            .receiver = "Sanwa RX-493i",
            .radio = "Sanwa M17",
            .gearing = "4.20 FDR",
            .notes = "Club-race asphalt profile",
        };
        current.recorded_at_utc = "2026-07-11T21:54:00.040Z";
        current.conditions.tire_set_id = "Set 3";
        current.conditions.tire_runs_before = 4;
        current.conditions.sauce_compound = "Yellow";
        current.conditions.sauce_minutes_before = 20;
        current.conditions.tire_warmer_minutes = 10;
        current.conditions.tire_warmer_temperature_c = 60.0;
        current.setup_changes = "Rear spring 2.6 to 2.8";
        current.post_run_notes = "More rotation, but nervous on power.";
        current.setup_sheet_enabled = true;
        current.setup_sheet.template_id = "template-a800r";
        current.setup_sheet.revision_id = "revision-current";
        current.setup_sheet.parent_revision_id = "revision-previous";
        current.setup_sheet.display_name = "A800R club base";
        current.setup_sheet.source_sha256 = "source-pdf-sha256";
        current.setup_sheet.rendered_sha256 = "rendered-pdf-sha256";
        current.setup_sheet.fields.push_back({
            .key = "rear-spring",
            .label = "Rear spring",
            .section = "Suspension",
            .value = "2.8",
            .unit = "lb/in",
            .page = 0,
            .left = 0.62,
            .top = 0.34,
            .right = 0.71,
            .bottom = 0.38,
            .confidence = 100,
            .confirmed = true,
        });
        current.checklist.front().checked = true;
        current.session_chat = {
            {"user", "Did the rear caster change help rotation?", "message-user-1", 1'700'000'000, "Alex S25 Ultra"},
            {"assistant", "Yes, the matched-corner data shows more rotation per steering input.", "message-assistant-1", 1'700'000'001, "crew-chief"},
        };
        day.conversation.push_back({
            "user",
            "Compare the spring change.",
            "message-comparison-1",
            1'700'000'002,
            "viewer",
            {day.runs[0].id, current.id},
            day.runs[0].id + "->" + current.id,
        });
        const auto identity_source =
            std::filesystem::temp_directory_path() /
            L"racebox-race-day-\u03a9-source-identity.vbo";
        {
            std::ofstream telemetry(identity_source,
                std::ios::binary | std::ios::trunc);
            telemetry << "privacy-bounded source identity";
        }
        current.telemetry_files = {identity_source};
        racebox::race_day::refresh_telemetry_source_identities(current);
        require(current.telemetry_source_identities.size() == 1,
                "Telemetry source identity was not captured");
        require(current.telemetry_source_identities.front().content_fingerprint.has_value(),
                "Telemetry source fingerprint was not captured");
        require(racebox::race_day::telemetry_source_state(current, 0) ==
                    racebox::race_day::TelemetrySourceState::Available,
                "Unchanged telemetry attachment was not available");
        require(racebox::race_day::telemetry_source_state(
                    current, 0, true) ==
                    racebox::race_day::TelemetrySourceState::Available,
                "Unchanged telemetry fingerprint did not verify");

        racebox::race_day::SetupKnowledgeRecord spring_result;
        spring_result.id = "setup-result-spring";
        spring_result.created_at_utc = "2026-07-26T20:00:00Z";
        spring_result.event_name = day.event_name;
        spring_result.track_name = day.track_name;
        spring_result.previous_run_id = day.runs[0].id;
        spring_result.previous_run_label = day.runs[0].label;
        spring_result.current_run_id = current.id;
        spring_result.current_run_label = current.label;
        spring_result.handling_question = "Did the rear spring improve rotation without losing rear grip?";
        spring_result.setup_change = current.setup_changes;
        spring_result.driver_result = current.post_run_notes;
        spring_result.previous_conditions = day.runs[0].conditions;
        spring_result.current_conditions = current.conditions;
        spring_result.verdict = "mixed";
        spring_result.confidence = 78;
        spring_result.summary = "Rotation improved, but the car was nervous on power.";
        spring_result.observations.push_back({
            "Lateral response", "+0.03 G matched response", "More response was measured.",
            {"dynamics:lateral:matched-speed-steering"}});
        spring_result.next_test = "Repeat A/B/A with the same tire set.";
        spring_result.causality_note = "One comparison is association, not proof.";
        spring_result.model = "openai/gpt-5.6-sol";
        spring_result.thinking = "xhigh";
        spring_result.selected_lane = "correlation";
        spring_result.evidence.analytics_contract = "racebox-setup-analytics-v3";
        spring_result.evidence.formula_version = 3;
        spring_result.evidence.quality_confidence = 84;
        spring_result.evidence.track_status = "compatible";
        spring_result.evidence.lap_time_delta_s = 0.08;
        spring_result.evidence.lateral_status = "measured";
        spring_result.evidence.lateral_response_delta_g = 0.03;
        spring_result.evidence.forward_status = "measured";
        spring_result.evidence.forward_bite_delta_g = -0.02;
        spring_result.evidence.previous_brake_indicators = 0;
        spring_result.evidence.current_brake_indicators = 1;
        racebox::race_day::upsert_setup_knowledge(day, spring_result);

        racebox::race_day::SetupKnowledgeRecord tire_result = spring_result;
        tire_result.id = "setup-result-tire";
        tire_result.track_name = "Different track";
        tire_result.handling_question = "Did a different tire sauce improve warmup?";
        tire_result.setup_change = "Changed tire sauce";
        tire_result.summary = "Warmup improved.";
        racebox::race_day::upsert_setup_knowledge(day, tire_result);

        const auto relevant = racebox::race_day::relevant_setup_knowledge(
            day, "The rear feels nervous under power after the spring change", current);
        require(!relevant.empty(), "Setup-knowledge retrieval returned no matching records");
        require(day.setup_knowledge[relevant.front()].id == spring_result.id,
                "Setup-knowledge retrieval did not prioritize the matching track and change");
        const auto prior_payload = racebox::race_day::build_prior_setup_results(day, relevant);
        require(prior_payload.is_array() && !prior_payload.empty(),
                "Prior setup-result payload was not built");
        require(prior_payload.dump().find("telemetry_files") == std::string::npos,
                "Prior setup-result payload leaked telemetry attachment metadata");

        const auto path = std::filesystem::temp_directory_path() / L"racebox-race-day-test.rbxday";
        std::string error;
        require(racebox::race_day::save(day, path, error), error.c_str());
        auto race_day_temporary = path;
        race_day_temporary += L".writing";
        require(!std::filesystem::exists(race_day_temporary),
                "Successful Race Day save left plaintext temporary data");
        racebox::race_day::Day restored;
        require(racebox::race_day::load(path, restored, error), error.c_str());
        std::error_code ignored;
        require(restored.runs.size() == day.runs.size(), "Race-day run count did not round trip");
        const auto restored_first_race = std::find_if(
            restored.runs.begin(), restored.runs.end(),
            [&](const auto& run) {
                return run.id == first_generic_race_id;
            });
        const auto restored_second_race = std::find_if(
            restored.runs.begin(), restored.runs.end(),
            [&](const auto& run) {
                return run.id == second_generic_race_id;
            });
        require(restored_first_race != restored.runs.end() &&
                    restored_second_race != restored.runs.end() &&
                    restored_first_race->main_leg == 0 &&
                    restored_second_race->main_leg == 0,
                "Repeated generic races did not survive save/load");
        require(restored.runs[1].conditions.tire_runs_before == 4, "Tire run count did not persist");
        require(restored.runs[1].conditions.tire_warmer_temperature_c == 60.0,
                "Tire warmer temperature did not persist");
        require(restored.runs[1].checklist.front().checked, "Checklist state did not persist");
        require(restored.runs[1].setup_changes == current.setup_changes, "Setup changes did not persist");
        require(restored.car_profile_id == day.car_profile_id &&
                    restored.car_profile.model == "A800R" &&
                    restored.car_profile.radio == "Sanwa M17",
                "Race Day v6 car-profile snapshot did not persist");
        require(restored.runs[1].setup_sheet_enabled &&
                    restored.runs[1].setup_sheet.revision_id == "revision-current" &&
                    restored.runs[1].setup_sheet.fields.size() == 1 &&
                    restored.runs[1].setup_sheet.fields.front().value == "2.8" &&
                    restored.runs[1].setup_sheet.fields.front().confirmed,
                "Race Day v6 setup-sheet snapshot did not persist");
        require(restored.runs[1].session_chat.size() == 2 &&
                    restored.runs[1].session_chat.back().role == "assistant" &&
                    restored.runs[1].session_chat.back().id == "message-assistant-1" &&
                    restored.runs[1].session_chat.back().origin == "crew-chief" &&
                    restored.runs[1].session_chat.back().content.find("more rotation") != std::string::npos,
                "Per-run Crew Chief conversation did not persist");
        const auto restored_comparison = std::find_if(
            restored.conversation.begin(), restored.conversation.end(),
            [](const auto& turn) {
                return turn.id == "message-comparison-1";
            });
        require(restored_comparison != restored.conversation.end() &&
                    restored_comparison->linked_run_ids.size() == 2 &&
                    restored_comparison->comparison_id ==
                        day.runs[0].id + "->" + current.id,
                "Race Day v6 comparison conversation links did not persist");
        require(restored.runs[1].recorded_at_utc ==
                    current.recorded_at_utc,
                "Run recording timestamp did not persist");
        require(restored.runs[1].telemetry_source_identities.size() == 1,
                "Race Day v3 source identity did not persist");
        const auto expected_filename_u8 =
            identity_source.filename().generic_u8string();
        const std::string expected_filename(
            reinterpret_cast<const char*>(expected_filename_u8.data()),
            expected_filename_u8.size());
        require(restored.runs[1].telemetry_source_identities.front()
                    .canonical_filename == expected_filename,
                "Race Day source identity retained the wrong filename");
        require(restored.runs[1].telemetry_source_identities.front()
                    .size_bytes == 31,
                "Race Day source identity retained the wrong byte size");
        require(restored.setup_knowledge.size() == 2, "Setup-knowledge records did not persist");
        require(restored.setup_knowledge.front().evidence.quality_confidence == 84,
                "Setup-knowledge evidence did not round trip");
        require(restored.setup_knowledge.front().thinking == "xhigh",
                "Setup-knowledge agent metadata did not round trip");
        {
            std::ofstream replacement(identity_source,
                std::ios::binary | std::ios::trunc);
            replacement << "different telemetry recording at the same path";
        }
        require(racebox::race_day::telemetry_source_state(
                    restored.runs[1], 0) ==
                    racebox::race_day::TelemetrySourceState::Changed,
                "Replaced telemetry file was silently accepted");
        require(racebox::race_day::save(restored, path, error),
                "Race Day with a changed source could not preserve its identity");
        racebox::race_day::Day changed_round_trip;
        require(racebox::race_day::load(path, changed_round_trip, error),
                "Race Day changed-source identity could not be reloaded");
        require(changed_round_trip.runs[1]
                    .telemetry_source_identities.front().size_bytes == 31,
                "Saving silently blessed the replacement telemetry file");

        std::vector<std::filesystem::path> legacy_paths;
        for (int legacy_version = 1; legacy_version <= 5; ++legacy_version) {
            const auto legacy_path = std::filesystem::temp_directory_path() /
                (L"racebox-race-day-v" + std::to_wstring(legacy_version) +
                 L"-test.rbxday");
            legacy_paths.push_back(legacy_path);
            nlohmann::json legacy;
            std::ifstream saved(path, std::ios::binary);
            saved >> legacy;
            legacy["version"] = legacy_version;
            legacy.erase("car_profile_id");
            legacy.erase("car_profile");
            legacy.erase("conversation");
            if (legacy_version == 1) legacy.erase("setup_knowledge");
            for (auto& legacy_run : legacy["runs"]) {
                legacy_run.erase("setup_sheet_enabled");
                legacy_run.erase("setup_sheet");
                if (legacy_version == 1) legacy_run.erase("recorded_at_utc");
                for (auto& turn : legacy_run["session_chat"]) {
                    turn.erase("id");
                    turn.erase("created_at");
                    turn.erase("origin");
                }
            }
            std::ofstream output(
                legacy_path, std::ios::binary | std::ios::trunc);
            output << legacy.dump(2);
            output.close();

            racebox::race_day::Day legacy_restored;
            require(racebox::race_day::load(
                        legacy_path, legacy_restored, error),
                    "Version 1-5 race-day files lost backward compatibility");
            require(legacy_restored.runs[1].session_chat.size() == 2 &&
                        !legacy_restored.runs[1].session_chat.front().id.empty() &&
                        legacy_restored.runs[1].session_chat.front().created_at == 0 &&
                        legacy_restored.runs[1].session_chat.front().origin == "viewer",
                    "Legacy chat metadata was not migrated safely");
            require(legacy_restored.conversation.size() >= 2 &&
                        legacy_restored.conversation.front().linked_run_ids.size() == 1 &&
                        legacy_restored.conversation.front().linked_run_ids.front() ==
                            legacy_restored.runs[1].id,
                    "Legacy per-run chat was not migrated into the Race Day timeline");
            if (legacy_version == 1) {
                require(legacy_restored.setup_knowledge.empty(),
                        "Version-1 race-day file unexpectedly created setup knowledge");
                require(legacy_restored.runs[1].recorded_at_utc.empty(),
                        "Version-1 race-day file invented a recording timestamp");
            }
        }

        auto boundary_day = racebox::race_day::standard_day(false, false);
        auto& boundary_chat = boundary_day.runs.front().session_chat;
        boundary_chat.reserve(racebox::race_day::kMaximumSessionChatTurns + 1);
        for (std::size_t index = 0;
             index < racebox::race_day::kMaximumSessionChatTurns;
             ++index) {
            boundary_chat.push_back({
                index % 2 == 0 ? "user" : "assistant",
                "bounded conversation turn",
                "boundary-message-" + std::to_string(index),
                static_cast<std::int64_t>(index),
                index % 2 == 0 ? "viewer" : "crew-chief",
            });
        }
        const auto boundary_path = std::filesystem::temp_directory_path() /
            L"racebox-race-day-chat-boundary.rbxday";
        require(racebox::race_day::save(
                    boundary_day, boundary_path, error),
                "Exactly 2000 Crew Chief turns could not be saved");
        boundary_chat.push_back({
            "user", "one message too many", "boundary-message-overflow",
            2'001, "viewer"});
        require(!racebox::race_day::save(
                    boundary_day, boundary_path, error) &&
                    error.find("2000-message") != std::string::npos,
                "More than 2000 Crew Chief turns were not rejected");
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(boundary_path, ignored);
        for (const auto& legacy_path : legacy_paths) {
            std::filesystem::remove(legacy_path, ignored);
        }
        std::filesystem::remove(identity_source, ignored);
        std::filesystem::remove(managed_vbo_a, ignored);
        std::filesystem::remove(managed_vbo_b, ignored);
        std::filesystem::remove(managed_racebox_csv, ignored);
        std::filesystem::remove(managed_sanwa_csv, ignored);
        std::filesystem::remove(managed_gpx, ignored);
        std::filesystem::remove(managed_archive, ignored);
        std::filesystem::remove(managed_unsupported, ignored);
        std::filesystem::remove(managed_empty_csv, ignored);

        const auto session = synthetic_session();
        const auto imu = racebox::imu::analyze(session.telemetry);
        const auto csv = racebox::race_day::build_analytics_csv(session, imu);
        require(csv.find("racebox-session-analytics-csv-v1") != std::string::npos,
                "Analytics CSV contract is missing");
        require(csv.find("racebox-setup-analytics-v3") != std::string::npos,
                "Setup analytics contract is missing");
        require(csv.find("steering_percent,throttle_percent,brake_percent") != std::string::npos,
                "Analytics CSV input channels are missing");
        require(csv.find("lap_progress_percent,latitude,longitude") != std::string::npos,
                "Analytics CSV physical track matching fields are missing");
        require(csv.find(",complete,") != std::string::npos, "Complete-lap labels are missing");

        racebox::race_day::Run golden_run;
        golden_run.telemetry_files = {
            std::filesystem::path(GOLDEN_DIR) / L"session.vbo",
            std::filesystem::path(GOLDEN_DIR) / L"session.csv",
            std::filesystem::path(GOLDEN_DIR) / L"sanwa.csv",
        };
        const auto golden = racebox::race_day::load_run_telemetry(golden_run);
        require(golden.session.telemetry.size() == 13'863, "Race-day loader changed the golden GPS row count");
        require(golden.session.radio.size() == 43'199, "Race-day loader did not include Sanwa data");
        const auto golden_csv = racebox::race_day::build_analytics_csv(golden.session, golden.imu_analysis);
        require(golden_csv.size() < 6'000'000, "Golden analytics CSV exceeded the gateway per-run bound");
        require(golden_csv.find("100.000000") != std::string::npos,
                "Golden analytics CSV did not contain aligned control values");

        const auto neutral_g = std::vector<float>(20, 0.0F);
        auto corner_lat = std::vector<float>(20, 0.2F);
        auto corner_steer = std::vector<float>(20, 0.0F);
        for (int index = 2; index <= 6; ++index) {
            corner_lat[static_cast<std::size_t>(index)] = 0.9F;
            corner_steer[static_cast<std::size_t>(index)] = 50.0F;
        }
        std::vector<float> trigger(20, 0.0F);
        for (int index = 7; index <= 14; ++index) trigger[static_cast<std::size_t>(index)] = 100.0F;
        for (int index = 15; index <= 18; ++index) trigger[static_cast<std::size_t>(index)] = -100.0F;
        auto before_exit = synthetic_dynamics_session(
            {40,41,42,43,44,45,46,50,53,56,59,62,65,68,71,71,65,59,53,48},
            trigger, corner_steer, corner_lat, neutral_g);
        auto after_exit = synthetic_dynamics_session(
            {40,41,42,43,44,45,46,55,58,61,64,67,70,73,76,76,70,64,58,53},
            trigger, corner_steer, corner_lat, neutral_g);
        const auto exit_summary = racebox::race_day::analyze_setup_change(
            before_exit, racebox::imu::analyze(before_exit.telemetry),
            after_exit, racebox::imu::analyze(after_exit.telemetry));
        require(exit_summary.straight_top_speed_delta_kmh && *exit_summary.straight_top_speed_delta_kmh > 4.0,
                "Straight top-speed delta was not measured");
        require(exit_summary.straight_entry_speed_delta_kmh && *exit_summary.straight_entry_speed_delta_kmh > 4.0,
                "Straight entry-speed delta was not measured");
        require(exit_summary.straight_speed_attribution == "exit_or_line_driven",
                "Straight speed from better exit was misclassified");

        auto after_accel = synthetic_dynamics_session(
            {40,41,42,43,44,45,46,50,55,60,65,70,75,80,85,85,79,73,67,60},
            trigger, corner_steer, corner_lat, neutral_g);
        const auto accel_summary = racebox::race_day::analyze_setup_change(
            before_exit, racebox::imu::analyze(before_exit.telemetry),
            after_accel, racebox::imu::analyze(after_accel.telemetry));
        require(accel_summary.straight_acceleration_delta_g && *accel_summary.straight_acceleration_delta_g > 0.10,
                "Straight acceleration delta was not measured");
        require(accel_summary.straight_speed_attribution == "forward_bite_or_power_delivery",
                "Straight speed from acceleration was misclassified");

        auto delayed_brake = synthetic_dynamics_session(
            {40,41,42,43,44,45,46,50,53,56,59,62,65,68,71,71,71,71,63,57},
            trigger, corner_steer, corner_lat, neutral_g);
        const auto brake_summary = racebox::race_day::analyze_setup_change(
            before_exit, racebox::imu::analyze(before_exit.telemetry),
            delayed_brake, racebox::imu::analyze(delayed_brake.telemetry));
        require(brake_summary.current_brake_indicators > brake_summary.previous_brake_indicators,
                "Possible brake lockup/low-grip indicator did not increase");
        require(brake_summary.brake_response_delay_delta_s && *brake_summary.brake_response_delay_delta_s >= 0.07,
                "Brake response delay was not measured");

        std::vector<float> clean_speed(80, 42.0F);
        std::vector<float> clean_trigger(80, 0.0F);
        std::vector<float> clean_steering(80, 35.0F);
        std::vector<float> clean_lateral(80, 0.70F);
        std::vector<float> overdrive_speed(80, 42.0F);
        std::vector<float> overdrive_steering(80, 70.0F);
        std::vector<float> overdrive_lateral(80, 0.80F);
        for (std::size_t index = 40; index < overdrive_speed.size(); ++index) {
            overdrive_speed[index] = 42.0F - static_cast<float>(index - 40) * 0.08F;
            overdrive_lateral[index] = 0.25F;
            overdrive_steering[index] = index % 2 == 0 ? 70.0F : 88.0F;
        }
        const auto clean = synthetic_dynamics_session(
            clean_speed, clean_trigger, clean_steering, clean_lateral, std::vector<float>(80, 0.0F));
        const auto overdriven = synthetic_dynamics_session(
            overdrive_speed, clean_trigger, overdrive_steering, overdrive_lateral, std::vector<float>(80, 0.0F));
        const auto overdrive_summary = racebox::race_day::analyze_setup_change(
            clean, racebox::imu::analyze(clean.telemetry),
            overdriven, racebox::imu::analyze(overdriven.telemetry));
        require(overdrive_summary.overdriving_index_delta &&
                *overdrive_summary.overdriving_index_delta > 8.0,
                "Overdriving index did not detect extra tire scrub demand");
        require(overdrive_summary.overdriving_risk_sample_delta_percent &&
                *overdrive_summary.overdriving_risk_sample_delta_percent > 10.0,
                "Overdriving risk-sample percentage did not increase");
        require(overdrive_summary.overdriving_status == "more_overdriving_no_lap_gain",
                "Overdriving without lap gain was not classified");

        const auto flatter_roll = synthetic_roll_session(0.04F, 0.60F, 1.00F);
        const auto more_roll = synthetic_roll_session(0.04F, 0.60F, 0.80F);
        const auto roll_summary = racebox::race_day::analyze_setup_change(
            flatter_roll, racebox::imu::analyze(flatter_roll.telemetry),
            more_roll, racebox::imu::analyze(more_roll.telemetry));
        require(roll_summary.chassis_roll_delta_deg &&
                *roll_summary.chassis_roll_delta_deg > 4.0,
                "Tilt-corrected chassis-roll signature did not increase");
        require(roll_summary.surface_tilt_delta_deg &&
                std::abs(*roll_summary.surface_tilt_delta_deg) < 0.01,
                "Stopped-point surface tilt correction changed unexpectedly");
        require(roll_summary.chassis_roll_status == "more_chassis_roll_signature",
                "Chassis-roll signature increase was not classified");

        const auto slower_roll_build = synthetic_roll_rate_session(3.0, 0.10);
        const auto faster_roll_build = synthetic_roll_rate_session(10.0, 0.25);
        const auto roll_rate_summary = racebox::race_day::analyze_setup_change(
            slower_roll_build, racebox::imu::analyze(slower_roll_build.telemetry),
            faster_roll_build, racebox::imu::analyze(faster_roll_build.telemetry));
        require(roll_rate_summary.roll_rate_delta_dps &&
                *roll_rate_summary.roll_rate_delta_dps > 35.0,
                "Tilt-corrected roll-rate signature did not increase");
        require(roll_rate_summary.previous_roll_rate_samples >= 25 &&
                roll_rate_summary.current_roll_rate_samples >= 25,
                "Roll-rate signature did not retain enough cornering samples");
        require(roll_rate_summary.roll_rate_status == "faster_roll_build",
                "Roll-rate signature increase was not classified");

        std::cout << "Race-day model, persistence, checklist, and CSV tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Race-day test failure: " << exception.what() << '\n';
        return 1;
    }
}
