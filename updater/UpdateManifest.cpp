#include "UpdateManifest.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaxManifestBytes = 1024 * 1024;
constexpr std::size_t kMaxJsonDepth = 16;
constexpr std::size_t kMaxJsonNodes = 12000;
constexpr std::size_t kMaxReleaseNotesCharacters = 32 * 1024;
constexpr std::size_t kMaxFilesPerArtifact = 1024;
constexpr std::uint64_t kMaxArtifactBytes = 1024ull * 1024 * 1024;
constexpr std::size_t kMaxSemVerCharacters = 128;

bool EqualsOrdinalIgnoreCase(std::wstring_view left, std::wstring_view right) {
    return left.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
           right.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
           CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) ==
               CSTR_EQUAL;
}

struct WindowsOrdinalCaseInsensitiveLess {
    bool operator()(const std::wstring& left,
                    const std::wstring& right) const {
        return CompareStringOrdinal(
                   left.data(), static_cast<int>(left.size()), right.data(),
                   static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    }
};

enum class JsonKind {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
};

struct JsonValue {
    JsonKind kind = JsonKind::Null;
    bool boolean = false;
    std::wstring text;
    std::vector<JsonValue> array;
    std::map<std::wstring, JsonValue> object;
};

bool IsJsonWhitespace(wchar_t value) {
    return value == L' ' || value == L'\t' || value == L'\r' ||
           value == L'\n';
}

void SetError(std::wstring& error, std::wstring_view message) {
    error.assign(message);
}

class JsonParser final {
public:
    explicit JsonParser(std::wstring_view input) : input_(input) {}

    bool Parse(JsonValue& value, std::wstring& error) {
        SkipWhitespace();
        if (!ParseValue(value, 0)) {
            SetError(error, error_.empty() ? L"Invalid JSON" : error_);
            return false;
        }
        SkipWhitespace();
        if (position_ != input_.size()) {
            SetError(error, L"Unexpected trailing JSON data");
            return false;
        }
        return true;
    }

private:
    bool ParseValue(JsonValue& value, std::size_t depth) {
        if (++nodeCount_ > kMaxJsonNodes) {
            return Fail(L"JSON contains too many values");
        }
        if (depth > kMaxJsonDepth || position_ >= input_.size()) {
            return Fail(L"JSON nesting or input limit exceeded");
        }

        const wchar_t current = input_[position_];
        if (current == L'{') {
            return ParseObject(value, depth);
        }
        if (current == L'[') {
            return ParseArray(value, depth);
        }
        if (current == L'"') {
            value.kind = JsonKind::String;
            return ParseString(value.text);
        }
        if (current == L't' && ConsumeLiteral(L"true")) {
            value.kind = JsonKind::Boolean;
            value.boolean = true;
            return true;
        }
        if (current == L'f' && ConsumeLiteral(L"false")) {
            value.kind = JsonKind::Boolean;
            value.boolean = false;
            return true;
        }
        if (current == L'n' && ConsumeLiteral(L"null")) {
            value.kind = JsonKind::Null;
            return true;
        }
        if (current >= L'0' && current <= L'9') {
            return ParseUnsignedInteger(value);
        }
        return Fail(L"Unsupported or malformed JSON value");
    }

    bool ParseObject(JsonValue& value, std::size_t depth) {
        value.kind = JsonKind::Object;
        ++position_;
        SkipWhitespace();
        if (Consume(L'}')) {
            return true;
        }

        for (;;) {
            if (position_ >= input_.size() || input_[position_] != L'"') {
                return Fail(L"Expected a JSON object key");
            }
            std::wstring key;
            if (!ParseString(key)) {
                return false;
            }
            SkipWhitespace();
            if (!Consume(L':')) {
                return Fail(L"Expected ':' after JSON object key");
            }
            SkipWhitespace();
            JsonValue child;
            if (!ParseValue(child, depth + 1)) {
                return false;
            }
            if (!value.object.emplace(std::move(key), std::move(child)).second) {
                return Fail(L"Duplicate JSON object key");
            }
            SkipWhitespace();
            if (Consume(L'}')) {
                return true;
            }
            if (!Consume(L',')) {
                return Fail(L"Expected ',' or '}' in JSON object");
            }
            SkipWhitespace();
        }
    }

