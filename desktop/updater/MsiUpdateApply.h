#pragma once

#include "UpdateManifest.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

struct InstalledProductInfo {
    bool registered = false;
    std::wstring productCode;
    std::wstring installLocation;
    std::wstring productVersion;
};

struct MsiUpdateResult {
    bool success = false;
    std::uint32_t windowsInstallerCode = 0;
    bool sameMsiVersionRepair = false;
    std::wstring error;
};

class IWindowsInstaller {
public:
    virtual ~IWindowsInstaller() = default;
    virtual std::uint32_t InstallPackage(const std::filesystem::path& package,
                                        std::wstring_view properties) = 0;
};

std::optional<std::wstring> MsiProductVersionForSemVer(
    std::wstring_view semanticVersion);
bool MsiProductVersionMatchesSemVer(std::wstring_view productVersion,
                                    std::wstring_view semanticVersion);
InstalledProductInfo QueryInstalledMsiProduct(
    const std::filesystem::path& expectedInstallLocation,
    std::wstring& error);

class MsiUpdateApply final {
public:
    static MsiUpdateResult Apply(
        const std::filesystem::path& validatedMsi,
        std::wstring_view localVersion,
        std::wstring_view targetVersion,
        const InstalledProductInfo& installedProduct,
        UpdateChannel localChannel,
        IWindowsInstaller& installer);
};
