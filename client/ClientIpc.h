#pragma once

#include "Protocol.h"

#include <cstdint>
#include <optional>
#include <string>

class ServiceIpcClient {
public:
    std::optional<ProtocolMessage> Call(const ProtocolMessage& request,
                                        std::uint32_t timeoutMs,
                                        std::wstring& error) const;
};
