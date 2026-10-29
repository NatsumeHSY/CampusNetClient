#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

constexpr std::uint32_t kProtocolVersion = 1;
constexpr std::uint32_t kMaxProtocolMessageBytes = 64 * 1024;
constexpr wchar_t kServicePipeName[] = L"\\\\.\\pipe\\CampusNetClient.v1";

enum class MessageType {
    GetStatus,
    GetConfig,
    UpdateConfig,
    SetCredentials,
    LoginNow,
    CheckInternet,
    GetLogs,
    ServiceCommand,
    CheckForUpdates,
    GetUpdateStatus,
    ShowUpdater,
    ApplyStagedUpdate,
    UpdateAgentStatus,
};

struct ProtocolMessage {
    std::uint32_t version{kProtocolVersion};
    MessageType type{MessageType::GetStatus};
    std::wstring requestId;
    std::string payload;
};

std::wstring MessageTypeName(MessageType type);
std::optional<MessageType> ParseMessageType(std::wstring_view name);

std::string EncodeMessage(const ProtocolMessage& message);
std::optional<ProtocolMessage> DecodeMessage(std::string_view encoded,
                                             std::wstring& error);

std::vector<std::uint8_t> FrameMessage(const ProtocolMessage& message);
std::optional<ProtocolMessage> DecodeFrame(
    const std::vector<std::uint8_t>& frame, std::wstring& error);
