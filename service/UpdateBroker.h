#pragma once

#include "Protocol.h"
#include "UpdateAgentStatus.h"
#include "UpdateManifest.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

struct TrustedUpdatePlan {
    std::wstring localVersion;
    std::wstring targetVersion;
    UpdateChannel channel = UpdateChannel::Portable;
    std::wstring sha256;
    std::uint64_t sizeBytes = 0;
    std::filesystem::path stagedPath;
};

struct UpdateBrokerCallbacks {
    std::function<bool()> requestCheck;
    std::function<bool()> hasInteractiveSession;
    std::function<bool()> showUpdater;
    std::function<bool(const TrustedUpdatePlan&)> applyStagedUpdate;
};

class UpdateBroker final {
public:
    UpdateBroker(std::wstring localVersion, UpdateChannel channel,
                 std::filesystem::path stagingDirectory,
                 UpdateBrokerCallbacks callbacks);

    ProtocolMessage Handle(const ProtocolMessage& request,
                           bool authorizedInteractiveClient);
    bool PublishAgentStatus(const UpdateAgentStatus& status);
    bool SetTrustedPlan(const TrustedUpdatePlan& plan, std::wstring& error);
    std::optional<TrustedUpdatePlan> CurrentTrustedPlan() const;
    UpdateAgentStatus Snapshot() const;

private:
    ProtocolMessage Reply(const ProtocolMessage& request) const;
    ProtocolMessage ReplyWithError(const ProtocolMessage& request,
                                   std::uint32_t errorCode) const;
    void SetError(std::uint32_t errorCode, UpdatePhase phase = UpdatePhase::Failed);

    const std::wstring localVersion_;
    const UpdateChannel channel_;
    const std::filesystem::path stagingDirectory_;
    UpdateBrokerCallbacks callbacks_;
    mutable std::mutex mutex_;
    UpdateAgentStatus status_;
    std::optional<TrustedUpdatePlan> trustedPlan_;
    bool checkPending_ = false;
    bool updaterUiLaunched_ = false;
};

