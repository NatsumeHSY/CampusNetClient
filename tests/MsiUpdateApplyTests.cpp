#include "../updater/MsiUpdateApply.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class FakeWindowsInstaller final : public IWindowsInstaller {
public:
    std::uint32_t InstallPackage(const std::filesystem::path& package,
                                 std::wstring_view properties) override {
        ++calls;
        seenPackage = package;
        seenProperties = properties;
        return resultCode;
    }

    unsigned calls = 0;
    std::uint32_t resultCode = 0;
    std::filesystem::path seenPackage;
    std::wstring seenProperties;
};

std::filesystem::path NewMsiFixture() {
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    assert(length != 0 && length < MAX_PATH);
    GUID id{};
    assert(SUCCEEDED(CoCreateGuid(&id)));
    wchar_t suffix[40]{};
    assert(StringFromGUID2(id, suffix, ARRAYSIZE(suffix)) != 0);
    std::filesystem::path path(temp);
    path /= std::wstring(L"CampusNetMsiUpdateTest-") + suffix;
    std::ofstream output(path, std::ios::binary | std::ios::out);
    output << "fixture";
    assert(output.good());
    return path;
}

InstalledProductInfo Installed() {
    InstalledProductInfo info;
    info.registered = true;
    info.productCode = L"{A1B2C3D4-1234-1234-1234-123456789012}";
    info.installLocation = L"C:\\Program Files\\CampusNetClient";
    info.productVersion = L"0.1.0.0";
    return info;
}

} // namespace

int RunMsiUpdateApplyTests() {
    const auto package = NewMsiFixture();
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{package};

    const auto mapped = MsiProductVersionForSemVer(L"0.1.0-beta.2+build7");
    assert(mapped && *mapped == L"0.1.0");
    assert(MsiProductVersionMatchesSemVer(L"0.1.0.0", L"0.1.0"));
    assert(!MsiProductVersionForSemVer(L"256.1.0"));

    auto installed = Installed();
    FakeWindowsInstaller installer;
    auto result = MsiUpdateApply::Apply(package, L"0.1.0", L"1.2.3",
        installed, UpdateChannel::Msi, installer);
    assert(result.success && !result.sameMsiVersionRepair);
    assert(installer.calls == 1 && installer.seenProperties.empty());

    result = MsiUpdateApply::Apply(package, L"0.1.0", L"0.0.9",
        installed, UpdateChannel::Msi, installer);
    assert(result.success && !result.sameMsiVersionRepair);
    assert(installer.seenProperties.empty());

    result = MsiUpdateApply::Apply(package, L"0.1.0-beta", L"0.1.0+repair",
        installed, UpdateChannel::Msi, installer);
    assert(result.success && result.sameMsiVersionRepair);
    assert(installer.seenProperties == L"REINSTALL=ALL REINSTALLMODE=vomus");

    auto invalid = installed;
    invalid.registered = false;
    const unsigned before = installer.calls;
    result = MsiUpdateApply::Apply(package, L"0.1.0", L"1.0.0",
        invalid, UpdateChannel::Msi, installer);
    assert(!result.success && installer.calls == before);

    result = MsiUpdateApply::Apply(package, L"0.1.0", L"1.0.0",
        installed, UpdateChannel::Portable, installer);
    assert(!result.success && installer.calls == before);

    invalid = installed;
    invalid.productVersion = L"9.9.9";
    result = MsiUpdateApply::Apply(package, L"0.1.0", L"1.0.0",
        invalid, UpdateChannel::Msi, installer);
    assert(!result.success && installer.calls == before);

    installer.resultCode = 1603;
    result = MsiUpdateApply::Apply(package, L"0.1.0", L"1.0.0",
        installed, UpdateChannel::Msi, installer);
    assert(!result.success && result.windowsInstallerCode == 1603);
    assert(result.error.find(L"1603") != std::wstring::npos);
    return 0;
}
