#include "UpdateDownloader.h"
#include "UpdateIntegrity.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kMaxArtifactBytes = 1024ull * 1024 * 1024;
constexpr std::uint64_t kManifestMaxBytes = 1024ull * 1024;
constexpr std::uint32_t kDefaultTotalTimeoutMs = 10 * 60 * 1000;
constexpr std::uint32_t kDefaultReadTimeoutMs = 30 * 1000;
constexpr std::uint32_t kMaxRedirects = 5;

bool IsLowerHexDigest(std::wstring_view digest) {
    return digest.size() == 64 &&
           std::all_of(digest.begin(), digest.end(), [](wchar_t ch) {
               return (ch >= L'0' && ch <= L'9') ||
                      (ch >= L'a' && ch <= L'f');
           });
}

bool CreateRandomToken(std::wstring& token, std::wstring& error) {
    std::array<UCHAR, 16> random{};
    const NTSTATUS status = BCryptGenRandom(
        nullptr, random.data(), static_cast<ULONG>(random.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        error = L"Could not generate a unique staging filename";
        return false;
    }
    static constexpr wchar_t hex[] = L"0123456789abcdef";
    token.clear();
    token.reserve(random.size() * 2);
    for (UCHAR byte : random) {
        token.push_back(hex[byte >> 4]);
        token.push_back(hex[byte & 0x0f]);
    }
    return true;
}

class PartialFile final {
public:
    ~PartialFile() {
        Close();
        if (!committed_ && !path_.empty()) {
            DeleteFileW(path_.c_str());
        }
    }

    bool Open(const std::filesystem::path& directory, std::wstring& error) {
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            std::wstring token;
            if (!CreateRandomToken(token, error)) {
                return false;
            }
            path_ = directory / (L"update-" + token + L".partial");
            handle_ = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY |
                                      FILE_FLAG_SEQUENTIAL_SCAN,
                                  nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                return true;
            }
            const DWORD lastError = GetLastError();
            if (lastError != ERROR_FILE_EXISTS &&
                lastError != ERROR_ALREADY_EXISTS) {
                error = L"Could not create a temporary update file";
                path_.clear();
                return false;
            }
            path_.clear();
        }
        error = L"Could not allocate a unique temporary update file";
        return false;
    }

    bool Write(std::span<const std::uint8_t> bytes, std::wstring& error) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto remaining = bytes.size() - offset;
            const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
                remaining, std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (!WriteFile(handle_, bytes.data() + offset, requested, &written,
                           nullptr) ||
                written == 0) {
                error = L"Could not write the staged update file";
                return false;
            }
            offset += written;
        }
        return true;
    }

    bool Finish(std::wstring& error) {
        if (handle_ == INVALID_HANDLE_VALUE) {
            error = L"Temporary update file is not open";
            return false;
        }
        if (!FlushFileBuffers(handle_)) {
            error = L"Could not flush the staged update file";
            return false;
        }
        if (!CloseHandle(handle_)) {
            handle_ = INVALID_HANDLE_VALUE;
            error = L"Could not close the staged update file";
            return false;
        }
        handle_ = INVALID_HANDLE_VALUE;
        return true;
    }

    const std::filesystem::path& Path() const { return path_; }
    void MarkCommitted() { committed_ = true; }

private:
    void Close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::filesystem::path path_;
    bool committed_ = false;
};

bool GetPackageExtension(std::wstring_view url, std::wstring& extension) {
    const auto slash = url.find_last_of(L'/');
    if (slash == std::wstring_view::npos || slash + 1 >= url.size()) {
        return false;
    }
    const auto dot = url.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot <= slash || dot + 1 >= url.size()) {
        return false;
    }
    const auto suffix = url.substr(dot);
    if (CompareStringOrdinal(suffix.data(), static_cast<int>(suffix.size()),
                             L".zip", 4, TRUE) == CSTR_EQUAL) {
        extension = L".zip";
        return true;
    }
    if (CompareStringOrdinal(suffix.data(), static_cast<int>(suffix.size()),
                             L".msi", 4, TRUE) == CSTR_EQUAL) {
        extension = L".msi";
        return true;
    }
    return false;
}

bool VerifyExistingStagedFile(const std::filesystem::path& path,
                              const UpdateArtifact& artifact,
                              std::wstring& error) {
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(path, fileError) || fileError) {
        error = L"A staged file with this digest already exists but is invalid";
        return false;
    }
    const auto size = std::filesystem::file_size(path, fileError);
    if (fileError || size != artifact.sizeBytes) {
        error = L"An existing staged file has an unexpected size";
        return false;
    }
    const auto digest = Sha256File(path, error);
    if (!digest || *digest != artifact.sha256) {
        if (error.empty()) {
            error = L"An existing staged file has an unexpected SHA-256";
        }
        return false;
    }
    return true;
}

} // namespace

UpdateDownloader::UpdateDownloader(IUpdateTransport& transport)
    : transport_(transport) {}

