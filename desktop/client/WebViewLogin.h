#pragma once

#include <windows.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

enum class WebViewLoginFailure {
    None,
    UiAutostartDisabled,
    RequestAlreadyActive,
    InvalidRequest,
    MissingLoginForm,
    MissingPasswordField,
    NavigationFailed,
    NavigationTimeout,
    LoginRejected,
    BrowserExited,
    ScriptFailed,
};

struct BrowserFallbackRequest {
    std::wstring requestId;
    std::wstring portalUrl;
    std::wstring username;
    std::wstring password;
    bool uiAutostart{true};
    std::chrono::milliseconds timeout{std::chrono::seconds(30)};
    bool mobile{false};
};

struct WebViewLoginResult {
    bool accepted{false};
    bool success{false};
    bool shouldSubmit{false};
    WebViewLoginFailure failure{WebViewLoginFailure::None};
    std::wstring message;
};

class WebViewLoginCoordinator {
public:
    WebViewLoginResult Begin(const BrowserFallbackRequest& request);
    WebViewLoginResult OnNavigationCompleted(bool navigationSucceeded,
                                              bool loginFormPresent,
                                              bool passwordFieldPresent);
    WebViewLoginResult OnTimeout();
    WebViewLoginResult OnBrowserExited();

    bool Active() const { return active_; }
    const BrowserFallbackRequest& Request() const { return request_; }

    // The returned script is executed only inside WebView2. It is never sent
    // to the logger or to the Service status payload.
    std::wstring BuildAutomationScript() const;

private:
    WebViewLoginResult Finish(bool success, WebViewLoginFailure failure,
                              std::wstring message);

    BrowserFallbackRequest request_;
    bool active_{false};
    bool submitted_{false};
};

using WebViewLoginResultHandler =
    std::function<void(const WebViewLoginResult& result)>;

class WebView2LoginHost {
public:
    WebView2LoginHost();
    ~WebView2LoginHost();

    WebView2LoginHost(const WebView2LoginHost&) = delete;
    WebView2LoginHost& operator=(const WebView2LoginHost&) = delete;

    // Starts an asynchronous WebView2 operation owned by the caller's UI
    // thread. No command-line arguments or credentials are logged.
    bool Start(HWND owner, const BrowserFallbackRequest& request,
               WebViewLoginResultHandler handler, std::wstring& error);
    void Cancel();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

