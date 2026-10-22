#include "UpdateManifest.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

bool VerifyManifestFileRequestedByEnvironment() {
    constexpr wchar_t variable[] = L"CAMPUSNET_TEST_UPDATE_MANIFEST";
    std::wstring path(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(variable, path.data(),
                                                  static_cast<DWORD>(path.size()));
    if (length == 0) {
        return GetLastError() == ERROR_ENVVAR_NOT_FOUND ||
               GetLastError() == ERROR_SUCCESS;
    }
    if (length >= path.size()) return false;
    path.resize(length);
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size <= 0 || size > 1024 * 1024) return false;
    input.seekg(0, std::ios::beg);
    std::string json(static_cast<std::size_t>(size), '\0');
    if (!input.read(json.data(), static_cast<std::streamsize>(json.size()))) {
        return false;
    }
    RemoteUpdateManifest manifest;
    std::wstring error;
    return ParseRemoteUpdateManifest(json, manifest, error);
}

} // namespace

namespace {

std::string MakeManifest(std::string_view portablePath = "bin/CampusNetClient.exe",
                         std::string_view portableUrl =
                             "https://github.com/NatsumeHSY/CampusNetClient/releases/download/v1.2.3/CampusNetClient-portable.zip",
                         std::string_view notes = "Bug fixes") {
    return "{\"schemaVersion\":1,\"version\":\"1.2.3\","
           "\"publishedAt\":\"2026-10-22T12:00:00Z\",\"releaseNotes\":\"" +
           std::string(notes) + "\",\"artifacts\":{\"portable\":{\"url\":\"" +
           std::string(portableUrl) +
           "\",\"sizeBytes\":1024,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"files\":{\"" +
           std::string(portablePath) +
           "\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}},"
           "\"msi\":{\"url\":\"https://github.com/NatsumeHSY/CampusNetClient/releases/download/v1.2.3/CampusNetClient.msi\",\"sizeBytes\":512,\"sha256\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\",\"files\":{\"CampusNetClient.exe\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\"}}}}";
}

void AssertRejected(std::string_view json) {
    RemoteUpdateManifest manifest;
    std::wstring error;
    assert(!ParseRemoteUpdateManifest(json, manifest, error));
    assert(!error.empty());
}

void ReplaceOnce(std::string& value, std::string_view from,
                 std::string_view to) {
    const auto position = value.find(from);
    assert(position != std::string::npos);
    value.replace(position, from.size(), to);
}

} // namespace

