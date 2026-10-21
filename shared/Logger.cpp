#include "Logger.h"

#include <windows.h>

#include <fstream>
#include <sstream>
#include <utility>

namespace {

std::wstring ReplaceAll(std::wstring value, std::wstring_view needle,
                        std::wstring_view replacement) {
    if (needle.empty()) {
        return value;
    }

    std::size_t position = 0;
    while ((position = value.find(needle, position)) != std::wstring::npos) {
        value.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
    return value;
}

std::wstring RedactField(std::wstring value, std::wstring_view field,
                         std::wstring_view terminators) {
    std::size_t searchFrom = 0;
    while ((searchFrom = value.find(field, searchFrom)) != std::wstring::npos) {
        const std::size_t valueStart = searchFrom + field.size();
        const std::size_t end = value.find_first_of(terminators, valueStart);
        value.replace(valueStart,
                      end == std::wstring::npos ? std::wstring::npos
                                                : end - valueStart,
                      L"<redacted>");
        searchFrom = valueStart + 10;
    }
    return value;
}

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

const wchar_t* LevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Info:
        return L"INFO";
    case LogLevel::Warn:
        return L"WARN";
    case LogLevel::Error:
        return L"ERROR";
    }
    return L"UNKNOWN";
}

} // namespace

Logger::Logger(std::filesystem::path path, Sink sink)
    : path_(std::move(path)), sink_(std::move(sink)) {}

void Logger::SetSecrets(std::vector<std::wstring> secrets) {
    std::scoped_lock lock(mutex_);
    secrets_.clear();
    for (auto& secret : secrets) {
        if (!secret.empty()) {
            secrets_.push_back(std::move(secret));
        }
    }
}

void Logger::Info(std::wstring_view message) { Log(LogLevel::Info, message); }

void Logger::Warn(std::wstring_view message) { Log(LogLevel::Warn, message); }

void Logger::Error(std::wstring_view message) {
    Log(LogLevel::Error, message);
}

std::wstring Logger::Redact(std::wstring_view message,
                            const std::vector<std::wstring>& secrets) {
    std::wstring result(message);
    for (const auto& secret : secrets) {
        result = ReplaceAll(std::move(result), secret, L"<redacted>");
    }

    result = RedactField(std::move(result), L"password=", L" \t\r\n,;&");
    result = RedactField(std::move(result), L"passwd=", L" \t\r\n,;&");
    result = RedactField(std::move(result), L"Cookie:", L"\r\n");
    return result;
}

void Logger::Log(LogLevel level, std::wstring_view message) {
    Sink sink;
    std::wstring redacted;
    {
        std::scoped_lock lock(mutex_);
        redacted = Redact(message, secrets_);
        sink = sink_;

        if (!path_.empty()) {
            const auto parent = path_.parent_path();
            if (!parent.empty()) {
                std::error_code error;
                std::filesystem::create_directories(parent, error);
            }

            SYSTEMTIME now{};
            GetLocalTime(&now);
            std::wstringstream line;
            line << L"[" << now.wYear << L"-";
            if (now.wMonth < 10) line << L'0';
            line << now.wMonth << L"-";
            if (now.wDay < 10) line << L'0';
            line << now.wDay << L" ";
            if (now.wHour < 10) line << L'0';
            line << now.wHour << L":";
            if (now.wMinute < 10) line << L'0';
            line << now.wMinute << L":";
            if (now.wSecond < 10) line << L'0';
            line << now.wSecond << L"] [" << LevelName(level) << L"] "
                 << redacted << L"\r\n";

            const std::string utf8 = ToUtf8(line.str());
            std::ofstream file(path_, std::ios::binary | std::ios::app);
            if (file) {
                file.write(utf8.data(),
                           static_cast<std::streamsize>(utf8.size()));
            }
        }
    }

    if (sink) {
        sink(level, redacted);
    }
}
