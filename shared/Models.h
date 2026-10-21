#pragma once

#include <string>

enum class LoginState {
    Idle,
    WaitingForNetwork,
    CheckingInternet,
    HttpLogin,
    RequestingBrowserFallback,
    BrowserLogin,
    VerifyingInternet,
    Succeeded,
    Failed,
    BrowserFallbackDisabled,
};

struct ServiceStatus {
    LoginState state{LoginState::Idle};
    bool serviceRunning{false};
    bool uiConnected{false};
    bool uiAutostart{true};
    bool internetAvailable{false};
    std::wstring lastMessage;
    unsigned long long lastChangeUnixMs{0};
};
