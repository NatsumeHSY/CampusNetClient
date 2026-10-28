#pragma once

#include "ServiceIpc.h"
#include "UpdateBroker.h"
#include "UpdaterProcessSupervisor.h"

#include <filesystem>
#include <string>
#include <string_view>

bool DecodeUpdateAgentStatusPayload(std::string_view payload,
                                    UpdateAgentStatus& status,
                                    std::wstring& error);
bool IsAuthorizedUpdateAgentContext(const ServiceIpcClientContext& context,
                                    DWORD expectedProcessId);

class UpdateAgentIpc final {
public:
    UpdateAgentIpc(UpdaterProcessSupervisor& supervisor, UpdateBroker& broker,
                   std::filesystem::path stagingDirectory);

    ProtocolMessage Handle(const ProtocolMessage& request,
                           const ServiceIpcClientContext& context);

private:
    bool VerifyReadyArtifact(const UpdateAgentStatus& status,
                             TrustedUpdatePlan& plan,
                             std::wstring& error) const;
    ProtocolMessage ErrorReply(const ProtocolMessage& request,
                               DWORD errorCode) const;

    UpdaterProcessSupervisor& supervisor_;
    UpdateBroker& broker_;
    std::filesystem::path stagingDirectory_;
};
