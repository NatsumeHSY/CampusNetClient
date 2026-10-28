#pragma once

#include "UpdateManifest.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

struct PortableUpdateResult {
    bool success = false;
    bool rolledBack = false;
    std::filesystem::path recoveryDirectory;
    std::wstring error;
};

class IPortableUpdateHost {
public:
    virtual ~IPortableUpdateHost() = default;
    virtual bool QueryServiceInstallation(bool& installed,
                                          std::wstring& error) = 0;
    virtual bool CloseClientProcesses(std::wstring& error) = 0;
    virtual bool StopService(std::wstring& error) = 0;
    virtual bool StartService(std::wstring& error) = 0;
    virtual bool WaitForServiceRunning(std::chrono::milliseconds timeout,
                                       std::wstring& error) = 0;
};

class PortableUpdateApply final {
public:
    static PortableUpdateResult Apply(
        const std::filesystem::path& stagedArchive,
        const std::filesystem::path& derivedInstallRoot,
        std::wstring_view targetVersion,
        const UpdateArtifact& artifact,
        IPortableUpdateHost& host);
};
