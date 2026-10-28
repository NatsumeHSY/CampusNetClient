#include "UpdaterProcessSupervisor.h"

#include <sddl.h>
#include <userenv.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Userenv.lib")

namespace {

constexpr wchar_t kAgentRoleName[] = L"CAMPUSNET_UPDATE_ROLE";
constexpr wchar_t kStopEventName[] = L"CAMPUSNET_UPDATE_STOP_EVENT";
constexpr wchar_t kCheckEventName[] = L"CAMPUSNET_UPDATE_CHECK_EVENT";
constexpr wchar_t kStagingDirectoryName[] = L"CAMPUSNET_UPDATE_STAGING";

class ScopedAgentHandle final {
public:
    explicit ScopedAgentHandle(HANDLE value = nullptr) : value_(value) {}
    ~ScopedAgentHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    ScopedAgentHandle(const ScopedAgentHandle&) = delete;
    ScopedAgentHandle& operator=(const ScopedAgentHandle&) = delete;
    HANDLE Get() const { return value_; }
    HANDLE* Put() { return &value_; }
    explicit operator bool() const {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = nullptr;
};

class LocalSid final {
public:
    ~LocalSid() {
        if (sid_ != nullptr) {
            FreeSid(sid_);
        }
    }
    PSID* Put() { return &sid_; }
    PSID Get() const { return sid_; }

private:
    PSID sid_ = nullptr;
};

bool BuildEnvironment(std::wstring_view stopHandle,
                      std::wstring_view checkHandle,
                      std::wstring_view stagingDirectory,
                      std::vector<wchar_t>& block) {
    LPWCH source = GetEnvironmentStringsW();
    if (source == nullptr) {
        return false;
    }
    std::vector<std::wstring> entries;
    for (const wchar_t* current = source; *current != L'\0';
         current += wcslen(current) + 1) {
        const std::wstring_view entry(current);
        const auto equal = entry.find(L'=');
        if (equal == std::wstring_view::npos || equal == 0) {
            entries.emplace_back(entry);
            continue;
        }
        const std::wstring_view name = entry.substr(0, equal);
        if (CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
                                 kAgentRoleName,
                                 static_cast<int>(std::size(kAgentRoleName) - 1),
                                 TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
                                 kStopEventName,
                                 static_cast<int>(std::size(kStopEventName) - 1),
                                 TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
                                 kCheckEventName,
                                 static_cast<int>(std::size(kCheckEventName) - 1),
                                 TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
                                 kStagingDirectoryName,
                                 static_cast<int>(std::size(kStagingDirectoryName) - 1),
                                 TRUE) == CSTR_EQUAL) {
            continue;
        }
        entries.emplace_back(entry);
    }
    FreeEnvironmentStringsW(source);

    entries.emplace_back(std::wstring(kAgentRoleName) + L"=agent");
    entries.emplace_back(std::wstring(kStopEventName) + L"=" +
                         std::wstring(stopHandle));
    entries.emplace_back(std::wstring(kCheckEventName) + L"=" +
                         std::wstring(checkHandle));
    entries.emplace_back(std::wstring(kStagingDirectoryName) + L"=" +
                         std::wstring(stagingDirectory));
    std::sort(entries.begin(), entries.end(), [](const auto& left,
                                                  const auto& right) {
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) ==
               CSTR_LESS_THAN;
    });

    block.clear();
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (entries.empty()) {
        block.push_back(L'\0');
    }
    return true;
}

std::wstring FormatHandle(HANDLE handle) {
    return std::to_wstring(reinterpret_cast<std::uintptr_t>(handle));
}

} // namespace

WindowsUpdaterProcessHost::WindowsUpdaterProcessHost(
    std::filesystem::path stagingDirectory)
    : stagingDirectory_(std::move(stagingDirectory)) {}

WindowsUpdaterProcessHost::~WindowsUpdaterProcessHost() {
    std::scoped_lock lock(mutex_);
    for (const auto& [process, handles] : events_) {
        if (handles.stop != nullptr) {
            CloseHandle(handles.stop);
        }
        if (handles.check != nullptr) {
            CloseHandle(handles.check);
        }
        if (process != nullptr && process != INVALID_HANDLE_VALUE) {
            CloseHandle(process);
        }
    }
}

bool WindowsUpdaterProcessHost::PrepareStagingDirectory(
    std::wstring& error) const {
    if (stagingDirectory_.empty() || !stagingDirectory_.is_absolute()) {
        error = L"Updater staging directory must be absolute";
        return false;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;RC)",
            SDDL_REVISION_1, &descriptor, nullptr)) {
        error = L"Could not construct the updater staging ACL";
        return false;
    }
    const auto secureDirectory = [&](const std::filesystem::path& path) {
        if (!CreateDirectoryW(path.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return false;
        }
        return SetFileSecurityW(
                   path.c_str(),
                   DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                   descriptor) != FALSE;
    };
    const auto root = stagingDirectory_.parent_path();
    const bool secured = !root.empty() && secureDirectory(root) &&
                         secureDirectory(stagingDirectory_);
    LocalFree(descriptor);
    if (!secured) {
        error = L"Could not create or secure the updater staging directories";
        return false;
    }
    return true;
}

