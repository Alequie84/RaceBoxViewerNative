#include "native_app.hpp"

#include <shlobj.h>
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

void set_initial_folder(
    IFileDialog* dialog,
    const std::filesystem::path& initial_folder) {
    if (!dialog || initial_folder.empty()) return;
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(
            initial_folder.c_str(), nullptr,
            IID_PPV_ARGS(&folder)))) {
        dialog->SetFolder(folder);
        folder->Release();
    }
}

}  // namespace

std::vector<std::filesystem::path> open_telemetry_files(
    HWND owner,
    const std::filesystem::path& initial_folder) {
    std::vector<std::filesystem::path> paths;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) return paths;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    const COMDLG_FILTERSPEC filters[] = {{L"Telemetry files", L"*.vbo;*.csv;*.gpx;*.rbxsession;*.rbxlap"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    set_initial_folder(dialog, initial_folder);
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

std::optional<std::filesystem::path> choose_telemetry_folder(
    HWND owner,
    const std::filesystem::path& initial_folder) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    dialog->SetTitle(L"Choose RaceBox import folder");
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(
        options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST |
        FOS_FORCEFILESYSTEM);
    set_initial_folder(dialog, initial_folder);
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

std::filesystem::path default_telemetry_download_folder() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &raw))) {
        return {};
    }
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
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
