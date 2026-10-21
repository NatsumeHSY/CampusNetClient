#include "NetworkMonitor.h"

#include <windows.h>
#include <iphlpapi.h>

#include <chrono>

#pragma comment(lib, "Iphlpapi.lib")

NetworkMonitor::~NetworkMonitor() { Stop(); }

bool NetworkMonitor::Start(Callback callback) {
    if (!callback || worker_.joinable()) {
        return false;
    }
    callback_ = std::move(callback);
    stopping_.store(false);
    pending_.store(false);
    const auto result = NotifyIpInterfaceChange(
        AF_UNSPEC,
        reinterpret_cast<PIPINTERFACE_CHANGE_CALLBACK>(
            &NetworkMonitor::InterfaceChanged),
        this,
        FALSE,
        reinterpret_cast<PHANDLE>(&notificationHandle_));
    if (result != NO_ERROR) {
        callback_ = {};
        notificationHandle_ = nullptr;
        return false;
    }
    worker_ = std::thread(&NetworkMonitor::Worker, this);
    return true;
}

void NetworkMonitor::Stop() {
    if (!worker_.joinable()) {
        return;
    }
    stopping_.store(true);
    if (notificationHandle_ != nullptr) {
        CancelMibChangeNotify2(notificationHandle_);
        notificationHandle_ = nullptr;
    }
    condition_.notify_all();
    worker_.join();
    callback_ = {};
}

void WINAPI NetworkMonitor::InterfaceChanged(void* context, void*, int) {
    auto* monitor = static_cast<NetworkMonitor*>(context);
    if (monitor == nullptr || monitor->stopping_.load()) {
        return;
    }
    {
        std::scoped_lock lock(monitor->mutex_);
        monitor->lastChange_ = std::chrono::steady_clock::now();
        monitor->pending_.store(true);
    }
    monitor->condition_.notify_all();
}

void NetworkMonitor::Worker() {
    constexpr auto debounce = std::chrono::milliseconds(700);
    std::unique_lock lock(mutex_);
    while (!stopping_.load()) {
        condition_.wait_for(lock, debounce, [this] {
            return stopping_.load() || pending_.load();
        });
        if (stopping_.load()) {
            break;
        }
        if (!pending_.load() ||
            std::chrono::steady_clock::now() - lastChange_ < debounce) {
            continue;
        }
        pending_.store(false);
        auto callback = callback_;
        lock.unlock();
        if (callback) {
            callback();
        }
        lock.lock();
    }
}
