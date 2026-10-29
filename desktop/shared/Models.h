#pragma once

#include <string>

enum class LoginState {
    Idle,
    WaitingForNetwork,
    CheckingInternet,
    HttpLogin,
    VerifyingInternet,
    Succeeded,
    Failed,
};

struct ServiceStatus {
    LoginState state{LoginState::Idle};
    bool serviceRunning{false};
    bool uiConnected{false};
    bool internetStatusKnown{false};
    bool internetAvailable{false};
    unsigned internetStatusCode{0};
    std::wstring publicIp;
    std::wstring lastMessage;
    unsigned long long lastChangeUnixMs{0};
};
