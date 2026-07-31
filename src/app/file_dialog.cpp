#include "native_app.hpp"

#include <shobjidl.h>

#include <iterator>

namespace racebox::app {
namespace {

std::optional<std::filesystem::path> shell_path(IShellItem* item) {
    PWSTR raw = nullptr;
    if (!item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) return std::nullopt;
    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    return path;
}

}  // namespace

std::vector<std::filesystem::path> open_telemetry_files(HWND owner) {
    std::vector<std::filesystem::path> paths;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) return paths;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    const COMDLG_FILTERSPEC filters[] = {{L"Telemetry files", L"*.vbo;*.csv;*.gpx;*.rbxsession;*.rbxlap"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->GetResults(&items))) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD index = 0; index < count; ++index) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(items->GetItemAt(index, &item))) {
                    if (const auto path = shell_path(item)) paths.push_back(*path);
                    item->Release();
                }
            }
            items->Release();
        }
    }
    dialog->Release();
    return paths;
}

std::optional<std::filesystem::path> open_race_day_file(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) return std::nullopt;
    dialog->SetTitle(L"Open RaceBox Race Day");
    const COMDLG_FILTERSPEC filters[] = {{L"RaceBox race day", L"*.rbxday"}};
    dialog->SetFileTypes(1, filters);
    dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    std::optional<std::filesystem::path> result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            result = shell_path(item);
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

std::optional<std::filesystem::path> open_annotation_file(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    dialog->SetTitle(L"Open RaceBox Annotation Review");
    const COMDLG_FILTERSPEC filters[] = {
        {L"RaceBox annotation review", L"*.json"}};
    dialog->SetFileTypes(1, filters);
    dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    std::optional<std::filesystem::path> result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            result = shell_path(item);
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

std::optional<std::filesystem::path> open_image_file(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) return std::nullopt;
    const COMDLG_FILTERSPEC filters[] = {{L"Map images", L"*.png;*.jpg;*.jpeg;*.bmp"}};
    dialog->SetFileTypes(1, filters);
    std::optional<std::filesystem::path> result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            result = shell_path(item);
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

std::optional<std::filesystem::path> save_file(HWND owner, const wchar_t* title, const wchar_t* extension, const wchar_t* filter) {
    IFileSaveDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) return std::nullopt;
    dialog->SetTitle(title);
    dialog->SetDefaultExtension(extension);
    const COMDLG_FILTERSPEC filters[] = {{title, filter}};
    dialog->SetFileTypes(1, filters);
    std::optional<std::filesystem::path> result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            result = shell_path(item);
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

}  // namespace racebox::app
