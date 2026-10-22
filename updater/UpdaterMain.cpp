#include "UpdateWindow.h"
#include "UpdateAgentStatus.h"
#include "UpdaterApply.h"
#include "UpdaterAgent.h"
#include "UpdaterRole.h"
#include "../client/ClientIpc.h"
#include "Protocol.h"

#include <Windows.h>
#include <Shellapi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr wchar_t kRoleEnvironmentVariable[] = L"CAMPUSNET_UPDATE_ROLE";
constexpr wchar_t kInstallRootEnvironmentVariable[] = L"CAMPUSNET_UPDATE_INSTALL_ROOT";
constexpr wchar_t kChannelEnvironmentVariable[] = L"CAMPUSNET_UPDATE_CHANNEL";

bool ReadInheritedRole(std::wstring& value) {
    std::array<wchar_t, 32> buffer{};
    const DWORD length = GetEnvironmentVariableW(
        kRoleEnvironmentVariable, buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0) {
        const DWORD lastError = GetLastError();
        return lastError == ERROR_ENVVAR_NOT_FOUND || lastError == ERROR_SUCCESS;
    }
    if (length >= buffer.size()) {
        return false;
    }
    value.assign(buffer.data(), length);
    return true;
}

bool ReadOptionalEnvironment(std::wstring_view name, std::wstring& value) {
    const std::wstring key(name);
    DWORD capacity = 32768;
    std::wstring buffer(capacity, L'\0');
    const DWORD length = GetEnvironmentVariableW(key.c_str(), buffer.data(), capacity);
    if (length == 0 || length >= capacity) return false;
    buffer.resize(length);
    value = std::move(buffer);
    return true;
}

bool RelaunchInstalledClient(UpdateChannel channel, std::wstring& error) {
    std::wstring installRootText;
    std::wstring channelText;
    if (!ReadOptionalEnvironment(kInstallRootEnvironmentVariable, installRootText) ||
        !ReadOptionalEnvironment(kChannelEnvironmentVariable, channelText) ||
        (channelText != L"portable" && channelText != L"msi") ||
        channelText != (channel == UpdateChannel::Portable ? L"portable" : L"msi")) {
        error = L"Updated client restart information is missing or invalid";
        return false;
    }
    const std::filesystem::path installRoot(installRootText);
    if (!installRoot.is_absolute()) {
        error = L"Updated client install directory is not absolute";
        return false;
    }
    const auto executable = channel == UpdateChannel::Portable
        ? installRoot / L"CampusNetLauncher.exe"
        : installRoot / L"CampusNetClient.exe";
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS;
    launch.lpVerb = L"runas";
    launch.lpFile = executable.c_str();
    launch.lpDirectory = installRoot.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch)) {
        const DWORD code = GetLastError();
        error = code == ERROR_CANCELLED
            ? L"Update succeeded, but UAC restart was cancelled. Open CampusNetClient manually."
            : L"Update succeeded, but the client could not be restarted (Windows error " +
                  std::to_wstring(code) + L"). Open CampusNetClient manually.";
        return false;
    }
    if (launch.hProcess != nullptr) CloseHandle(launch.hProcess);
    return true;
}

UpdateWindowState ToWindowState(const UpdateAgentStatus& status) {
    UpdateWindowState state;
    state.localVersion = status.localVersion.empty() ? L"Unknown" : status.localVersion;
    state.targetVersion = status.targetVersion.empty() ? L"Unknown" : status.targetVersion;
    state.channel = status.channel == UpdateChannel::Portable ? L"portable" : L"msi";
    state.releaseNotes = status.releaseNotes;
    state.status = L"Update phase: " + UpdatePhaseName(status.phase);
    state.receivedBytes = status.receivedBytes;
    state.totalBytes = status.totalBytes;
    state.bytesPerSecond = status.bytesPerSecond;
    if (status.errorCode != 0) {
        state.error = L"Windows error " + std::to_wstring(status.errorCode);
    }
    return state;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    std::wstring roleText;
    if (!ReadInheritedRole(roleText)) {
        return ERROR_INVALID_PARAMETER;
    }
    const UpdaterRole role = ResolveUpdaterRole(roleText);
    if (role == UpdaterRole::Invalid) {
        return ERROR_INVALID_PARAMETER;
    }
    if (role == UpdaterRole::Agent) {
        return RunUpdaterAgentFromEnvironment();
    }
    if (role == UpdaterRole::Apply) {
        return RunUpdaterApplyFromEnvironment();
    }

    UpdateWindowState state;
    state.status = L"Checking for updates...";
    UpdaterWindow window(std::move(state));
    std::atomic_bool stopping{false};
    std::thread statusPoller([&window, &stopping] {
        ServiceIpcClient ipc;
        bool requestedCheck = false;
        bool applyRequested = false;
        bool restartAttempted = false;
        while (!stopping.load()) {
            std::wstring ipcError;
            if (!requestedCheck) {
                ipc.Call({kProtocolVersion, MessageType::CheckForUpdates,
                          L"updater-gui-check", "{}"}, 2500, ipcError);
                requestedCheck = true;
            }

            auto response = ipc.Call(
                {kProtocolVersion, MessageType::GetUpdateStatus,
                 L"updater-gui-status", "{}"}, 2500, ipcError);
            if (!response) {
                UpdateWindowState unavailable;
                unavailable.status = L"Service unavailable";
                unavailable.error = std::move(ipcError);
                window.SetState(std::move(unavailable));
            } else {
                UpdateAgentStatus status;
                std::wstring parseError;
                if (!DecodeUpdateStatusPayload(response->payload, status, false,
                                               parseError)) {
                    UpdateWindowState invalid;
                    invalid.status = L"Invalid service status";
                    invalid.error = std::move(parseError);
                    window.SetState(std::move(invalid));
                } else {
                    window.SetState(ToWindowState(status));
                    if (status.phase == UpdatePhase::Ready && !applyRequested) {
                        auto applyReply = ipc.Call(
                            {kProtocolVersion, MessageType::ApplyStagedUpdate,
                             L"updater-gui-apply", "{}"}, 2500, ipcError);
                        applyRequested = applyReply.has_value();
                        if (!applyReply) {
                            UpdateWindowState applyFailure = ToWindowState(status);
                            applyFailure.error = std::move(ipcError);
                            window.SetState(std::move(applyFailure));
                        }
                    }
                    if (status.phase == UpdatePhase::Failed ||
                        status.phase == UpdatePhase::UpToDate) {
                        if (status.phase == UpdatePhase::UpToDate &&
                            applyRequested && !restartAttempted) {
                            restartAttempted = true;
                            std::wstring restartError;
                            if (RelaunchInstalledClient(status.channel, restartError)) {
                                UpdateWindowState complete = ToWindowState(status);
                                complete.status = L"Update installed. CampusNetClient is starting.";
                                complete.requiresUacRestart = true;
                                window.SetState(std::move(complete));
                            } else {
                                UpdateWindowState restartFailure = ToWindowState(status);
                                restartFailure.status = L"Update installed, but restart needs attention.";
                                restartFailure.error = std::move(restartError);
                                window.SetState(std::move(restartFailure));
                            }
                        }
                        applyRequested = false;
                    }
                }
            }

            for (unsigned tick = 0; tick < 5 && !stopping.load(); ++tick) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
    const int result = window.Run(instance);
    stopping.store(true);
    if (statusPoller.joinable()) statusPoller.join();
    return result;
}
