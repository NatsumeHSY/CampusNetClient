#include "UpdateApplyProcessHost.h"

#include "UpdateIntegrity.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <sddl.h>

#include <algorithm>
#include <cwchar>
#include <vector>

#pragma comment(lib, "Advapi32.lib")

namespace {

constexpr wchar_t kApplyRole[] = L"CAMPUSNET_UPDATE_ROLE";
constexpr wchar_t kLocalVersion[] = L"CAMPUSNET_UPDATE_LOCAL_VERSION";
constexpr wchar_t kTargetVersion[] = L"CAMPUSNET_UPDATE_TARGET_VERSION";
constexpr wchar_t kChannel[] = L"CAMPUSNET_UPDATE_CHANNEL";
constexpr wchar_t kStaging[] = L"CAMPUSNET_UPDATE_STAGING";
constexpr wchar_t kInstallRoot[] = L"CAMPUSNET_UPDATE_INSTALL_ROOT";
constexpr wchar_t kArtifactSha[] = L"CAMPUSNET_UPDATE_ARTIFACT_SHA256";
constexpr wchar_t kArtifactSize[] = L"CAMPUSNET_UPDATE_ARTIFACT_SIZE";

class LocalDescriptor final {
public:
    ~LocalDescriptor() { if (value_ != nullptr) LocalFree(value_); }
    PSECURITY_DESCRIPTOR* Put() { return &value_; }
    PSECURITY_DESCRIPTOR Get() const { return value_; }
private:
    PSECURITY_DESCRIPTOR value_ = nullptr;
};

bool EnsureDirectory(const std::filesystem::path& path,
                     std::wstring_view sddl,
                     std::wstring& error) {
    LocalDescriptor descriptor;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            std::wstring(sddl).c_str(), SDDL_REVISION_1,
            descriptor.Put(), nullptr)) {
        error = L"Could not create an updater directory security descriptor";
        return false;
    }
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor.Get();
    if (!CreateDirectoryW(path.c_str(), &security) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        error = L"Could not create updater working directory";
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !SetFileSecurityW(path.c_str(),
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            descriptor.Get())) {
        error = L"Updater working directory has an unsafe type or ACL";
        return false;
    }
    return true;
}

bool BuildEnvironment(const std::vector<std::pair<std::wstring, std::wstring>>& values,
                      std::vector<wchar_t>& block) {
    LPWCH raw = GetEnvironmentStringsW();
    if (raw == nullptr) return false;
    std::vector<std::wstring> entries;
    for (const wchar_t* current = raw; *current != L'\0';
         current += wcslen(current) + 1) {
        entries.emplace_back(current);
    }
    FreeEnvironmentStringsW(raw);
    for (const auto& [name, value] : values) {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [&](const std::wstring& entry) {
                const auto equals = entry.find(L'=');
                return equals != std::wstring::npos &&
                    CompareStringOrdinal(entry.data(), static_cast<int>(equals),
                        name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
            }), entries.end());
        entries.push_back(name + L"=" + value);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) ==
               CSTR_LESS_THAN;
    });
    block.clear();
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (entries.empty()) block.push_back(L'\0');
    return true;
}

bool CopyToHashPath(const std::filesystem::path& source,
                    const std::filesystem::path& directory,
                    std::filesystem::path& copied,
                    std::wstring& error) {
    const DWORD attributes = GetFileAttributesW(source.c_str());
    if (source.empty() || attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        error = L"Installed updater source is missing or unsafe";
        return false;
    }
    const auto sourceHash = Sha256File(source, error);
    if (!sourceHash) return false;
    copied = directory / (*sourceHash + L".exe");
    const DWORD existing = GetFileAttributesW(copied.c_str());
    if (existing != INVALID_FILE_ATTRIBUTES) {
        const auto existingHash = Sha256File(copied, error);
        if (existingHash && *existingHash == *sourceHash) return true;
        error = L"Existing updater copy does not match its digest-derived name";
        return false;
    }
    if (!CopyFileW(source.c_str(), copied.c_str(), TRUE)) {
        error = L"Could not copy updater into its protected working directory";
        return false;
    }
    const auto copiedHash = Sha256File(copied, error);
    if (!copiedHash || *copiedHash != *sourceHash) {
        DeleteFileW(copied.c_str());
        if (error.empty()) error = L"Copied updater failed SHA-256 verification";
        return false;
    }
    return true;
}

} // namespace

WindowsUpdateApplyProcessHost::WindowsUpdateApplyProcessHost(
    std::filesystem::path programDataRoot,
    std::filesystem::path installedUpdater,
    std::filesystem::path stagingDirectory)
    : programDataRoot_(std::move(programDataRoot)),
      installedUpdater_(std::move(installedUpdater)),
      stagingDirectory_(std::move(stagingDirectory)) {}

bool WindowsUpdateApplyProcessHost::PrepareInteractiveGui(
    std::filesystem::path& copiedUpdater, std::wstring& error) const {
    const auto directory = programDataRoot_ / L"UpdateGui";
    constexpr std::wstring_view kGuiAcl =
        L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;RX;;;BU)";
    if (!EnsureDirectory(programDataRoot_, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", error) ||
        !EnsureDirectory(directory, kGuiAcl, error)) return false;
    return CopyToHashPath(installedUpdater_, directory, copiedUpdater, error);
}

bool WindowsUpdateApplyProcessHost::LaunchApply(
    const TrustedUpdatePlan& plan,
    const std::filesystem::path& installRoot,
    std::filesystem::path& copiedUpdater,
    std::wstring& error) const {
    error.clear();
    const auto directory = programDataRoot_ / L"UpdateApply";
    if (!EnsureDirectory(programDataRoot_, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", error) ||
        !EnsureDirectory(directory, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", error) ||
        !CopyToHashPath(installedUpdater_, directory, copiedUpdater, error)) return false;

    std::vector<std::pair<std::wstring, std::wstring>> variables{
        {kApplyRole, L"apply"},
        {kLocalVersion, plan.localVersion},
        {kTargetVersion, plan.targetVersion},
        {kChannel, plan.channel == UpdateChannel::Portable ? L"portable" : L"msi"},
        {kStaging, stagingDirectory_.wstring()},
        {kInstallRoot, installRoot.wstring()},
        {kArtifactSha, plan.sha256},
        {kArtifactSize, std::to_wstring(plan.sizeBytes)},
    };
    std::vector<wchar_t> environment;
    if (!BuildEnvironment(variables, environment)) {
        error = L"Could not build the trusted updater apply environment";
        return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + copiedUpdater.wstring() + L"\"";
    const BOOL created = CreateProcessW(
        copiedUpdater.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, environment.data(),
        directory.c_str(), &startup, &process);
    if (!created) {
        error = L"Could not start the protected update apply worker";
        return false;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, 500);
    if (wait == WAIT_OBJECT_0) {
        DWORD exitCode = ERROR_GEN_FAILURE;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        error = L"Update apply worker exited during initialization with code " +
                std::to_wstring(exitCode);
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
