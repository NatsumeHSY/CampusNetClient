#pragma once

#include "Models.h"

#include <functional>
#include <mutex>
#include <string_view>

class ServiceState {
public:
    explicit ServiceState(bool serviceRunning = true);

    ServiceStatus Snapshot() const;
    void Update(LoginState state, std::wstring_view message);
    void SetUiConnected(bool connected);
    void SetUiAutostart(bool enabled);
    void SetInternetAvailable(bool available);
    void SetInternetStatus(bool available, std::wstring_view publicIp,
                           unsigned statusCode);
    void SetServiceRunning(bool running);

private:
    void Touch();

    mutable std::mutex mutex_;
    ServiceStatus status_;
};
