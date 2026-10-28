#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

enum class UpdateChannel {
    Portable,
    Msi,
};

enum class UpdateDecision {
    UpToDate,
    InstallRemoteVersion,
    RepairFiles,
    Invalid,
};

struct UpdateArtifact {
    std::wstring url;
    std::uint64_t sizeBytes = 0;
    std::wstring sha256;
    std::map<std::wstring, std::wstring> files;
};

struct RemoteUpdateManifest {
    std::uint32_t schemaVersion = 0;
    std::wstring version;
    std::wstring publishedAt;
    std::wstring releaseNotes;
    UpdateArtifact portable;
    UpdateArtifact msi;
};

struct LocalInstallInfo {
    std::wstring version;
    UpdateChannel channel = UpdateChannel::Portable;
};

bool ParseRemoteUpdateManifest(std::string_view json,
                               RemoteUpdateManifest& manifest,
                               std::wstring& error);
bool ParseLocalInstallInfo(std::string_view json, LocalInstallInfo& local,
                           std::wstring& error);
UpdateDecision DecideUpdate(
    const LocalInstallInfo& local, const RemoteUpdateManifest& remote,
    const std::map<std::wstring, std::wstring>& localDigests);
bool IsValidSemVer(std::wstring_view version);
bool IsSafeUpdateRelativePath(std::wstring_view path);
