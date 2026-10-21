#include "LauncherPaths.h"

std::filesystem::path ResolveClientExecutablePath(
    const std::filesystem::path& launcherExecutable) {
    return launcherExecutable.parent_path() / L"bin" / L"CampusNetClient.exe";
}
