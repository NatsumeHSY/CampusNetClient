#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

class UserSessionLauncher {
public:
    static DWORD ActiveConsoleSessionId();
    static bool HasInteractiveUser();

    // Launches the executable in the active user's session with no user-
    // supplied command-line arguments. This is intended for the Service's
    // UI handoff and does not create a shell, scheduled task, or script.
    static bool LaunchInActiveSession(const std::filesystem::path& executable,
                                      std::wstring& error);
};

