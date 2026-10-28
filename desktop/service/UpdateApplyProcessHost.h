#pragma once

#include "UpdateBroker.h"

#include <filesystem>
#include <string>

class WindowsUpdateApplyProcessHost final {
public:
    WindowsUpdateApplyProcessHost(std::filesystem::path programDataRoot,
                                  std::filesystem::path installedUpdater,
                                  std::filesystem::path stagingDirectory);

    bool PrepareInteractiveGui(std::filesystem::path& copiedUpdater,
                               std::wstring& error) const;
    bool LaunchApply(const TrustedUpdatePlan& plan,
                     const std::filesystem::path& installRoot,
                     std::filesystem::path& copiedUpdater,
                     std::wstring& error) const;

private:
    std::filesystem::path programDataRoot_;
    std::filesystem::path installedUpdater_;
    std::filesystem::path stagingDirectory_;
};
