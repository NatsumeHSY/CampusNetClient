#include "CredentialStore.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>

#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#pragma comment(lib, "Advapi32.lib")

namespace {

void ClearString(std::wstring& value) {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
    }
    value.clear();
}

} // namespace

WindowsCredentialStore::WindowsCredentialStore(std::wstring target)
    : target_(std::move(target)) {}

std::optional<Credentials> WindowsCredentialStore::Read() {
    PCREDENTIALW raw = nullptr;
    if (!CredReadW(target_.c_str(), CRED_TYPE_GENERIC, 0, &raw)) {
        return std::nullopt;
    }

    Credentials result;
    if (raw->UserName != nullptr) {
        result.username = raw->UserName;
    }

    if (raw->CredentialBlob != nullptr && raw->CredentialBlobSize != 0) {
        if (raw->CredentialBlobSize % sizeof(wchar_t) != 0) {
            CredFree(raw);
            return std::nullopt;
        }

        const auto characterCount =
            raw->CredentialBlobSize / static_cast<DWORD>(sizeof(wchar_t));
        const auto* characters =
            reinterpret_cast<const wchar_t*>(raw->CredentialBlob);
        result.password.assign(characters, characters + characterCount);
        while (!result.password.empty() && result.password.back() == L'\0') {
            result.password.pop_back();
        }
    }

    CredFree(raw);
    return result;
}

bool WindowsCredentialStore::Write(const Credentials& credentials) {
    if (credentials.password.size() >
        (std::numeric_limits<DWORD>::max() / sizeof(wchar_t)) - 1) {
        return false;
    }

    std::wstring password = credentials.password;
    std::vector<BYTE> blob((password.size() + 1) * sizeof(wchar_t));
    if (!password.empty()) {
        std::memcpy(blob.data(), password.data(),
                    password.size() * sizeof(wchar_t));
    }

    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target_.c_str());
    credential.UserName = const_cast<LPWSTR>(credentials.username.c_str());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
    credential.CredentialBlob = blob.data();

    const BOOL written = CredWriteW(&credential, 0);
    if (!blob.empty()) {
        SecureZeroMemory(blob.data(), blob.size());
    }
    ClearString(password);
    return written == TRUE;
}

bool WindowsCredentialStore::Clear() {
    if (CredDeleteW(target_.c_str(), CRED_TYPE_GENERIC, 0)) {
        return true;
    }
    return GetLastError() == ERROR_NOT_FOUND;
}
