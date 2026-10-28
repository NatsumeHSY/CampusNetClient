#include "UpdaterApply.h"

#include "MsiUpdateApply.h"
#include "PortableUpdateApply.h"
#include "UpdateDownloader.h"
#include "UpdateIntegrity.h"
#include "UpdateManifest.h"
#include "WinHttpUpdateTransport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Winhttp.h>
#include <TlHelp32.h>
#include <Msi.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "Msi.lib")

namespace {

constexpr wchar_t kLocalVersionVariable[] = L"CAMPUSNET_UPDATE_LOCAL_VERSION";
constexpr wchar_t kTargetVersionVariable[] = L"CAMPUSNET_UPDATE_TARGET_VERSION";
constexpr wchar_t kChannelVariable[] = L"CAMPUSNET_UPDATE_CHANNEL";
constexpr wchar_t kStagingVariable[] = L"CAMPUSNET_UPDATE_STAGING";
constexpr wchar_t kInstallRootVariable[] = L"CAMPUSNET_UPDATE_INSTALL_ROOT";
constexpr wchar_t kArtifactShaVariable[] = L"CAMPUSNET_UPDATE_ARTIFACT_SHA256";
constexpr wchar_t kArtifactSizeVariable[] = L"CAMPUSNET_UPDATE_ARTIFACT_SIZE";
constexpr wchar_t kServiceName[] = L"CampusNetService";
constexpr std::uint64_t kMaxArtifactBytes = 1024ull * 1024 * 1024;

bool ReadEnvironment(std::wstring_view name, std::wstring& value) {
    const std::wstring key(name);
    DWORD capacity = 256;
    while (capacity <= 32768) {
        std::wstring buffer(capacity, L'\0');
        const DWORD length = GetEnvironmentVariableW(key.c_str(), buffer.data(),
                                                     capacity);
        if (length == 0) return false;
        if (length < capacity) {
            buffer.resize(length);
            value = std::move(buffer);
            return true;
        }
        capacity = length + 1;
    }
    return false;
}

bool ParseUnsigned(std::wstring_view text, std::uint64_t& value) {
    if (text.empty() || (text.size() > 1 && text.front() == L'0')) return false;
    value = 0;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return false;
        const auto digit = static_cast<std::uint64_t>(ch - L'0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}

bool IsAuthorizedApplyProcess() {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return false;
    struct TokenHandle {
        HANDLE value;
        ~TokenHandle() { if (value != nullptr) CloseHandle(value); }
    } token{rawToken};
    DWORD sessionId = MAXDWORD;
    DWORD returned = 0;
    if (!GetTokenInformation(token.value, TokenSessionId, &sessionId,
                             sizeof(sessionId), &returned)) return false;
    DWORD required = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &required);
    if (required == 0) return false;
    std::vector<std::byte> buffer(required);
    if (!GetTokenInformation(token.value, TokenUser, buffer.data(), required,
                             &required)) return false;
    auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> systemSid{};
    DWORD systemSidSize = static_cast<DWORD>(systemSid.size());
    const bool isSystem = CreateWellKnownSid(
        WinLocalSystemSid, nullptr, systemSid.data(), &systemSidSize) &&
        EqualSid(user->User.Sid, systemSid.data()) != FALSE;
    if (isSystem && sessionId == 0) return true;
    if (sessionId == 0) return false;

    TOKEN_ELEVATION elevation{};
    if (!GetTokenInformation(token.value, TokenElevation, &elevation,
                             sizeof(elevation), &returned) ||
        elevation.TokenIsElevated == 0) return false;
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> administratorSid{};
    DWORD administratorSidSize =
        static_cast<DWORD>(administratorSid.size());
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                            administratorSid.data(), &administratorSidSize)) {
        return false;
    }
    BOOL isAdministrator = FALSE;
    return CheckTokenMembership(token.value, administratorSid.data(),
                                &isAdministrator) && isAdministrator != FALSE;
}

