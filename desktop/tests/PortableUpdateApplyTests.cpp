#include "../updater/PortableUpdateApply.h"
#include "../updater/UpdateIntegrity.h"
#include "../third_party/miniz/miniz.h"

#include <Windows.h>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using Entry = std::pair<std::string, std::string>;

class TestHost final : public IPortableUpdateHost {
public:
    bool CloseClientProcesses(std::wstring&) override {
        ++closeCalls;
        return closeSucceeds;
    }
    bool StopService(std::wstring&) override {
        ++stopCalls;
        return stopSucceeds;
    }
    bool StartService(std::wstring& error) override {
        ++startCalls;
        if (failFirstStart && startCalls == 1) {
            error = L"simulated updated service startup failure";
            return false;
        }
        return startSucceeds;
    }
    bool WaitForServiceRunning(std::chrono::milliseconds,
                               std::wstring&) override {
        ++waitCalls;
        return waitSucceeds;
    }

    unsigned closeCalls = 0;
    unsigned stopCalls = 0;
    unsigned startCalls = 0;
    unsigned waitCalls = 0;
    bool closeSucceeds = true;
    bool stopSucceeds = true;
    bool startSucceeds = true;
    bool waitSucceeds = true;
    bool failFirstStart = false;
};

std::filesystem::path NewTempDirectory() {
    wchar_t temp[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    assert(length != 0 && length < MAX_PATH);
    GUID id{};
    assert(SUCCEEDED(CoCreateGuid(&id)));
    wchar_t suffix[40]{};
    assert(StringFromGUID2(id, suffix, ARRAYSIZE(suffix)) != 0);
    std::filesystem::path path(temp);
    path /= std::wstring(L"CampusNetPortableTest-") + suffix;
    assert(std::filesystem::create_directory(path));
    return path;
}

bool WriteText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return output.good();
}

bool CreateArchive(const std::filesystem::path& path,
                   const std::vector<Entry>& files,
                   std::string_view reparseName = {},
                   std::string_view replaceFrom = {},
                   std::string_view replaceTo = {}) {
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_heap(&archive, 0, 1024)) return false;
    bool ok = true;
    for (const auto& [name, contents] : files) {
        if (!mz_zip_writer_add_mem(&archive, name.c_str(), contents.data(),
                                   contents.size(), MZ_DEFAULT_COMPRESSION)) {
            ok = false;
            break;
        }
    }
    void* bytes = nullptr;
    size_t size = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&archive, &bytes, &size) != 0;
    mz_zip_writer_end(&archive);
    if (!ok || bytes == nullptr || size == 0) {
        if (bytes != nullptr) mz_free(bytes);
        return false;
    }
    std::vector<std::uint8_t> archiveBytes(
        static_cast<std::uint8_t*>(bytes),
        static_cast<std::uint8_t*>(bytes) + size);
    mz_free(bytes);

    if (!replaceFrom.empty()) {
        if (replaceFrom.size() != replaceTo.size()) return false;
        unsigned patchedNames = 0;
        for (std::size_t offset = 0; offset + 30 <= archiveBytes.size(); ++offset) {
            auto* p = archiveBytes.data() + offset;
            const bool localHeader = p[0] == 0x50 && p[1] == 0x4b &&
                                     p[2] == 0x03 && p[3] == 0x04;
            const bool centralHeader = p[0] == 0x50 && p[1] == 0x4b &&
                                       p[2] == 0x01 && p[3] == 0x02;
            if (!localHeader && !centralHeader) continue;
            const std::size_t nameLengthOffset = localHeader ? 26 : 28;
            const std::size_t extraLengthOffset = localHeader ? 28 : 30;
            const auto nameLength = static_cast<std::uint16_t>(
                p[nameLengthOffset] | (p[nameLengthOffset + 1] << 8));
            const auto extraLength = static_cast<std::uint16_t>(
                p[extraLengthOffset] | (p[extraLengthOffset + 1] << 8));
            const std::size_t nameOffset = localHeader ? 30 : 46;
            if (nameLength != replaceFrom.size() ||
                offset + nameOffset + nameLength > archiveBytes.size()) continue;
            if (std::string_view(reinterpret_cast<const char*>(p + nameOffset),
                                 nameLength) == replaceFrom) {
                std::copy(replaceTo.begin(), replaceTo.end(),
                          archiveBytes.begin() + offset + nameOffset);
                ++patchedNames;
            }
            offset += nameOffset + nameLength + extraLength - 1;
        }
        if (patchedNames < 2) return false;
    }

    if (!reparseName.empty()) {
        bool patched = false;
        for (std::size_t offset = 0; offset + 46 <= archiveBytes.size(); ++offset) {
            const auto* p = archiveBytes.data() + offset;
            if (p[0] != 0x50 || p[1] != 0x4b || p[2] != 0x01 || p[3] != 0x02) continue;
            const std::uint16_t nameLength = static_cast<std::uint16_t>(p[28] | (p[29] << 8));
            const std::uint16_t extraLength = static_cast<std::uint16_t>(p[30] | (p[31] << 8));
            const std::uint16_t commentLength = static_cast<std::uint16_t>(p[32] | (p[33] << 8));
            if (offset + 46 + nameLength + extraLength + commentLength > archiveBytes.size()) return false;
            const std::string_view entryName(
                reinterpret_cast<const char*>(p + 46), nameLength);
            if (entryName == reparseName) {
                constexpr std::uint32_t kUnixSymlinkMode = 0120777u << 16;
                const std::size_t externalAttributesOffset = offset + 38;
                archiveBytes[externalAttributesOffset] =
                    static_cast<std::uint8_t>(kUnixSymlinkMode & 0xff);
                archiveBytes[externalAttributesOffset + 1] =
                    static_cast<std::uint8_t>((kUnixSymlinkMode >> 8) & 0xff);
                archiveBytes[externalAttributesOffset + 2] =
                    static_cast<std::uint8_t>((kUnixSymlinkMode >> 16) & 0xff);
                archiveBytes[externalAttributesOffset + 3] =
                    static_cast<std::uint8_t>((kUnixSymlinkMode >> 24) & 0xff);
                patched = true;
                break;
            }
            offset += 45 + nameLength + extraLength + commentLength;
        }
        if (!patched) return false;
    }

    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(archiveBytes.data()),
                 static_cast<std::streamsize>(archiveBytes.size()));
    return output.good();
}