    bool ParseArray(JsonValue& value, std::size_t depth) {
        value.kind = JsonKind::Array;
        ++position_;
        SkipWhitespace();
        if (Consume(L']')) {
            return true;
        }

        for (;;) {
            JsonValue child;
            if (!ParseValue(child, depth + 1)) {
                return false;
            }
            value.array.push_back(std::move(child));
            SkipWhitespace();
            if (Consume(L']')) {
                return true;
            }
            if (!Consume(L',')) {
                return Fail(L"Expected ',' or ']' in JSON array");
            }
            SkipWhitespace();
        }
    }

    bool ParseString(std::wstring& value) {
        if (!Consume(L'"')) {
            return Fail(L"Expected a JSON string");
        }
        value.clear();
        while (position_ < input_.size()) {
            wchar_t current = input_[position_++];
            if (current == L'"') {
                return true;
            }
            if (current < 0x20) {
                return Fail(L"Unescaped control character in JSON string");
            }
            if (current == L'\\') {
                if (position_ >= input_.size()) {
                    return Fail(L"Incomplete JSON escape sequence");
                }
                current = input_[position_++];
                switch (current) {
                case L'"': value.push_back(L'"'); break;
                case L'\\': value.push_back(L'\\'); break;
                case L'/': value.push_back(L'/'); break;
                case L'b': value.push_back(L'\b'); break;
                case L'f': value.push_back(L'\f'); break;
                case L'n': value.push_back(L'\n'); break;
                case L'r': value.push_back(L'\r'); break;
                case L't': value.push_back(L'\t'); break;
                case L'u':
                    if (!AppendUnicodeEscape(value)) {
                        return false;
                    }
                    break;
                default:
                    return Fail(L"Invalid JSON escape sequence");
                }
                continue;
            }
            if (current >= 0xD800 && current <= 0xDBFF) {
                if (position_ >= input_.size() || input_[position_] < 0xDC00 ||
                    input_[position_] > 0xDFFF) {
                    return Fail(L"Unpaired UTF-16 surrogate in JSON string");
                }
                value.push_back(current);
                value.push_back(input_[position_++]);
                continue;
            }
            if (current >= 0xDC00 && current <= 0xDFFF) {
                return Fail(L"Unpaired UTF-16 surrogate in JSON string");
            }
            value.push_back(current);
        }
        return Fail(L"Unterminated JSON string");
    }

    bool AppendUnicodeEscape(std::wstring& value) {
        std::uint16_t first = 0;
        if (!ReadHexCodeUnit(first)) {
            return false;
        }
        if (first >= 0xD800 && first <= 0xDBFF) {
            if (position_ + 2 > input_.size() || input_[position_] != L'\\' ||
                input_[position_ + 1] != L'u') {
                return Fail(L"High surrogate is missing its low surrogate");
            }
            position_ += 2;
            std::uint16_t second = 0;
            if (!ReadHexCodeUnit(second) || second < 0xDC00 || second > 0xDFFF) {
                return Fail(L"Invalid low surrogate in JSON escape");
            }
            value.push_back(static_cast<wchar_t>(first));
            value.push_back(static_cast<wchar_t>(second));
            return true;
        }
        if (first >= 0xDC00 && first <= 0xDFFF) {
            return Fail(L"Unexpected low surrogate in JSON escape");
        }
        value.push_back(static_cast<wchar_t>(first));
        return true;
    }

    bool ReadHexCodeUnit(std::uint16_t& value) {
        if (position_ + 4 > input_.size()) {
            return Fail(L"Incomplete Unicode escape in JSON string");
        }
        std::uint16_t result = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const wchar_t current = input_[position_++];
            result = static_cast<std::uint16_t>(result << 4);
            if (current >= L'0' && current <= L'9') {
                result |= static_cast<std::uint16_t>(current - L'0');
            } else if (current >= L'a' && current <= L'f') {
                result |= static_cast<std::uint16_t>(current - L'a' + 10);
            } else if (current >= L'A' && current <= L'F') {
                result |= static_cast<std::uint16_t>(current - L'A' + 10);
            } else {
                return Fail(L"Invalid hexadecimal Unicode escape");
            }
        }
        value = result;
        return true;
    }

