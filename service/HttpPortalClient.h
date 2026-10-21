#pragma once

#include "Config.h"
#include "CredentialStore.h"

#include <string>
#include <string_view>

class Logger;

enum class HttpLoginOutcome {
    Success,
    LoginRejected,
    Redirected,
    InvalidResponse,
    TransportError,
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
};

HttpLoginResult ClassifyLoginResponse(unsigned statusCode,
                                      std::wstring_view location,
                                      std::wstring_view body);
InternetStatus ClassifyInternetResponse(unsigned statusCode,
                                         unsigned expectedStatus);

class HttpPortalClient {
public:
    explicit HttpPortalClient(Logger* logger = nullptr);

    HttpLoginResult Login(const AppConfig& config,
                          const Credentials& credentials) const;
    InternetStatus CheckInternet(const AppConfig& config) const;

private:
    Logger* logger_;
};
