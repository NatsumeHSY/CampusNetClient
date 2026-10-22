#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Protocol.h"

#include <Windows.h>

#include <atomic>
#include <functional>
#include <thread>

struct ServiceIpcClientContext {
    DWORD processId = 0;
    DWORD sessionId = 0;
    bool interactiveUser = false;
    bool restrictedToken = false;
    bool systemUser = false;
    bool restrictedCodeSid = false;
};

class ServiceIpcServer {
public:
    using Handler = std::function<ProtocolMessage(
        const ProtocolMessage&, const ServiceIpcClientContext&)>;
    using LegacyHandler =
        std::function<ProtocolMessage(const ProtocolMessage&)>;

    ServiceIpcServer() = default;
    ~ServiceIpcServer();

    ServiceIpcServer(const ServiceIpcServer&) = delete;
    ServiceIpcServer& operator=(const ServiceIpcServer&) = delete;

    bool Start(Handler handler);
    bool Start(LegacyHandler handler);
    void Stop();

private:
    void Run();

    Handler handler_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};
