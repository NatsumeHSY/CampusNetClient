#pragma once

#include "Config.h"
#include "CredentialStore.h"

#include <atomic>
#include <string>
#include <string_view>

class Logger;

enum class HttpLoginOutcome {
    Success,
    LoginRejected,
    Redirected,
    InvalidResponse,
    TransportError,
    Cancelled,
};

struct HttpLoginResult {
    HttpLoginOutcome outcome{HttpLoginOutcome::TransportError};
    unsigned statusCode{0};
    std::wstring location;
    std::wstring message;
};

struct InternetStatus {
    bool reachable{false};
    unsigned statusCode{0};
    std::wstring endpoint;
    std::wstring publicIp;
};

HttpLoginResult ClassifyLoginResponse(unsigned statusCode,
                                      std::wstring_view location,
                                      std::wstring_view body);
bool IsPortalAlreadyOnline(std::wstring_view body);
InternetStatus ClassifyInternetResponse(unsigned statusCode,
                                         unsigned expectedStatus);
std::wstring ParsePublicIpResponse(std::wstring_view response);

class HttpPortalClient {
public:
    explicit HttpPortalClient(Logger* logger = nullptr);

    HttpLoginResult Login(const AppConfig& config,
                          const Credentials& credentials) const;
    InternetStatus CheckInternet(const AppConfig& config) const;
    void Cancel() noexcept;

private:
    Logger* logger_;
    mutable std::atomic<bool> cancelled_{false};
};
