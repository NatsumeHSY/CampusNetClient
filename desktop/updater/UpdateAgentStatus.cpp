#include "UpdateAgentStatus.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace {

std::wstring EscapeJson(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        switch (character) {
        case L'"': result += L"\\\""; break;
        case L'\\': result += L"\\\\"; break;
        case L'\b': result += L"\\b"; break;
        case L'\f': result += L"\\f"; break;
        case L'\n': result += L"\\n"; break;
        case L'\r': result += L"\\r"; break;
        case L'\t': result += L"\\t"; break;
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

std::wstring ChannelName(UpdateChannel channel) {
    return channel == UpdateChannel::Portable ? L"portable" : L"msi";
}

std::string ToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                         value.data(),
                                         static_cast<int>(value.size()), nullptr,
                                         0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size,
                            nullptr, nullptr) != size) {
        return {};
    }
    return result;
}

class StatusJsonReader final {
public:
    StatusJsonReader(std::wstring text, bool includeDigest)
        : text_(std::move(text)), includeDigest_(includeDigest) {}

    bool Read(UpdateAgentStatus& status, std::wstring& error) {
        error.clear();
        SkipWhitespace();
        if (!Consume(L'{')) return Fail(error, L"Status must be an object");
        std::array<bool, 10> seen{};
        SkipWhitespace();
        if (!Consume(L'}')) {
            while (true) {
                std::wstring key;
                if (!ReadString(key)) return Fail(error, L"Invalid property name");
                SkipWhitespace();
                if (!Consume(L':')) return Fail(error, L"Missing property colon");
                SkipWhitespace();
                const auto field = FieldIndex(key);
                if (!field || (*field == 9 && !includeDigest_) || seen[*field]) {
                    return Fail(error, L"Unknown or duplicate status field");
                }
                seen[*field] = true;
                bool parsed = false;
                switch (*field) {
                case 0: {
                    std::wstring value;
                    parsed = ReadString(value) && ParsePhase(value, status.phase);
                    break;
                }
                case 1: parsed = ReadString(status.localVersion); break;
                case 2: parsed = ReadString(status.targetVersion); break;
                case 3: {
                    std::wstring value;
                    parsed = ReadString(value);
                    if (parsed && value == L"portable") status.channel = UpdateChannel::Portable;
                    else if (parsed && value == L"msi") status.channel = UpdateChannel::Msi;
                    else parsed = false;
                    break;
                }
                case 4: parsed = ReadString(status.releaseNotes); break;
                case 5: parsed = ReadUnsigned(status.receivedBytes); break;
                case 6: parsed = ReadUnsigned(status.totalBytes); break;
                case 7: parsed = ReadUnsigned(status.bytesPerSecond); break;
                case 8: {
                    std::uint64_t value = 0;
                    parsed = ReadUnsigned(value) &&
                             value <= std::numeric_limits<std::uint32_t>::max();
                    if (parsed) status.errorCode = static_cast<std::uint32_t>(value);
                    break;
                }
                case 9: parsed = ReadString(status.artifactSha256); break;
                }
                if (!parsed) return Fail(error, L"Invalid status field value");
                SkipWhitespace();
                if (Consume(L'}')) break;
                if (!Consume(L',')) return Fail(error, L"Malformed status object");
                SkipWhitespace();
            }
        }
        SkipWhitespace();
        const std::size_t required = includeDigest_ ? 10 : 9;
        if (position_ != text_.size() ||
            !std::all_of(seen.begin(), seen.begin() + required,
                         [](bool value) { return value; })) {
            return Fail(error, L"Missing status field or trailing data");
        }
        return true;
    }

private:
    static std::optional<std::size_t> FieldIndex(std::wstring_view key) {
        static constexpr std::array<std::wstring_view, 10> fields{
            L"phase", L"localVersion", L"targetVersion", L"channel",
            L"releaseNotes", L"receivedBytes", L"totalBytes",
            L"bytesPerSecond", L"errorCode", L"artifactSha256"};
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (key == fields[i]) return i;
        }
        return std::nullopt;
    }

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

    void SkipWhitespace() {
        while (position_ < text_.size() &&
               (text_[position_] == L' ' || text_[position_] == L'\t' ||
                text_[position_] == L'\r' || text_[position_] == L'\n')) ++position_;
    }

    bool Consume(wchar_t value) {
        if (position_ >= text_.size() || text_[position_] != value) return false;
        ++position_;
        return true;
    }

    bool ReadString(std::wstring& value) {
        value.clear();
        if (!Consume(L'"')) return false;
        while (position_ < text_.size()) {
            const wchar_t ch = text_[position_++];
            if (ch == L'"') return true;
            if (ch < 0x20) return false;
            if (ch != L'\\') { value.push_back(ch); continue; }
            if (position_ >= text_.size()) return false;
            switch (text_[position_++]) {
            case L'"': value.push_back(L'"'); break;
            case L'\\': value.push_back(L'\\'); break;
            case L'/': value.push_back(L'/'); break;
            case L'b': value.push_back(L'\b'); break;
            case L'f': value.push_back(L'\f'); break;
            case L'n': value.push_back(L'\n'); break;
            case L'r': value.push_back(L'\r'); break;
            case L't': value.push_back(L'\t'); break;
            case L'u': {
                std::uint16_t unit = 0;
                if (!ReadHexUnit(unit) || (unit >= 0xdc00 && unit <= 0xdfff)) return false;
                if (unit >= 0xd800 && unit <= 0xdbff) {
                    if (position_ + 2 > text_.size() || text_[position_] != L'\\' ||
                        text_[position_ + 1] != L'u') return false;
                    position_ += 2;
                    std::uint16_t low = 0;
                    if (!ReadHexUnit(low) || low < 0xdc00 || low > 0xdfff) return false;
                    value.push_back(static_cast<wchar_t>(unit));
                    value.push_back(static_cast<wchar_t>(low));
                } else value.push_back(static_cast<wchar_t>(unit));
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool ReadHexUnit(std::uint16_t& value) {
        if (position_ + 4 > text_.size()) return false;
        value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const wchar_t ch = text_[position_++];
            unsigned digit = 0;
            if (ch >= L'0' && ch <= L'9') digit = ch - L'0';
            else if (ch >= L'a' && ch <= L'f') digit = ch - L'a' + 10;
            else if (ch >= L'A' && ch <= L'F') digit = ch - L'A' + 10;
            else return false;
            value = static_cast<std::uint16_t>((value << 4) | digit);
        }
        return true;
    }

    bool ReadUnsigned(std::uint64_t& value) {
        const std::size_t start = position_;
        while (position_ < text_.size() && text_[position_] >= L'0' &&
               text_[position_] <= L'9') ++position_;
        if (position_ == start || (position_ - start > 1 && text_[start] == L'0')) return false;
        value = 0;
        for (std::size_t i = start; i < position_; ++i) {
            const unsigned digit = text_[i] - L'0';
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
            value = value * 10 + digit;
        }
        return true;
    }

    static bool Fail(std::wstring& error, std::wstring message) {
        error = std::move(message);
        return false;
    }

    std::wstring text_;
    std::size_t position_ = 0;
    bool includeDigest_ = false;
};

bool DecodeUtf8(std::string_view input, std::wstring& text) {
    if (input.empty() || input.size() > 64 * 1024) return false;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) return false;
    text.resize(static_cast<std::size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), text.data(), count) == count;
}

} // namespace