    bool ParseUnsignedInteger(JsonValue& value) {
        const std::size_t start = position_;
        if (input_[position_] == L'0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= L'0' &&
                input_[position_] <= L'9') {
                return Fail(L"JSON integer has a leading zero");
            }
        } else {
            while (position_ < input_.size() && input_[position_] >= L'0' &&
                   input_[position_] <= L'9') {
                ++position_;
            }
        }
        value.kind = JsonKind::Number;
        value.text.assign(input_.substr(start, position_ - start));
        return true;
    }

    bool ConsumeLiteral(std::wstring_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    bool Consume(wchar_t expected) {
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    void SkipWhitespace() {
        while (position_ < input_.size() && IsJsonWhitespace(input_[position_])) {
            ++position_;
        }
    }

    bool Fail(std::wstring_view message) {
        if (error_.empty()) {
            error_.assign(message);
        }
        return false;
    }

    std::wstring_view input_;
    std::size_t position_ = 0;
    std::size_t nodeCount_ = 0;
    std::wstring error_;
};

bool DecodeUtf8(std::string_view input, std::wstring& output,
                std::wstring& error) {
    if (input.size() > kMaxManifestBytes ||
        input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        SetError(error, L"Update manifest exceeds the 1 MiB limit");
        return false;
    }
    if (input.empty()) {
        SetError(error, L"Update manifest is empty");
        return false;
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (required <= 0) {
        SetError(error, L"Update manifest is not valid UTF-8");
        return false;
    }
    output.resize(static_cast<std::size_t>(required));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), output.data(),
                            required) != required) {
        SetError(error, L"Could not decode update manifest UTF-8");
        return false;
    }
    return true;
}

const JsonValue* Find(const JsonValue& object, std::wstring_view key) {
    if (object.kind != JsonKind::Object) {
        return nullptr;
    }
    const auto it = object.object.find(std::wstring(key));
    return it == object.object.end() ? nullptr : &it->second;
}

bool HasExactKeys(const JsonValue& object,
                  std::initializer_list<std::wstring_view> keys) {
    if (object.kind != JsonKind::Object || object.object.size() != keys.size()) {
        return false;
    }
    return std::all_of(keys.begin(), keys.end(), [&](std::wstring_view key) {
        return object.object.contains(std::wstring(key));
    });
}

bool GetString(const JsonValue& object, std::wstring_view key,
               std::wstring& result) {
    const auto* value = Find(object, key);
    if (value == nullptr || value->kind != JsonKind::String) {
        return false;
    }
    result = value->text;
    return true;
}

bool GetUnsigned(const JsonValue& object, std::wstring_view key,
                 std::uint64_t& result) {
    const auto* value = Find(object, key);
    if (value == nullptr || value->kind != JsonKind::Number ||
        value->text.empty()) {
        return false;
    }
    result = 0;
    for (wchar_t digit : value->text) {
        if (digit < L'0' || digit > L'9') {
            return false;
        }
        const unsigned valueDigit = static_cast<unsigned>(digit - L'0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - valueDigit) / 10) {
            return false;
        }
        result = result * 10 + valueDigit;
    }
    return true;
}

bool IsAsciiAlphaNumeric(wchar_t value) {
    return (value >= L'a' && value <= L'z') ||
           (value >= L'A' && value <= L'Z') ||
           (value >= L'0' && value <= L'9');
}

bool IsNumericIdentifier(std::wstring_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](wchar_t ch) {
        return ch >= L'0' && ch <= L'9';
    });
}

