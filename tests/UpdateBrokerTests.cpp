#include "UpdateBroker.h"
#include "UpdateAgentIpc.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

ProtocolMessage Request(MessageType type, std::string payload = "{}") {
    return {kProtocolVersion, type, L"update-test", std::move(payload)};
}

class Actions final {
public:
    UpdateBrokerCallbacks Callbacks() {
        return {
            [this] {
                ++checkCalls;
                return checkSucceeds;
            },
            [this] {
                ++sessionChecks;
                return hasInteractiveSession;
            },
            [this] {
                ++showCalls;
                return true;
            },
            [this](const TrustedUpdatePlan& plan) {
                ++applyCalls;
                appliedPlan = plan;
                return applySucceeds;
            },
        };
    }

    unsigned checkCalls = 0;
    unsigned sessionChecks = 0;
    unsigned showCalls = 0;
    unsigned applyCalls = 0;
    bool checkSucceeds = true;
    bool hasInteractiveSession = true;
    bool applySucceeds = true;
    TrustedUpdatePlan appliedPlan;
};

TrustedUpdatePlan ValidPlan(const std::filesystem::path& staging) {
    TrustedUpdatePlan plan;
    plan.localVersion = L"0.1.0";
    plan.targetVersion = L"1.2.3";
    plan.channel = UpdateChannel::Portable;
    plan.sha256 =
        L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    plan.sizeBytes = 3;
    plan.stagedPath = staging / (plan.sha256 + L".zip");
    return plan;
}

} // namespace

