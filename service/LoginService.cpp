#include "LoginService.h"

#include <thread>
#include <utility>

namespace {

std::chrono::steady_clock::time_point DefaultClock() {
    return std::chrono::steady_clock::now();
}

void DefaultSleeper(int milliseconds) {
    if (milliseconds > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
}

} // namespace

LoginService::LoginService(AppConfig config, ICredentialStore& credentials,
                           ILoginTransport& transport,
                           IBrowserFallbackRequester& browser, Clock clock,
                           Sleeper sleeper)
    : config_(std::move(config)),
      credentials_(credentials),
      transport_(transport),
      browser_(browser),
      state_(true),
      clock_(clock ? std::move(clock) : DefaultClock),
      sleeper_(sleeper ? std::move(sleeper) : DefaultSleeper) {
    state_.SetUiAutostart(config_.uiAutostart);
}

LoginRunSummary LoginService::RunOnce() {
    if (running_.exchange(true)) {
        return {LoginRunResult::DuplicateSuppressed, Status()};
    }

    struct ResetRunning {
        std::atomic<bool>& running;
        ~ResetRunning() { running.store(false); }
    } reset{running_};
    AppConfig config;
    {
        std::scoped_lock lock(mutex_);
        config = config_;
    }

    {
        std::scoped_lock lock(mutex_);
        if (nextRetry_.has_value() && clock_() < *nextRetry_) {
            return {LoginRunResult::BackoffSuppressed, Status()};
        }
    }

    state_.Update(LoginState::WaitingForNetwork, L"checking credentials");
    const auto stored = credentials_.Read();
    if (!stored.has_value() || stored->username.empty() ||
        stored->password.empty()) {
        state_.Update(LoginState::Failed, L"credentials are missing");
        return {LoginRunResult::CredentialsMissing, Status()};
    }

    HttpLoginResult lastLogin;
    for (int attempt = 0; attempt < config.maxLoginAttempts; ++attempt) {
        state_.Update(LoginState::HttpLogin, L"submitting HTTP portal login");
        lastLogin = transport_.Login(config, *stored);
        if (lastLogin.outcome == HttpLoginOutcome::Cancelled) {
            return Finish(LoginRunResult::Failed, LoginState::Failed,
                          L"HTTP portal login cancelled");
        }
        if (lastLogin.outcome != HttpLoginOutcome::Success) {
            if (attempt + 1 < config.maxLoginAttempts) {
                sleeper_(config.retryDelayMs);
            }
            continue;
        }
        std::scoped_lock lock(mutex_);
        nextRetry_.reset();
        const std::wstring message = lastLogin.message.empty()
                                         ? L"HTTP portal login succeeded"
                                         : lastLogin.message;
        return Finish(LoginRunResult::Succeeded, LoginState::Succeeded,
                      message);
    }

    return FallbackAfterHttpFailure();
}

LoginRunSummary LoginService::OnNetworkChanged() {
    {
        std::scoped_lock lock(mutex_);
        nextRetry_.reset();
    }
    return RunOnce();
}

void LoginService::OnBrowserLoginResult(bool success, std::wstring_view message) {
    std::scoped_lock lock(mutex_);
    if (!browserRequestPending_) {
        return;
    }
    browserRequestPending_ = false;
    nextRetry_.reset();
    if (success) {
        state_.Update(LoginState::Succeeded, message.empty()
                                             ? L"browser login succeeded"
                                             : message);
    } else {
        state_.Update(LoginState::Failed, message.empty()
                                         ? L"browser login failed"
                                         : message);
    }
}

ServiceStatus LoginService::Status() const { return state_.Snapshot(); }

void LoginService::SetConfig(AppConfig config) {
    std::scoped_lock lock(mutex_);
    config_ = std::move(config);
    state_.SetUiAutostart(config_.uiAutostart);
}

void LoginService::SetUiConnected(bool connected) {
    state_.SetUiConnected(connected);
}

void LoginService::SetInternetStatus(bool available,
                                     std::wstring_view publicIp,
                                     unsigned statusCode) {
    state_.SetInternetStatus(available, publicIp, statusCode);
}

LoginRunSummary LoginService::Finish(LoginRunResult result, LoginState state,
                                     std::wstring_view message) {
    state_.Update(state, message);
    return {result, Status()};
}

LoginRunSummary LoginService::FallbackAfterHttpFailure() {
    bool uiAutostart = false;
    {
        std::scoped_lock lock(mutex_);
        uiAutostart = config_.uiAutostart;
    }

    if (!uiAutostart || !browser_.HasInteractiveUser()) {
        ScheduleBackoff();
        return Finish(LoginRunResult::BrowserFallbackDisabled,
                      LoginState::BrowserFallbackDisabled,
                      L"HTTP login failed and browser fallback is disabled");
    }

    const std::wstring requestId = NextRequestId();
    state_.Update(LoginState::RequestingBrowserFallback,
                  L"requesting browser fallback from UI");
    if (!browser_.RequestBrowserLogin(requestId)) {
        ScheduleBackoff();
        return Finish(LoginRunResult::BrowserFallbackFailed, LoginState::Failed,
                      L"UI did not accept browser fallback request");
    }

    {
        std::scoped_lock lock(mutex_);
        browserRequestPending_ = true;
    }
    return {LoginRunResult::BrowserFallbackRequested, Status()};
}

std::wstring LoginService::NextRequestId() {
    return L"browser-" + std::to_wstring(++requestSequence_);
}

void LoginService::ScheduleBackoff() {
    std::scoped_lock lock(mutex_);
    nextRetry_ = clock_() + std::chrono::milliseconds(config_.retryDelayMs);
}
