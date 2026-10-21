#include "Protocol.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <limits>

namespace {

std::string ToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring JsonEscape(std::wstring_view value) {
    std::wstring result;
    for (const wchar_t character : value) {
        switch (character) {
        case L'"':
            result += L"\\\"";
            break;
        case L'\\':
            result += L"\\\\";
            break;
        case L'\b':
            result += L"\\b";
            break;
        case L'\f':
            result += L"\\f";
            break;
        case L'\n':
            result += L"\\n";
            break;
        case L'\r':
            result += L"\\r";
            break;
        case L'\t':
            result += L"\\t";
            break;
        default:
            if (character < 0x20) {
                constexpr wchar_t hex[] = L"0123456789abcdef";
                result += L"\\u00";
                result.push_back(hex[(character >> 4) & 0x0f]);
                result.push_back(hex[character & 0x0f]);
            } else {
                result.push_back(character);
            }
            break;
        }
    }
    return result;
}

void SkipWhitespace(std::wstring_view json, std::size_t& position) {
    while (position < json.size() && std::iswspace(json[position])) {
        ++position;
    }
}

bool ParseJsonString(std::wstring_view json, std::size_t& position,
                     std::wstring& result) {
    if (position >= json.size() || json[position] != L'"') {
        return false;
    }
    ++position;
    result.clear();
    while (position < json.size()) {
        const wchar_t character = json[position++];
        if (character == L'"') {
            return true;
        }
        if (character != L'\\') {
            if (character < 0x20) {
                return false;
            }
            result.push_back(character);
            continue;
        }
        if (position >= json.size()) {
            return false;
        }
        switch (json[position++]) {
        case L'"':
            result.push_back(L'"');
            break;
        case L'\\':
            result.push_back(L'\\');
            break;
        case L'/':
            result.push_back(L'/');
            break;
        case L'b':
            result.push_back(L'\b');
            break;
        case L'f':
            result.push_back(L'\f');
            break;
        case L'n':
            result.push_back(L'\n');
            break;
        case L'r':
            result.push_back(L'\r');
            break;
        case L't':
            result.push_back(L'\t');
            break;
        case L'u': {
            if (position + 4 > json.size()) {
                return false;
            }
            unsigned value = 0;
            for (std::size_t i = 0; i < 4; ++i) {
                const wchar_t digit = json[position++];
                value <<= 4;
                if (digit >= L'0' && digit <= L'9') {
                    value += static_cast<unsigned>(digit - L'0');
                } else if (digit >= L'a' && digit <= L'f') {
                    value += static_cast<unsigned>(digit - L'a' + 10);
                } else if (digit >= L'A' && digit <= L'F') {
                    value += static_cast<unsigned>(digit - L'A' + 10);
                } else {
                    return false;
                }
            }
            result.push_back(static_cast<wchar_t>(value));
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

bool CaptureJsonValue(std::wstring_view json, std::size_t& position,
                      std::wstring& raw) {
    const std::size_t start = position;
    if (position >= json.size()) {
        return false;
    }

    if (json[position] == L'"') {
        std::wstring ignored;
        if (!ParseJsonString(json, position, ignored)) {
            return false;
        }
        raw = std::wstring(json.substr(start, position - start));
        return true;
    }

    if (json[position] == L'{' || json[position] == L'[') {
        const wchar_t opener = json[position];
        const wchar_t closer = opener == L'{' ? L'}' : L']';
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        for (; position < json.size(); ++position) {
            const wchar_t character = json[position];
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (character == L'\\') {
                    escaped = true;
                } else if (character == L'"') {
                    inString = false;
                }
                continue;
            }
            if (character == L'"') {
                inString = true;
            } else if (character == opener) {
                ++depth;
            } else if (character == closer && --depth == 0) {
                ++position;
                raw = std::wstring(json.substr(start, position - start));
                return true;
            } else if (character == L'}' || character == L']') {
                return false;
            }
        }
        return false;
    }

    while (position < json.size() && json[position] != L',' &&
           json[position] != L'}' && json[position] != L']' &&
           !std::iswspace(json[position])) {
        ++position;
    }
    if (position == start) {
        return false;
    }
    raw = std::wstring(json.substr(start, position - start));
    return raw == L"true" || raw == L"false" || raw == L"null" ||
           std::all_of(raw.begin(), raw.end(), [](wchar_t c) {
               return (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' ||
                      c == L'+' || c == L'e' || c == L'E';
           });
}

bool ParseVersion(std::wstring_view value, std::uint32_t& version) {
    if (value.empty() ||
        !std::all_of(value.begin(), value.end(),
                     [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const wchar_t character : value) {
        parsed = parsed * 10 + static_cast<unsigned>(character - L'0');
        if (parsed > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
    }
    version = static_cast<std::uint32_t>(parsed);
    return true;
}

} // namespace

std::wstring MessageTypeName(MessageType type) {
    switch (type) {
    case MessageType::GetStatus:
        return L"GetStatus";
    case MessageType::GetConfig:
        return L"GetConfig";
    case MessageType::UpdateConfig:
        return L"UpdateConfig";
    case MessageType::SetCredentials:
        return L"SetCredentials";
    case MessageType::LoginNow:
        return L"LoginNow";
    case MessageType::CheckInternet:
        return L"CheckInternet";
    case MessageType::BrowserLoginRequest:
        return L"BrowserLoginRequest";
    case MessageType::BrowserLoginResult:
        return L"BrowserLoginResult";
    case MessageType::SetUiAutostart:
        return L"SetUiAutostart";
    case MessageType::GetLogs:
        return L"GetLogs";
    case MessageType::ServiceCommand:
        return L"ServiceCommand";
    }
    return {};
}

std::optional<MessageType> ParseMessageType(std::wstring_view name) {
    constexpr MessageType values[] = {
        MessageType::GetStatus,          MessageType::GetConfig,
        MessageType::UpdateConfig,       MessageType::SetCredentials,
        MessageType::LoginNow,           MessageType::CheckInternet,
        MessageType::BrowserLoginRequest, MessageType::BrowserLoginResult,
        MessageType::SetUiAutostart,     MessageType::GetLogs,
        MessageType::ServiceCommand,
    };
    for (const auto value : values) {
        if (MessageTypeName(value) == name) {
            return value;
        }
    }
    return std::nullopt;
}

std::string EncodeMessage(const ProtocolMessage& message) {
    const std::wstring json =
        L"{\"version\":" + std::to_wstring(message.version) +
        L",\"type\":\"" + JsonEscape(MessageTypeName(message.type)) +
        L"\",\"requestId\":\"" + JsonEscape(message.requestId) +
        L"\",\"payload\":" + FromUtf8(message.payload) + L"}";
    return ToUtf8(json);
}

std::optional<ProtocolMessage> DecodeMessage(std::string_view encoded,
                                             std::wstring& error) {
    error.clear();
    if (encoded.size() > kMaxProtocolMessageBytes) {
        error = L"message exceeds maximum size";
        return std::nullopt;
    }
    const std::wstring json = FromUtf8(encoded);
    if (encoded.size() != 0 && json.empty()) {
        error = L"message is not valid UTF-8";
        return std::nullopt;
    }

    ProtocolMessage message;
    bool hasVersion = false;
    bool hasType = false;
    bool hasRequestId = false;
    bool hasPayload = false;
    std::size_t position = 0;
    SkipWhitespace(json, position);
    if (position >= json.size() || json[position++] != L'{') {
        error = L"message must be a JSON object";
        return std::nullopt;
    }

    while (true) {
        SkipWhitespace(json, position);
        if (position < json.size() && json[position] == L'}') {
            ++position;
            break;
        }
        std::wstring key;
        if (!ParseJsonString(json, position, key)) {
            error = L"invalid JSON property name";
            return std::nullopt;
        }
        SkipWhitespace(json, position);
        if (position >= json.size() || json[position++] != L':') {
            error = L"missing JSON property separator";
            return std::nullopt;
        }
        SkipWhitespace(json, position);
        const std::size_t valueStart = position;
        std::wstring raw;
        if (!CaptureJsonValue(json, position, raw)) {
            error = L"invalid JSON property value";
            return std::nullopt;
        }

        if (key == L"version") {
            if (hasVersion || !ParseVersion(raw, message.version)) {
                error = L"invalid protocol version";
                return std::nullopt;
            }
            hasVersion = true;
        } else if (key == L"type") {
            if (hasType || raw.size() < 2) {
                error = L"invalid message type";
                return std::nullopt;
            }
            std::size_t stringPosition = valueStart;
            std::wstring typeName;
            if (!ParseJsonString(json, stringPosition, typeName)) {
                error = L"message type must be a string";
                return std::nullopt;
            }
            const auto type = ParseMessageType(typeName);
            if (!type.has_value()) {
                error = L"unknown message type";
                return std::nullopt;
            }
            message.type = *type;
            hasType = true;
        } else if (key == L"requestId") {
            if (hasRequestId) {
                error = L"duplicate request id";
                return std::nullopt;
            }
            std::size_t stringPosition = valueStart;
            if (!ParseJsonString(json, stringPosition, message.requestId) ||
                message.requestId.empty()) {
                error = L"requestId must be a non-empty string";
                return std::nullopt;
            }
            hasRequestId = true;
        } else if (key == L"payload") {
            if (hasPayload) {
                error = L"duplicate payload";
                return std::nullopt;
            }
            message.payload = ToUtf8(raw);
            hasPayload = true;
        }

        SkipWhitespace(json, position);
        if (position >= json.size()) {
            error = L"truncated JSON object";
            return std::nullopt;
        }
        if (json[position] == L',') {
            ++position;
            continue;
        }
        if (json[position] == L'}') {
            ++position;
            break;
        }
        error = L"invalid JSON object separator";
        return std::nullopt;
    }

    SkipWhitespace(json, position);
    if (position != json.size()) {
        error = L"trailing data after message";
        return std::nullopt;
    }
    if (!hasVersion || !hasType || !hasRequestId || !hasPayload) {
        error = L"message is missing a required field";
        return std::nullopt;
    }
    if (message.version != kProtocolVersion) {
        error = L"incompatible protocol version";
        return std::nullopt;
    }
    return message;
}

std::vector<std::uint8_t> FrameMessage(const ProtocolMessage& message) {
    const std::string encoded = EncodeMessage(message);
    if (encoded.empty() || encoded.size() > kMaxProtocolMessageBytes ||
        encoded.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }
    const auto size = static_cast<std::uint32_t>(encoded.size());
    std::vector<std::uint8_t> frame(4 + encoded.size());
    frame[0] = static_cast<std::uint8_t>(size & 0xff);
    frame[1] = static_cast<std::uint8_t>((size >> 8) & 0xff);
    frame[2] = static_cast<std::uint8_t>((size >> 16) & 0xff);
    frame[3] = static_cast<std::uint8_t>((size >> 24) & 0xff);
    std::copy(encoded.begin(), encoded.end(), frame.begin() + 4);
    return frame;
}

std::optional<ProtocolMessage> DecodeFrame(
    const std::vector<std::uint8_t>& frame, std::wstring& error) {
    error.clear();
    if (frame.size() < 4) {
        error = L"frame is shorter than its length prefix";
        return std::nullopt;
    }
    const std::uint32_t size =
        static_cast<std::uint32_t>(frame[0]) |
        (static_cast<std::uint32_t>(frame[1]) << 8) |
        (static_cast<std::uint32_t>(frame[2]) << 16) |
        (static_cast<std::uint32_t>(frame[3]) << 24);
    if (size > kMaxProtocolMessageBytes || frame.size() != size + 4) {
        error = L"invalid frame length";
        return std::nullopt;
    }
    const std::string encoded(reinterpret_cast<const char*>(frame.data() + 4),
                               size);
    return DecodeMessage(encoded, error);
}
