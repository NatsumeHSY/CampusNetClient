#include "UpdateBroker.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace {

constexpr std::uint32_t kErrorUnauthorized = ERROR_ACCESS_DENIED;
constexpr std::uint32_t kErrorInvalidRequest = ERROR_INVALID_PARAMETER;
constexpr std::uint32_t kErrorUnsupported = ERROR_NOT_SUPPORTED;
constexpr std::uint32_t kErrorNoInteractiveSession = ERROR_NOT_FOUND;
constexpr std::uint32_t kErrorOperationFailed = ERROR_GEN_FAILURE;
constexpr std::uint32_t kErrorNotReady = ERROR_NOT_READY;
constexpr std::uint32_t kErrorInvalidData = ERROR_INVALID_DATA;
constexpr std::uint64_t kMaxArtifactBytes = 1024ull * 1024 * 1024;
constexpr std::size_t kMaxReleaseNotesCharacters = 32 * 1024;

std::wstring EscapeJson(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        switch (character) {
        case L'"':
            result += L"\\\"";
            break;
        case L'\\':
            result += L"\\\\";
            break;
        case L'\b':
            result += L"\\b";
            break;
        case L'\f':
            result += L"\\f";
            break;
        case L'\n':
            result += L"\\n";
            break;
        case L'\r':
            result += L"\\r";
            break;
        case L'\t':
            result += L"\\t";
            break;
        default:
            if (character < 0x20) {
                constexpr wchar_t hex[] = L"0123456789abcdef";
                result += L"\\u00";
                result.push_back(hex[(character >> 4) & 0x0f]);
                result.push_back(hex[character & 0x0f]);
            } else {
                result.push_back(character);
            }
            break;
        }
    }
    return result;
}

std::string ToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                         value.data(),
                                         static_cast<int>(value.size()), nullptr,
                                         0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size,
                            nullptr, nullptr) != size) {
        return {};
    }
    return result;
}

bool IsLowerHexDigest(std::wstring_view digest) {
    return digest.size() == 64 &&
           std::all_of(digest.begin(), digest.end(), [](wchar_t character) {
               return (character >= L'0' && character <= L'9') ||
                      (character >= L'a' && character <= L'f');
           });
}

bool IsEmptyObject(std::string_view payload) {
    while (!payload.empty() &&
           (payload.front() == ' ' || payload.front() == '\t' ||
            payload.front() == '\r' || payload.front() == '\n')) {
        payload.remove_prefix(1);
    }
    while (!payload.empty() &&
           (payload.back() == ' ' || payload.back() == '\t' ||
            payload.back() == '\r' || payload.back() == '\n')) {
        payload.remove_suffix(1);
    }
    return payload == "{}";
}

bool IsPublicUpdateRequest(MessageType type) {
    return type == MessageType::CheckForUpdates ||
           type == MessageType::GetUpdateStatus ||
           type == MessageType::ShowUpdater ||
           type == MessageType::ApplyStagedUpdate;
}

std::wstring ChannelName(UpdateChannel channel) {
    return channel == UpdateChannel::Portable ? L"portable" : L"msi";
}

std::wstring SerializeStatus(const UpdateAgentStatus& status) {
    return L"{\"phase\":\"" + UpdatePhaseName(status.phase) +
           L"\",\"localVersion\":\"" + EscapeJson(status.localVersion) +
           L"\",\"targetVersion\":\"" + EscapeJson(status.targetVersion) +
           L"\",\"channel\":\"" + ChannelName(status.channel) +
           L"\",\"releaseNotes\":\"" + EscapeJson(status.releaseNotes) +
           L"\",\"receivedBytes\":" + std::to_wstring(status.receivedBytes) +
           L",\"totalBytes\":" + std::to_wstring(status.totalBytes) +
           L",\"bytesPerSecond\":" +
           std::to_wstring(status.bytesPerSecond) + L",\"errorCode\":" +
           std::to_wstring(status.errorCode) + L"}";
}

bool SamePathOrdinalIgnoreCase(const std::filesystem::path& left,
                               const std::filesystem::path& right) {
    const std::wstring leftText = left.lexically_normal().native();
    const std::wstring rightText = right.lexically_normal().native();
    return CompareStringOrdinal(leftText.data(), static_cast<int>(leftText.size()),
                                rightText.data(),
                                static_cast<int>(rightText.size()), TRUE) ==
           CSTR_EQUAL;
}

} // namespace

