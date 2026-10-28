#pragma once

#include "UpdateManifest.h"

#include <cstdint>
#include <string>
#include <string_view>

enum class UpdatePhase {
    Idle,
    Checking,
    UpToDate,
    Available,
    Downloading,
    Ready,
    Applying,
    Failed,
};

struct UpdateAgentStatus {
    UpdatePhase phase = UpdatePhase::Idle;
    std::wstring localVersion;
    std::wstring targetVersion;
    UpdateChannel channel = UpdateChannel::Portable;
    std::wstring releaseNotes;
    std::uint64_t receivedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t bytesPerSecond = 0;
    std::uint32_t errorCode = 0;
    std::wstring artifactSha256;
};

std::wstring UpdatePhaseName(UpdatePhase phase);
std::string EncodeUpdateAgentStatus(const UpdateAgentStatus& status);
bool DecodeUpdateStatusPayload(std::string_view payload,
                               UpdateAgentStatus& status,
                               bool includeArtifactSha256,
                               std::wstring& error);
