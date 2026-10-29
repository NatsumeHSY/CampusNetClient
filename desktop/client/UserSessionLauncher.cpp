#include "UserSessionLauncher.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <vector>
#include <algorithm>
#include <cwchar>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Userenv.lib")
#pragma comment(lib, "Wtsapi32.lib")

namespace {

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_;
};

std::wstring QuoteExecutable(const std::filesystem::path& executable) {
    std::wstring value = executable.wstring();
    std::wstring quoted = L"\"";
    for (const wchar_t ch : value) {
        if (ch == L'"') {
            quoted += L"\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted += L"\"";
    return quoted;
}

std::vector<wchar_t> AddEnvironmentValues(
    LPVOID environment,
    const std::vector<std::pair<std::wstring, std::wstring>>& extraEnvironment) {
    const auto* source = static_cast<const wchar_t*>(environment);
    std::vector<std::wstring> entries;
    for (const wchar_t* current = source; *current != L'\0';
         current += wcslen(current) + 1) {
        entries.emplace_back(current);
    }
    const auto setValue = [&](std::wstring_view name, std::wstring_view value) {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [&](const std::wstring& entry) {
                const auto equal = entry.find(L'=');
                return equal != std::wstring::npos && equal != 0 &&
                    CompareStringOrdinal(entry.data(), static_cast<int>(equal),
                        name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
            }), entries.end());
        entries.emplace_back(std::wstring(name) + L"=" + std::wstring(value));
    };
    setValue(L"CAMPUSNET_UI_START_HIDDEN", L"1");
    for (const auto& [name, value] : extraEnvironment) setValue(name, value);
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) ==
               CSTR_LESS_THAN;
    });
    std::vector<wchar_t> result;
    for (const auto& entry : entries) {
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    if (entries.empty()) result.push_back(L'\0');
    return result;
}

} // namespace

DWORD UserSessionLauncher::ActiveConsoleSessionId() {
    return WTSGetActiveConsoleSessionId();
}

bool UserSessionLauncher::HasInteractiveUser() {
    return ActiveConsoleSessionId() != 0xFFFFFFFF;
}

bool UserSessionLauncher::LaunchInActiveSession(
    const std::filesystem::path& executable, std::wstring& error,
    const std::vector<std::pair<std::wstring, std::wstring>>& extraEnvironment) {
    error.clear();
    if (executable.empty() || !std::filesystem::exists(executable)) {
        error = L"the UI executable does not exist";
        return false;
    }

    const DWORD sessionId = ActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        error = L"there is no active interactive session";
        return false;
    }

    HANDLE userToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &userToken)) {
        error = L"the active user's token could not be opened";
        return false;
    }
    Handle token(userToken);

    TOKEN_ELEVATION_TYPE elevationType{};
    DWORD returned = 0;
    if (!GetTokenInformation(token.get(), TokenElevationType, &elevationType, sizeof(elevationType), &returned)) {
        error = L"unable to inspect the interactive token";
        return false;
    }
    TOKEN_LINKED_TOKEN linked{};
    HANDLE effectiveToken = token.get();
    if (elevationType == TokenElevationTypeLimited) {
        if (!GetTokenInformation(token.get(), TokenLinkedToken, &linked, sizeof(linked), &returned)) {
            error = L"an elevated administrator token is unavailable";
            return false;
        }
        effectiveToken = linked.LinkedToken;
    }
    Handle elevated(linked.LinkedToken);
    TOKEN_ELEVATION elevation{};
    if (!GetTokenInformation(effectiveToken, TokenElevation, &elevation, sizeof(elevation), &returned) || !elevation.TokenIsElevated) {
        error = L"the interactive user has no elevated administrator token";
        return false;
    }
    HANDLE primaryToken = nullptr;
    if (!DuplicateTokenEx(effectiveToken, TOKEN_ALL_ACCESS, nullptr,
                          SecurityImpersonation, TokenPrimary, &primaryToken)) {
        error = L"the active user's primary token could not be created";
        return false;
    }
    Handle primary(primaryToken);

    LPVOID environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, primary.get(), FALSE)) {
        error = L"the interactive environment could not be created";
        return false;
    }
    const auto environmentWithMarker = AddEnvironmentValues(environment,
                                                             extraEnvironment);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    std::wstring commandLine = QuoteExecutable(executable);
    const bool created = CreateProcessAsUserW(
        primary.get(), executable.wstring().c_str(), commandLine.data(),
        nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
        const_cast<wchar_t*>(environmentWithMarker.data()),
        executable.parent_path().wstring().c_str(), &startup,
        &process) != FALSE;
    DestroyEnvironmentBlock(environment);
    if (!created) {
        error = L"the UI could not be launched in the interactive session";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