UpdateBroker::UpdateBroker(std::wstring localVersion, UpdateChannel channel,
                           std::filesystem::path stagingDirectory,
                           UpdateBrokerCallbacks callbacks)
    : localVersion_(std::move(localVersion)), channel_(channel),
      stagingDirectory_(std::move(stagingDirectory)),
      callbacks_(std::move(callbacks)) {
    status_.localVersion = localVersion_;
    status_.targetVersion = localVersion_;
    status_.channel = channel_;
}

ProtocolMessage UpdateBroker::Handle(
    const ProtocolMessage& request, bool authorizedInteractiveClient) {
    if (!authorizedInteractiveClient) {
        return ReplyWithError(request, kErrorUnauthorized);
    }
    if (!IsPublicUpdateRequest(request.type)) {
        return ReplyWithError(request, kErrorUnsupported);
    }
    if (!IsEmptyObject(request.payload)) {
        return ReplyWithError(request, kErrorInvalidRequest);
    }

    switch (request.type) {
    case MessageType::GetUpdateStatus:
        return Reply(request);
    case MessageType::CheckForUpdates: {
        bool alreadyPending = false;
        {
            std::scoped_lock lock(mutex_);
            if (checkPending_ || status_.phase == UpdatePhase::Checking ||
                status_.phase == UpdatePhase::Downloading) {
                alreadyPending = true;
            } else {
                checkPending_ = true;
                status_.phase = UpdatePhase::Checking;
                status_.errorCode = 0;
            }
        }
        if (alreadyPending) {
            return Reply(request);
        }
        if (!callbacks_.requestCheck || !callbacks_.requestCheck()) {
            SetError(kErrorOperationFailed);
        }
        return Reply(request);
    }
    case MessageType::ShowUpdater: {
        if (!callbacks_.hasInteractiveSession ||
            !callbacks_.hasInteractiveSession()) {
            SetError(kErrorNoInteractiveSession);
            return Reply(request);
        }
        if (!callbacks_.showUpdater || !callbacks_.showUpdater()) {
            SetError(kErrorOperationFailed);
        } else {
            std::scoped_lock lock(mutex_);
            status_.errorCode = 0;
        }
        return Reply(request);
    }
    case MessageType::ApplyStagedUpdate: {
        std::optional<TrustedUpdatePlan> plan;
        bool isReady = false;
        {
            std::scoped_lock lock(mutex_);
            isReady = status_.phase == UpdatePhase::Ready && trustedPlan_ &&
                      status_.localVersion == trustedPlan_->localVersion &&
                      status_.targetVersion == trustedPlan_->targetVersion &&
                      status_.channel == trustedPlan_->channel &&
                      status_.artifactSha256 == trustedPlan_->sha256;
            if (isReady) {
                plan = trustedPlan_;
            }
        }
        if (!isReady) {
            return ReplyWithError(request, kErrorNotReady);
        }
        if (!callbacks_.applyStagedUpdate ||
            !callbacks_.applyStagedUpdate(*plan)) {
            SetError(kErrorOperationFailed);
        } else {
            std::scoped_lock lock(mutex_);
            status_.phase = UpdatePhase::Applying;
            status_.errorCode = 0;
        }
        return Reply(request);
    }
    default:
        SetError(kErrorUnsupported);
        return Reply(request);
    }
}