std::vector<Entry> ValidFiles() {
    return {
        {"CampusNetLauncher.exe", "new launcher"},
        {"bin/CampusNetClient.exe", "new client"},
        {"bin/CampusNetService.exe", "new service"},
        {"bin/CampusNetUpdater.exe", "new updater"},
        {"bin/version.json", "{\"schemaVersion\":1,\"version\":\"1.2.3\",\"channel\":\"portable\"}"},
    };
}

UpdateArtifact MakeArtifact(const std::filesystem::path& archive,
                            const std::vector<Entry>& files) {
    UpdateArtifact artifact;
    for (const auto& [path, content] : files) {
        const auto temp = archive.parent_path() / (L"digest-" +
            std::to_wstring(artifact.files.size()) + L".tmp");
        assert(WriteText(temp, content));
        std::wstring error;
        const auto digest = Sha256File(temp, error);
        assert(digest.has_value());
        artifact.files.emplace(std::filesystem::path(path).wstring(), *digest);
        std::filesystem::remove(temp);
    }
    std::error_code error;
    artifact.sizeBytes = std::filesystem::file_size(archive, error);
    assert(!error);
    std::wstring digestError;
    const auto digest = Sha256File(archive, digestError);
    assert(digest.has_value());
    artifact.sha256 = *digest;
    return artifact;
}

void CreateInstalledRoot(const std::filesystem::path& root) {
    assert(WriteText(root / L"CampusNetLauncher.exe", "old launcher"));
    assert(WriteText(root / L"bin" / L"CampusNetClient.exe", "old client"));
    assert(WriteText(root / L"bin" / L"CampusNetService.exe", "old service"));
    assert(WriteText(root / L"bin" / L"CampusNetUpdater.exe", "old updater"));
    assert(WriteText(root / L"bin" / L"version.json",
        "{\"schemaVersion\":1,\"version\":\"0.1.0\",\"channel\":\"portable\"}"));
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool RunArchiveCase(const std::vector<Entry>& archiveFiles,
                    const std::vector<Entry>& manifestFiles,
                    bool expectSuccess, bool failFirstStart = false,
                    std::string_view reparseName = {},
                    std::string_view replaceFrom = {},
                    std::string_view replaceTo = {}) {
    const auto temp = NewTempDirectory();
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{temp};
    const auto root = temp / L"install";
    const auto archive = temp / L"update.zip";
    CreateInstalledRoot(root);
    if (!CreateArchive(archive, archiveFiles, reparseName, replaceFrom,
                       replaceTo)) {
        std::wcerr << L"Could not create test ZIP; reparse entry requested: "
                   << (reparseName.empty() ? L"none" : L"yes") << L"\n";
        return false;
    }
    const auto artifact = MakeArtifact(archive, manifestFiles);
    TestHost host;
    host.failFirstStart = failFirstStart;
    const auto result = PortableUpdateApply::Apply(archive, root, L"1.2.3",
                                                   artifact, host);
    if (expectSuccess) {
        if (!result.success) {
            std::wcerr << L"Portable apply failed: " << result.error << L"\n";
            return false;
        }
        assert(result.success);
        assert(!result.rolledBack);
        assert(ReadText(root / L"bin" / L"CampusNetClient.exe") == "new client");
        return true;
    } else if (failFirstStart) {
        assert(!result.success && result.rolledBack);
        assert(ReadText(root / L"bin" / L"CampusNetClient.exe") == "old client");
        assert(host.startCalls == 2);
        return true;
    } else {
        assert(!result.success);
        assert(ReadText(root / L"bin" / L"CampusNetClient.exe") == "old client");
        return true;
    }
    return false;
}

bool RunCase(const std::vector<Entry>& files, bool expectSuccess,
             bool failFirstStart = false, std::string_view reparseName = {}) {
    return RunArchiveCase(files, files, expectSuccess, failFirstStart, reparseName);
}

} // namespace

int RunPortableUpdateApplyTests() {
    if (!RunCase(ValidFiles(), true)) return 1;

    auto traversal = ValidFiles();
    traversal.emplace_back("xx/escape.tx", "must not be extracted");
    if (!RunArchiveCase(traversal, ValidFiles(), false, false, {},
                        "xx/escape.tx", "../escape.tx")) return 2;

    auto absolute = ValidFiles();
    absolute.emplace_back("xevil.txt", "must not be extracted");
    if (!RunArchiveCase(absolute, ValidFiles(), false, false, {},
                        "xevil.txt", "/evil.txt")) return 3;

    auto duplicate = ValidFiles();
    duplicate.emplace_back("bin/campusnetclient.exe", "duplicate");
    if (!RunArchiveCase(duplicate, ValidFiles(), false)) return 4;

    auto extra = ValidFiles();
    extra.emplace_back("bin/extra.dll", "not in manifest");
    if (!RunArchiveCase(extra, ValidFiles(), false)) return 5;

    if (!RunCase(ValidFiles(), false, false, "bin/CampusNetUpdater.exe")) return 6;
    if (!RunCase(ValidFiles(), false, true)) return 7;
    return 0;
}
