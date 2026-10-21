#include "ServiceIpc.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

#include <vector>

#pragma comment(lib, "Advapi32.lib")

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

std::vector<std::uint8_t> ReadFrame(HANDLE pipe) {
    std::uint8_t prefix[4]{};
    if (!ReadExact(pipe, prefix, sizeof(prefix))) {
        return {};
    }
    const std::uint32_t size =
        static_cast<std::uint32_t>(prefix[0]) |
        (static_cast<std::uint32_t>(prefix[1]) << 8) |
        (static_cast<std::uint32_t>(prefix[2]) << 16) |
        (static_cast<std::uint32_t>(prefix[3]) << 24);
    if (size == 0 || size > kMaxProtocolMessageBytes) {
        return {};
    }
    std::vector<std::uint8_t> frame(size + 4);
    std::copy(std::begin(prefix), std::end(prefix), frame.begin());
    if (!ReadExact(pipe, frame.data() + 4, size)) {
        return {};
    }
    return frame;
}

bool WriteFrame(HANDLE pipe, const std::vector<std::uint8_t>& frame) {
    return !frame.empty() &&
           WriteExact(pipe, frame.data(), static_cast<DWORD>(frame.size()));
}

PSECURITY_DESCRIPTOR MakePipeSecurity() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)", SDDL_REVISION_1,
            &descriptor, nullptr)) {
        return nullptr;
    }
    return descriptor;
}

} // namespace

ServiceIpcServer::~ServiceIpcServer() { Stop(); }

bool ServiceIpcServer::Start(Handler handler) {
    if (!handler || thread_.joinable()) {
        return false;
    }
    handler_ = std::move(handler);
    stopping_.store(false);
    thread_ = std::thread(&ServiceIpcServer::Run, this);
    return true;
}

void ServiceIpcServer::Stop() {
    if (!thread_.joinable()) {
        return;
    }
    stopping_.store(true);
    Handle wake(CreateFileW(kServicePipeName, GENERIC_READ | GENERIC_WRITE, 0,
                            nullptr, OPEN_EXISTING, 0, nullptr));
    thread_.join();
}

void ServiceIpcServer::Run() {
    while (!stopping_.load()) {
        PSECURITY_DESCRIPTOR descriptor = MakePipeSecurity();
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;

        Handle pipe(CreateNamedPipeW(
            kServicePipeName, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
            kMaxProtocolMessageBytes + 4, kMaxProtocolMessageBytes + 4, 1000,
            descriptor == nullptr ? nullptr : &attributes));
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
        if (!pipe) {
            return;
        }
        // Stop() may try to open the pipe before this instance exists. In
        // that case its wake-up attempt cannot reach us, so re-check before
        // entering the blocking ConnectNamedPipe call.
        if (stopping_.load()) {
            return;
        }

        const BOOL connected = ConnectNamedPipe(pipe.get(), nullptr)
                                   ? TRUE
                                   : GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected) {
            continue;
        }
        if (stopping_.load()) {
            return;
        }

        const auto frame = ReadFrame(pipe.get());
        if (frame.empty()) {
            DisconnectNamedPipe(pipe.get());
            continue;
        }
        std::wstring error;
        const auto request = DecodeFrame(frame, error);
        if (!request.has_value()) {
            DisconnectNamedPipe(pipe.get());
            continue;
        }

        const ProtocolMessage response = handler_(*request);
        const auto responseFrame = FrameMessage(response);
        WriteFrame(pipe.get(), responseFrame);
        FlushFileBuffers(pipe.get());
        DisconnectNamedPipe(pipe.get());
    }
}
