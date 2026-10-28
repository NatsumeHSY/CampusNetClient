#include "UpdateAgentIpc.h"

#include "UpdateIntegrity.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace {

class JsonReader final {
public:
    explicit JsonReader(std::wstring input) : input_(std::move(input)) {}

    bool ReadStatus(UpdateAgentStatus& status, std::wstring& error) {
        error.clear();
        std::array<bool, 10> seen{};
        SkipWhitespace();
        if (!Consume(L'{')) {
            return Fail(error, L"Agent status must be a JSON object");
        }
        SkipWhitespace();
        if (Consume(L'}')) {
            return Fail(error, L"Agent status is missing required fields");
        }
        while (true) {
            std::wstring key;
            if (!ReadString(key)) {
                return Fail(error, L"Agent status contains an invalid property name");
            }
            SkipWhitespace();
            if (!Consume(L':')) {
                return Fail(error, L"Agent status property is missing a colon");
            }
            SkipWhitespace();
            const auto field = FieldIndex(key);
            if (!field || seen[*field]) {
                return Fail(error, L"Agent status has an unknown or duplicate field");
            }
            seen[*field] = true;
            bool parsed = false;
            switch (*field) {
            case 0: {
                std::wstring value;
                parsed = ReadString(value) && ParsePhase(value, status.phase);
                break;
            }
            case 1:
                parsed = ReadString(status.localVersion);
                break;
            case 2:
                parsed = ReadString(status.targetVersion);
                break;
            case 3: {
                std::wstring value;
                parsed = ReadString(value);
                if (parsed && value == L"portable") {
                    status.channel = UpdateChannel::Portable;
                } else if (parsed && value == L"msi") {
                    status.channel = UpdateChannel::Msi;
                } else {
                    parsed = false;
                }
                break;
            }
            case 4:
                parsed = ReadString(status.releaseNotes);
                break;
            case 5:
                parsed = ReadUnsigned(status.receivedBytes);
                break;
            case 6:
                parsed = ReadUnsigned(status.totalBytes);
                break;
            case 7:
                parsed = ReadUnsigned(status.bytesPerSecond);
                break;
            case 8: {
                std::uint64_t value = 0;
                parsed = ReadUnsigned(value) &&
                         value <= std::numeric_limits<std::uint32_t>::max();
                if (parsed) {
                    status.errorCode = static_cast<std::uint32_t>(value);
                }
                break;
            }
            case 9:
                parsed = ReadString(status.artifactSha256);
                break;
            }
            if (!parsed) {
                return Fail(error, L"Agent status field has an invalid value");
            }
            SkipWhitespace();
            if (Consume(L'}')) {
                break;
            }
            if (!Consume(L',')) {
                return Fail(error, L"Agent status object is malformed");
            }
            SkipWhitespace();
        }
        SkipWhitespace();
        if (position_ != input_.size() ||
            !std::all_of(seen.begin(), seen.end(), [](bool value) { return value; })) {
            return Fail(error, L"Agent status has missing fields or trailing data");
        }
        return true;
    }

private:
    static bool ParsePhase(std::wstring_view value, UpdatePhase& phase) {
        if (value == L"idle") phase = UpdatePhase::Idle;
        else if (value == L"checking") phase = UpdatePhase::Checking;
        else if (value == L"upToDate") phase = UpdatePhase::UpToDate;
        else if (value == L"available") phase = UpdatePhase::Available;
        else if (value == L"downloading") phase = UpdatePhase::Downloading;
        else if (value == L"ready") phase = UpdatePhase::Ready;
        else if (value == L"applying") phase = UpdatePhase::Applying;
        else if (value == L"failed") phase = UpdatePhase::Failed;
        else return false;
        return true;
    }