bool WindowsUpdaterProcessHost::StartRestrictedAgent(
    const std::filesystem::path& executable, HANDLE& process,
    std::wstring& error) {
    process = nullptr;
    error.clear();
    if (executable.empty() || !executable.is_absolute()) {
        error = L"Updater executable path must be absolute";
        return false;
    }
    const DWORD executableAttributes = GetFileAttributesW(executable.c_str());
    if (executableAttributes == INVALID_FILE_ATTRIBUTES ||
        (executableAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (executableAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = L"Updater executable is missing or is a reparse point";
        return false;
    }
    if (!PrepareStagingDirectory(error)) {
        return false;
    }

    ScopedAgentHandle sourceToken;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY |
                              TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                          sourceToken.Put())) {
        error = L"Could not open the service token for updater isolation";
        return false;
    }
    LocalSid restrictedCode;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&ntAuthority, 1, SECURITY_RESTRICTED_CODE_RID,
                                  0, 0, 0, 0, 0, 0, 0,
                                  restrictedCode.Put())) {
        error = L"Could not create the updater restricted-code SID";
        return false;
    }
    SID_AND_ATTRIBUTES restrictedSids[]{{restrictedCode.Get(), 0}};
    ScopedAgentHandle restrictedToken;
    if (!CreateRestrictedToken(sourceToken.Get(), DISABLE_MAX_PRIVILEGE, 0,
                               nullptr, 0, nullptr, 1, restrictedSids,
                               restrictedToken.Put())) {
        error = L"Could not create the restricted updater token";
        return false;
    }

    SECURITY_ATTRIBUTES eventAttributes{};
    eventAttributes.nLength = sizeof(eventAttributes);
    eventAttributes.bInheritHandle = TRUE;
    ScopedAgentHandle stopEvent(
        CreateEventW(&eventAttributes, TRUE, FALSE, nullptr));
    ScopedAgentHandle checkEvent(
        CreateEventW(&eventAttributes, FALSE, FALSE, nullptr));
    if (!stopEvent || !checkEvent) {
        error = L"Could not create updater control events";
        return false;
    }

    SIZE_T attributeListSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeListSize);
    if (attributeListSize == 0) {
        error = L"Could not size the updater process attributes";
        return false;
    }
    std::vector<std::byte> attributeStorage(attributeListSize);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributeList, 1, 0,
                                           &attributeListSize)) {
        error = L"Could not initialize updater process attributes";
        return false;
    }
    struct AttributeListGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeListGuard() { DeleteProcThreadAttributeList(value); }
    } attributeGuard{attributeList};
    HANDLE inheritedHandles[]{stopEvent.Get(), checkEvent.Get()};
    if (!UpdateProcThreadAttribute(attributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inheritedHandles, sizeof(inheritedHandles),
                                   nullptr, nullptr)) {
        error = L"Could not restrict inherited updater handles";
        return false;
    }

    std::vector<wchar_t> environment;
    if (!BuildEnvironment(FormatHandle(stopEvent.Get()),
                          FormatHandle(checkEvent.Get()),
                          stagingDirectory_.wstring(), environment)) {
        error = L"Could not build the restricted updater environment";
        return false;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION information{};
    std::wstring commandLine = L"\"" + executable.wstring() + L"\"";
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT |
                        CREATE_NO_WINDOW;
    if (!CreateProcessAsUserW(
            restrictedToken.Get(), executable.c_str(), commandLine.data(),
            nullptr, nullptr, TRUE, flags, environment.data(),
            executable.parent_path().c_str(), &startup.StartupInfo,
            &information)) {
        error = L"Could not start the restricted updater agent";
        return false;
    }
    CloseHandle(information.hThread);

    {
        std::scoped_lock lock(mutex_);
        events_.emplace(information.hProcess,
                        AgentEvents{stopEvent.Get(), checkEvent.Get()});
    }
    // Ownership of these two handles is now shared with the child and retained
    // by this host until Release().
    *stopEvent.Put() = nullptr;
    *checkEvent.Put() = nullptr;
    process = information.hProcess;
    return true;
}

void WindowsUpdaterProcessHost::RequestStop(HANDLE process) {
    std::scoped_lock lock(mutex_);
    const auto it = events_.find(process);
    if (it != events_.end()) {
        SetEvent(it->second.stop);
    }
}

bool WindowsUpdaterProcessHost::WaitForExit(
    HANDLE process, std::chrono::milliseconds timeout) {
    const auto bounded = std::clamp<std::int64_t>(timeout.count(), 0, INFINITE - 1);
    return WaitForSingleObject(process, static_cast<DWORD>(bounded)) == WAIT_OBJECT_0;
}

bool WindowsUpdaterProcessHost::IsRunning(HANDLE process) {
    DWORD exitCode = 0;
    return GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
}

void WindowsUpdaterProcessHost::Terminate(HANDLE process) {
    TerminateProcess(process, ERROR_PROCESS_ABORTED);
}

