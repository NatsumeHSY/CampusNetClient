#pragma once

#include "Config.h"

#include <string>

class ClientViewModel {
public:
    explicit ClientViewModel(AppConfig config);

    bool UiAutostart() const;
    bool WebViewFallbackEnabled() const;
    std::wstring WebViewFallbackNotice() const;
    void SetUiAutostart(bool enabled);

    const AppConfig& Config() const;

private:
    AppConfig config_;
};
