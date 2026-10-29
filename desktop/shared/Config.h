#pragma once
#include <filesystem>
#include <string>
#include <vector>
struct InternetCheckEndpoint {
    std::wstring url;
    unsigned expectedStatus{200};
};
struct AppConfig {
    std::wstring carrier{L"telecom"};
    std::wstring mobileAcName{L"hhxy"};
    std::wstring mobileVlan{L"3017"};
    std::wstring gateway{L"10.10.12.13"};
    std::wstring portalUrl{L"http://10.10.12.13/"};
    std::vector<InternetCheckEndpoint> internetChecks{
        {L"http://connect.rom.miui.com/generate_204", 204},
        {L"http://www.baidu.com/", 200},
    };
    int httpTimeoutMs{3500};
    int verifyTimeoutMs{2500};
    int maxLoginAttempts{1};
    int retryDelayMs{5000};
    int verificationAttempts{8};
    int verifyIntervalMs{600};
    bool uiAutostart{true};
    bool autoLogout{false};
};
AppConfig LoadDefaultConfig();
AppConfig LoadConfig(const std::filesystem::path& path);
bool SaveConfig(const std::filesystem::path& path, const AppConfig& config);
bool ValidateConfig(const AppConfig& config, std::wstring& error);
std::filesystem::path ConfigPath();
std::wstring LoginPortalUrl(const AppConfig& config);
