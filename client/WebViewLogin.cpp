#include "WebViewLogin.h"

#include <algorithm>
#include <string_view>

#if defined(CAMPUSNET_WEBVIEW2_HOST)
#include <wrl.h>
#include <WebView2.h>
#endif

namespace {

std::wstring EscapeJavaScriptString(std::wstring_view value) {
    std::wstring escaped;
    escaped.reserve(value.size() + 8);
    for (const wchar_t ch : value) {
        switch (ch) {
        case L'\\':
            escaped += L"\\\\";
            break;
        case L'"':
            escaped += L"\\\"";
            break;
        case L'\r':
            escaped += L"\\r";
            break;
        case L'\n':
            escaped += L"\\n";
            break;
        case 0x2028:
            escaped += L"\\u2028";
            break;
        case 0x2029:
            escaped += L"\\u2029";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

} // namespace

WebViewLoginResult WebViewLoginCoordinator::Begin(
    const BrowserFallbackRequest& request) {
    if (!request.uiAutostart) {
        return {false, false, false, WebViewLoginFailure::UiAutostartDisabled,
                L"WebView2 fallback is disabled because UI autostart is off"};
    }
    if (active_) {
        return {false, false, false, WebViewLoginFailure::RequestAlreadyActive,
                L"a WebView2 login request is already active"};
    }
    if (request.requestId.empty() || request.portalUrl.empty() ||
        request.username.empty() || request.password.empty() ||
        request.timeout.count() <= 0) {
        return {false, false, false, WebViewLoginFailure::InvalidRequest,
                L"the WebView2 login request is incomplete"};
    }

    request_ = request;
    active_ = true;
    submitted_ = false;
    return {true, false, false, WebViewLoginFailure::None,
            L"WebView2 login request accepted"};
}

WebViewLoginResult WebViewLoginCoordinator::OnNavigationCompleted(
    bool navigationSucceeded, bool loginFormPresent,
    bool passwordFieldPresent) {
    if (!active_) {
        return {false, false, false, WebViewLoginFailure::BrowserExited,
                L"no active WebView2 login request"};
    }
    if (!navigationSucceeded) {
        return Finish(false, WebViewLoginFailure::NavigationFailed,
                     L"WebView2 navigation failed");
    }

    if (!submitted_) {
        if (!loginFormPresent) {
            return Finish(false, WebViewLoginFailure::MissingLoginForm,
                         L"the portal login form was not found");
        }
        if (!passwordFieldPresent) {
            return Finish(false, WebViewLoginFailure::MissingPasswordField,
                         L"the portal password field was not found");
        }
        submitted_ = true;
        return {true, false, true, WebViewLoginFailure::None,
                L"portal form automation submitted"};
    }

    if (loginFormPresent || passwordFieldPresent) {
        return Finish(false, WebViewLoginFailure::LoginRejected,
                     L"the portal still shows the login form");
    }
    return Finish(true, WebViewLoginFailure::None,
                  L"WebView2 login completed and the login form is gone");
}

WebViewLoginResult WebViewLoginCoordinator::OnTimeout() {
    if (!active_) {
        return {false, false, false, WebViewLoginFailure::BrowserExited,
                L"no active WebView2 login request"};
    }
    return Finish(false, WebViewLoginFailure::NavigationTimeout,
                  L"WebView2 login timed out");
}

WebViewLoginResult WebViewLoginCoordinator::OnBrowserExited() {
    if (!active_) {
        return {false, false, false, WebViewLoginFailure::BrowserExited,
                L"no active WebView2 login request"};
    }
    return Finish(false, WebViewLoginFailure::BrowserExited,
                  L"WebView2 browser process exited");
}

std::wstring WebViewLoginCoordinator::BuildAutomationScript() const {
    const std::wstring username = EscapeJavaScriptString(request_.username);
    const std::wstring password = EscapeJavaScriptString(request_.password);
    return L"(function(){"
           L"const f=document.forms['goLoginForm'];"
           L"if(!f)return 'missing-form';"
           L"const u=f.elements['userId']||document.getElementById('userId');"
           L"const p=f.elements['passwd']||document.getElementById('passwd')||"
           L"f.elements['password']||document.getElementById('password');"
           L"if(!u||!p)return 'missing-field';"
           L"u.value=\"" + username + L"\";"
           L"p.value=\"" + password + L"\";"
           L"const t=f.elements['terms']||document.getElementById('terms')||"
           L"document.querySelector(\"input[type=checkbox][name*=term i]\");"
           L"if(t&&!t.checked)t.click();"
           L"if(typeof f.requestSubmit==='function')f.requestSubmit();"
           L"else f.submit();"
           L"return 'submitted';"
           L"})()";
}

WebViewLoginResult WebViewLoginCoordinator::Finish(
    bool success, WebViewLoginFailure failure, std::wstring message) {
    active_ = false;
    return {true, success, false, failure, std::move(message)};
}

struct WebView2LoginHost::Impl {
    WebViewLoginCoordinator coordinator;
    WebViewLoginResultHandler handler;

#if defined(CAMPUSNET_WEBVIEW2_HOST)
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> webView;
    EventRegistrationToken navigationToken{};
#endif

    void Complete(const WebViewLoginResult& result) {
        if (handler) {
            handler(result);
        }
    }
};

#if defined(CAMPUSNET_WEBVIEW2_HOST)
namespace {

bool JsonBoolean(std::wstring_view json, std::wstring_view key) {
    const std::wstring marker = L"\"" + std::wstring(key) + L"\":";
    const auto position = json.find(marker);
    return position != std::wstring_view::npos &&
           json.substr(position + marker.size(), 4) == L"true";
}

std::wstring LocalAppDataPath() {
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (size == 0) {
        return L"";
    }
    std::wstring result(size, L'\0');
    GetEnvironmentVariableW(L"LOCALAPPDATA", result.data(), size);
    result.resize(result.find(L'\0'));
    return result + L"\\CampusNetClient\\WebView2";
}

} // namespace

#endif

WebView2LoginHost::WebView2LoginHost() : impl_(std::make_shared<Impl>()) {}

WebView2LoginHost::~WebView2LoginHost() { Cancel(); }

bool WebView2LoginHost::Start(HWND owner, const BrowserFallbackRequest& request,
                              WebViewLoginResultHandler handler,
                              std::wstring& error) {
    error.clear();
    const auto accepted = impl_->coordinator.Begin(request);
    if (!accepted.accepted) {
        error = accepted.message;
        return false;
    }
    impl_->handler = std::move(handler);

#if defined(CAMPUSNET_WEBVIEW2_HOST)
    if (owner == nullptr) {
        error = L"WebView2 requires an owner window";
        impl_->Complete(impl_->coordinator.OnBrowserExited());
        return false;
    }

    const auto weak = std::weak_ptr<Impl>(impl_);
    const std::wstring userDataFolder = LocalAppDataPath();
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.empty() ? nullptr : userDataFolder.c_str(),
        nullptr,
        Microsoft::WRL::Callback<
            ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [weak, owner](HRESULT result,
                          ICoreWebView2Environment* environment) -> HRESULT {
                const auto impl = weak.lock();
                if (!impl) {
                    return S_OK;
                }
                if (FAILED(result) || environment == nullptr) {
                    impl->Complete(impl->coordinator.OnBrowserExited());
                    return S_OK;
                }
                impl->environment = environment;
                impl->environment->CreateCoreWebView2Controller(
                    owner,
                    Microsoft::WRL::Callback<
                        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [weak](HRESULT controllerResult,
                               ICoreWebView2Controller* controller) -> HRESULT {
                            const auto impl = weak.lock();
                            if (!impl) {
                                return S_OK;
                            }
                            if (FAILED(controllerResult) || controller == nullptr) {
                                impl->Complete(impl->coordinator.OnBrowserExited());
                                return S_OK;
                            }
                            impl->controller = controller;
                            impl->controller->put_IsVisible(FALSE);
                            if (FAILED(impl->controller->get_CoreWebView2(
                                    &impl->webView))) {
                                impl->Complete(impl->coordinator.OnBrowserExited());
                                return S_OK;
                            }
                            impl->webView->add_NavigationCompleted(
                                Microsoft::WRL::Callback<
                                    ICoreWebView2NavigationCompletedEventHandler>(
                                    [weak](ICoreWebView2*,
                                           ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                        const auto impl = weak.lock();
                                        if (!impl) {
                                            return S_OK;
                                        }
                                        BOOL success = FALSE;
                                        if (args != nullptr) {
                                            args->get_IsSuccess(&success);
                                        }
                                        impl->webView->ExecuteScript(
                                            L"JSON.stringify({form:!!document.forms['goLoginForm'],"
                                            L"password:!!(document.forms['goLoginForm']&&"
                                            L"(document.forms['goLoginForm'].elements['passwd']||"
                                            L"document.forms['goLoginForm'].elements['password']||"
                                            L"document.getElementById('passwd')||"
                                            L"document.getElementById('password')))})",
                                            Microsoft::WRL::Callback<
                                                ICoreWebView2ExecuteScriptCompletedHandler>(
                                                [weak, success](HRESULT scriptResult,
                                                               LPCWSTR json) -> HRESULT {
                                                    const auto impl = weak.lock();
                                                    if (!impl) {
                                                        return S_OK;
                                                    }
                                                    if (FAILED(scriptResult) || json == nullptr) {
                                                        impl->Complete(impl->coordinator.OnBrowserExited());
                                                        return S_OK;
                                                    }
                                                    const std::wstring_view value(json);
                                                    const auto result = impl->coordinator.OnNavigationCompleted(
                                                        success != FALSE,
                                                        JsonBoolean(value, L"form"),
                                                        JsonBoolean(value, L"password"));
                                                    if (result.shouldSubmit) {
                                                        const auto script = impl->coordinator.BuildAutomationScript();
                                                        impl->webView->ExecuteScript(
                                                            script.c_str(),
                                                            Microsoft::WRL::Callback<
                                                                ICoreWebView2ExecuteScriptCompletedHandler>(
                                                                [weak](HRESULT submitResult,
                                                                       LPCWSTR) -> HRESULT {
                                                                    const auto impl = weak.lock();
                                                                    if (impl && FAILED(submitResult)) {
                                                                        impl->Complete(impl->coordinator.OnBrowserExited());
                                                                    }
                                                                    return S_OK;
                                                                }).Get());
                                                    } else if (result.failure != WebViewLoginFailure::None ||
                                                               result.success) {
                                                        impl->Complete(result);
                                                    }
                                                    return S_OK;
                                                }).Get());
                                        return S_OK;
                                    }).Get(),
                                &impl->navigationToken);
                            impl->webView->Navigate(
                                impl->coordinator.Request().portalUrl.c_str());
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        error = L"WebView2 environment creation failed";
        impl_->Complete(impl_->coordinator.OnBrowserExited());
        return false;
    }
    return true;
#else
    (void)owner;
    impl_->Complete(impl_->coordinator.OnBrowserExited());
    error = L"WebView2 host is not enabled in this build";
    return false;
#endif
}

void WebView2LoginHost::Cancel() {
    if (!impl_) {
        return;
    }
#if defined(CAMPUSNET_WEBVIEW2_HOST)
    if (impl_->webView && impl_->navigationToken.value != 0) {
        impl_->webView->remove_NavigationCompleted(impl_->navigationToken);
    }
    impl_->webView.Reset();
    impl_->controller.Reset();
    impl_->environment.Reset();
#endif
    impl_->handler = {};
    impl_ = std::make_shared<Impl>();
}

