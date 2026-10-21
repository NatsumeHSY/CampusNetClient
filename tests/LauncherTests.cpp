#include "LauncherPaths.h"

#include <cassert>
#include <filesystem>

int RunLauncherTests() {
    const auto spacedPath = ResolveClientExecutablePath(
        std::filesystem::path(L"C:\\Portable Folder\\CampusNetLauncher.exe"));
    assert(spacedPath == std::filesystem::path(L"C:\\Portable Folder\\bin\\CampusNetClient.exe"));

    const auto unicodePath = ResolveClientExecutablePath(
        std::filesystem::path(L"D:\\\u6D4B\u8BD5\u76EE\u5F55\\\u6821\u56ED\u7F51\\CampusNetLauncher.exe"));
    assert(unicodePath == std::filesystem::path(L"D:\\\u6D4B\u8BD5\u76EE\u5F55\\\u6821\u56ED\u7F51\\bin\\CampusNetClient.exe"));

    return 0;
}
