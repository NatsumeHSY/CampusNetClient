#pragma once

#include "../service/UpdateAgentIpc.h"
#include "../service/UpdateApplyProcessHost.h"
#include "../service/UpdateBroker.h"
#include "../service/UpdaterProcessSupervisor.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

class StandaloneUpdateRuntime final {
public:
    explicit StandaloneUpdateRuntime(std::filesystem::path updaterDirectory);
    ~StandaloneUpdateRuntime();

    StandaloneUpdateRuntime(const StandaloneUpdateRuntime&) = delete;
    StandaloneUpdateRuntime& operator=(const StandaloneUpdateRuntime&) = delete;

    bool Start(std::wstring& error);
    void Stop();

private:
    bool RequestAgentCheck();
    bool ShowUpdater();
    bool ApplyStagedUpdate(const TrustedUpdatePlan& plan);
    ProtocolMessage Handle(const ProtocolMessage& request,
                           const ServiceIpcClientContext& client);

    std::filesystem::path updaterDirectory_;
    std::filesystem::path updaterExecutable_;
    std::filesystem::path installRoot_;
    std::filesystem::path stagingDirectory_;
    LocalInstallInfo localInstallInfo_;
    WindowsUpdaterProcessHost updaterProcessHost_;
    UpdaterProcessSupervisor updaterSupervisor_;
    WindowsUpdateApplyProcessHost updateApplyProcessHost_;
    ServiceIpcServer ipc_;
    std::unique_ptr<UpdateBroker> broker_;
    std::unique_ptr<UpdateAgentIpc> agentIpc_;
    std::mutex mutex_;
    bool started_ = false;
    bool agentStarted_ = false;
};
