#include "Config.h"
#include "Protocol.h"
#include "ServiceManager.h"

#include <cassert>

int RunEndToEndLocalTests() {
    // The local end-to-end harness deliberately avoids mutating the machine's
    // SCM database. These assertions cover the same integration contracts the
    // GUI installer uses before a manual first-run installation.
    assert(MapServiceState(SERVICE_STOPPED) == ManagedServiceState::Stopped);
    assert(MapServiceState(SERVICE_RUNNING) == ManagedServiceState::Running);
    assert(MapServiceState(SERVICE_START_PENDING) ==
           ManagedServiceState::StartPending);

    ServiceCommandGate gate;
    assert(gate.TryAcquire());
    assert(!gate.TryAcquire());
    gate.Release();
    assert(gate.TryAcquire());
    gate.Release();

    AppConfig config = LoadDefaultConfig();

    std::wstring error;
    const ProtocolMessage request{kProtocolVersion, MessageType::GetStatus,
                                  L"e2e-local", "{}"};
    const auto frame = FrameMessage(request);
    const auto decoded = DecodeFrame(frame, error);
    assert(decoded.has_value());
    assert(decoded->requestId == request.requestId);
    assert(decoded->type == request.type);

    return 0;
}

