#include "UserSessionLauncher.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <vector>

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

std::vector<wchar_t> AddHiddenUiMarker(LPVOID environment) {
    const auto* source = static_cast<const wchar_t*>(environment);
    std::size_t length = 0;
    while (!(source[length] == L'\0' && source[length + 1] == L'\0')) {
        ++length;
    }
    ++length;

    std::vector<wchar_t> result(source, source + length);
    constexpr wchar_t marker[] = L"CAMPUSNET_UI_START_HIDDEN=1";
    result.insert(result.end(), marker, marker + ARRAYSIZE(marker) - 1);
    result.push_back(L'\0');
    result.push_back(L'\0');
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
    const std::filesystem::path& executable, std::wstring& error) {
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

    HANDLE primaryToken = nullptr;
    if (!DuplicateTokenEx(token.get(), TOKEN_ALL_ACCESS, nullptr,
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
    const auto environmentWithMarker = AddHiddenUiMarker(environment);

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