bool SamePath(const std::filesystem::path& left,
              const std::filesystem::path& right) {
    const auto a = std::filesystem::absolute(left).lexically_normal().wstring();
    const auto b = std::filesystem::absolute(right).lexically_normal().wstring();
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool LoadLocalVersion(const std::filesystem::path& root, UpdateChannel channel,
                      LocalInstallInfo& local, std::wstring& error) {
    const auto path = channel == UpdateChannel::Portable
        ? root / L"bin" / L"version.json" : root / L"version.json";
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = L"Could not open installed version.json"; return false; }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length <= 0 || length > 64 * 1024) {
        error = L"Installed version.json size is invalid";
        return false;
    }
    input.seekg(0, std::ios::beg);
    std::string bytes(static_cast<std::size_t>(length), '\0');
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
        !ParseLocalInstallInfo(bytes, local, error)) return false;
    return true;
}

class WindowsInstallerApi final : public IWindowsInstaller {
public:
    std::uint32_t InstallPackage(const std::filesystem::path& package,
                                std::wstring_view properties) override {
        HWND owner = nullptr;
        const INSTALLUILEVEL previous = MsiSetInternalUI(INSTALLUILEVEL_NONE,
                                                         &owner);
        const std::wstring propertyText(properties);
        const UINT result = MsiInstallProductW(
            package.c_str(), propertyText.empty() ? nullptr : propertyText.c_str());
        MsiSetInternalUI(previous, &owner);
        return result;
    }
};

bool ProcessPath(DWORD processId, std::wstring& path, HANDLE& process) {
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE |
                          SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) return false;
    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &length)) {
        CloseHandle(process);
        process = nullptr;
        return false;
    }
    path.assign(buffer.data(), length);
    return true;
}

