#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct InternetCheckEndpoint {
    std::wstring url;
    unsigned expectedStatus{200};
};

struct AppConfig {
    std::wstring gateway{L"1.1.1.1"};
    std::wstring portalUrl{L"http://1.1.1.1/"};
    std::vector<InternetCheckEndpoint> internetChecks{
        {L"http://connect.rom.miui.com/generate_204", 204},
        {L"http://www.baidu.com/", 200},
    };
    int httpTimeoutMs{3500};
    int verifyTimeoutMs{2500};
    int maxLoginAttempts{2};
    int retryDelayMs{5000};
    int verificationAttempts{8};
    int verifyIntervalMs{600};
    bool uiAutostart{true};
    bool showWebView{false};
    bool autoLogout{false};
};

AppConfig LoadDefaultConfig();
AppConfig LoadConfig(const std::filesystem::path& path);
bool SaveConfig(const std::filesystem::path& path, const AppConfig& config);
bool ValidateConfig(const AppConfig& config, std::wstring& error);
std::filesystem::path ConfigPath();