bool ValidateIdentifiers(std::wstring_view value, bool prerelease) {
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t end = value.find(L'.', begin);
        const std::wstring_view item = value.substr(
            begin, end == std::wstring_view::npos ? value.size() - begin
                                                  : end - begin);
        if (item.empty() || !std::all_of(item.begin(), item.end(), [](wchar_t ch) {
                return IsAsciiAlphaNumeric(ch) || ch == L'-';
            })) {
            return false;
        }
        if (prerelease && IsNumericIdentifier(item) && item.size() > 1 &&
            item.front() == L'0') {
            return false;
        }
        if (end == std::wstring_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

bool IsCanonicalUtcTimestamp(std::wstring_view value) {
    if (value.size() != 20 || value[4] != L'-' || value[7] != L'-' ||
        value[10] != L'T' || value[13] != L':' || value[16] != L':' ||
        value[19] != L'Z') {
        return false;
    }
    const auto parse = [&](std::size_t offset, std::size_t count,
                           int& result) {
        result = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const wchar_t digit = value[offset + i];
            if (digit < L'0' || digit > L'9') {
                return false;
            }
            result = result * 10 + static_cast<int>(digit - L'0');
        }
        return true;
    };
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!parse(0, 4, year) || !parse(5, 2, month) || !parse(8, 2, day) ||
        !parse(11, 2, hour) || !parse(14, 2, minute) ||
        !parse(17, 2, second)) {
        return false;
    }
    using namespace std::chrono;
    const year_month_day date{std::chrono::year{year},
                              std::chrono::month{static_cast<unsigned>(month)},
                              std::chrono::day{static_cast<unsigned>(day)}};
    return date.ok() && hour <= 23 && minute <= 59 && second <= 59;
}

bool IsLowerHexDigest(std::wstring_view value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](wchar_t ch) {
               return (ch >= L'0' && ch <= L'9') ||
                      (ch >= L'a' && ch <= L'f');
           });
}

bool IsAllowedReleaseUrl(std::wstring_view value) {
    constexpr std::wstring_view prefix = L"https://github.com/";
    constexpr std::wstring_view pathPrefix =
        L"NatsumeHSY/CampusNetClient/releases/download/";
    if (!value.starts_with(prefix) || value.find_first_of(L"?#\\@ \t\r\n") !=
                                          std::wstring_view::npos) {
        return false;
    }
    const auto path = value.substr(prefix.size());
    return path.starts_with(pathPrefix) && path.size() > pathPrefix.size();
}

bool ParseArtifact(const JsonValue& value, UpdateArtifact& artifact,
                   std::wstring& error) {
    if (!HasExactKeys(value, {L"url", L"sizeBytes", L"sha256", L"files"})) {
        SetError(error, L"Artifact has missing or unknown fields");
        return false;
    }
    std::uint64_t size = 0;
    if (!GetString(value, L"url", artifact.url) ||
        !IsAllowedReleaseUrl(artifact.url) ||
        !GetUnsigned(value, L"sizeBytes", size) || size == 0 ||
        size > kMaxArtifactBytes ||
        !GetString(value, L"sha256", artifact.sha256) ||
        !IsLowerHexDigest(artifact.sha256)) {
        SetError(error, L"Artifact URL, size, or SHA-256 is invalid");
        return false;
    }
    artifact.sizeBytes = size;

    const auto* files = Find(value, L"files");
    if (files == nullptr || files->kind != JsonKind::Object ||
        files->object.empty() || files->object.size() > kMaxFilesPerArtifact) {
        SetError(error, L"Artifact file manifest is empty or exceeds its limit");
        return false;
    }
    std::set<std::wstring, WindowsOrdinalCaseInsensitiveLess> normalizedPaths;
    for (const auto& [path, digest] : files->object) {
        if (digest.kind != JsonKind::String ||
            !IsSafeUpdateRelativePath(path) ||
            !IsLowerHexDigest(digest.text)) {
            SetError(error, L"Artifact file path or SHA-256 is invalid");
            return false;
        }
        if (!normalizedPaths.emplace(path).second) {
            SetError(error, L"Artifact contains case-insensitive duplicate paths");
            return false;
        }
        artifact.files.emplace(path, digest.text);
    }
    return true;
}

