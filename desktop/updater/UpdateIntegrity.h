#pragma once

#include "UpdateManifest.h"

#include <filesystem>
#include <optional>
#include <string>

std::optional<std::wstring> Sha256File(const std::filesystem::path& path,
                                       std::wstring& error);
bool VerifyManagedFiles(const std::filesystem::path& root,
                        const UpdateArtifact& artifact,
                        std::wstring& error);
