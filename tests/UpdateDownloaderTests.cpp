#include "UpdateDownloader.h"
#include "WinHttpUpdateTransport.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto suffix = std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() /
                (L"campusnet-update-downloader-" + suffix);
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

class MemoryTransport final : public IUpdateTransport {
public:
    bool Get(std::wstring_view url, const HttpLimits& limits,
             HttpResponse& response, std::wstring& error) override {
        ++getCalls;
        lastUrl.assign(url);
        lastLimits = limits;
        if (!getSucceeds) {
            error = getError;
            return false;
        }
        response = getResponse;
        return true;
    }

    bool Download(std::wstring_view url, const HttpLimits& limits,
                  const ChunkSink& sink,
                  const ProgressCallback& progress,
                  std::wstring& error) override {
        ++downloadCalls;
        lastUrl.assign(url);
        lastLimits = limits;
        std::uint64_t received = 0;
        for (std::size_t offset = 0; offset < downloadBytes.size();) {
            const std::size_t count =
                std::min(chunkSize, downloadBytes.size() - offset);
            const auto chunk = std::span<const std::uint8_t>(
                downloadBytes.data() + offset, count);
            if (!sink(chunk)) {
                error = L"Chunk sink rejected data";
                return false;
            }
            offset += count;
            received += count;
            if (progress) {
                progress(received, downloadBytes.size(), 100);
            }
            if (cancelAfterFirstChunk != nullptr && offset > 0) {
                cancelAfterFirstChunk->store(true);
                cancelAfterFirstChunk = nullptr;
            }
        }
        if (!downloadSucceeds) {
            error = downloadError;
            return false;
        }
        return true;
    }

    HttpResponse getResponse{
        200, 2,
        L"https://raw.githubusercontent.com/NatsumeHSY/CampusNetClient/main/version.json",
        "{}"};
    std::vector<std::uint8_t> downloadBytes{'a', 'b', 'c'};
    std::size_t chunkSize = 2;
    std::atomic_bool* cancelAfterFirstChunk = nullptr;
    std::wstring getError = L"simulated timeout";
    std::wstring downloadError = L"simulated timeout";
    std::wstring lastUrl;
    HttpLimits lastLimits{};
    bool getSucceeds = true;
    bool downloadSucceeds = true;
    int getCalls = 0;
    int downloadCalls = 0;
};

UpdateArtifact TestArtifact() {
    UpdateArtifact artifact;
    artifact.url =
        L"https://github.com/NatsumeHSY/CampusNetClient/releases/download/v1.2.3/CampusNetClient-portable.zip";
    artifact.sizeBytes = 3;
    artifact.sha256 =
        L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    return artifact;
}

std::size_t CountDirectoryEntries(const std::filesystem::path& directory) {
    return static_cast<std::size_t>(
        std::distance(std::filesystem::directory_iterator(directory),
                      std::filesystem::directory_iterator{}));
}

void TestAllowedUpdateUrls() {
    assert(IsAllowedUpdateUrl(kRemoteManifestUrl, UpdateUrlPurpose::Manifest));
    assert(!IsAllowedUpdateUrl(
        L"https://raw.githubusercontent.com/attacker/repo/main/version.json",
        UpdateUrlPurpose::Manifest));
    assert(IsAllowedUpdateUrl(TestArtifact().url, UpdateUrlPurpose::Artifact));
    assert(IsAllowedUpdateUrl(
        L"https://release-assets.githubusercontent.com/asset/123?signature=abc",
        UpdateUrlPurpose::ArtifactRedirect));
    assert(IsAllowedUpdateUrl(
        L"https://objects.githubusercontent.com/github-production-release-asset/1",
        UpdateUrlPurpose::ArtifactRedirect));
    assert(!IsAllowedUpdateUrl(
        L"https://github.com/attacker/repo/releases/download/v1/file.zip",
        UpdateUrlPurpose::ArtifactRedirect));
    assert(!IsAllowedUpdateUrl(
        L"https://github.com.evil.example/NatsumeHSY/CampusNetClient/releases/download/v1/file.zip",
        UpdateUrlPurpose::ArtifactRedirect));
    assert(!IsAllowedUpdateUrl(
        L"http://github.com/NatsumeHSY/CampusNetClient/releases/download/v1/file.zip",
        UpdateUrlPurpose::Artifact));
    assert(!IsAllowedUpdateUrl(
        L"https://user@github.com/NatsumeHSY/CampusNetClient/releases/download/v1/file.zip",
        UpdateUrlPurpose::Artifact));
}

void TestManifestFetchRequiresFixedHttpsSource() {
    MemoryTransport transport;
    UpdateDownloader downloader(transport);
    std::string body;
    std::wstring error;
    assert(downloader.FetchManifest(kRemoteManifestUrl, body, error));
    assert(body == "{}");
    assert(transport.lastUrl == kRemoteManifestUrl);
    assert(transport.lastLimits.maxBytes == 1024 * 1024);

    assert(!downloader.FetchManifest(
        L"https://evil.example/version.json", body, error));
    assert(transport.getCalls == 1);
}

void TestManifestStatusDestinationAndSizeAreChecked() {
    MemoryTransport transport;
    UpdateDownloader downloader(transport);
    std::string body;
    std::wstring error;

    transport.getResponse.statusCode = 503;
    assert(!downloader.FetchManifest(kRemoteManifestUrl, body, error));

    transport.getResponse.statusCode = 200;
    transport.getResponse.finalUrl = L"https://evil.example/version.json";
    assert(!downloader.FetchManifest(kRemoteManifestUrl, body, error));

    transport.getResponse.finalUrl = std::wstring(kRemoteManifestUrl);
    transport.getResponse.contentLength = 1024 * 1024 + 1;
    assert(!downloader.FetchManifest(kRemoteManifestUrl, body, error));

    transport.getResponse.contentLength = 3;
    transport.getResponse.body = "{}";
    assert(!downloader.FetchManifest(kRemoteManifestUrl, body, error));

    transport.getSucceeds = false;
    assert(!downloader.FetchManifest(kRemoteManifestUrl, body, error));
    assert(error == L"simulated timeout");
}

