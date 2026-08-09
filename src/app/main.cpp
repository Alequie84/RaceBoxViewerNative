#include "native_app.hpp"
#include "ui_preferences.hpp"
#include "ui_theme.hpp"
#if defined(RACEBOX_ENABLE_CODEX_REVIEW)
#include "codex_review.hpp"
#endif

#include <d3d11.h>
#include <dbghelp.h>
#include <dxgi.h>
#include <windows.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <implot.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <shellapi.h>
#include <string>
#include <string_view>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace {

ID3D11Device* g_device{};
ID3D11DeviceContext* g_context{};
IDXGISwapChain* g_swap_chain{};
ID3D11RenderTargetView* g_render_target{};
bool g_software_renderer{};
float g_pending_dpi_scale{};
racebox::app::NativeApp* g_app{};

std::vector<std::filesystem::path> bundled_demo_files() {
    wchar_t executable_path[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, executable_path, static_cast<DWORD>(std::size(executable_path)));
    if (length == 0 || length >= std::size(executable_path)) return {};
    const auto demo = std::filesystem::path(executable_path).parent_path() / L"demo";
    std::vector<std::filesystem::path> files{
        demo / L"session.vbo",
        demo / L"session.csv",
        demo / L"sanwa.csv"};
    for (const auto& file : files) {
        if (!std::filesystem::is_regular_file(file)) return {};
    }
    return files;
}

std::vector<std::filesystem::path> remembered_session_files(
    const racebox::app::UiPreferences& preferences) {
    if (preferences.recent_session_files.empty()) return {};
    for (const auto& file : preferences.recent_session_files) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error) || error ||
            std::filesystem::file_size(file, error) == 0 || error) {
            return {};
        }
    }
    return preferences.recent_session_files;
}

void release_render_target() {
    if (g_render_target) g_render_target->Release();
    g_render_target = nullptr;
}

bool create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return false;
    const auto result = g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target);
    back_buffer->Release();
    return SUCCEEDED(result);
}

void release_device() {
    release_render_target();
    if (g_swap_chain) g_swap_chain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_swap_chain = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

bool create_device(HWND window, D3D_DRIVER_TYPE driver) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    D3D_FEATURE_LEVEL selected{};
    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    auto result = D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, flags, requested,
        static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION, &description, &g_swap_chain,
        &g_device, &selected, &g_context);
    if (result == E_INVALIDARG) {
        result = D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, flags, requested + 1,
            static_cast<UINT>(std::size(requested) - 1), D3D11_SDK_VERSION, &description,
            &g_swap_chain, &g_device, &selected, &g_context);
    }
    if (FAILED(result) || !create_render_target()) {
        release_device();
        return false;
    }
    return true;
}

LONG WINAPI write_crash_dump(EXCEPTION_POINTERS* exception) {
    const auto directory = racebox::settings_directory() / L"crashes";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t name[96]{};
    swprintf_s(name, L"RaceBoxTelemetryViewer-%04u%02u%02u-%02u%02u%02u.dmp",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    const auto path = directory / name;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), exception, TRUE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
            MiniDumpWithIndirectlyReferencedMemory, &info, nullptr, nullptr);
        CloseHandle(file);
    }
    racebox::write_log("Unhandled exception; local crash dump written");
    return EXCEPTION_EXECUTE_HANDLER;
}

