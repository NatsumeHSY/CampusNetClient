#include "ServiceManager.h"

#include <utility>

namespace {

class ScHandle {
public:
    explicit ScHandle(SC_HANDLE handle = nullptr) : handle_(handle) {}
    ~ScHandle() {
        if (handle_ != nullptr) {
            CloseServiceHandle(handle_);
        }
    }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    SC_HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    SC_HANDLE handle_;
};

ScHandle OpenManager(DWORD access) {
    return ScHandle(OpenSCManagerW(nullptr, nullptr, access));
}

} // namespace

ManagedServiceState MapServiceState(DWORD state) {
    switch (state) {
    case SERVICE_STOPPED:
        return ManagedServiceState::Stopped;
    case SERVICE_START_PENDING:
        return ManagedServiceState::StartPending;
    case SERVICE_RUNNING:
        return ManagedServiceState::Running;
    case SERVICE_STOP_PENDING:
        return ManagedServiceState::StopPending;
    default:
        return ManagedServiceState::Failed;
    }
}

bool ServiceCommandGate::TryAcquire() {
    bool expected = false;
    return active_.compare_exchange_strong(expected, true);
}

void ServiceCommandGate::Release() { active_.store(false); }

ServiceManager::ServiceManager(std::wstring serviceName)
    : serviceName_(std::move(serviceName)) {}

bool ServiceManager::Install(const std::wstring& binaryPath,
                             const std::wstring& displayName,
                             std::wstring& error) const {
    error.clear();
    const ScHandle manager = OpenManager(SC_MANAGER_CREATE_SERVICE);
    if (!manager) {
        error = LastErrorMessage(GetLastError());
        return false;
    }
    const std::wstring commandLine = L"\"" + binaryPath + L"\"";
    ScHandle service(CreateServiceW(
        manager.get(), serviceName_.c_str(), displayName.c_str(),
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, commandLine.c_str(), nullptr, nullptr, nullptr,
        nullptr, nullptr));
    if (!service) {
        error = LastErrorMessage(GetLastError());
        return false;
    }
    return true;
}

bool ServiceManager::Start(std::wstring& error) const {
    error.clear();
    const ScHandle manager = OpenManager(SC_MANAGER_CONNECT);
    if (!manager) {
        error = LastErrorMessage(GetLastError());
        return false;
    }
    const ScHandle service(OpenServiceW(
        manager.get(), serviceName_.c_str(), SERVICE_START | SERVICE_QUERY_STATUS));
    if (!service) {
        error = LastErrorMessage(GetLastError());
        return false;
    }
    if (StartServiceW(service.get(), 0, nullptr) ||
        GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
        return true;
    }
    error = LastErrorMessage(GetLastError());
    return false;
}

bool ServiceManager::Stop(std::wstring& error) const {
    error.clear();
    const ScHandle manager = OpenManager(SC_MANAGER_CONNECT);
    if (!manager) {
        error = LastErrorMessage(GetLastError());
        return false;
    }
    const ScHandle service(OpenServiceW(
        manager.get(), serviceName_.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!service) {
        error = LastErrorMessage(GetLastError());
        return false;
    }
    SERVICE_STATUS status{};
    if (ControlService(service.get(), SERVICE_CONTROL_STOP, &status) ||
        GetLastError() == ERROR_SERVICE_NOT_ACTIVE) {
        return true;
    }
    error = LastErrorMessage(GetLastError());
    return false;
}

bool ServiceManager::Uninstall(std::wstring& error) const {
    error.clear();
    const ScHandle manager = OpenManager(SC_MANAGER_CONNECT);
    if (!manager) {
        error = LastErrorMessage(GetLastError());
        return false;
    }
    const ScHandle service(OpenServiceW(
        manager.get(), serviceName_.c_str(), DELETE | SERVICE_STOP));
    if (!service) {
        error = LastErrorMessage(GetLastError());
        return false;
    }
    SERVICE_STATUS status{};
    ControlService(service.get(), SERVICE_CONTROL_STOP, &status);
    if (!DeleteService(service.get())) {
        error = LastErrorMessage(GetLastError());
        return false;
    }
    return true;
}

ManagedServiceState ServiceManager::Query(std::wstring& error) const {
    error.clear();
    const ScHandle manager = OpenManager(SC_MANAGER_CONNECT);
    if (!manager) {
        error = LastErrorMessage(GetLastError());
        return ManagedServiceState::Failed;
    }
    const ScHandle service(OpenServiceW(
        manager.get(), serviceName_.c_str(), SERVICE_QUERY_STATUS));
    if (!service) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            return ManagedServiceState::Missing;
        }
        error = LastErrorMessage(GetLastError());
        return ManagedServiceState::Failed;
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD bytesNeeded = 0;
    if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<LPBYTE>(&status),
                              sizeof(status), &bytesNeeded)) {
        error = LastErrorMessage(GetLastError());
        return ManagedServiceState::Failed;
    }
    return MapServiceState(status.dwCurrentState);
}

std::wstring ServiceManager::LastErrorMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = size == 0 ? L"Windows Service Manager error" :
                                       std::wstring(buffer, size);
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message;
}
