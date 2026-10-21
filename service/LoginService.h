#pragma once

#include "Config.h"
#include "CredentialStore.h"
#include "HttpPortalClient.h"
#include "ServiceState.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

class ILoginTransport {
public:
    virtual ~ILoginTransport() = default;

    virtual HttpLoginResult Login(const AppConfig& config,
                                  const Credentials& credentials) = 0;
    virtual InternetStatus CheckInternet(const AppConfig& config) = 0;
};

class IBrowserFallbackRequester {
public:
    virtual ~IBrowserFallbackRequester() = default;

    virtual bool HasInteractiveUser() const = 0;
    virtual bool RequestBrowserLogin(std::wstring_view requestId) = 0;
};

enum class LoginRunResult {
    Succeeded,
    CredentialsMissing,
    BrowserFallbackRequested,
    BrowserFallbackDisabled,
    BrowserFallbackFailed,
    DuplicateSuppressed,
    BackoffSuppressed,
    Failed,
};

struct LoginRunSummary {
    LoginRunResult result{LoginRunResult::Failed};
    ServiceStatus status;
};

class LoginService {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using Sleeper = std::function<void(int)>;

    LoginService(AppConfig config, ICredentialStore& credentials,
                 ILoginTransport& transport,
                 IBrowserFallbackRequester& browser,
                 Clock clock = {}, Sleeper sleeper = {});

    LoginRunSummary RunOnce();
    LoginRunSummary OnNetworkChanged();
    void OnBrowserLoginResult(bool success, std::wstring_view message);

    ServiceStatus Status() const;
    void SetConfig(AppConfig config);
    void SetUiConnected(bool connected);

private:
    LoginRunSummary Finish(LoginRunResult result, LoginState state,
                           std::wstring_view message);
    LoginRunSummary FallbackAfterHttpFailure();
    std::wstring NextRequestId();
    void ScheduleBackoff();

    mutable std::mutex mutex_;
    AppConfig config_;
    ICredentialStore& credentials_;
    ILoginTransport& transport_;
    IBrowserFallbackRequester& browser_;
    ServiceState state_;
    Clock clock_;
    Sleeper sleeper_;
    std::optional<std::chrono::steady_clock::time_point> nextRetry_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> requestSequence_{0};
    bool browserRequestPending_{false};
};