bool UpdateDownloader::FetchManifest(std::wstring_view url, std::string& body,
                                     std::wstring& error) {
    error.clear();
    body.clear();
    if (url != kRemoteManifestUrl ||
        !IsAllowedUpdateUrl(url, UpdateUrlPurpose::Manifest)) {
        error = L"Update manifest URL is not the configured trusted endpoint";
        return false;
    }

    const HttpLimits limits{kManifestMaxBytes, 30000, 15000, kMaxRedirects};
    HttpResponse response;
    if (!transport_.Get(url, limits, response, error)) {
        return false;
    }
    if (response.statusCode != 200) {
        error = L"Update manifest request returned a non-success HTTP status";
        return false;
    }
    if (!IsAllowedUpdateUrl(response.finalUrl, UpdateUrlPurpose::Manifest)) {
        error = L"Update manifest redirected outside the trusted endpoint";
        return false;
    }
    if (response.contentLength > limits.maxBytes ||
        response.body.size() > limits.maxBytes) {
        error = L"Update manifest exceeds the configured size limit";
        return false;
    }
    if (response.contentLength != 0 &&
        response.contentLength != response.body.size()) {
        error = L"Update manifest response length is incomplete";
        return false;
    }
    body = std::move(response.body);
    return true;
}

bool UpdateDownloader::DownloadArtifact(
    const UpdateArtifact& artifact,
    const std::filesystem::path& stagingDirectory,
    const std::atomic_bool& cancel, ProgressCallback progress,
    std::filesystem::path& stagedPath, std::wstring& error) {
    error.clear();
    stagedPath.clear();
    if (cancel.load()) {
        error = L"Update download was cancelled";
        return false;
    }
    if (!IsAllowedUpdateUrl(artifact.url, UpdateUrlPurpose::Artifact) ||
        artifact.sizeBytes == 0 || artifact.sizeBytes > kMaxArtifactBytes ||
        !IsLowerHexDigest(artifact.sha256)) {
        error = L"Update artifact URL, size, or SHA-256 is invalid";
        return false;
    }
    const DWORD directoryAttributes = GetFileAttributesW(stagingDirectory.c_str());
    if (directoryAttributes == INVALID_FILE_ATTRIBUTES ||
        (directoryAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (directoryAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = L"Update staging directory is missing or is a reparse point";
        return false;
    }

    std::wstring extension;
    if (!GetPackageExtension(artifact.url, extension)) {
        error = L"Update artifact must be a ZIP or MSI package";
        return false;
    }
    const auto finalPath = stagingDirectory /
                           (artifact.sha256 + extension);
    std::error_code existsError;
    if (std::filesystem::exists(finalPath, existsError)) {
        if (existsError ||
            !VerifyExistingStagedFile(finalPath, artifact, error)) {
            return false;
        }
        stagedPath = finalPath;
        return true;
    }
    if (existsError) {
        error = L"Could not inspect the update staging directory";
        return false;
    }

    PartialFile partial;
    if (!partial.Open(stagingDirectory, error)) {
        return false;
    }

    std::uint64_t receivedBytes = 0;
    std::wstring sinkError;
    const HttpLimits limits{artifact.sizeBytes, kDefaultTotalTimeoutMs,
                            kDefaultReadTimeoutMs, kMaxRedirects};
    const ChunkSink sink = [&](std::span<const std::uint8_t> chunk) {
        if (cancel.load()) {
            sinkError = L"Update download was cancelled";
            return false;
        }
        if (chunk.size() > artifact.sizeBytes - receivedBytes) {
            sinkError = L"Downloaded artifact exceeds its declared size";
            return false;
        }
        if (!partial.Write(chunk, sinkError)) {
            return false;
        }
        receivedBytes += chunk.size();
        return true;
    };

    if (!transport_.Download(artifact.url, limits, sink, progress, error)) {
        if (!sinkError.empty()) {
            error = std::move(sinkError);
        }
        return false;
    }
    if (!sinkError.empty()) {
        error = std::move(sinkError);
        return false;
    }
    if (cancel.load()) {
        error = L"Update download was cancelled";
        return false;
    }
    if (receivedBytes != artifact.sizeBytes) {
        error = L"Downloaded artifact length does not match the manifest";
        return false;
    }
    if (!partial.Finish(error)) {
        return false;
    }

    std::error_code fileError;
    const auto actualSize = std::filesystem::file_size(partial.Path(), fileError);
    if (fileError || actualSize != artifact.sizeBytes) {
        error = L"Staged artifact length does not match the manifest";
        return false;
    }
    const auto digest = Sha256File(partial.Path(), error);
    if (!digest || *digest != artifact.sha256) {
        if (error.empty()) {
            error = L"Staged artifact SHA-256 does not match the manifest";
        }
        return false;
    }

    if (!MoveFileExW(partial.Path().c_str(), finalPath.c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveError = GetLastError();
        if ((moveError == ERROR_ALREADY_EXISTS ||
             moveError == ERROR_FILE_EXISTS) &&
            VerifyExistingStagedFile(finalPath, artifact, error)) {
            stagedPath = finalPath;
            return true;
        }
        if (error.empty()) {
            error = L"Could not atomically commit the verified update package";
        }
        return false;
    }
    partial.MarkCommitted();
    stagedPath = finalPath;
    return true;
}
