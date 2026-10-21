#include "ClientViewModel.h"

#include <utility>

ClientViewModel::ClientViewModel(AppConfig config)
    : config_(std::move(config)) {}

bool ClientViewModel::UiAutostart() const { return config_.uiAutostart; }

bool ClientViewModel::WebViewFallbackEnabled() const {
    return config_.uiAutostart;
}

std::wstring ClientViewModel::WebViewFallbackNotice() const {
    if (WebViewFallbackEnabled()) {
        return L"UI autostart is enabled; WebView2 fallback is available.";
    }
    return L"UI autostart is disabled; WebView2 fallback will not run automatically.";
}

void ClientViewModel::SetUiAutostart(bool enabled) {
    config_.uiAutostart = enabled;
}

const AppConfig& ClientViewModel::Config() const { return config_; }