void WindowsUpdaterProcessHost::Release(HANDLE process) {
    std::scoped_lock lock(mutex_);
    const auto it = events_.find(process);
    if (it != events_.end()) {
        if (it->second.stop != nullptr) {
            CloseHandle(it->second.stop);
        }
        if (it->second.check != nullptr) {
            CloseHandle(it->second.check);
        }
        events_.erase(it);
    }
    if (process != nullptr && process != INVALID_HANDLE_VALUE) {
        CloseHandle(process);
    }
}

DWORD WindowsUpdaterProcessHost::ProcessId(HANDLE process) {
    return GetProcessId(process);
}

bool WindowsUpdaterProcessHost::RequestCheck(HANDLE process) {
    std::scoped_lock lock(mutex_);
    const auto it = events_.find(process);
    return it != events_.end() && SetEvent(it->second.check) != FALSE;
}

UpdaterProcessSupervisor::UpdaterProcessSupervisor(
    IUpdaterProcessHost& host, std::filesystem::path executable,
    std::chrono::milliseconds monitorInterval,
    std::chrono::milliseconds stopTimeout, NowFunction now)
    : host_(host), executable_(std::move(executable)),
      monitorInterval_(std::max(monitorInterval,
                                std::chrono::milliseconds(1))),
      stopTimeout_(std::max(stopTimeout, std::chrono::milliseconds(1))),
      now_(std::move(now)) {}

UpdaterProcessSupervisor::~UpdaterProcessSupervisor() { Stop(); }

bool UpdaterProcessSupervisor::Start() {
    {
        std::scoped_lock lock(mutex_);
        if (started_) {
            return false;
        }
        started_ = true;
        stopping_ = false;
        failureCount_ = 0;
        retryAfter_ = {};
    }
    EnsureRunning();
    try {
        monitorThread_ = std::thread(&UpdaterProcessSupervisor::MonitorLoop, this);
    } catch (...) {
        Stop();
        return false;
    }
    return true;
}

void UpdaterProcessSupervisor::Stop() {
    {
        std::scoped_lock lock(mutex_);
        if (!started_) {
            return;
        }
        stopping_ = true;
    }
    condition_.notify_all();
    if (monitorThread_.joinable()) {
        monitorThread_.join();
    }

    HANDLE process = nullptr;
    {
        std::scoped_lock lock(mutex_);
        process = std::exchange(agentProcess_, nullptr);
        started_ = false;
    }
    if (process != nullptr) {
        host_.RequestStop(process);
        if (!host_.WaitForExit(process, stopTimeout_)) {
            host_.Terminate(process);
            host_.WaitForExit(process, stopTimeout_);
        }
        host_.Release(process);
    }
}

bool UpdaterProcessSupervisor::EnsureRunning() {
    std::scoped_lock lock(mutex_);
    if (!started_ || stopping_) {
        return false;
    }
    if (agentProcess_ != nullptr) {
        if (host_.IsRunning(agentProcess_)) {
            return true;
        }
        host_.Release(agentProcess_);
        agentProcess_ = nullptr;
        failureCount_ = std::max(failureCount_, 1u);
        retryAfter_ = now_() + RestartDelay(failureCount_);
        return false;
    }
    if (now_() < retryAfter_) {
        return false;
    }

    HANDLE process = nullptr;
    std::wstring error;
    if (!host_.StartRestrictedAgent(executable_, process, error) ||
        process == nullptr) {
        ++failureCount_;
        retryAfter_ = now_() + RestartDelay(failureCount_);
        return false;
    }
    agentProcess_ = process;
    failureCount_ = 0;
    retryAfter_ = {};
    return true;
}

bool UpdaterProcessSupervisor::RequestCheck() {
    if (EnsureRunning()) {
        const HANDLE process = AgentProcessHandle();
        return process != nullptr && host_.RequestCheck(process);
    }
    return false;
}

DWORD UpdaterProcessSupervisor::AgentProcessId() const {
    const HANDLE process = AgentProcessHandle();
    return process == nullptr ? 0 : host_.ProcessId(process);
}

bool UpdaterProcessSupervisor::IsExpectedAgentProcess(DWORD processId) const {
    return processId != 0 && processId == AgentProcessId();
}

HANDLE UpdaterProcessSupervisor::AgentProcessHandle() const {
    std::scoped_lock lock(mutex_);
    return agentProcess_;
}

void UpdaterProcessSupervisor::MonitorLoop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        condition_.wait_for(lock, monitorInterval_, [this] { return stopping_; });
        if (stopping_) {
            break;
        }
        lock.unlock();
        EnsureRunning();
        lock.lock();
    }
}

std::chrono::milliseconds RestartDelay(unsigned failureCount) {
    if (failureCount <= 1) {
        return std::chrono::seconds(1);
    }
    const unsigned exponent = std::min(failureCount - 1, 8u);
    const std::uint64_t seconds = std::min<std::uint64_t>(
        std::uint64_t{1} << exponent, 300);
    return std::chrono::seconds(seconds);
}