int RunUpdateManifestTests() {
    {
        RemoteUpdateManifest manifest;
        std::wstring error;
        const std::string json = MakeManifest();
        assert(ParseRemoteUpdateManifest(json, manifest, error));
        assert(manifest.schemaVersion == 1);
        assert(manifest.version == L"1.2.3");
        assert(manifest.releaseNotes == L"Bug fixes");
        assert(manifest.portable.sizeBytes == 1024);
        assert(manifest.msi.sizeBytes == 512);
        assert(manifest.portable.files.size() == 1);
        assert(manifest.portable.files.contains(L"bin/CampusNetClient.exe"));
    }
    {
        auto json = MakeManifest();
        ReplaceOnce(json, "Bug fixes", "\\u4fee\\u590d");
        RemoteUpdateManifest manifest;
        std::wstring error;
        assert(ParseRemoteUpdateManifest(json, manifest, error));
        assert(manifest.releaseNotes == L"\x4fee\x590d");
    }
    {
        LocalInstallInfo local;
        std::wstring error;
        assert(ParseLocalInstallInfo(
            R"({"schemaVersion":1,"version":"0.1.0","channel":"portable"})",
            local, error));
        assert(local.version == L"0.1.0");
        assert(local.channel == UpdateChannel::Portable);
        assert(ParseLocalInstallInfo(
            R"({"schemaVersion":1,"version":"0.1.0","channel":"msi"})",
            local, error));
        assert(local.channel == UpdateChannel::Msi);
    }
    {
        RemoteUpdateManifest remote;
        std::wstring error;
        assert(ParseRemoteUpdateManifest(MakeManifest(), remote, error));
        LocalInstallInfo local{L"0.9.0", UpdateChannel::Portable};
        assert(DecideUpdate(local, remote, {}) ==
               UpdateDecision::InstallRemoteVersion);

        auto rollbackManifest = MakeManifest();
        ReplaceOnce(rollbackManifest, "\"version\":\"1.2.3\"",
                    "\"version\":\"0.1.0\"");
        assert(ParseRemoteUpdateManifest(rollbackManifest, remote, error));
        local.version = L"9.9.9";
        assert(DecideUpdate(local, remote, {}) ==
               UpdateDecision::InstallRemoteVersion);

        local.version = remote.version;
        const std::map<std::wstring, std::wstring> matchingFiles{
            {L"bin/CampusNetClient.exe",
             L"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}};
        assert(DecideUpdate(local, remote, matchingFiles) ==
               UpdateDecision::UpToDate);
        assert(DecideUpdate(local, remote, {}) == UpdateDecision::RepairFiles);
    }
    for (const std::string_view version : {"v1.2.3", "01.2.3", "1.2",
                                           "1.2.3.4", "1.2.3-01"}) {
        auto json = MakeManifest();
        ReplaceOnce(json, "\"version\":\"1.2.3\"",
                    "\"version\":\"" + std::string(version) + "\"");
        AssertRejected(json);
    }
    {
        auto json = MakeManifest();
        ReplaceOnce(json, "\"version\":\"1.2.3\"",
                    "\"version\":\"1.2.3\",\"version\":\"9.9.9\"");
        AssertRejected(json);
    }
    {
        auto json = MakeManifest();
        ReplaceOnce(json, "\"schemaVersion\":1", "\"schemaVersion\":99");
        AssertRejected(json);
    }
    {
        auto json = MakeManifest();
        ReplaceOnce(json, "\"schemaVersion\":1",
                    "\"schemaVersion\":1,\"unexpected\":true");
        AssertRejected(json);
    }
    {
        const std::string oversized(1024 * 1024 + 1, ' ');
        AssertRejected(oversized);
    }
    {
        const std::string longNotes(32 * 1024 + 1, 'n');
        AssertRejected(MakeManifest("bin/CampusNetClient.exe",
                                    "https://github.com/NatsumeHSY/CampusNetClient/releases/download/v1.2.3/CampusNetClient-portable.zip",
                                    longNotes));
    }
    {
        std::string entries;
        constexpr std::string_view digest =
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        for (std::size_t i = 0; i <= 1024; ++i) {
            if (!entries.empty()) {
                entries += ',';
            }
            entries += "\"bin/file-" + std::to_string(i) + ".exe\":\"" +
                       std::string(digest) + "\"";
        }
        auto json = MakeManifest();
        ReplaceOnce(
            json,
            "\"bin/CampusNetClient.exe\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"",
            entries);
        AssertRejected(json);
    }
    {
        auto json = MakeManifest();
        ReplaceOnce(
            json,
            "\"bin/CampusNetClient.exe\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"",
            "\"bin/Client.exe\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"bin/client.exe\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"");
        AssertRejected(json);
    }
    for (const std::string_view path : {"../outside.exe", "/rooted.exe",
                                        "C:/absolute.exe"}) {
        AssertRejected(MakeManifest(path));
    }
    AssertRejected(MakeManifest(
        "bin/CampusNetClient.exe",
        "http://github.com/NatsumeHSY/CampusNetClient/releases/download/v1.2.3/update.zip"));
    AssertRejected(MakeManifest(
        "bin/CampusNetClient.exe",
        "https://evil.example/releases/update.zip"));

    {
        auto json = MakeManifest();
        ReplaceOnce(json, "Bug fixes", "\\uD800");
        AssertRejected(json);
    }
    {
        auto json = MakeManifest();
        json.push_back(static_cast<char>(0xFF));
        AssertRejected(json);
    }

    {
        auto json = MakeManifest();
        ReplaceOnce(json, "\"version\":\"1.2.3\"",
                    "\"version\":\"1.2.3-alpha.1+build.5\"");
        RemoteUpdateManifest prerelease;
        std::wstring error;
        assert(ParseRemoteUpdateManifest(json, prerelease, error));
        assert(prerelease.version == L"1.2.3-alpha.1+build.5");
    }

    auto invalidLocal =
        R"({"schemaVersion":1,"version":"0.1.0","channel":"other"})";
    LocalInstallInfo local;
    std::wstring error;
    assert(!ParseLocalInstallInfo(invalidLocal, local, error));
    assert(!ParseLocalInstallInfo(
        R"({"schemaVersion":1,"version":"0.1.0.0","channel":"msi"})",
        local, error));
    return VerifyManifestFileRequestedByEnvironment() ? 0 : 90;
}
