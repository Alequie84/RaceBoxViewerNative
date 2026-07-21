#include "native_app.hpp"
#include "ui_theme.hpp"

#include <d3d11.h>
#include <dbghelp.h>
#include <dxgi.h>
#include <windows.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <implot.h>

#include <filesystem>
#include <memory>
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
    swprintf_s(name, L"RaceBoxViewer-%04u%02u%02u-%02u%02u%02u.dmp",
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
    std::vector<std::filesystem::path> startup_files;
    int argument_count = 0;
    if (auto** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count)) {
        for (int index = 1; index < argument_count; ++index) {
            if (std::wstring_view(arguments[index]) == L"--warp") force_warp = true;
            else if (std::wstring_view(arguments[index]) == L"--soak") soak_mode = true;
            else startup_files.emplace_back(arguments[index]);
        }
        LocalFree(arguments);
    }

    WNDCLASSEXW window_class{sizeof(WNDCLASSEXW), CS_CLASSDC, window_proc, 0, 0, instance,
        LoadIconW(instance, MAKEINTRESOURCEW(101)), nullptr, nullptr, nullptr,
        L"RaceBoxViewerNativeWindow", LoadIconW(instance, MAKEINTRESOURCEW(101))};
    RegisterClassExW(&window_class);
    const auto window = CreateWindowW(window_class.lpszClassName, L"RaceBox Viewer Native",
        WS_OVERLAPPEDWINDOW, 80, 60, 1600, 950, nullptr, nullptr, instance, nullptr);
    if (!window) {
        UnregisterClassW(window_class.lpszClassName, instance);
        CoUninitialize();
        return 1;
    }

    if (force_warp || !create_device(window, D3D_DRIVER_TYPE_HARDWARE)) {
        g_software_renderer = true;
        if (!create_device(window, D3D_DRIVER_TYPE_WARP)) {
            MessageBoxW(window, L"DirectX 11 and the WARP fallback could not start.", L"RaceBox Viewer", MB_ICONERROR);
            DestroyWindow(window);
            UnregisterClassW(window_class.lpszClassName, instance);
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
    racebox::app::ui::apply_theme(racebox::app::ui::ThemeMode::Dark,
        racebox::app::ui::dpi_scale_for_window(window));
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_context);

    racebox::app::NativeApp app(window, g_device, g_software_renderer);
    if (soak_mode) app.enable_soak_mode();
    if (!startup_files.empty()) app.open_files(startup_files);
    bool running = true;
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
        if (!app.render()) running = false;
        ImGui::Render();

        const auto& canvas = racebox::app::ui::color(racebox::app::ui::ColorToken::Canvas);
        const float clear_color[4] = {canvas.x, canvas.y, canvas.z, canvas.w};
        g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
        g_context->ClearRenderTargetView(g_render_target, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap_chain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    release_device();
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, instance);
    CoUninitialize();
    return 0;
}
