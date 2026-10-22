#include "ServiceState.h"

#include <chrono>

namespace {

std::int64_t UnixMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

ServiceState::ServiceState(bool serviceRunning) {
    status_.serviceRunning = serviceRunning;
    status_.state = LoginState::Idle;
    status_.lastChangeUnixMs = UnixMilliseconds();
}

ServiceStatus ServiceState::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return status_;
}

void ServiceState::Update(LoginState state, std::wstring_view message) {
    std::scoped_lock lock(mutex_);
    status_.state = state;
    status_.lastMessage = message;
    Touch();
}

void ServiceState::SetUiConnected(bool connected) {
    std::scoped_lock lock(mutex_);
    status_.uiConnected = connected;
    Touch();
}

void ServiceState::SetUiAutostart(bool enabled) {
    std::scoped_lock lock(mutex_);
    status_.uiAutostart = enabled;
    Touch();
}

void ServiceState::SetInternetAvailable(bool available) {
    std::scoped_lock lock(mutex_);
    status_.internetAvailable = available;
    status_.internetStatusKnown = true;
    status_.internetStatusCode = 0;
    if (!available) {
        status_.publicIp.clear();
    }
    Touch();
}

void ServiceState::SetInternetStatus(bool available,
                                     std::wstring_view publicIp,
                                     unsigned statusCode) {
    std::scoped_lock lock(mutex_);
    if (status_.internetStatusKnown &&
        status_.internetAvailable == available &&
        status_.internetStatusCode == statusCode &&
        status_.publicIp == publicIp) {
        return;
    }
    status_.internetStatusKnown = true;
    status_.internetAvailable = available;
    status_.internetStatusCode = statusCode;
    status_.publicIp = available ? publicIp : std::wstring{};
    Touch();
}

void ServiceState::SetServiceRunning(bool running) {
    std::scoped_lock lock(mutex_);
    status_.serviceRunning = running;
    Touch();
}

void ServiceState::Touch() { status_.lastChangeUnixMs = UnixMilliseconds(); }
