#pragma once

#include "Protocol.h"

#include <atomic>
#include <functional>
#include <thread>

class ServiceIpcServer {
public:
    using Handler = std::function<ProtocolMessage(const ProtocolMessage&)>;

    ServiceIpcServer() = default;
    ~ServiceIpcServer();

    ServiceIpcServer(const ServiceIpcServer&) = delete;
    ServiceIpcServer& operator=(const ServiceIpcServer&) = delete;

    bool Start(Handler handler);
    void Stop();

private:
    void Run();

    Handler handler_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};
