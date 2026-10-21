#include "ClientIpc.h"
#include "Protocol.h"
#include "ServiceIpc.h"

#include <cassert>
#include <chrono>
#include <thread>

int RunProtocolTests() {
    const ProtocolMessage original{
        kProtocolVersion,
        MessageType::GetStatus,
        L"request-42",
        R"({"includeLogs":false})"};

    const std::string encoded = EncodeMessage(original);
    std::wstring error;
    const auto decoded = DecodeMessage(encoded, error);
    assert(decoded.has_value());
    assert(decoded->version == original.version);
    assert(decoded->type == original.type);
    assert(decoded->requestId == original.requestId);
    assert(decoded->payload == original.payload);

    const auto framed = FrameMessage(original);
    assert(framed.size() > 4);
    const auto unframed = DecodeFrame(framed, error);
    assert(unframed.has_value());
    assert(unframed->requestId == original.requestId);

    assert(!DecodeMessage(
                R"({"type":"GetStatus","requestId":"r","payload":{}})",
                error)
                .has_value());
    assert(!DecodeMessage(
                R"({"version":999,"type":"GetStatus","requestId":"r","payload":{}})",
                error)
                .has_value());
    assert(!DecodeMessage(
                R"({"version":1,"type":"Unknown","requestId":"r","payload":{}})",
                error)
                .has_value());
    assert(!DecodeMessage(
                R"({"version":1,"type":"GetStatus","requestId":"r","payload":})",
                error)
                .has_value());

    ServiceIpcServer server;
    assert(server.Start([](const ProtocolMessage& request) {
        return ProtocolMessage{kProtocolVersion, MessageType::GetStatus,
                               request.requestId, R"({"ok":true})"};
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ServiceIpcClient client;
    const auto response = client.Call(original, 1500, error);
    assert(response.has_value());
    assert(response->requestId == original.requestId);
    assert(response->payload == R"({"ok":true})");
    server.Stop();

    for (int i = 0; i < 128; ++i) {
        ServiceIpcServer startupRaceServer;
        assert(startupRaceServer.Start([](const ProtocolMessage& request) {
            return ProtocolMessage{kProtocolVersion, MessageType::GetStatus,
                                   request.requestId, R"({"ok":true})"};
        }));
        startupRaceServer.Stop();
    }
    return 0;
}
