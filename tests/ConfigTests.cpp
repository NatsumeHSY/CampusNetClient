#include "Config.h"

#include <cassert>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
#include <filesystem>
#include <fstream>
#include <string>

int RunCredentialStoreTests();
int RunLoggerTests();
int RunPortalFormParserTests();
int RunHttpPortalClientTests();
int RunProtocolTests();
int RunLoginStateTests();
int RunClientManagementTests();
int RunLauncherTests();
void RunWebViewRequestTests();
int RunEndToEndLocalTests();
int RunUpdateManifestTests();
int RunUpdateIntegrityTests();
int RunUpdateDownloaderTests();
int RunUpdaterRoleTests();
int RunUpdaterSupervisorTests();
int RunUpdateBrokerTests();
int RunUpdateCheckPresentationTests();
int RunPortableUpdateApplyTests();
int RunMsiUpdateApplyTests();

int wmain() {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    const AppConfig defaults = LoadDefaultConfig();
    assert(defaults.gateway == L"10.10.12.13");
    assert(defaults.portalUrl == L"http://10.10.12.13/");
    assert(defaults.maxLoginAttempts == 1);
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

    const auto legacyPath =
        std::filesystem::temp_directory_path() / L"campus-net-legacy-config-test.json";
    {
        std::ofstream legacy(legacyPath, std::ios::binary | std::ios::trunc);
        legacy << R"({"gateway":"1.1.1.1","portalUrl":"http://1.1.1.1/","maxLoginAttempts":2})";
    }
    const AppConfig migrated = LoadConfig(legacyPath);
    assert(migrated.gateway == L"10.10.12.13");
    assert(migrated.portalUrl == L"http://10.10.12.13/");
    assert(migrated.maxLoginAttempts == 1);
    std::filesystem::remove(legacyPath);

    assert(RunCredentialStoreTests() == 0);
    assert(RunLoggerTests() == 0);
    assert(RunPortalFormParserTests() == 0);
    assert(RunHttpPortalClientTests() == 0);
    assert(RunProtocolTests() == 0);
    assert(RunLoginStateTests() == 0);
    assert(RunClientManagementTests() == 0);
    assert(RunLauncherTests() == 0);
    RunWebViewRequestTests();
    assert(RunEndToEndLocalTests() == 0);
    assert(RunUpdateManifestTests() == 0);
    assert(RunUpdateIntegrityTests() == 0);
    assert(RunUpdateDownloaderTests() == 0);
    assert(RunUpdaterRoleTests() == 0);
    assert(RunUpdaterSupervisorTests() == 0);
    assert(RunUpdateBrokerTests() == 0);
    assert(RunUpdateCheckPresentationTests() == 0);
    if (RunPortableUpdateApplyTests() != 0) return 1;
    assert(RunMsiUpdateApplyTests() == 0);

    return 0;
}
