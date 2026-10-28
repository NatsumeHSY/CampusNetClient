#include "WebViewLogin.h"

#include <cassert>

namespace {

BrowserFallbackRequest Request(bool uiAutostart = true) {
    return {L"browser-test", L"http://127.0.0.1:18080/", L"alice",
            L"p\\\"ass", uiAutostart, std::chrono::seconds(5)};
}

void TestUiAutostartDisabled() {
    WebViewLoginCoordinator coordinator;
    const auto result = coordinator.Begin(Request(false));
    assert(!result.accepted);
    assert(result.failure == WebViewLoginFailure::UiAutostartDisabled);
}

void TestDuplicateRequestRejection() {
    WebViewLoginCoordinator coordinator;
    assert(coordinator.Begin(Request()).accepted);
    const auto result = coordinator.Begin(Request());
    assert(!result.accepted);
    assert(result.failure == WebViewLoginFailure::RequestAlreadyActive);
}

void TestSuccessfulResultMapping() {
    WebViewLoginCoordinator coordinator;
    assert(coordinator.Begin(Request()).accepted);

    const auto submitted = coordinator.OnNavigationCompleted(true, true, true);
    assert(submitted.accepted);
    assert(!submitted.success);
    assert(submitted.shouldSubmit);

    const auto completed = coordinator.OnNavigationCompleted(true, false, false);
    assert(completed.accepted);
    assert(completed.success);
    assert(completed.failure == WebViewLoginFailure::None);
    assert(!coordinator.Active());
}

void TestMissingPasswordField() {
    WebViewLoginCoordinator coordinator;
    assert(coordinator.Begin(Request()).accepted);
    const auto result = coordinator.OnNavigationCompleted(true, true, false);
    assert(!result.success);
    assert(result.failure == WebViewLoginFailure::MissingPasswordField);
}

void TestNavigationTimeout() {
    WebViewLoginCoordinator coordinator;
    assert(coordinator.Begin(Request()).accepted);
    const auto result = coordinator.OnTimeout();
    assert(!result.success);
    assert(result.failure == WebViewLoginFailure::NavigationTimeout);
    assert(!coordinator.Active());
}

void TestBrowserExit() {
    WebViewLoginCoordinator coordinator;
    assert(coordinator.Begin(Request()).accepted);
    const auto result = coordinator.OnBrowserExited();
    assert(!result.success);
    assert(result.failure == WebViewLoginFailure::BrowserExited);
}

void TestAutomationScriptTargetsRecoveredForm() {
    WebViewLoginCoordinator coordinator;
    assert(coordinator.Begin(Request()).accepted);
    const auto script = coordinator.BuildAutomationScript();
    assert(script.find(L"goLoginForm") != std::wstring::npos);
    assert(script.find(L"requestSubmit") != std::wstring::npos);
    assert(script.find(L"p\\\\\\\"ass") != std::wstring::npos);
}

} // namespace

void RunWebViewRequestTests() {
    TestUiAutostartDisabled();
    TestDuplicateRequestRejection();
    TestSuccessfulResultMapping();
    TestMissingPasswordField();
    TestNavigationTimeout();
    TestBrowserExit();
    TestAutomationScriptTargetsRecoveredForm();
}

