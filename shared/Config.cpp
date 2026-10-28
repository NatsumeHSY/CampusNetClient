#include "Config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fstream>
#include <sstream>

namespace {

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string EscapeJson(const std::wstring& value) {
    std::string output;
    for (const wchar_t ch : value) {
        switch (ch) {
        case L'\\': output += "\\\\"; break;
        case L'"': output += "\\\""; break;
        case L'\n': output += "\\n"; break;
        case L'\r': output += "\\r"; break;
        case L'\t': output += "\\t"; break;
        default: output += ToUtf8(std::wstring(1, ch)); break;
        }
    }
    return output;
}

bool FindJsonString(const std::string& json, const std::string& key, std::wstring& value) {
    const std::string marker = "\"" + key + "\"";
    const size_t markerPos = json.find(marker);
    if (markerPos == std::string::npos) {
        return false;
    }
    size_t pos = json.find(':', markerPos + marker.size());
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return false;
    }
    ++pos;
    std::string raw;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (escaped) {
            switch (ch) {
            case 'n': raw.push_back('\n'); break;
            case 'r': raw.push_back('\r'); break;
            case 't': raw.push_back('\t'); break;
            default: raw.push_back(ch); break;
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            value = FromUtf8(raw);
            return true;
        } else {
            raw.push_back(ch);
        }
    }
    return false;
}

bool FindJsonBool(const std::string& json, const std::string& key, bool& value) {
    const std::string marker = "\"" + key + "\"";
    const size_t markerPos = json.find(marker);
    if (markerPos == std::string::npos) {
        return false;
    }
    size_t pos = json.find(':', markerPos + marker.size());
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (json.compare(pos, 4, "true") == 0) {
        value = true;
        return true;
    }
    if (json.compare(pos, 5, "false") == 0) {
        value = false;
        return true;
    }
    return false;
}

bool FindJsonInt(const std::string& json, const std::string& key, int& value) {
    const std::string marker = "\"" + key + "\"";
    const size_t markerPos = json.find(marker);
    if (markerPos == std::string::npos) {
        return false;
    }
    size_t pos = json.find(':', markerPos + marker.size());
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    try {
        size_t consumed = 0;
        const int parsed = std::stoi(json.substr(pos), &consumed);
        if (consumed == 0) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

AppConfig LoadDefaultConfig() {
    return {};
}

AppConfig LoadConfig(const std::filesystem::path& path) {
    AppConfig config = LoadDefaultConfig();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return config;
    }
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

    std::wstring stringValue;
    if (FindJsonString(json, "carrier", stringValue)) config.carrier = stringValue;
    if (FindJsonString(json, "gateway", stringValue)) config.gateway = stringValue;
    if (FindJsonString(json, "portalUrl", stringValue)) config.portalUrl = stringValue;
    if (config.gateway == L"1.1.1.1") {
        config.gateway = L"10.10.12.13";
    }
    const bool legacyPortal = config.portalUrl == L"http://1.1.1.1/";
    if (legacyPortal) {
        config.portalUrl = L"http://10.10.12.13/";
    }
    FindJsonInt(json, "httpTimeoutMs", config.httpTimeoutMs);
    FindJsonInt(json, "verifyTimeoutMs", config.verifyTimeoutMs);
    FindJsonInt(json, "maxLoginAttempts", config.maxLoginAttempts);
    if (legacyPortal && config.maxLoginAttempts == 2) {
        config.maxLoginAttempts = 1;
    }
    FindJsonInt(json, "retryDelayMs", config.retryDelayMs);
    FindJsonInt(json, "verificationAttempts", config.verificationAttempts);
    FindJsonInt(json, "verifyIntervalMs", config.verifyIntervalMs);
    FindJsonBool(json, "uiAutostart", config.uiAutostart);
    FindJsonBool(json, "showWebView", config.showWebView);
    FindJsonBool(json, "autoLogout", config.autoLogout);

    std::wstring error;
    if (!ValidateConfig(config, error)) {
        return LoadDefaultConfig();
    }
    return config;
}

bool SaveConfig(const std::filesystem::path& path, const AppConfig& config) {
    std::wstring error;
    if (!ValidateConfig(config, error)) {
        return false;
    }
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return false;
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output << "{\n"
           << "  \"carrier\": \"" << EscapeJson(config.carrier) << "\",\n"
           << "  \"gateway\": \"" << EscapeJson(config.gateway) << "\",\n"
           << "  \"portalUrl\": \"" << EscapeJson(config.portalUrl) << "\",\n"
           << "  \"httpTimeoutMs\": " << config.httpTimeoutMs << ",\n"
           << "  \"verifyTimeoutMs\": " << config.verifyTimeoutMs << ",\n"
           << "  \"maxLoginAttempts\": " << config.maxLoginAttempts << ",\n"
           << "  \"retryDelayMs\": " << config.retryDelayMs << ",\n"
           << "  \"verificationAttempts\": " << config.verificationAttempts << ",\n"
           << "  \"verifyIntervalMs\": " << config.verifyIntervalMs << ",\n"
           << "  \"uiAutostart\": " << (config.uiAutostart ? "true" : "false") << ",\n"
           << "  \"showWebView\": " << (config.showWebView ? "true" : "false") << ",\n"
           << "  \"autoLogout\": " << (config.autoLogout ? "true" : "false") << "\n"
           << "}\n";
    return output.good();
}

bool ValidateConfig(const AppConfig& config, std::wstring& error) {
    if (config.carrier != L"telecom" && config.carrier != L"mobile") {
        error = L"unsupported carrier";
        return false;
    }
    if (config.gateway.empty()) {
        error = L"gateway cannot be empty";
        return false;
    }
    if (config.portalUrl.empty()) {
        error = L"portalUrl cannot be empty";
        return false;
    }
    if (config.httpTimeoutMs <= 0 || config.verifyTimeoutMs <= 0) {
        error = L"timeouts must be positive";
        return false;
    }
    if (config.maxLoginAttempts <= 0 || config.verificationAttempts <= 0) {
        error = L"attempt counts must be positive";
        return false;
    }
    if (config.retryDelayMs < 0 || config.verifyIntervalMs < 0) {
        error = L"retry intervals cannot be negative";
        return false;
    }
    if (config.internetChecks.empty()) {
        error = L"at least one internet check is required";
        return false;
    }
    error.clear();
    return true;
}

std::filesystem::path ConfigPath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::filesystem::current_path() / L"config.json";
    }
    return std::filesystem::path(buffer).parent_path() / L"config.json";
}

std::wstring LoginPortalUrl(const AppConfig& config) {
    return config.carrier == L"mobile" ? L"http://1.1.1.1/" : config.portalUrl;
}
