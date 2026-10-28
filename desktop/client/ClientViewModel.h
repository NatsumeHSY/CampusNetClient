#pragma once

#include "Config.h"

#include <optional>
#include <string>

class ClientViewModel {
public:
    explicit ClientViewModel(AppConfig config);

    bool UiAutostart() const;
    bool WebViewFallbackEnabled() const;
    std::wstring WebViewFallbackNotice() const;
    void SetUiAutostart(bool enabled);
    bool CanRequestLogin(std::optional<bool> internetReachable,
                         bool loginSucceeded,
                         bool loginInProgress = false) const;

    const AppConfig& Config() const;

private:
    AppConfig config_;
};
