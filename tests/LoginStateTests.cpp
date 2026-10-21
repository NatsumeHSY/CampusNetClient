#include "LoginService.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

class FakeCredentials final : public ICredentialStore {
public:
    std::optional<Credentials> Read() override { return credentials; }
    bool Write(const Credentials& value) override {
        credentials = value;
        return true;
    }
    bool Clear() override {
        credentials.reset();
        return true;
    }
    std::optional<Credentials> credentials;
};

class FakeTransport final : public ILoginTransport {
public:
    HttpLoginResult Login(const AppConfig&, const Credentials&) override {
        loginCalls.fetch_add(1);
        {
            std::scoped_lock lock(mutex);
            loginEntered = true;
        }
        entered.notify_all();
        if (blockLogin.load()) {
            std::unique_lock lock(mutex);
            release.wait(lock, [this] { return !blockLogin.load(); });
        }
        return loginResult;
    }

    InternetStatus CheckInternet(const AppConfig&) override {
        const auto index = checkCalls.fetch_add(1);
        if (index < internetResults.size()) {
            return internetResults[index];
        }
        return internetResults.empty() ? InternetStatus{}
                                       : internetResults.back();
    }

    void WaitUntilLoginEntered() {
        std::unique_lock lock(mutex);
        entered.wait(lock, [this] { return loginEntered; });
    }

    std::atomic<int> loginCalls{0};
    std::atomic<int> checkCalls{0};
    std::atomic<bool> blockLogin{false};
    HttpLoginResult loginResult{HttpLoginOutcome::Success, 200, {}, L"ok"};
    std::vector<InternetStatus> internetResults;
    std::mutex mutex;
    std::condition_variable entered;
    std::condition_variable release;
    bool loginEntered{false};
};

class FakeBrowserRequester final : public IBrowserFallbackRequester {
public:
    bool HasInteractiveUser() const override { return interactive; }
    bool RequestBrowserLogin(std::wstring_view) override {
        requests++;
        return requestAccepted;
    }

    bool interactive{true};
    bool requestAccepted{true};
    int requests{0};
};

AppConfig TestConfig() {
    AppConfig config = LoadDefaultConfig();
    config.maxLoginAttempts = 1;
    config.verificationAttempts = 1;
    config.retryDelayMs = 5000;
    config.verifyIntervalMs = 0;
    return config;
}

InternetStatus Online() { return {true, 204, L"fixture"}; }
InternetStatus Offline() { return {false, 0, L"fixture"}; }

} // namespace

int RunLoginStateTests() {
    {
        FakeCredentials credentials;
        credentials.credentials = Credentials{L"student", L"secret"};
        FakeTransport transport;
        transport.internetResults = {Offline(), Online()};
        FakeBrowserRequester browser;
        LoginService service(TestConfig(), credentials, transport, browser);
        const auto result = service.RunOnce();
        assert(result.result == LoginRunResult::Succeeded);
        assert(result.status.state == LoginState::Succeeded);
        assert(transport.loginCalls == 1);
    }

    {
        FakeCredentials credentials;
        credentials.credentials = Credentials{L"student", L"secret"};
        FakeTransport transport;
        transport.internetResults = {Offline()};
        transport.loginResult = {HttpLoginOutcome::LoginRejected, 200, {},
                                 L"rejected"};
        FakeBrowserRequester browser;
        browser.interactive = false;
        auto config = TestConfig();
        config.uiAutostart = false;
        LoginService service(config, credentials, transport, browser);
        const auto result = service.RunOnce();
        assert(result.result == LoginRunResult::BrowserFallbackDisabled);
        assert(result.status.state == LoginState::BrowserFallbackDisabled);
        assert(browser.requests == 0);
    }

    {
        FakeCredentials credentials;
        credentials.credentials = Credentials{L"student", L"secret"};
        FakeTransport transport;
        transport.internetResults = {Offline()};
        transport.loginResult = {HttpLoginOutcome::TransportError, 0, {},
                                 L"transport"};
        FakeBrowserRequester browser;
        LoginService service(TestConfig(), credentials, transport, browser);
        const auto result = service.RunOnce();
        assert(result.result == LoginRunResult::BrowserFallbackRequested);
        assert(result.status.state == LoginState::RequestingBrowserFallback);
        assert(browser.requests == 1);
    }

    {
        FakeCredentials credentials;
        FakeTransport transport;
        FakeBrowserRequester browser;
        LoginService service(TestConfig(), credentials, transport, browser);
        const auto result = service.RunOnce();
        assert(result.result == LoginRunResult::CredentialsMissing);
        assert(result.status.state == LoginState::Failed);
    }

    {
        FakeCredentials credentials;
        credentials.credentials = Credentials{L"student", L"secret"};
        FakeTransport transport;
        transport.internetResults = {Offline(), Online()};
        transport.blockLogin = true;
        FakeBrowserRequester browser;
        LoginService service(TestConfig(), credentials, transport, browser);

        LoginRunSummary first;
        std::thread worker([&] { first = service.RunOnce(); });
        transport.WaitUntilLoginEntered();
        const auto duplicate = service.RunOnce();
        assert(duplicate.result == LoginRunResult::DuplicateSuppressed);
        transport.blockLogin = false;
        transport.release.notify_all();
        worker.join();
        assert(first.result == LoginRunResult::Succeeded);
    }

    {
        FakeCredentials credentials;
        credentials.credentials = Credentials{L"student", L"secret"};
        FakeTransport transport;
        transport.internetResults = {Offline()};
        transport.loginResult = {HttpLoginOutcome::TransportError, 0, {},
                                 L"transport"};
        FakeBrowserRequester browser;
        browser.interactive = false;
        auto config = TestConfig();
        config.uiAutostart = false;
        LoginService service(config, credentials, transport, browser);
        const auto first = service.RunOnce();
        const auto second = service.RunOnce();
        assert(first.result == LoginRunResult::BrowserFallbackDisabled);
        assert(second.result == LoginRunResult::BackoffSuppressed);
        assert(transport.loginCalls == 1);
    }
    return 0;
}
