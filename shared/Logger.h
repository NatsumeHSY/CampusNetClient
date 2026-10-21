#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

enum class LogLevel {
    Info,
    Warn,
    Error,
};

class Logger {
public:
    using Sink = std::function<void(LogLevel, std::wstring_view)>;

    explicit Logger(std::filesystem::path path = {}, Sink sink = {});

    void SetSecrets(std::vector<std::wstring> secrets);
    void Info(std::wstring_view message);
    void Warn(std::wstring_view message);
    void Error(std::wstring_view message);

    static std::wstring Redact(
        std::wstring_view message,
        const std::vector<std::wstring>& secrets);

private:
    void Log(LogLevel level, std::wstring_view message);

    std::filesystem::path path_;
    Sink sink_;
    std::vector<std::wstring> secrets_;
    std::mutex mutex_;
};