void TestSuccessfulDownloadCommitsAtomicallyAndReportsProgress() {
    TemporaryDirectory temporary;
    const auto staging = temporary.Path() / L"staging";
    const auto install = temporary.Path() / L"install";
    std::filesystem::create_directories(staging);
    std::filesystem::create_directories(install);
    {
        std::ofstream sentinel(install / L"keep.txt", std::ios::binary);
        sentinel << "unchanged";
    }

    MemoryTransport transport;
    UpdateDownloader downloader(transport);
    auto artifact = TestArtifact();
    std::atomic_bool cancel{false};
    std::filesystem::path stagedPath;
    std::wstring error;
    std::vector<std::uint64_t> progress;
    assert(downloader.DownloadArtifact(
        artifact, staging, cancel,
        [&](std::uint64_t received, std::uint64_t total, std::uint64_t speed) {
            assert(total == 3);
            assert(speed == 100);
            progress.push_back(received);
        },
        stagedPath, error));
    assert(stagedPath.parent_path() == staging);
    assert(stagedPath.extension() == L".zip");
    assert(std::filesystem::file_size(stagedPath) == 3);
    assert(progress.size() == 2);
    assert(progress.back() == 3);
    assert(CountDirectoryEntries(staging) == 1);
    assert(std::filesystem::exists(install / L"keep.txt"));
    std::ifstream sentinel(install / L"keep.txt", std::ios::binary);
    std::string contents;
    sentinel >> contents;
    assert(contents == "unchanged");
}

void TestShortReadAndHashMismatchNeverCommit() {
    for (const bool wrongHash : {false, true}) {
        TemporaryDirectory temporary;
        const auto staging = temporary.Path() / L"staging";
        std::filesystem::create_directories(staging);
        MemoryTransport transport;
        UpdateDownloader downloader(transport);
        auto artifact = TestArtifact();
        if (wrongHash) {
            artifact.sha256 = std::wstring(64, L'0');
        } else {
            transport.downloadBytes = {'a', 'b'};
        }
        std::atomic_bool cancel{false};
        std::filesystem::path stagedPath;
        std::wstring error;
        assert(!downloader.DownloadArtifact(artifact, staging, cancel, {},
                                            stagedPath, error));
        assert(!error.empty());
        assert(std::filesystem::is_empty(staging));
    }
}

void TestOversizeTimeoutAndCancellationCleanPartialFiles() {
    {
        TemporaryDirectory temporary;
        const auto staging = temporary.Path() / L"staging";
        std::filesystem::create_directories(staging);
        MemoryTransport transport;
        transport.downloadBytes = {'a', 'b', 'c', 'd'};
        UpdateDownloader downloader(transport);
        auto artifact = TestArtifact();
        std::atomic_bool cancel{false};
        std::filesystem::path stagedPath;
        std::wstring error;
        assert(!downloader.DownloadArtifact(artifact, staging, cancel, {},
                                            stagedPath, error));
        assert(std::filesystem::is_empty(staging));
    }
    {
        TemporaryDirectory temporary;
        const auto staging = temporary.Path() / L"staging";
        std::filesystem::create_directories(staging);
        MemoryTransport transport;
        transport.downloadSucceeds = false;
        UpdateDownloader downloader(transport);
        auto artifact = TestArtifact();
        std::atomic_bool cancel{false};
        std::filesystem::path stagedPath;
        std::wstring error;
        assert(!downloader.DownloadArtifact(artifact, staging, cancel, {},
                                            stagedPath, error));
        assert(error == L"simulated timeout");
        assert(std::filesystem::is_empty(staging));
    }
    {
        TemporaryDirectory temporary;
        const auto staging = temporary.Path() / L"staging";
        std::filesystem::create_directories(staging);
        MemoryTransport transport;
        UpdateDownloader downloader(transport);
        auto artifact = TestArtifact();
        std::atomic_bool cancel{true};
        std::filesystem::path stagedPath;
        std::wstring error;
        assert(!downloader.DownloadArtifact(artifact, staging, cancel, {},
                                            stagedPath, error));
        assert(transport.downloadCalls == 0);
        assert(std::filesystem::is_empty(staging));
    }
    {
        TemporaryDirectory temporary;
        const auto staging = temporary.Path() / L"staging";
        std::filesystem::create_directories(staging);
        MemoryTransport transport;
        UpdateDownloader downloader(transport);
        auto artifact = TestArtifact();
        std::atomic_bool cancel{false};
        transport.cancelAfterFirstChunk = &cancel;
        std::filesystem::path stagedPath;
        std::wstring error;
        assert(!downloader.DownloadArtifact(artifact, staging, cancel, {},
                                            stagedPath, error));
        assert(std::filesystem::is_empty(staging));
    }
}

} // namespace

int RunUpdateDownloaderTests() {
    TestAllowedUpdateUrls();
    TestManifestFetchRequiresFixedHttpsSource();
    TestManifestStatusDestinationAndSizeAreChecked();
    TestSuccessfulDownloadCommitsAtomicallyAndReportsProgress();
    TestShortReadAndHashMismatchNeverCommit();
    TestOversizeTimeoutAndCancellationCleanPartialFiles();
    return 0;
}
