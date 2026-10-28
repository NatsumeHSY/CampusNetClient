#include "MsiUpdateApply.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Msi.h>
#include <MsiQuery.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <vector>

#pragma comment(lib, "Msi.lib")

namespace {

constexpr wchar_t kUpgradeCode[] = L"{9B7E1B47-0F50-4F63-A1FC-6A3B8E2D5A17}";

bool SamePath(const std::filesystem::path& left,
              const std::filesystem::path& right) {
    const auto leftText = std::filesystem::absolute(left).lexically_normal().wstring();
    const auto rightText = std::filesystem::absolute(right).lexically_normal().wstring();
    return leftText.size() == rightText.size() &&
           CompareStringOrdinal(leftText.data(), static_cast<int>(leftText.size()),
                                rightText.data(), static_cast<int>(rightText.size()),
                                TRUE) == CSTR_EQUAL;
}

bool ReadProductProperty(std::wstring_view productCode,
                         const wchar_t* property,
                         std::wstring& value) {
    DWORD length = 0;
    UINT result = MsiGetProductInfoW(std::wstring(productCode).c_str(), property,
                                     nullptr, &length);
    if (result != ERROR_MORE_DATA && result != ERROR_SUCCESS) return false;
    std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1);
    DWORD capacity = static_cast<DWORD>(buffer.size());
    result = MsiGetProductInfoW(std::wstring(productCode).c_str(), property,
                               buffer.data(), &capacity);
    if (result != ERROR_SUCCESS) return false;
    value.assign(buffer.data(), capacity);
    return true;
}

} // namespace

std::optional<std::wstring> MsiProductVersionForSemVer(
    std::wstring_view semanticVersion) {
    if (!IsValidSemVer(semanticVersion)) return std::nullopt;
    const auto suffix = semanticVersion.find_first_of(L"-+");
    const auto core = semanticVersion.substr(0, suffix);
    std::array<std::uint32_t, 3> parts{};
    std::size_t begin = 0;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto end = core.find(L'.', begin);
        const auto component = core.substr(
            begin, end == std::wstring_view::npos ? core.size() - begin : end - begin);
        if (component.empty()) return std::nullopt;
        std::uint32_t value = 0;
        for (const wchar_t ch : component) {
            if (ch < L'0' || ch > L'9') return std::nullopt;
            const auto digit = static_cast<std::uint32_t>(ch - L'0');
            if (value > (65535u - digit) / 10u) return std::nullopt;
            value = value * 10 + digit;
        }
        if ((index < 2 && value > 255) || value > 65535) return std::nullopt;
        parts[index] = value;
        if (index != parts.size() - 1) {
            if (end == std::wstring_view::npos) return std::nullopt;
            begin = end + 1;
        }
    }
    if (core.find(L'.', begin) != std::wstring_view::npos) return std::nullopt;
    return std::to_wstring(parts[0]) + L"." + std::to_wstring(parts[1]) +
           L"." + std::to_wstring(parts[2]);
}

bool MsiProductVersionMatchesSemVer(std::wstring_view productVersion,
                                    std::wstring_view semanticVersion) {
    const auto mapped = MsiProductVersionForSemVer(semanticVersion);
    if (!mapped) return false;
    const auto dot = productVersion.find(L'.');
    if (dot == std::wstring_view::npos) return false;
    const auto secondDot = productVersion.find(L'.', dot + 1);
    if (secondDot == std::wstring_view::npos) return false;
    const auto thirdDot = productVersion.find(L'.', secondDot + 1);
    const auto threeFields = productVersion.substr(
        0, thirdDot == std::wstring_view::npos ? productVersion.size() : thirdDot);
    return threeFields == *mapped;
}

InstalledProductInfo QueryInstalledMsiProduct(
    const std::filesystem::path& expectedInstallLocation,
    std::wstring& error) {
    error.clear();
    InstalledProductInfo info;
    std::array<wchar_t, 39> productCode{};
    const UINT enumeration = MsiEnumRelatedProductsW(kUpgradeCode, 0, 0,
                                                      productCode.data());
    if (enumeration == ERROR_NO_MORE_ITEMS) {
        error = L"No installed product with this UpgradeCode was found";
        return info;
    }
    if (enumeration != ERROR_SUCCESS) {
        error = L"Could not enumerate installed CampusNet MSI products";
        return info;
    }
    std::wstring installLocation;
    std::wstring version;
    if (!ReadProductProperty(productCode.data(), INSTALLPROPERTY_INSTALLLOCATION,
                             installLocation) ||
        !ReadProductProperty(productCode.data(), INSTALLPROPERTY_VERSIONSTRING,
                             version)) {
        error = L"Installed MSI registration is incomplete";
        return info;
    }
    if (installLocation.empty() || !SamePath(installLocation, expectedInstallLocation)) {
        error = L"Installed MSI location differs from the service install directory";
        return info;
    }
    wchar_t anotherProduct[39]{};
    if (MsiEnumRelatedProductsW(kUpgradeCode, 0, 1, anotherProduct) == ERROR_SUCCESS) {
        error = L"More than one related CampusNet MSI product is registered";
        return info;
    }
    info.registered = true;
    info.productCode = productCode.data();
    info.installLocation = std::move(installLocation);
    info.productVersion = std::move(version);
    return info;
}

MsiUpdateResult MsiUpdateApply::Apply(
    const std::filesystem::path& validatedMsi,
    std::wstring_view localVersion,
    std::wstring_view targetVersion,
    const InstalledProductInfo& installedProduct,
    UpdateChannel localChannel,
    IWindowsInstaller& installer) {
    MsiUpdateResult result;
    const DWORD attributes = GetFileAttributesW(validatedMsi.c_str());
    if (validatedMsi.empty() || !validatedMsi.is_absolute() ||
        attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        result.error = L"Validated MSI package is missing or is not a regular file";
        return result;
    }
    if (localChannel != UpdateChannel::Msi || !installedProduct.registered ||
        installedProduct.productCode.empty() || installedProduct.installLocation.empty()) {
        result.error = L"Local MSI channel and Windows Installer registration do not agree";
        return result;
    }
    if (!IsValidSemVer(localVersion) || !IsValidSemVer(targetVersion) ||
        !MsiProductVersionMatchesSemVer(installedProduct.productVersion,
                                        localVersion)) {
        result.error = L"Installed MSI product version conflicts with local version.json";
        return result;
    }
    if (!MsiProductVersionForSemVer(targetVersion)) {
        result.error = L"Target semantic version cannot be represented by Windows Installer";
        return result;
    }

    const auto currentMapped = MsiProductVersionForSemVer(localVersion);
    const auto targetMapped = MsiProductVersionForSemVer(targetVersion);
    result.sameMsiVersionRepair = currentMapped == targetMapped;
    const std::wstring properties = result.sameMsiVersionRepair
        ? L"REINSTALL=ALL REINSTALLMODE=vomus" : L"";
    result.windowsInstallerCode = installer.InstallPackage(validatedMsi,
                                                             properties);
    result.success = result.windowsInstallerCode == ERROR_SUCCESS;
    if (!result.success) {
        result.error = L"Windows Installer failed with code " +
                       std::to_wstring(result.windowsInstallerCode);
    }
    return result;
}