bool UpdateBroker::PublishAgentStatus(const UpdateAgentStatus& status) {
    if (!IsValidSemVer(localVersion_) || status.localVersion != localVersion_ ||
        status.channel != channel_ ||
        !IsValidSemVer(status.targetVersion) ||
        status.releaseNotes.size() > kMaxReleaseNotesCharacters ||
        status.receivedBytes > kMaxArtifactBytes ||
        status.totalBytes > kMaxArtifactBytes ||
        (status.totalBytes != 0 && status.receivedBytes > status.totalBytes) ||
        (!status.artifactSha256.empty() &&
         !IsLowerHexDigest(status.artifactSha256))) {
        return false;
    }
    switch (status.phase) {
    case UpdatePhase::Idle:
    case UpdatePhase::Checking:
    case UpdatePhase::UpToDate:
    case UpdatePhase::Available:
    case UpdatePhase::Downloading:
    case UpdatePhase::Ready:
    case UpdatePhase::Failed:
        break;
    case UpdatePhase::Applying:
        return false;
    default:
        return false;
    }

    bool considerLaunchingUpdater = false;
    {
        std::scoped_lock lock(mutex_);
        if (status.phase == UpdatePhase::Ready && !trustedPlan_) {
            status_.phase = UpdatePhase::Failed;
            status_.errorCode = kErrorInvalidData;
            return false;
        }
        if (status.phase == UpdatePhase::Ready &&
            (status.localVersion != trustedPlan_->localVersion ||
             status.targetVersion != trustedPlan_->targetVersion ||
             status.channel != trustedPlan_->channel ||
             status.artifactSha256 != trustedPlan_->sha256 ||
             status.totalBytes != trustedPlan_->sizeBytes)) {
            status_.phase = UpdatePhase::Failed;
            status_.errorCode = kErrorInvalidData;
            return false;
        }
        status_ = status;
        checkPending_ = status.phase == UpdatePhase::Checking ||
                        status.phase == UpdatePhase::Downloading;
        if (status.phase == UpdatePhase::UpToDate ||
            status.phase == UpdatePhase::Failed ||
            status.phase == UpdatePhase::Idle) {
            updaterUiLaunched_ = false;
        } else if ((status.phase == UpdatePhase::Available ||
                    status.phase == UpdatePhase::Downloading ||
                    status.phase == UpdatePhase::Ready) &&
                   !updaterUiLaunched_) {
            considerLaunchingUpdater = true;
        }
    }
    bool tryLaunchUpdater =
        considerLaunchingUpdater && callbacks_.hasInteractiveSession &&
        callbacks_.hasInteractiveSession();
    if (tryLaunchUpdater) {
        std::scoped_lock lock(mutex_);
        if (updaterUiLaunched_ ||
            (status_.phase != UpdatePhase::Available &&
             status_.phase != UpdatePhase::Downloading &&
             status_.phase != UpdatePhase::Ready)) {
            tryLaunchUpdater = false;
        } else {
            updaterUiLaunched_ = true;
        }
    }
    if (tryLaunchUpdater &&
        (!callbacks_.showUpdater || !callbacks_.showUpdater())) {
        std::scoped_lock lock(mutex_);
        updaterUiLaunched_ = false;
    }
    return true;
}

bool UpdateBroker::SetTrustedPlan(const TrustedUpdatePlan& plan,
                                 std::wstring& error) {
    error.clear();
    const auto extension = channel_ == UpdateChannel::Portable ? L".zip" : L".msi";
    const auto expectedPath = stagingDirectory_ / (plan.sha256 + extension);
    if (!IsValidSemVer(localVersion_)) {
        error = L"Installed version is invalid";
        return false;
    }
    if (plan.localVersion != localVersion_) {
        error = L"Plan local version differs from installed version";
        return false;
    }
    if (plan.channel != channel_) {
        error = L"Plan channel differs from installed channel";
        return false;
    }
    if (!IsValidSemVer(plan.targetVersion)) {
        error = L"Plan target version is invalid";
        return false;
    }
    if (!IsLowerHexDigest(plan.sha256)) {
        error = L"Plan digest is invalid";
        return false;
    }
    if (plan.sizeBytes == 0 || plan.sizeBytes > kMaxArtifactBytes) {
        error = L"Plan artifact size is invalid";
        return false;
    }
    if (!SamePathOrdinalIgnoreCase(plan.stagedPath, expectedPath)) {
        error = L"Plan staged path does not match the digest-derived path";
        return false;
    }
    if (!SamePathOrdinalIgnoreCase(plan.stagedPath.parent_path(),
                                   stagingDirectory_)) {
        error = L"Plan staged path is outside the staging directory";
        return false;
    }
    std::scoped_lock lock(mutex_);
    trustedPlan_ = plan;
    return true;
}

std::optional<TrustedUpdatePlan> UpdateBroker::CurrentTrustedPlan() const {
    std::scoped_lock lock(mutex_);
    return trustedPlan_;
}

UpdateAgentStatus UpdateBroker::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return status_;
}

ProtocolMessage UpdateBroker::Reply(const ProtocolMessage& request) const {
    UpdateAgentStatus snapshot;
    {
        std::scoped_lock lock(mutex_);
        snapshot = status_;
    }
    return {kProtocolVersion, request.type, request.requestId,
            ToUtf8(SerializeStatus(snapshot))};
}

ProtocolMessage UpdateBroker::ReplyWithError(const ProtocolMessage& request,
                                              std::uint32_t errorCode) const {
    UpdateAgentStatus snapshot;
    {
        std::scoped_lock lock(mutex_);
        snapshot = status_;
    }
    snapshot.errorCode = errorCode;
    return {kProtocolVersion, request.type, request.requestId,
            ToUtf8(SerializeStatus(snapshot))};
}

void UpdateBroker::SetError(std::uint32_t errorCode, UpdatePhase phase) {
    std::scoped_lock lock(mutex_);
    status_.phase = phase;
    status_.errorCode = errorCode;
    checkPending_ = false;
}
