#include "Logger.h"

#include <cassert>
#include <string>
#include <vector>

int RunLoggerTests() {
    std::vector<std::wstring> messages;
    Logger logger({}, [&messages](LogLevel, std::wstring_view message) {
        messages.emplace_back(message);
    });
    logger.SetSecrets({L"secret-value", L"cookie-value"});
    logger.Info(L"user=student password=secret-value Cookie: cookie-value");
    assert(messages.size() == 1);
    assert(messages[0].find(L"secret-value") == std::wstring::npos);
    assert(messages[0].find(L"cookie-value") == std::wstring::npos);
    assert(messages[0].find(L"<redacted>") != std::wstring::npos);
    return 0;
}
