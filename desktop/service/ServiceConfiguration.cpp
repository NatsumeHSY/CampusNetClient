#include "ServiceConfiguration.h"

#include <sddl.h>

#include <array>

#pragma comment(lib, "Advapi32.lib")

namespace {

constexpr wchar_t kServiceName[] = L"CampusNetService";

class LocalDescriptor final {
public:
    ~LocalDescriptor() {
        if (value_ != nullptr) {
            LocalFree(value_);
        }
    }
    PSECURITY_DESCRIPTOR* Put() { return &value_; }
    PSECURITY_DESCRIPTOR Get() const { return value_; }

private:
    PSECURITY_DESCRIPTOR value_ = nullptr;
};

std::wstring FormatError(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = length == 0
        ? L"Windows service configuration error " + std::to_wstring(code)
        : std::wstring(buffer, length);
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message;
}

} // namespace

bool ConfigureCampusNetServiceHandle(SC_HANDLE service, std::wstring& error) {
    error.clear();
    if (service == nullptr) {
        error = L"Invalid CampusNetService handle";
        return false;
    }

    std::array<SC_ACTION, 4> actions{{
        {SC_ACTION_RESTART, 5000},
        {SC_ACTION_RESTART, 30000},
        {SC_ACTION_RESTART, 60000},
        {SC_ACTION_RESTART, 300000},
    }};
    SERVICE_FAILURE_ACTIONSW failureActions{};
    failureActions.dwResetPeriod = INFINITE;
    failureActions.lpRebootMsg = nullptr;
    failureActions.lpCommand = nullptr;
    failureActions.cActions = static_cast<DWORD>(actions.size());
    failureActions.lpsaActions = actions.data();
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS,
                               &failureActions)) {
        error = FormatError(GetLastError());
        return false;
    }

    SERVICE_FAILURE_ACTIONS_FLAG failureFlags{};
    failureFlags.fFailureActionsOnNonCrashFailures = TRUE;
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
                               &failureFlags)) {
        error = FormatError(GetLastError());
        return false;
    }

    LocalDescriptor descriptor;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GR;;;BU)", SDDL_REVISION_1,
            descriptor.Put(), nullptr)) {
        error = FormatError(GetLastError());
        return false;
    }
    if (!SetServiceObjectSecurity(
            service,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            descriptor.Get())) {
        error = FormatError(GetLastError());
        return false;
    }
    return true;
}

bool ConfigureCampusNetService(std::wstring& error) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        error = FormatError(GetLastError());
        return false;
    }
    SC_HANDLE service = OpenServiceW(
        manager, kServiceName,
        SERVICE_CHANGE_CONFIG | WRITE_DAC | READ_CONTROL);
    if (service == nullptr) {
        const DWORD code = GetLastError();
        CloseServiceHandle(manager);
        error = FormatError(code);
        return false;
    }
    const bool configured = ConfigureCampusNetServiceHandle(service, error);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return configured;
}