bool ParseJson(std::string_view bytes, JsonValue& value, std::wstring& error) {
    std::wstring decoded;
    if (!DecodeUtf8(bytes, decoded, error)) {
        return false;
    }
    JsonParser parser(decoded);
    return parser.Parse(value, error);
}

const UpdateArtifact& ArtifactFor(const RemoteUpdateManifest& remote,
                                 UpdateChannel channel) {
    return channel == UpdateChannel::Portable ? remote.portable : remote.msi;
}

} // namespace

bool IsValidSemVer(std::wstring_view version) {
    if (version.empty() || version.size() > kMaxSemVerCharacters) {
        return false;
    }
    const auto plus = version.find(L'+');
    if (plus != std::wstring_view::npos &&
        version.find(L'+', plus + 1) != std::wstring_view::npos) {
        return false;
    }
    const auto dash = version.find(L'-');
    const bool hasPrerelease =
        dash != std::wstring_view::npos &&
        (plus == std::wstring_view::npos || dash < plus);
    const std::size_t coreEnd =
        hasPrerelease ? dash
                      : (plus == std::wstring_view::npos ? version.size() : plus);
    const std::wstring_view core = version.substr(0, coreEnd);
    std::array<std::wstring_view, 3> numbers{};
    std::size_t begin = 0;
    for (std::size_t i = 0; i < numbers.size(); ++i) {
        const std::size_t end = core.find(L'.', begin);
        if ((i < 2 && end == std::wstring_view::npos) ||
            (i == 2 && end != std::wstring_view::npos)) {
            return false;
        }
        numbers[i] = core.substr(
            begin, end == std::wstring_view::npos ? core.size() - begin
                                                  : end - begin);
        if (numbers[i].empty() ||
            !std::all_of(numbers[i].begin(), numbers[i].end(), [](wchar_t ch) {
                return ch >= L'0' && ch <= L'9';
            }) ||
            (numbers[i].size() > 1 && numbers[i].front() == L'0')) {
            return false;
        }
        begin = end == std::wstring_view::npos ? core.size() : end + 1;
    }
    if (plus != std::wstring_view::npos &&
        !ValidateIdentifiers(version.substr(plus + 1), false)) {
        return false;
    }
    if (hasPrerelease) {
        const std::size_t end = plus == std::wstring_view::npos ? version.size() : plus;
        if (!ValidateIdentifiers(version.substr(dash + 1, end - dash - 1), true)) {
            return false;
        }
    }
    return true;
}