LRESULT WINAPI window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return true;
    switch (message) {
        case WM_SIZE:
            if (g_device && wparam != SIZE_MINIMIZED) {
                release_render_target();
                g_swap_chain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
                create_render_target();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            g_pending_dpi_scale = static_cast<float>(HIWORD(wparam)) / 96.0F;
            return 0;
        }
        case WM_CLOSE:
            if (g_app) {
                g_app->request_close();
                return 0;
            }
            break;
        case WM_DROPFILES: {
            const auto drop = reinterpret_cast<HDROP>(wparam);
            const auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::filesystem::path> files;
            files.reserve(count);
            for (UINT index = 0; index < count; ++index) {
                const auto length = DragQueryFileW(drop, index, nullptr, 0);
                std::wstring path(length + 1, L'\0');
                DragQueryFileW(drop, index, path.data(), length + 1);
                path.resize(length);
                files.emplace_back(std::move(path));
            }
            DragFinish(drop);
            if (g_app && !files.empty()) g_app->drop_files(files);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    SetUnhandledExceptionFilter(write_crash_dump);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    bool force_warp = false;
    bool soak_mode = false;
    bool demo_profile = false;
#if defined(RACEBOX_ENABLE_CODEX_REVIEW)
    bool capture_review_once = false;
#endif
    std::vector<std::filesystem::path> startup_files;
    std::optional<std::filesystem::path> startup_race_day;
    int argument_count = 0;
    if (auto** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count)) {
        for (int index = 1; index < argument_count; ++index) {
            if (std::wstring_view(arguments[index]) == L"--warp") force_warp = true;
            else if (std::wstring_view(arguments[index]) == L"--soak") soak_mode = true;
            else if (std::wstring_view(arguments[index]) == L"--demo-profile") demo_profile = true;
#if defined(RACEBOX_ENABLE_CODEX_REVIEW)
            else if (std::wstring_view(arguments[index]) == L"--capture-review-once") capture_review_once = true;
#endif
            else {
                std::filesystem::path candidate(arguments[index]);
                auto extension = candidate.extension().wstring();
                std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
                if (extension == L".rbxday") startup_race_day = std::move(candidate);
                else startup_files.emplace_back(std::move(candidate));
            }
        }
        LocalFree(arguments);
    }
    if (!racebox::configure_settings_profile(demo_profile)) {
        CoUninitialize();
        return 1;
    }

    const auto instance_name = demo_profile
        ? L"Local\\RaceBoxTelemetryViewer-v2-demo"
        : L"Local\\RaceBoxTelemetryViewer-v2-normal";
    HANDLE instance_mutex = CreateMutexW(nullptr, FALSE, instance_name);
    if (!instance_mutex) {
        CoUninitialize();
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const auto title = demo_profile
            ? L"RaceBox Telemetry Viewer 2 [Demo]"
            : L"RaceBox Telemetry Viewer 2";
        if (const auto existing = FindWindowW(L"RaceBoxTelemetryViewerWindow", title)) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(instance_mutex);
        CoUninitialize();
        return 0;
    }
    const auto startup_preferences = racebox::app::load_ui_preferences(
        racebox::settings_directory() / L"preferences.json");
    if (startup_files.empty() && demo_profile) {
        startup_files = remembered_session_files(
            startup_preferences.preferences);
    }
    if (startup_files.empty() && demo_profile) startup_files = bundled_demo_files();

    auto large_icon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    auto small_icon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    if (!large_icon) large_icon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!small_icon) small_icon = LoadIconW(nullptr, IDI_APPLICATION);
    WNDCLASSEXW window_class{sizeof(WNDCLASSEXW), CS_CLASSDC, window_proc, 0, 0, instance,
        large_icon, nullptr, nullptr, nullptr,
        L"RaceBoxTelemetryViewerWindow", small_icon};
    RegisterClassExW(&window_class);
    const auto window_title = demo_profile
        ? L"RaceBox Telemetry Viewer 2 [Demo]"
        : L"RaceBox Telemetry Viewer 2";
    const auto window = CreateWindowW(window_class.lpszClassName, window_title,
        WS_OVERLAPPEDWINDOW, 80, 60, 1600, 950, nullptr, nullptr, instance, nullptr);
    if (!window) {
        UnregisterClassW(window_class.lpszClassName, instance);
        CloseHandle(instance_mutex);
        CoUninitialize();
        return 1;
    }
    DragAcceptFiles(window, TRUE);

    if (force_warp || !create_device(window, D3D_DRIVER_TYPE_HARDWARE)) {
        g_software_renderer = true;
        if (!create_device(window, D3D_DRIVER_TYPE_WARP)) {
            MessageBoxW(window, L"DirectX 11 and the WARP fallback could not start.",
                        L"RaceBox Telemetry Viewer 2", MB_ICONERROR);
            DestroyWindow(window);
            UnregisterClassW(window_class.lpszClassName, instance);
            CloseHandle(instance_mutex);
            CoUninitialize();
            return 1;
        }
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    static const auto ini_path = (racebox::settings_directory() / L"layout.ini").string();
    io.IniFilename = ini_path.c_str();
    const auto font = racebox::app::ui::load_windows_ui_font(io);
    racebox::write_log(std::string("UI font: ") + font.family);
    racebox::app::ui::apply_theme(
        startup_preferences.preferences.light_theme
            ? racebox::app::ui::ThemeMode::Light
            : racebox::app::ui::ThemeMode::Dark,
        racebox::app::ui::dpi_scale_for_window(window));
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_context);

    racebox::app::NativeApp app(window, g_device, g_software_renderer);
    g_app = &app;
    if (soak_mode) app.enable_soak_mode();
    if (startup_race_day) app.open_race_day_document(*startup_race_day);
    else if (!startup_files.empty()) app.open_files(startup_files);
    bool running = true;
#if defined(RACEBOX_ENABLE_CODEX_REVIEW)
    int capture_review_countdown = capture_review_once ? 120 : -1;
#endif
    while (running) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (g_pending_dpi_scale > 0.0F) {
            racebox::app::ui::apply_theme(racebox::app::ui::current_theme(), g_pending_dpi_scale);
            g_pending_dpi_scale = 0.0F;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
#if defined(RACEBOX_ENABLE_CODEX_REVIEW)
        if (capture_review_countdown > 0) --capture_review_countdown;
        else if (capture_review_countdown == 0) {
            app.request_codex_review();
            capture_review_countdown = -1;
        }
#endif
        if (!app.render()) running = false;
        ImGui::Render();

        const auto& canvas = racebox::app::ui::color(racebox::app::ui::ColorToken::Canvas);
        const float clear_color[4] = {canvas.x, canvas.y, canvas.z, canvas.w};
        g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
        g_context->ClearRenderTargetView(g_render_target, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
#if defined(RACEBOX_ENABLE_CODEX_REVIEW)
        if (app.consume_codex_review_request()) {
            const auto capture = racebox::app::codex_review::capture(
                g_device, g_context, g_swap_chain,
                app.codex_review_context());
            app.complete_codex_review(capture.ok, capture.error);
        }
#endif
        g_swap_chain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    g_app = nullptr;
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    release_device();
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, instance);
    CloseHandle(instance_mutex);
    CoUninitialize();
    return 0;
}
