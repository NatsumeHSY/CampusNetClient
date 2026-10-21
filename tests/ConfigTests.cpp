#include "Config.h"

#include <cassert>
#include <filesystem>
#include <string>

int RunCredentialStoreTests();
int RunLoggerTests();
int RunPortalFormParserTests();
int RunHttpPortalClientTests();
int RunProtocolTests();
int RunLoginStateTests();
int RunClientManagementTests();
void RunWebViewRequestTests();
int RunEndToEndLocalTests();

int wmain() {
    const AppConfig defaults = LoadDefaultConfig();
    assert(defaults.gateway == L"1.1.1.1");
    assert(defaults.portalUrl == L"http://1.1.1.1/");
    assert(defaults.uiAutostart);
    assert(!defaults.showWebView);
    assert(defaults.httpTimeoutMs > 0);

    std::wstring error;
    assert(ValidateConfig(defaults, error));

    AppConfig invalid = defaults;
    invalid.portalUrl.clear();
    assert(!ValidateConfig(invalid, error));

    invalid = defaults;
    invalid.httpTimeoutMs = 0;
    assert(!ValidateConfig(invalid, error));

    const auto path = std::filesystem::temp_directory_path() / L"campus-net-config-test.json";
    assert(SaveConfig(path, defaults));
    const AppConfig loaded = LoadConfig(path);
    assert(loaded.gateway == defaults.gateway);
    assert(loaded.portalUrl == defaults.portalUrl);
    assert(loaded.uiAutostart == defaults.uiAutostart);
    assert(loaded.showWebView == defaults.showWebView);
    assert(loaded.httpTimeoutMs == defaults.httpTimeoutMs);
    std::filesystem::remove(path);

    assert(RunCredentialStoreTests() == 0);
    assert(RunLoggerTests() == 0);
    assert(RunPortalFormParserTests() == 0);
    assert(RunHttpPortalClientTests() == 0);
    assert(RunProtocolTests() == 0);
    assert(RunLoginStateTests() == 0);
    assert(RunClientManagementTests() == 0);
    RunWebViewRequestTests();
    assert(RunEndToEndLocalTests() == 0);

    return 0;
}