std::wstring UpdatePhaseName(UpdatePhase phase) {
    switch (phase) {
    case UpdatePhase::Idle: return L"idle";
    case UpdatePhase::Checking: return L"checking";
    case UpdatePhase::UpToDate: return L"upToDate";
    case UpdatePhase::Available: return L"available";
    case UpdatePhase::Downloading: return L"downloading";
    case UpdatePhase::Ready: return L"ready";
    case UpdatePhase::Applying: return L"applying";
    case UpdatePhase::Failed: return L"failed";
    }
    return L"failed";
}

std::string EncodeUpdateAgentStatus(const UpdateAgentStatus& status) {
    const std::wstring json =
        L"{\"phase\":\"" + UpdatePhaseName(status.phase) +
        L"\",\"localVersion\":\"" + EscapeJson(status.localVersion) +
        L"\",\"targetVersion\":\"" + EscapeJson(status.targetVersion) +
        L"\",\"channel\":\"" + ChannelName(status.channel) +
        L"\",\"releaseNotes\":\"" + EscapeJson(status.releaseNotes) +
        L"\",\"receivedBytes\":" + std::to_wstring(status.receivedBytes) +
        L",\"totalBytes\":" + std::to_wstring(status.totalBytes) +
        L",\"bytesPerSecond\":" + std::to_wstring(status.bytesPerSecond) +
        L",\"errorCode\":" + std::to_wstring(status.errorCode) +
        L",\"artifactSha256\":\"" + EscapeJson(status.artifactSha256) + L"\"}";
    return ToUtf8(json);
}

bool DecodeUpdateStatusPayload(std::string_view payload,
                               UpdateAgentStatus& status,
                               bool includeArtifactSha256,
                               std::wstring& error) {
    std::wstring text;
    if (!DecodeUtf8(payload, text)) {
        error = L"Status is not bounded valid UTF-8";
        return false;
    }
    return StatusJsonReader(std::move(text), includeArtifactSha256).Read(status, error);
}
