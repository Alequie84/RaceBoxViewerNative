#pragma once

#include <cstddef>

#include <imgui.h>

namespace racebox::app::ui {

enum class ThemeMode {
    Dark,
    Light,
};

// Semantic colors shared by the application shell, plots, map overlays, and
// analysis widgets. The lap-role colors intentionally match the established
// native telemetry colors.
enum class ColorToken : std::size_t {
    Canvas,
    Window,
    Panel,
    PanelRaised,
    Input,
    Border,
    BorderStrong,
    Text,
    TextMuted,
    Accent,
    AccentHovered,
    AccentActive,
    Reference,
    CompareA,
    CompareB,
    Playback,
    PlaybackCursor,
    Inspection,
    Positive,
    Warning,
    Danger,
    GridMajor,
    GridMinor,
    Count,
};

struct FontLoadResult {
    ImFont* font{};
    const char* family{"ImGui embedded"};
    bool system_font{};
};

// Load a readable installed Windows UI font. The embedded ImGui vector font is
// always installed as the final fallback, so startup never depends on an asset.
FontLoadResult load_windows_ui_font(ImGuiIO& io, float base_size_pixels = 15.0F);

// Rebuild both styles from unscaled base values. Passing zero retains the last
// known DPI scale, which makes theme switches safe and prevents cumulative
// ScaleAllSizes() calls.
void apply_theme(ThemeMode mode, float dpi_scale = 0.0F);

ThemeMode current_theme() noexcept;
float current_dpi_scale() noexcept;
float dpi_scale_for_window(void* native_window) noexcept;

const ImVec4& color(ColorToken token) noexcept;
ImU32 color_u32(ColorToken token) noexcept;

}  // namespace racebox::app::ui