int RunUpdateBrokerTests() {
    {
        UpdateAgentStatus expected;
        expected.phase = UpdatePhase::Downloading;
        expected.localVersion = L"0.1.0";
        expected.targetVersion = L"1.2.3";
        expected.channel = UpdateChannel::Portable;
        expected.releaseNotes = L"\u4fee\u590d\n\u5f15\u53f7 \" \u4e0e\u659c\u6760 \\\\ \u7ed3\u675f";
        expected.receivedBytes = 12;
        expected.totalBytes = 30;
        expected.bytesPerSecond = 42;
        expected.errorCode = 0;
        expected.artifactSha256 = std::wstring(64, L'a');
        const auto encoded = EncodeUpdateAgentStatus(expected);
        UpdateAgentStatus decoded;
        std::wstring parseError;
        assert(DecodeUpdateAgentStatusPayload(encoded, decoded, parseError));
        assert(decoded.releaseNotes == expected.releaseNotes);
        assert(decoded.receivedBytes == expected.receivedBytes);
        assert(decoded.artifactSha256 == expected.artifactSha256);
        assert(!DecodeUpdateAgentStatusPayload(
            R"({"phase":"idle","phase":"idle"})", decoded, parseError));
        assert(!DecodeUpdateAgentStatusPayload(
            R"({"unexpected":true})", decoded, parseError));
        const std::string brokerStatus =
            R"({"phase":"downloading","localVersion":"0.1.0","targetVersion":"1.2.3","channel":"portable","releaseNotes":"line 1\nline 2","receivedBytes":12,"totalBytes":30,"bytesPerSecond":42,"errorCode":0})";
        assert(DecodeUpdateStatusPayload(brokerStatus, decoded, false,
                                         parseError));
        assert(decoded.phase == UpdatePhase::Downloading);
        assert(decoded.releaseNotes == L"line 1\nline 2");
        assert(decoded.receivedBytes == 12 && decoded.totalBytes == 30);
        assert(!DecodeUpdateStatusPayload(brokerStatus, decoded, true,
                                          parseError));
        assert(!DecodeUpdateStatusPayload(
            R"({"phase":"idle","phase":"idle"})", decoded, false,
            parseError));
    }
    {
        ServiceIpcClientContext context;
        context.processId = 44;
        context.sessionId = 0;
        context.restrictedToken = true;
        context.systemUser = true;
        context.restrictedCodeSid = true;
        assert(IsAuthorizedUpdateAgentContext(context, 44));
        assert(!IsAuthorizedUpdateAgentContext(context, 45));
        context.sessionId = 1;
        assert(!IsAuthorizedUpdateAgentContext(context, 44));
    }
    {
        Actions autoActions;
        autoActions.hasInteractiveSession = false;
        UpdateBroker autoBroker(L"0.1.0", UpdateChannel::Portable,
                                L"C:\\ProgramData\\CampusNet\\updates",
                                autoActions.Callbacks());
        UpdateAgentStatus status;
        status.phase = UpdatePhase::Available;
        status.localVersion = L"0.1.0";
        status.targetVersion = L"0.0.9";
        status.channel = UpdateChannel::Portable;
        assert(autoBroker.PublishAgentStatus(status));
        assert(autoActions.showCalls == 0);
        autoActions.hasInteractiveSession = true;
        status.phase = UpdatePhase::Downloading;
        status.totalBytes = 10;
        status.receivedBytes = 2;
        assert(autoBroker.PublishAgentStatus(status));
        assert(autoActions.showCalls == 1);
        status.receivedBytes = 5;
        assert(autoBroker.PublishAgentStatus(status));
        assert(autoActions.showCalls == 1);
    }

    const std::filesystem::path staging = L"C:\\ProgramData\\CampusNet\\updates";
    Actions actions;
    UpdateBroker broker(L"0.1.0", UpdateChannel::Portable, staging,
                        actions.Callbacks());

    auto reply = broker.Handle(Request(MessageType::CheckForUpdates), false);
    assert(reply.payload.find("\"errorCode\":5") != std::string::npos);
    assert(actions.checkCalls == 0);

    reply = broker.Handle(Request(MessageType::GetConfig), true);
    assert(reply.payload.find("\"errorCode\":50") != std::string::npos);

    reply = broker.Handle(Request(MessageType::CheckForUpdates,
                                  R"({"path":"C:\\Windows\\system32"})"), true);
    assert(reply.payload.find("\"errorCode\":87") != std::string::npos);
    assert(actions.checkCalls == 0);

    assert(broker.Handle(Request(MessageType::CheckForUpdates), true).payload.find(
               "\"phase\":\"checking\"") != std::string::npos);
    assert(broker.Handle(Request(MessageType::CheckForUpdates), true).payload.find(
               "\"phase\":\"checking\"") != std::string::npos);
    assert(actions.checkCalls == 1);

    UpdateAgentStatus status;
    status.phase = UpdatePhase::UpToDate;
    status.localVersion = L"0.1.0";
    status.targetVersion = L"0.1.0";
    status.channel = UpdateChannel::Portable;
    assert(broker.PublishAgentStatus(status));
    broker.Handle(Request(MessageType::CheckForUpdates), true);
    assert(actions.checkCalls == 2);

    actions.hasInteractiveSession = false;
    broker.Handle(Request(MessageType::ShowUpdater), true);
    assert(actions.sessionChecks == 1);
    assert(actions.showCalls == 0);
    actions.hasInteractiveSession = true;
    broker.Handle(Request(MessageType::ShowUpdater), true);
    assert(actions.showCalls == 1);

    std::wstring error;
    TrustedUpdatePlan plan = ValidPlan(staging);
    auto invalidPlan = plan;
    invalidPlan.channel = UpdateChannel::Msi;
    assert(!broker.SetTrustedPlan(invalidPlan, error));
    invalidPlan = plan;
    invalidPlan.localVersion = L"9.9.9";
    assert(!broker.SetTrustedPlan(invalidPlan, error));
    invalidPlan = plan;
    invalidPlan.targetVersion = L"not-semver";
    assert(!broker.SetTrustedPlan(invalidPlan, error));
    invalidPlan = plan;
    invalidPlan.sha256 = std::wstring(64, L'G');
    assert(!broker.SetTrustedPlan(invalidPlan, error));
    invalidPlan = plan;
    invalidPlan.stagedPath = L"C:\\Windows\\system32\\payload.zip";
    assert(!broker.SetTrustedPlan(invalidPlan, error));
    if (!broker.SetTrustedPlan(plan, error)) {
        std::wcerr << L"Trusted plan rejection: " << error << L"\n";
        assert(false);
    }

    status.phase = UpdatePhase::Ready;
    status.localVersion = L"0.1.0";
    status.targetVersion = L"1.2.3";
    status.channel = UpdateChannel::Portable;
    status.artifactSha256 = plan.sha256;
    status.totalBytes = plan.sizeBytes;
    status.receivedBytes = plan.sizeBytes;
    assert(broker.PublishAgentStatus(status));

    reply = broker.Handle(Request(MessageType::ApplyStagedUpdate,
                                  R"({"path":"C:\\Windows\\system32\\evil.msi"})"), true);
    assert(reply.payload.find("\"errorCode\":87") != std::string::npos);
    assert(actions.applyCalls == 0);
    broker.Handle(Request(MessageType::ApplyStagedUpdate), true);
    assert(actions.applyCalls == 1);
    assert(actions.appliedPlan.stagedPath == plan.stagedPath);
    assert(actions.appliedPlan.sha256 == plan.sha256);
    assert(actions.appliedPlan.targetVersion == plan.targetVersion);

    status.targetVersion = L"2.0.0";
    assert(!broker.PublishAgentStatus(status));

    return 0;
}
