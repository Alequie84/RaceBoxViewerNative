#pragma once

#include "racebox/domain.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace racebox {

bool save_session_archive(const Session& session, const std::filesystem::path& destination, std::string& error);
bool save_lap_archive(const Session& session, const LapInfo& lap, const std::filesystem::path& destination, std::string& error);
bool load_session_archive(const std::filesystem::path& source, Session& session, std::string& error);
bool export_lap_csv(const Session& session, const LapInfo& lap, const std::filesystem::path& destination, std::string& error);

// Must be selected before the first settings_directory() call. The demo
// profile is deliberately isolated so engineering/demo launches cannot change
// normal recent files, Race Days, layouts, logs, or Codex Review handoffs.
bool configure_settings_profile(bool demo_profile) noexcept;
bool demo_settings_profile() noexcept;
std::filesystem::path settings_directory();
std::filesystem::path log_directory();
void write_log(std::string_view message);

}  // namespace racebox
