#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

class NetworkMonitor {
public:
    using Callback = std::function<void()>;

    NetworkMonitor() = default;
    ~NetworkMonitor();

    NetworkMonitor(const NetworkMonitor&) = delete;
    NetworkMonitor& operator=(const NetworkMonitor&) = delete;

    bool Start(Callback callback);
    void Stop();

private:
    static void WINAPI InterfaceChanged(void* context, void* row,
                                        int notificationType);
    void Worker();

    Callback callback_;
    void* notificationHandle_{nullptr};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> pending_{false};
    std::mutex mutex_;
    std::condition_variable condition_;
    std::chrono::steady_clock::time_point lastChange_{};
    std::thread worker_;
};