    static std::optional<std::size_t> FieldIndex(std::wstring_view key) {
        static constexpr std::array<std::wstring_view, 10> names{
            L"phase", L"localVersion", L"targetVersion", L"channel",
            L"releaseNotes", L"receivedBytes", L"totalBytes",
            L"bytesPerSecond", L"errorCode", L"artifactSha256"};
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (key == names[index]) {
                return index;
            }
        }
        return std::nullopt;
    }

    void SkipWhitespace() {
        while (position_ < input_.size() &&
               (input_[position_] == L' ' || input_[position_] == L'\t' ||
                input_[position_] == L'\r' || input_[position_] == L'\n')) {
            ++position_;
        }
    }

    bool Consume(wchar_t character) {
        if (position_ >= input_.size() || input_[position_] != character) {
            return false;
        }
        ++position_;
        return true;
    }

    bool ReadString(std::wstring& value) {
        value.clear();
        if (!Consume(L'"')) {
            return false;
        }
        while (position_ < input_.size()) {
            const wchar_t character = input_[position_++];
            if (character == L'"') {
                return true;
            }
            if (character < 0x20) {
                return false;
            }
            if (character != L'\\') {
                value.push_back(character);
                continue;
            }
            if (position_ >= input_.size()) {
                return false;
            }
            switch (input_[position_++]) {
            case L'"': value.push_back(L'"'); break;
            case L'\\': value.push_back(L'\\'); break;
            case L'/': value.push_back(L'/'); break;
            case L'b': value.push_back(L'\b'); break;
            case L'f': value.push_back(L'\f'); break;
            case L'n': value.push_back(L'\n'); break;
            case L'r': value.push_back(L'\r'); break;
            case L't': value.push_back(L'\t'); break;
            case L'u': {
                std::uint16_t first = 0;
                if (!ReadHexCodeUnit(first)) {
                    return false;
                }
                if (first >= 0xd800 && first <= 0xdbff) {
                    if (position_ + 2 > input_.size() ||
                        input_[position_] != L'\\' ||
                        input_[position_ + 1] != L'u') {
                        return false;
                    }
                    position_ += 2;
                    std::uint16_t second = 0;
                    if (!ReadHexCodeUnit(second) || second < 0xdc00 ||
                        second > 0xdfff) {
                        return false;
                    }
                    value.push_back(static_cast<wchar_t>(first));
                    value.push_back(static_cast<wchar_t>(second));
                } else if (first >= 0xdc00 && first <= 0xdfff) {
                    return false;
                } else {
                    value.push_back(static_cast<wchar_t>(first));
                }
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool ReadHexCodeUnit(std::uint16_t& value) {
        if (position_ + 4 > input_.size()) {
            return false;
        }
        value = 0;
        for (unsigned index = 0; index < 4; ++index) {
            const wchar_t character = input_[position_++];
            unsigned digit = 0;
            if (character >= L'0' && character <= L'9') {
                digit = character - L'0';
            } else if (character >= L'a' && character <= L'f') {
                digit = character - L'a' + 10;
            } else if (character >= L'A' && character <= L'F') {
                digit = character - L'A' + 10;
            } else {
                return false;
            }
            value = static_cast<std::uint16_t>((value << 4) | digit);
        }
        return true;
    }

    bool ReadUnsigned(std::uint64_t& value) {
        const std::size_t start = position_;
        while (position_ < input_.size() && input_[position_] >= L'0' &&
               input_[position_] <= L'9') {
            ++position_;
        }
        if (start == position_ ||
            (position_ - start > 1 && input_[start] == L'0')) {
            return false;
        }
        value = 0;
        for (std::size_t index = start; index < position_; ++index) {
            const unsigned digit = input_[index] - L'0';
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                return false;
            }
            value = value * 10 + digit;
        }
        return true;
    }

    static bool Fail(std::wstring& error, std::wstring message) {
        error = std::move(message);
        return false;
    }

    std::wstring input_;
    std::size_t position_ = 0;
};

bool DecodeUtf8(std::string_view input, std::wstring& output) {
    output.clear();
    if (input.empty() || input.size() > kMaxProtocolMessageBytes) {
        return false;
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           input.data(),
                                           static_cast<int>(input.size()),
                                           nullptr, 0);
    if (count <= 0) {
        return false;
    }
    output.resize(static_cast<std::size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                               static_cast<int>(input.size()), output.data(),
                               count) == count;
}

bool IsLowerHexDigest(std::wstring_view digest) {
    return digest.size() == 64 &&
           std::all_of(digest.begin(), digest.end(), [](wchar_t character) {
               return (character >= L'0' && character <= L'9') ||
                      (character >= L'a' && character <= L'f');
           });
}

} // namespace

bool DecodeUpdateAgentStatusPayload(std::string_view payload,
                                    UpdateAgentStatus& status,
                                    std::wstring& error) {
    std::wstring text;
    if (!DecodeUtf8(payload, text)) {
        error = L"Agent status is not bounded valid UTF-8";
        return false;
    }
    JsonReader reader(std::move(text));
    return reader.ReadStatus(status, error);
}

bool IsAuthorizedUpdateAgentContext(const ServiceIpcClientContext& context,
                                    DWORD expectedProcessId) {
    return expectedProcessId != 0 && context.processId == expectedProcessId &&
           context.sessionId == 0 && context.systemUser &&
           context.restrictedToken && context.restrictedCodeSid;
}

UpdateAgentIpc::UpdateAgentIpc(
    UpdaterProcessSupervisor& supervisor, UpdateBroker& broker,
    std::filesystem::path stagingDirectory)
    : supervisor_(supervisor), broker_(broker),
      stagingDirectory_(std::move(stagingDirectory)) {}

ProtocolMessage UpdateAgentIpc::Handle(
    const ProtocolMessage& request, const ServiceIpcClientContext& context) {
    if (request.type != MessageType::UpdateAgentStatus) {
        return ErrorReply(request, ERROR_NOT_SUPPORTED);
    }
    if (!supervisor_.IsExpectedAgentProcess(context.processId) ||
        !IsAuthorizedUpdateAgentContext(context, supervisor_.AgentProcessId())) {
        return ErrorReply(request, ERROR_ACCESS_DENIED);
    }
    UpdateAgentStatus status;
    std::wstring error;
    if (!DecodeUpdateAgentStatusPayload(request.payload, status, error)) {
        return ErrorReply(request, ERROR_INVALID_DATA);
    }
    if (status.phase == UpdatePhase::Ready) {
        TrustedUpdatePlan plan;
        if (!VerifyReadyArtifact(status, plan, error) ||
            !broker_.SetTrustedPlan(plan, error)) {
            UpdateAgentStatus failed = status;
            failed.phase = UpdatePhase::Failed;
            failed.errorCode = ERROR_INVALID_DATA;
            failed.artifactSha256.clear();
            broker_.PublishAgentStatus(failed);
            return ErrorReply(request, ERROR_INVALID_DATA);
        }
    }
    if (!broker_.PublishAgentStatus(status)) {
        return ErrorReply(request, ERROR_INVALID_DATA);
    }
    return {kProtocolVersion, request.type, request.requestId, "{\"ok\":true}"};
}

bool UpdateAgentIpc::VerifyReadyArtifact(const UpdateAgentStatus& status,
                                         TrustedUpdatePlan& plan,
                                         std::wstring& error) const {
    if (!IsLowerHexDigest(status.artifactSha256) || status.totalBytes == 0) {
        error = L"Ready status has no artifact digest or length";
        return false;
    }
    const auto extension = status.channel == UpdateChannel::Portable ? L".zip"
                                                                     : L".msi";
    const auto path = stagingDirectory_ /
                      (status.artifactSha256 + extension);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = L"Ready artifact is missing or is not a regular file";
        return false;
    }
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize != status.totalBytes) {
        error = L"Ready artifact length differs from the reported manifest";
        return false;
    }
    const auto digest = Sha256File(path, error);
    if (!digest || *digest != status.artifactSha256) {
        if (error.empty()) {
            error = L"Ready artifact digest differs from the reported manifest";
        }
        return false;
    }
    plan.localVersion = status.localVersion;
    plan.targetVersion = status.targetVersion;
    plan.channel = status.channel;
    plan.sha256 = status.artifactSha256;
    plan.sizeBytes = status.totalBytes;
    plan.stagedPath = path;
    return true;
}

ProtocolMessage UpdateAgentIpc::ErrorReply(const ProtocolMessage& request,
                                           DWORD errorCode) const {
    return {kProtocolVersion, request.type, request.requestId,
            "{\"errorCode\":" + std::to_string(errorCode) + "}"};
}
