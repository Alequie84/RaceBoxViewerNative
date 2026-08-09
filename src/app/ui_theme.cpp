#include "ui_theme.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>

#include <implot.h>

namespace racebox::app::ui {
namespace {

using Palette = std::array<ImVec4, static_cast<std::size_t>(ColorToken::Count)>;

ThemeMode active_theme = ThemeMode::Dark;
float active_dpi_scale = 1.0F;
Palette active_palette{};

constexpr ImVec4 rgba(float red, float green, float blue, float alpha = 1.0F) noexcept {
    return {red, green, blue, alpha};
}

ImVec4 with_alpha(ImVec4 value, float alpha) noexcept {
    value.w = alpha;
    return value;
}

constexpr std::size_t index(ColorToken token) noexcept {
    return static_cast<std::size_t>(token);
}

Palette make_palette(ThemeMode mode) {
    Palette result{};
    const auto set = [&](ColorToken token, ImVec4 value) { result[index(token)] = value; };

    if (mode == ThemeMode::Dark) {
        set(ColorToken::Canvas, rgba(0.018F, 0.030F, 0.046F));
        set(ColorToken::Window, rgba(0.025F, 0.043F, 0.065F));
        set(ColorToken::Panel, rgba(0.032F, 0.054F, 0.079F));
        set(ColorToken::PanelRaised, rgba(0.047F, 0.074F, 0.105F));
        set(ColorToken::Input, rgba(0.022F, 0.039F, 0.060F));
        set(ColorToken::Border, rgba(0.105F, 0.158F, 0.214F));
        set(ColorToken::BorderStrong, rgba(0.175F, 0.241F, 0.309F));
        set(ColorToken::Text, rgba(0.890F, 0.914F, 0.938F));
        set(ColorToken::TextMuted, rgba(0.520F, 0.588F, 0.655F));
        set(ColorToken::Accent, rgba(0.925F, 0.075F, 0.180F));
        set(ColorToken::AccentHovered, rgba(0.985F, 0.145F, 0.255F));
        set(ColorToken::AccentActive, rgba(0.755F, 0.040F, 0.130F));
        set(ColorToken::GridMajor, rgba(0.210F, 0.285F, 0.355F, 0.55F));
        set(ColorToken::GridMinor, rgba(0.160F, 0.220F, 0.285F, 0.28F));
    } else {
        set(ColorToken::Canvas, rgba(0.900F, 0.922F, 0.943F));
        set(ColorToken::Window, rgba(0.965F, 0.974F, 0.982F));
        set(ColorToken::Panel, rgba(0.985F, 0.990F, 0.995F));
        set(ColorToken::PanelRaised, rgba(0.925F, 0.943F, 0.960F));
        set(ColorToken::Input, rgba(0.940F, 0.953F, 0.966F));
        set(ColorToken::Border, rgba(0.720F, 0.760F, 0.800F));
        set(ColorToken::BorderStrong, rgba(0.570F, 0.625F, 0.680F));
        set(ColorToken::Text, rgba(0.070F, 0.098F, 0.125F));
        set(ColorToken::TextMuted, rgba(0.335F, 0.390F, 0.445F));
        set(ColorToken::Accent, rgba(0.835F, 0.035F, 0.125F));
        set(ColorToken::AccentHovered, rgba(0.930F, 0.070F, 0.175F));
        set(ColorToken::AccentActive, rgba(0.705F, 0.020F, 0.095F));
        set(ColorToken::GridMajor, rgba(0.405F, 0.465F, 0.525F, 0.45F));
        set(ColorToken::GridMinor, rgba(0.515F, 0.565F, 0.615F, 0.24F));
    }

    // These are data identities, not theme decoration. Keep them stable across
    // dark/light modes so a lap never appears to change role with the theme.
    set(ColorToken::Reference, rgba(0.20F, 0.78F, 0.40F));
    set(ColorToken::CompareA, rgba(1.00F, 0.68F, 0.12F));
    set(ColorToken::CompareB, rgba(0.95F, 0.25F, 0.28F));
    set(ColorToken::Playback, rgba(0.10F, 0.85F, 0.95F));
    set(ColorToken::PlaybackCursor, rgba(1.00F, 0.73F, 0.15F));
    set(ColorToken::Inspection, rgba(0.10F, 0.85F, 0.95F));
    set(ColorToken::Positive, rgba(0.20F, 0.78F, 0.40F));
    set(ColorToken::Warning, rgba(1.00F, 0.68F, 0.12F));
    set(ColorToken::Danger, rgba(0.95F, 0.25F, 0.28F));
    return result;
}

float safe_scale(float value) noexcept {
    return std::clamp(value, 0.75F, 3.0F);
}

void apply_imgui_style(float scale) {
    ImGuiStyle style{};
    if (active_theme == ThemeMode::Dark) ImGui::StyleColorsDark(&style);
    else ImGui::StyleColorsLight(&style);

    const auto& p = active_palette;
    const auto c = [&](ColorToken token) -> const ImVec4& { return p[index(token)]; };
    auto* colors = style.Colors;

    colors[ImGuiCol_Text] = c(ColorToken::Text);
    colors[ImGuiCol_TextDisabled] = c(ColorToken::TextMuted);
    colors[ImGuiCol_WindowBg] = c(ColorToken::Window);
    colors[ImGuiCol_ChildBg] = c(ColorToken::Panel);
    colors[ImGuiCol_PopupBg] = c(ColorToken::PanelRaised);
    colors[ImGuiCol_Border] = c(ColorToken::Border);
    colors[ImGuiCol_BorderShadow] = rgba(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_FrameBg] = c(ColorToken::Input);
    colors[ImGuiCol_FrameBgHovered] = c(ColorToken::PanelRaised);
    colors[ImGuiCol_FrameBgActive] = with_alpha(c(ColorToken::Accent), 0.23F);
    colors[ImGuiCol_TitleBg] = c(ColorToken::Canvas);
    colors[ImGuiCol_TitleBgActive] = c(ColorToken::PanelRaised);
    colors[ImGuiCol_TitleBgCollapsed] = c(ColorToken::Canvas);
    colors[ImGuiCol_MenuBarBg] = c(ColorToken::Canvas);
    colors[ImGuiCol_ScrollbarBg] = with_alpha(c(ColorToken::Canvas), 0.72F);
    colors[ImGuiCol_ScrollbarGrab] = c(ColorToken::BorderStrong);
    colors[ImGuiCol_ScrollbarGrabHovered] = c(ColorToken::TextMuted);
    colors[ImGuiCol_ScrollbarGrabActive] = c(ColorToken::Accent);
    colors[ImGuiCol_CheckMark] = c(ColorToken::AccentHovered);
    colors[ImGuiCol_CheckboxSelectedBg] = with_alpha(c(ColorToken::Accent), 0.28F);
    colors[ImGuiCol_SliderGrab] = c(ColorToken::Accent);
    colors[ImGuiCol_SliderGrabActive] = c(ColorToken::AccentHovered);
    colors[ImGuiCol_Button] = c(ColorToken::PanelRaised);
    colors[ImGuiCol_ButtonHovered] = with_alpha(c(ColorToken::Accent), 0.72F);
    colors[ImGuiCol_ButtonActive] = c(ColorToken::AccentActive);
    colors[ImGuiCol_Header] = with_alpha(c(ColorToken::PanelRaised), 0.82F);
    colors[ImGuiCol_HeaderHovered] = with_alpha(c(ColorToken::Accent), 0.38F);
    colors[ImGuiCol_HeaderActive] = with_alpha(c(ColorToken::Accent), 0.58F);
    colors[ImGuiCol_Separator] = c(ColorToken::Border);
    colors[ImGuiCol_SeparatorHovered] = c(ColorToken::AccentHovered);
    colors[ImGuiCol_SeparatorActive] = c(ColorToken::Accent);
    colors[ImGuiCol_ResizeGrip] = with_alpha(c(ColorToken::BorderStrong), 0.24F);
    colors[ImGuiCol_ResizeGripHovered] = with_alpha(c(ColorToken::AccentHovered), 0.65F);
    colors[ImGuiCol_ResizeGripActive] = c(ColorToken::Accent);
    colors[ImGuiCol_InputTextCursor] = c(ColorToken::AccentHovered);
    colors[ImGuiCol_Tab] = c(ColorToken::Panel);
    colors[ImGuiCol_TabHovered] = with_alpha(c(ColorToken::Accent), 0.40F);
    colors[ImGuiCol_TabSelected] = c(ColorToken::PanelRaised);
    colors[ImGuiCol_TabSelectedOverline] = c(ColorToken::Accent);
    colors[ImGuiCol_TabDimmed] = c(ColorToken::Window);
    colors[ImGuiCol_TabDimmedSelected] = c(ColorToken::Panel);
    colors[ImGuiCol_TabDimmedSelectedOverline] = with_alpha(c(ColorToken::Accent), 0.55F);
    colors[ImGuiCol_DockingPreview] = with_alpha(c(ColorToken::Accent), 0.55F);
    colors[ImGuiCol_DockingEmptyBg] = c(ColorToken::Canvas);
    colors[ImGuiCol_PlotLines] = c(ColorToken::Reference);
    colors[ImGuiCol_PlotLinesHovered] = c(ColorToken::PlaybackCursor);
    colors[ImGuiCol_PlotHistogram] = c(ColorToken::CompareA);
    colors[ImGuiCol_PlotHistogramHovered] = c(ColorToken::Warning);
    colors[ImGuiCol_TableHeaderBg] = c(ColorToken::PanelRaised);
    colors[ImGuiCol_TableBorderStrong] = c(ColorToken::BorderStrong);
    colors[ImGuiCol_TableBorderLight] = c(ColorToken::Border);
    colors[ImGuiCol_TableRowBg] = rgba(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_TableRowBgAlt] = with_alpha(c(ColorToken::PanelRaised), 0.35F);
    colors[ImGuiCol_TextLink] = c(ColorToken::Playback);
    colors[ImGuiCol_TextSelectedBg] = with_alpha(c(ColorToken::Accent), 0.35F);
    colors[ImGuiCol_TreeLines] = c(ColorToken::BorderStrong);
    colors[ImGuiCol_DragDropTarget] = c(ColorToken::PlaybackCursor);
    colors[ImGuiCol_DragDropTargetBg] = with_alpha(c(ColorToken::PlaybackCursor), 0.12F);
    colors[ImGuiCol_UnsavedMarker] = c(ColorToken::Warning);
    colors[ImGuiCol_NavCursor] = c(ColorToken::Playback);
    colors[ImGuiCol_NavWindowingHighlight] = c(ColorToken::Playback);
    colors[ImGuiCol_NavWindowingDimBg] = rgba(0.0F, 0.0F, 0.0F, 0.24F);
    colors[ImGuiCol_ModalWindowDimBg] = rgba(0.0F, 0.0F, 0.0F, 0.55F);

    // Every value comes from a 96-DPI base. Replacing the complete style is
    // deliberate: repeated theme/DPI changes cannot multiply prior values.
    style.FontScaleDpi = scale;
    style.WindowPadding = {10.0F * scale, 8.0F * scale};
    style.WindowRounding = 7.0F * scale;
    style.WindowBorderSize = 1.0F * scale;
    style.WindowBorderHoverPadding = 4.0F * scale;
    style.WindowMinSize = {32.0F * scale, 32.0F * scale};
    style.ChildRounding = 6.0F * scale;
    style.ChildBorderSize = 1.0F * scale;
    style.PopupRounding = 6.0F * scale;
    style.PopupBorderSize = 1.0F * scale;
    style.FramePadding = {9.0F * scale, 5.0F * scale};
    style.FrameRounding = 4.0F * scale;
    style.ItemSpacing = {8.0F * scale, 6.0F * scale};
    style.ItemInnerSpacing = {6.0F * scale, 4.0F * scale};
    style.CellPadding = {8.0F * scale, 4.0F * scale};
    style.IndentSpacing = 20.0F * scale;
    style.ColumnsMinSpacing = 8.0F * scale;
    style.ScrollbarSize = 12.0F * scale;
    style.ScrollbarRounding = 6.0F * scale;
    style.GrabMinSize = 8.0F * scale;
    style.GrabRounding = 4.0F * scale;
    style.ImageRounding = 4.0F * scale;
    style.TabRounding = 4.0F * scale;
    style.TabBorderSize = 0.0F;
    style.TabBarBorderSize = 1.0F * scale;
    style.TabBarOverlineSize = 2.0F * scale;
    style.SeparatorSize = 1.0F * scale;
    style.SeparatorTextBorderSize = 1.0F * scale;
    style.SeparatorTextPadding = {16.0F * scale, 4.0F * scale};
    style.DisplayWindowPadding = {18.0F * scale, 18.0F * scale};
    style.DisplaySafeAreaPadding = {3.0F * scale, 3.0F * scale};
    // The map/telemetry divider is an everyday control, including while the
    // rest of the dock layout is locked or annotation mode is active.
    style.DockingSeparatorSize = 7.0F * scale;
    style.DragDropTargetRounding = 4.0F * scale;
    style.DragDropTargetBorderSize = 2.0F * scale;
    style.DragDropTargetPadding = 3.0F * scale;
    ImGui::GetStyle() = style;
}

void apply_implot_style(float scale) {
    ImPlotStyle style{};
    if (active_theme == ThemeMode::Dark) ImPlot::StyleColorsDark(&style);
    else ImPlot::StyleColorsLight(&style);

    const auto c = [](ColorToken token) -> const ImVec4& { return active_palette[index(token)]; };
    style.PlotDefaultSize = {400.0F * scale, 300.0F * scale};
    style.PlotMinSize = {150.0F * scale, 80.0F * scale};
    style.PlotBorderSize = 1.0F * scale;
    style.MinorAlpha = 0.42F;
    style.MajorTickLen = {8.0F * scale, 8.0F * scale};
    style.MinorTickLen = {4.0F * scale, 4.0F * scale};
    style.MajorTickSize = {1.0F * scale, 1.0F * scale};
    style.MinorTickSize = {1.0F * scale, 1.0F * scale};
    style.MajorGridSize = {1.0F * scale, 1.0F * scale};
    style.MinorGridSize = {1.0F * scale, 1.0F * scale};
    style.PlotPadding = {8.0F * scale, 6.0F * scale};
    style.LabelPadding = {5.0F * scale, 4.0F * scale};
    style.LegendPadding = {8.0F * scale, 8.0F * scale};
    style.LegendInnerPadding = {6.0F * scale, 4.0F * scale};
    style.LegendSpacing = {6.0F * scale, 2.0F * scale};
    style.MousePosPadding = {8.0F * scale, 8.0F * scale};
    style.AnnotationPadding = {4.0F * scale, 3.0F * scale};
    style.DigitalPadding = 16.0F * scale;
    style.DigitalSpacing = 3.0F * scale;

    style.Colors[ImPlotCol_FrameBg] = c(ColorToken::Input);
    style.Colors[ImPlotCol_PlotBg] = c(ColorToken::Canvas);
    style.Colors[ImPlotCol_PlotBorder] = c(ColorToken::Border);
    style.Colors[ImPlotCol_LegendBg] = with_alpha(c(ColorToken::PanelRaised), 0.92F);
    style.Colors[ImPlotCol_LegendBorder] = c(ColorToken::Border);
    style.Colors[ImPlotCol_LegendText] = c(ColorToken::Text);
    style.Colors[ImPlotCol_TitleText] = c(ColorToken::Text);
    style.Colors[ImPlotCol_InlayText] = c(ColorToken::TextMuted);
    style.Colors[ImPlotCol_AxisText] = c(ColorToken::TextMuted);
    style.Colors[ImPlotCol_AxisGrid] = c(ColorToken::GridMajor);
    style.Colors[ImPlotCol_AxisTick] = c(ColorToken::GridMajor);
    style.Colors[ImPlotCol_AxisBg] = rgba(0.0F, 0.0F, 0.0F, 0.0F);
    style.Colors[ImPlotCol_AxisBgHovered] = with_alpha(c(ColorToken::Accent), 0.10F);
    style.Colors[ImPlotCol_AxisBgActive] = with_alpha(c(ColorToken::Accent), 0.18F);
    style.Colors[ImPlotCol_Selection] = c(ColorToken::PlaybackCursor);
    style.Colors[ImPlotCol_Crosshairs] = c(ColorToken::Inspection);
    ImPlot::GetStyle() = style;
}

}  // namespace

FontLoadResult load_windows_ui_font(ImGuiIO& io, float base_size_pixels) {
    base_size_pixels = std::clamp(base_size_pixels, 12.0F, 24.0F);
    wchar_t windows_directory[MAX_PATH]{};
    const auto length = GetWindowsDirectoryW(windows_directory, MAX_PATH);
    const std::filesystem::path font_directory = length > 0 && length < MAX_PATH
        ? std::filesystem::path(windows_directory) / L"Fonts"
        : std::filesystem::path(L"C:\\Windows\\Fonts");

    struct Candidate {
        const wchar_t* file;
        const char* family;
    };
    constexpr Candidate candidates[] = {
        {L"segoeui.ttf", "Segoe UI"},
        {L"segoeuisl.ttf", "Segoe UI Semilight"},
        {L"tahoma.ttf", "Tahoma"},
        {L"arial.ttf", "Arial"},
    };

    for (const auto& candidate : candidates) {
        const auto path = font_directory / candidate.file;
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) continue;
        ImFontConfig config{};
        config.OversampleH = 2;
        config.OversampleV = 1;
        config.RasterizerMultiply = 1.03F;
        std::snprintf(config.Name, sizeof(config.Name), "%s %.0fpx", candidate.family, base_size_pixels);
        if (auto* font = io.Fonts->AddFontFromFileTTF(path.string().c_str(), base_size_pixels, &config,
                                                      io.Fonts->GetGlyphRangesDefault())) {
            io.FontDefault = font;
            return {font, candidate.family, true};
        }
    }

