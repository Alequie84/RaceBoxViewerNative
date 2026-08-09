#include "racebox/credential_store.hpp"

#include <windows.h>
#include <wincred.h>

#include <vector>

namespace racebox::credential_store {
namespace {

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

void set_error(std::string* error, std::string_view action) {
    if (error) *error = std::string(action) + " (Windows error " + std::to_string(GetLastError()) + ")";
}

}  // namespace

bool write(std::string_view reference, std::string_view secret, std::string* error) {
    if (reference.empty()) {
        if (error) *error = "Credential reference is empty";
        return false;
    }
    if (secret.empty()) return erase(reference, error);
    const auto target = widen(reference);
    if (target.empty()) {
        if (error) *error = "Credential reference is not valid UTF-8";
        return false;
    }
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
    credential.CredentialBlob = reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"RaceBox Crew Chief gateway client");
    if (!CredWriteW(&credential, 0)) {
        set_error(error, "Could not store the gateway token in Windows Credential Manager");
        return false;
    }
    return true;
}

std::optional<std::string> read(std::string_view reference, std::string* error) {
    const auto target = widen(reference);
    if (target.empty()) {
        if (error) *error = "Credential reference is empty or invalid";
        return std::nullopt;
    }
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        if (GetLastError() != ERROR_NOT_FOUND) {
            set_error(error, "Could not read the gateway token from Windows Credential Manager");
        }
        return std::nullopt;
    }
    std::string secret(
        reinterpret_cast<const char*>(credential->CredentialBlob),
        reinterpret_cast<const char*>(credential->CredentialBlob) + credential->CredentialBlobSize);
    CredFree(credential);
    return secret;
}

bool erase(std::string_view reference, std::string* error) {
    const auto target = widen(reference);
    if (target.empty()) {
        if (error) *error = "Credential reference is empty or invalid";
        return false;
    }
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND) {
        return true;
    }
    set_error(error, "Could not remove the gateway token from Windows Credential Manager");
    return false;
}

}  // namespace racebox::credential_store
