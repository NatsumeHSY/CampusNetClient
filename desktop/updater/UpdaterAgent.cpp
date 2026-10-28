#include "UpdaterAgent.h"

#include "ClientIpc.h"
#include "UpdateAgentStatus.h"
#include "UpdateDownloader.h"
#include "UpdateIntegrity.h"
#include "WinHttpUpdateTransport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <WinHttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr std::size_t kMaxLocalVersionBytes = 64 * 1024;
constexpr std::size_t kMaxReleaseNoteCharacters = 4096;
constexpr auto kNormalCheckPeriod = std::chrono::hours(6);
constexpr auto kNormalJitter = std::chrono::minutes(15);
constexpr std::uint64_t kRetryBaseMinutes = 5;
constexpr std::uint64_t kRetryMaximumMinutes = 6 * 60;
constexpr DWORD kPipeCallTimeoutMs = 2500;

std::filesystem::path ModuleDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

bool ReadLocalInstallInfo(const std::filesystem::path& path,
                          LocalInstallInfo& local, std::wstring& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = L"Local version record could not be opened";
        return false;
    }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length <= 0 ||
        static_cast<std::uint64_t>(length) > kMaxLocalVersionBytes) {
        error = L"Local version record has an invalid size";
        return false;
    }
    input.seekg(0, std::ios::beg);
    std::string json(static_cast<std::size_t>(length), '\0');
    if (!input.read(json.data(), static_cast<std::streamsize>(json.size()))) {
        error = L"Local version record could not be read";
        return false;
    }
    return ParseLocalInstallInfo(json, local, error);
}

std::wstring TruncateReleaseNotes(std::wstring notes) {
    if (notes.size() > kMaxReleaseNoteCharacters) {
        notes.resize(kMaxReleaseNoteCharacters);
        if (!notes.empty() && notes.back() >= 0xd800 && notes.back() <= 0xdbff) {
            notes.pop_back();
        }
    }
    return notes;
}

class UpdaterAgent final {
public:
    explicit UpdaterAgent(std::filesystem::path stagingDirectory)
        : stagingDirectory_(std::move(stagingDirectory)) {}

    int Run(HANDLE stopEvent, HANDLE checkEvent) {
        if (stopEvent == nullptr || checkEvent == nullptr ||
            stopEvent == INVALID_HANDLE_VALUE ||
            checkEvent == INVALID_HANDLE_VALUE ||
            !stagingDirectory_.is_absolute()) {
            return ERROR_INVALID_PARAMETER;
        }
        const auto attributes = GetFileAttributesW(stagingDirectory_.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return ERROR_PATH_NOT_FOUND;
        }
        const auto installDirectory = ModuleDirectory();
        if (installDirectory.empty()) {
            return ERROR_BAD_PATHNAME;
        }
        updaterDirectory_ = installDirectory;
        std::wstring error;
        if (!ReadLocalInstallInfo(updaterDirectory_ / L"version.json", local_,
                                  error)) {
            return ERROR_INVALID_DATA;
        }
        installDirectory_ = local_.channel == UpdateChannel::Portable
            ? updaterDirectory_.parent_path() : updaterDirectory_;
        if (installDirectory_.empty()) {
            return ERROR_BAD_PATHNAME;
        }

        CheckNow();
        unsigned consecutiveFailures = lastCheckSucceeded_ ? 0 : 1;
        std::mt19937 random(std::random_device{}());
        while (true) {
            const DWORD timeout = static_cast<DWORD>(NextDelay(
                consecutiveFailures, random).count());
            HANDLE events[]{stopEvent, checkEvent};
            const DWORD wait = WaitForMultipleObjects(2, events, FALSE, timeout);
            if (wait == WAIT_OBJECT_0) {
                return ERROR_SUCCESS;
            }
            if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_TIMEOUT) {
                CheckNow();
                if (lastCheckSucceeded_) {
                    consecutiveFailures = 0;
                } else {
                    consecutiveFailures = std::min(consecutiveFailures + 1, 16u);
                }
                continue;
            }
            return GetLastError() == ERROR_SUCCESS ? ERROR_GEN_FAILURE
                                                    : GetLastError();
        }
    }