    ImFontConfig fallback{};
    fallback.SizePixels = base_size_pixels;
    std::snprintf(fallback.Name, sizeof(fallback.Name), "ImGui embedded %.0fpx", base_size_pixels);
    auto* font = io.Fonts->AddFontDefaultVector(&fallback);
    io.FontDefault = font;
    return {font, "ImGui embedded", false};
}

void apply_theme(ThemeMode mode, float dpi_scale) {
    active_theme = mode;
    if (dpi_scale > 0.0F) active_dpi_scale = safe_scale(dpi_scale);
    active_palette = make_palette(active_theme);
    apply_imgui_style(active_dpi_scale);
    apply_implot_style(active_dpi_scale);
}

ThemeMode current_theme() noexcept {
    return active_theme;
}

float current_dpi_scale() noexcept {
    return active_dpi_scale;
}

float dpi_scale_for_window(void* native_window) noexcept {
    if (!native_window) return 1.0F;
    const auto dpi = GetDpiForWindow(static_cast<HWND>(native_window));
    return dpi > 0 ? safe_scale(static_cast<float>(dpi) / 96.0F) : 1.0F;
}

const ImVec4& color(ColorToken token) noexcept {
    const auto value = index(token);
    return active_palette[value < active_palette.size() ? value : index(ColorToken::Text)];
}

ImU32 color_u32(ColorToken token) noexcept {
    return ImGui::ColorConvertFloat4ToU32(color(token));
}

}  // namespace racebox::app::ui
