#pragma once

#include <filesystem>

std::filesystem::path ResolveClientExecutablePath(
    const std::filesystem::path& launcherExecutable);
