#include "ClientIpc.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vector>

namespace {

class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
            CloseHandle(value_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }

private:
    HANDLE value_;
};

bool ReadExact(HANDLE pipe, void* buffer, DWORD size) {
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    DWORD offset = 0;
    while (offset < size) {
        DWORD read = 0;
        if (!ReadFile(pipe, bytes + offset, size - offset, &read, nullptr) ||
            read == 0) {
            return false;
        }
        offset += read;
    }
    return true;
}

bool WriteExact(HANDLE pipe, const void* buffer, DWORD size) {
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    DWORD offset = 0;
    while (offset < size) {
        DWORD written = 0;
        if (!WriteFile(pipe, bytes + offset, size - offset, &written, nullptr) ||
            written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

} // namespace

std::optional<ProtocolMessage> ServiceIpcClient::Call(
    const ProtocolMessage& request, std::uint32_t timeoutMs,
    std::wstring& error) const {
    error.clear();
    const auto frame = FrameMessage(request);
    if (frame.empty()) {
        error = L"request cannot be framed";
        return std::nullopt;
    }
    if (!WaitNamedPipeW(kServicePipeName, timeoutMs)) {
        error = L"service pipe is unavailable";
        return std::nullopt;
    }

    Handle pipe(CreateFileW(kServicePipeName, GENERIC_READ | GENERIC_WRITE, 0,
                            nullptr, OPEN_EXISTING, 0, nullptr));
    if (!pipe) {
        error = L"service pipe could not be opened";
        return std::nullopt;
    }
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr);

    if (!WriteExact(pipe.get(), frame.data(), static_cast<DWORD>(frame.size()))) {
        error = L"request could not be written";
        return std::nullopt;
    }

    std::uint8_t prefix[4]{};
    if (!ReadExact(pipe.get(), prefix, sizeof(prefix))) {
        error = L"response length could not be read";
        return std::nullopt;
    }
    const std::uint32_t size =
        static_cast<std::uint32_t>(prefix[0]) |
        (static_cast<std::uint32_t>(prefix[1]) << 8) |
        (static_cast<std::uint32_t>(prefix[2]) << 16) |
        (static_cast<std::uint32_t>(prefix[3]) << 24);
    if (size == 0 || size > kMaxProtocolMessageBytes) {
        error = L"response length is invalid";
        return std::nullopt;
    }

    std::vector<std::uint8_t> response(size + 4);
    std::copy(std::begin(prefix), std::end(prefix), response.begin());
    if (!ReadExact(pipe.get(), response.data() + 4, size)) {
        error = L"response body could not be read";
        return std::nullopt;
    }
    const auto decoded = DecodeFrame(response, error);
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    if (decoded->requestId != request.requestId) {
        error = L"response requestId does not match request";
        return std::nullopt;
    }
    return decoded;
}