BOOL CALLBACK RequestWindowClose(HWND window, LPARAM parameter) {
    const DWORD targetProcess = static_cast<DWORD>(parameter);
    DWORD ownerProcess = 0;
    GetWindowThreadProcessId(window, &ownerProcess);
    if (ownerProcess == targetProcess) {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

bool CloseInstallProcesses(const std::filesystem::path& installRoot,
                           UpdateChannel channel, std::wstring& error) {
    const auto client = installRoot / L"CampusNetClient.exe";
    const auto updater = installRoot / L"CampusNetUpdater.exe";
    const auto launcher = installRoot / L"CampusNetLauncher.exe";
    const auto portableClient = installRoot / L"bin" / L"CampusNetClient.exe";
    const auto portableUpdater = installRoot / L"bin" / L"CampusNetUpdater.exe";
    const auto portableLauncher = installRoot / L"CampusNetLauncher.exe";
    const auto matches = [&](const std::filesystem::path& image) {
        return channel == UpdateChannel::Portable
            ? SamePath(image, portableClient) || SamePath(image, portableUpdater) ||
                  SamePath(image, portableLauncher)
            : SamePath(image, client) || SamePath(image, updater) || SamePath(image, launcher);
    };
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        error = L"Could not enumerate CampusNet client processes";
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool success = true;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) continue;
            std::wstring path;
            HANDLE process = nullptr;
            if (!ProcessPath(entry.th32ProcessID, path, process)) continue;
            const bool shouldClose = matches(path);
            if (shouldClose) {
                EnumWindows(&RequestWindowClose,
                            static_cast<LPARAM>(entry.th32ProcessID));
                DWORD wait = WaitForSingleObject(process, 3000);
                if (wait == WAIT_TIMEOUT) {
                    if (!TerminateProcess(process, ERROR_CANCELLED) &&
                        GetLastError() != ERROR_ACCESS_DENIED) {
                        const DWORD code = GetLastError();
                        error = L"Could not terminate a CampusNet process (Windows error " +
                                std::to_wstring(code) + L")";
                        success = false;
                    }
                    wait = WaitForSingleObject(process, 10000);
                }
                if (wait != WAIT_OBJECT_0) {
                    error = L"Timed out waiting for a CampusNet process to exit";
                    success = false;
                }
            }
            CloseHandle(process);
            if (!success) break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return success;
}

class PortableApplyHost final : public IPortableUpdateHost {
public:
    explicit PortableApplyHost(std::filesystem::path installRoot)
        : installRoot_(std::move(installRoot)) {}

    bool QueryServiceInstallation(bool& installed,
                                  std::wstring& error) override {
        installed = false;
        error.clear();
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scm == nullptr) {
            error = L"Could not open Service Control Manager";
            return false;
        }
        SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
        if (service == nullptr) {
            const DWORD code = GetLastError();
            CloseServiceHandle(scm);
            if (code == ERROR_SERVICE_DOES_NOT_EXIST) return true;
            error = L"Could not query CampusNetService";
            return false;
        }
        installed = true;
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return true;
    }

    bool CloseClientProcesses(std::wstring& error) override {
        return CloseInstallProcesses(installRoot_, UpdateChannel::Portable, error);
    }
    bool StopService(std::wstring& error) override {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scm == nullptr) { error = L"Could not open Service Control Manager"; return false; }
        SC_HANDLE service = OpenServiceW(scm, kServiceName,
            SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_START);
        if (service == nullptr) {
            CloseServiceHandle(scm);
            error = L"Could not open CampusNetService";
            return false;
        }
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&status), sizeof(status), &needed);
        if (status.dwCurrentState != SERVICE_STOPPED) {
            SERVICE_STATUS basic{};
            ControlService(service, SERVICE_CONTROL_STOP, &basic);
        }
        const bool stopped = WaitState(service, SERVICE_STOPPED,
                                       std::chrono::seconds(30), error);
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return stopped;
    }
    bool StartService(std::wstring& error) override {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scm == nullptr) { error = L"Could not open Service Control Manager"; return false; }
        SC_HANDLE service = OpenServiceW(scm, kServiceName,
            SERVICE_START | SERVICE_QUERY_STATUS);
        if (service == nullptr) {
            CloseServiceHandle(scm);
            error = L"Could not open CampusNetService for start";
            return false;
        }
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&status), sizeof(status), &needed);
        if (status.dwCurrentState == SERVICE_STOPPED &&
            !::StartServiceW(service, 0, nullptr) &&
            GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
            error = L"Could not start CampusNetService";
            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            return false;
        }
        const bool started = WaitState(service, SERVICE_RUNNING,
                                       std::chrono::seconds(30), error);
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return started;
    }
    bool WaitForServiceRunning(std::chrono::milliseconds timeout,
                               std::wstring& error) override {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scm == nullptr) { error = L"Could not open Service Control Manager"; return false; }
        SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
        if (service == nullptr) {
            CloseServiceHandle(scm);
            error = L"Could not query CampusNetService";
            return false;
        }
        const bool running = WaitState(service, SERVICE_RUNNING, timeout, error);
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return running;
    }

private:
    static bool WaitState(SC_HANDLE service, DWORD desired,
                          std::chrono::milliseconds timeout,
                          std::wstring& error) {
        const ULONGLONG deadline = GetTickCount64() + timeout.count();
        while (GetTickCount64() < deadline) {
            SERVICE_STATUS_PROCESS status{};
            DWORD needed = 0;
            if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                    reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
                error = L"Could not query CampusNetService status";
                return false;
            }
            if (status.dwCurrentState == desired) return true;
            Sleep(200);
        }
        error = L"Timed out waiting for CampusNetService state";
        return false;
    }

    std::filesystem::path installRoot_;
};

} // namespace