bool IsSafeUpdateRelativePath(std::wstring_view path) {
    if (path.empty() || path.size() > 260 || path.front() == L'/' ||
        path.back() == L'/' || path.find_first_of(L"\\:<>\"|?*") !=
                                   std::wstring_view::npos) {
        return false;
    }
    std::size_t begin = 0;
    while (begin < path.size()) {
        const auto end = path.find(L'/', begin);
        const auto component = path.substr(
            begin, end == std::wstring_view::npos ? path.size() - begin
                                                  : end - begin);
        if (component.empty() || component == L"." || component == L".." ||
            component.back() == L'.' || component.back() == L' ' ||
            std::any_of(component.begin(), component.end(), [](wchar_t ch) {
                return ch < 0x20;
            })) {
            return false;
        }
        const auto dot = component.find(L'.');
        const auto stem = component.substr(0, dot);
        if (EqualsOrdinalIgnoreCase(stem, L"con") ||
            EqualsOrdinalIgnoreCase(stem, L"prn") ||
            EqualsOrdinalIgnoreCase(stem, L"aux") ||
            EqualsOrdinalIgnoreCase(stem, L"nul") ||
            EqualsOrdinalIgnoreCase(stem, L"clock$")) {
            return false;
        }
        if (stem.size() == 4 &&
            (EqualsOrdinalIgnoreCase(stem.substr(0, 3), L"com") ||
             EqualsOrdinalIgnoreCase(stem.substr(0, 3), L"lpt")) &&
            stem[3] >= L'1' && stem[3] <= L'9') {
            return false;
        }
        if (end == std::wstring_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

bool ParseRemoteUpdateManifest(std::string_view json,
                               RemoteUpdateManifest& manifest,
                               std::wstring& error) {
    error.clear();
    JsonValue root;
    if (!ParseJson(json, root, error)) {
        return false;
    }
    if (!HasExactKeys(root, {L"schemaVersion", L"version", L"publishedAt",
                             L"releaseNotes", L"artifacts"})) {
        SetError(error, L"Manifest has missing or unknown fields");
        return false;
    }
    std::uint64_t schema = 0;
    if (!GetUnsigned(root, L"schemaVersion", schema) || schema != 1 ||
        !GetString(root, L"version", manifest.version) ||
        !IsValidSemVer(manifest.version) ||
        !GetString(root, L"publishedAt", manifest.publishedAt) ||
        !IsCanonicalUtcTimestamp(manifest.publishedAt) ||
        !GetString(root, L"releaseNotes", manifest.releaseNotes) ||
        manifest.releaseNotes.size() > kMaxReleaseNotesCharacters) {
        SetError(error, L"Manifest schema, version, date, or release notes are invalid");
        return false;
    }
    const auto* artifacts = Find(root, L"artifacts");
    if (artifacts == nullptr ||
        !HasExactKeys(*artifacts, {L"portable", L"msi"})) {
        SetError(error, L"Manifest must contain exactly portable and msi artifacts");
        return false;
    }
    if (!ParseArtifact(*Find(*artifacts, L"portable"), manifest.portable, error) ||
        !ParseArtifact(*Find(*artifacts, L"msi"), manifest.msi, error)) {
        return false;
    }
    manifest.schemaVersion = static_cast<std::uint32_t>(schema);
    return true;
}

bool ParseLocalInstallInfo(std::string_view json, LocalInstallInfo& local,
                           std::wstring& error) {
    error.clear();
    JsonValue root;
    if (!ParseJson(json, root, error)) {
        return false;
    }
    if (!HasExactKeys(root, {L"schemaVersion", L"version", L"channel"})) {
        SetError(error, L"Local install record has missing or unknown fields");
        return false;
    }
    std::uint64_t schema = 0;
    std::wstring channel;
    if (!GetUnsigned(root, L"schemaVersion", schema) || schema != 1 ||
        !GetString(root, L"version", local.version) ||
        !IsValidSemVer(local.version) || !GetString(root, L"channel", channel)) {
        SetError(error, L"Local install record is invalid");
        return false;
    }
    if (channel == L"portable") {
        local.channel = UpdateChannel::Portable;
    } else if (channel == L"msi") {
        local.channel = UpdateChannel::Msi;
    } else {
        SetError(error, L"Local install channel is not supported");
        return false;
    }
    return true;
}

UpdateDecision DecideUpdate(
    const LocalInstallInfo& local, const RemoteUpdateManifest& remote,
    const std::map<std::wstring, std::wstring>& localDigests) {
    if (!IsValidSemVer(local.version) || !IsValidSemVer(remote.version) ||
        (local.channel != UpdateChannel::Portable &&
         local.channel != UpdateChannel::Msi)) {
        return UpdateDecision::Invalid;
    }
    const auto& artifact = ArtifactFor(remote, local.channel);
    if (artifact.files.empty()) {
        return UpdateDecision::Invalid;
    }
    if (local.version != remote.version) {
        return UpdateDecision::InstallRemoteVersion;
    }

    std::map<std::wstring, std::wstring, WindowsOrdinalCaseInsensitiveLess>
        normalizedLocal;
    for (const auto& [path, digest] : localDigests) {
        if (!IsSafeUpdateRelativePath(path) || !IsLowerHexDigest(digest) ||
            !normalizedLocal.emplace(path, digest).second) {
            return UpdateDecision::Invalid;
        }
    }
    for (const auto& [path, digest] : artifact.files) {
        const auto current = normalizedLocal.find(path);
        if (current == normalizedLocal.end() || current->second != digest) {
            return UpdateDecision::RepairFiles;
        }
    }
    return UpdateDecision::UpToDate;
}
