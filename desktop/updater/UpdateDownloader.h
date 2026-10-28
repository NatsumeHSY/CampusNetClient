#pragma once

#include "UpdateManifest.h"
#include "WinHttpUpdateTransport.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>

inline constexpr std::wstring_view kRemoteManifestUrl =
    L"https://raw.githubusercontent.com/NatsumeHSY/CampusNetClient/main/version.json";

class UpdateDownloader final {
public:
    explicit UpdateDownloader(IUpdateTransport& transport);

    bool FetchManifest(std::wstring_view url, std::string& body,
                       std::wstring& error);
    bool DownloadArtifact(const UpdateArtifact& artifact,
                          const std::filesystem::path& stagingDirectory,
                          const std::atomic_bool& cancel,
                          ProgressCallback progress,
                          std::filesystem::path& stagedPath,
                          std::wstring& error);

private:
    IUpdateTransport& transport_;
};