int RunUpdaterApplyFromEnvironment() {
    if (!IsAuthorizedApplyProcess()) return ERROR_ACCESS_DENIED;
    std::wstring localVersion;
    std::wstring targetVersion;
    std::wstring channelText;
    std::wstring stagingText;
    std::wstring installText;
    std::wstring sha256;
    std::wstring sizeText;
    if (!ReadEnvironment(kLocalVersionVariable, localVersion) ||
        !ReadEnvironment(kTargetVersionVariable, targetVersion) ||
        !ReadEnvironment(kChannelVariable, channelText) ||
        !ReadEnvironment(kStagingVariable, stagingText) ||
        !ReadEnvironment(kInstallRootVariable, installText) ||
        !ReadEnvironment(kArtifactShaVariable, sha256) ||
        !ReadEnvironment(kArtifactSizeVariable, sizeText)) return ERROR_INVALID_DATA;

    const UpdateChannel channel = channelText == L"portable"
        ? UpdateChannel::Portable : channelText == L"msi"
            ? UpdateChannel::Msi : static_cast<UpdateChannel>(-1);
    std::uint64_t size = 0;
    if ((channel != UpdateChannel::Portable && channel != UpdateChannel::Msi) ||
        !IsValidSemVer(localVersion) || !IsValidSemVer(targetVersion) ||
        sha256.size() != 64 || !ParseUnsigned(sizeText, size) || size == 0 ||
        size > kMaxArtifactBytes) return ERROR_INVALID_DATA;
    if (!std::all_of(sha256.begin(), sha256.end(), [](wchar_t ch) {
            return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
        })) return ERROR_INVALID_DATA;

    const std::filesystem::path stagingDirectory(stagingText);
    const std::filesystem::path installRoot(installText);
    if (!stagingDirectory.is_absolute() || !installRoot.is_absolute()) return ERROR_BAD_PATHNAME;
    LocalInstallInfo local;
    std::wstring error;
    if (!LoadLocalVersion(installRoot, channel, local, error) ||
        local.channel != channel || local.version != localVersion) return ERROR_INVALID_DATA;

    WinHttpUpdateTransport transport;
    UpdateDownloader downloader(transport);
    std::string manifestBytes;
    if (!downloader.FetchManifest(kRemoteManifestUrl, manifestBytes, error)) return ERROR_WINHTTP_CANNOT_CONNECT;
    RemoteUpdateManifest manifest;
    if (!ParseRemoteUpdateManifest(manifestBytes, manifest, error) ||
        manifest.version != targetVersion) return ERROR_INVALID_DATA;
    const UpdateArtifact& artifact = channel == UpdateChannel::Portable
        ? manifest.portable : manifest.msi;
    if (artifact.sha256 != sha256 || artifact.sizeBytes != size) return ERROR_INVALID_DATA;

    const std::filesystem::path stagedPath = stagingDirectory /
        (sha256 + (channel == UpdateChannel::Portable ? L".zip" : L".msi"));
    const DWORD attributes = GetFileAttributesW(stagedPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return ERROR_FILE_NOT_FOUND;
    }
    std::error_code sizeError;
    if (std::filesystem::file_size(stagedPath, sizeError) != size || sizeError) {
        return ERROR_INVALID_DATA;
    }
    const auto stagedDigest = Sha256File(stagedPath, error);
    if (!stagedDigest || *stagedDigest != sha256) return ERROR_CRC;

    if (channel == UpdateChannel::Portable) {
        PortableApplyHost host(installRoot);
        const auto result = PortableUpdateApply::Apply(
            stagedPath, installRoot, targetVersion, artifact, host);
        return result.success ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    }

    if (!CloseInstallProcesses(installRoot, UpdateChannel::Msi, error)) {
        return ERROR_BUSY;
    }
    const auto installed = QueryInstalledMsiProduct(installRoot, error);
    if (!installed.registered) return ERROR_PRODUCT_UNINSTALLED;
    WindowsInstallerApi installer;
    const auto result = MsiUpdateApply::Apply(
        stagedPath, localVersion, targetVersion, installed, channel, installer);
    return result.success ? ERROR_SUCCESS : result.windowsInstallerCode;
}