private:
    std::chrono::milliseconds NextDelay(unsigned failures,
                                        std::mt19937& random) const {
        std::uniform_int_distribution<std::int64_t> normalJitter(
            0, std::chrono::duration_cast<std::chrono::milliseconds>(
                   kNormalJitter).count());
        std::uniform_int_distribution<std::int64_t> retryJitter(0, 30000);
        if (failures == 0) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       kNormalCheckPeriod) +
                   std::chrono::milliseconds(normalJitter(random));
        }
        const auto exponent = std::min(failures - 1, 6u);
        const auto retryMinutes = std::min<std::uint64_t>(
            kRetryBaseMinutes * (std::uint64_t{1} << exponent),
            kRetryMaximumMinutes);
        const std::chrono::minutes retry(
            static_cast<std::chrono::minutes::rep>(retryMinutes));
        return std::chrono::duration_cast<std::chrono::milliseconds>(retry) +
               std::chrono::milliseconds(retryJitter(random));
    }

    bool Publish(const UpdateAgentStatus& status) {
        const std::string payload = EncodeUpdateAgentStatus(status);
        if (payload.empty() || payload.size() > kMaxProtocolMessageBytes / 2) {
            return false;
        }
        const auto requestNumber = ++requestCounter_;
        const std::wstring requestId =
            L"agent-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(requestNumber);
        ProtocolMessage request{kProtocolVersion,
                                MessageType::UpdateAgentStatus,
                                requestId,
                                payload};
        std::wstring error;
        const auto reply = client_.Call(request, kPipeCallTimeoutMs, error);
        return reply.has_value() && reply->type == request.type &&
               reply->requestId == request.requestId;
    }

    UpdateAgentStatus NewStatus(UpdatePhase phase) const {
        UpdateAgentStatus status;
        status.phase = phase;
        status.localVersion = local_.version;
        status.targetVersion = local_.version;
        status.channel = local_.channel;
        return status;
    }

    void CheckNow() {
        auto status = NewStatus(UpdatePhase::Checking);
        Publish(status);

        UpdateDownloader downloader(transport_);
        std::string manifestJson;
        std::wstring error;
        if (!downloader.FetchManifest(kRemoteManifestUrl, manifestJson, error)) {
            PublishFailure(status, ERROR_WINHTTP_CANNOT_CONNECT);
            lastCheckSucceeded_ = false;
            return;
        }
        RemoteUpdateManifest remote;
        if (!ParseRemoteUpdateManifest(manifestJson, remote, error)) {
            PublishFailure(status, ERROR_INVALID_DATA);
            lastCheckSucceeded_ = false;
            return;
        }

        const UpdateArtifact& artifact =
            local_.channel == UpdateChannel::Portable ? remote.portable
                                                      : remote.msi;
        const bool versionMismatch = remote.version != local_.version;
        if (!versionMismatch &&
            VerifyManagedFiles(installDirectory_, artifact, error)) {
            status.phase = UpdatePhase::UpToDate;
            status.targetVersion = local_.version;
            status.releaseNotes = TruncateReleaseNotes(remote.releaseNotes);
            Publish(status);
            lastCheckSucceeded_ = true;
            return;
        }

        status.phase = UpdatePhase::Available;
        status.targetVersion = remote.version;
        status.releaseNotes = TruncateReleaseNotes(remote.releaseNotes);
        status.totalBytes = artifact.sizeBytes;
        status.artifactSha256 = artifact.sha256;
        if (artifact.sizeBytes == 0 || artifact.sha256.empty()) {
            PublishFailure(status, ERROR_INVALID_DATA);
            lastCheckSucceeded_ = false;
            return;
        }
        Publish(status);

        status.phase = UpdatePhase::Downloading;
        status.receivedBytes = 0;
        Publish(status);
        std::filesystem::path stagedPath;
        auto lastPublish = std::chrono::steady_clock::now();
        const bool downloaded = downloader.DownloadArtifact(
            artifact, stagingDirectory_, cancel_,
            [&](std::uint64_t received, std::uint64_t total,
                std::uint64_t bytesPerSecond) {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastPublish < std::chrono::milliseconds(400) &&
                    received != total) {
                    return;
                }
                lastPublish = now;
                status.receivedBytes = received;
                status.totalBytes = total;
                status.bytesPerSecond = bytesPerSecond;
                Publish(status);
            },
            stagedPath, error);
        if (!downloaded) {
            PublishFailure(status, ERROR_GEN_FAILURE);
            lastCheckSucceeded_ = false;
            return;
        }
        status.phase = UpdatePhase::Ready;
        status.receivedBytes = artifact.sizeBytes;
        status.totalBytes = artifact.sizeBytes;
        status.bytesPerSecond = 0;
        Publish(status);
        lastCheckSucceeded_ = true;
    }

    void PublishFailure(UpdateAgentStatus status, DWORD errorCode) {
        status.phase = UpdatePhase::Failed;
        status.errorCode = errorCode;
        status.receivedBytes = 0;
        status.bytesPerSecond = 0;
        Publish(status);
    }

    ServiceIpcClient client_;
    WinHttpUpdateTransport transport_;
    std::filesystem::path stagingDirectory_;
    std::filesystem::path updaterDirectory_;
    std::filesystem::path installDirectory_;
    LocalInstallInfo local_;
    std::atomic_bool cancel_{false};
    bool lastCheckSucceeded_ = false;
    std::uint64_t requestCounter_ = 0;
};

bool ReadEnvironmentValue(std::wstring_view name, std::wstring& value) {
    const std::wstring variable(name);
    DWORD capacity = 256;
    while (capacity <= 32768) {
        std::wstring buffer(capacity, L'\0');
        const DWORD length = GetEnvironmentVariableW(variable.c_str(),
                                                     buffer.data(), capacity);
        if (length == 0) {
            return false;
        }
        if (length < capacity) {
            buffer.resize(length);
            value = std::move(buffer);
            return true;
        }
        capacity = length + 1;
    }
    return false;
}

bool ReadInheritedHandle(std::wstring_view name, HANDLE& handle) {
    std::wstring value;
    if (!ReadEnvironmentValue(name, value) || value.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const unsigned long long raw = std::wcstoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != L'\0' || raw == 0 ||
        raw > std::numeric_limits<std::uintptr_t>::max()) {
        return false;
    }
    handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(raw));
    DWORD flags = 0;
    return GetHandleInformation(handle, &flags) != FALSE;
}

} // namespace

int RunUpdaterAgent(HANDLE stopEvent, HANDLE checkEvent,
                    const std::filesystem::path& stagingDirectory) {
    UpdaterAgent agent(stagingDirectory);
    return agent.Run(stopEvent, checkEvent);
}

int RunUpdaterAgentFromEnvironment() {
    HANDLE stopEvent = nullptr;
    HANDLE checkEvent = nullptr;
    std::wstring staging;
    if (!ReadInheritedHandle(L"CAMPUSNET_UPDATE_STOP_EVENT", stopEvent) ||
        !ReadInheritedHandle(L"CAMPUSNET_UPDATE_CHECK_EVENT", checkEvent) ||
        !ReadEnvironmentValue(L"CAMPUSNET_UPDATE_STAGING", staging)) {
        return ERROR_INVALID_PARAMETER;
    }
    return RunUpdaterAgent(stopEvent, checkEvent, std::filesystem::path(staging));
}
