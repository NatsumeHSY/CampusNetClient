#include "HttpPortalClient.h"
#include "LoginService.h"
#include "NetworkMonitor.h"
#include "ServiceIpc.h"
#include "UpdateAgentIpc.h"
#include "UpdateApplyProcessHost.h"
#include "UpdateBroker.h"
#include "UpdaterProcessSupervisor.h"

#include "Config.h"
#include "CredentialStore.h"
#include "Logger.h"
#include "UserSessionLauncher.h"
#include "UpdateManifest.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace {

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};

std::wstring JsonEscape(std::wstring value) {
    std::size_t position = 0;
    while ((position = value.find(L'\\', position)) != std::wstring::npos) {
        value.replace(position, 1, L"\\\\");
        position += 2;
    }
    position = 0;
    while ((position = value.find(L'"', position)) != std::wstring::npos) {
        value.replace(position, 1, L"\\\"");
        position += 2;
    }
    return value;
}

std::wstring ExtractString(std::string_view payload, std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\":\"";
    const std::size_t start = payload.find(marker);
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t valueStart = start + marker.size();
    const std::size_t valueEnd = payload.find('"', valueStart);
    if (valueEnd == std::string_view::npos) {
        return {};
    }
    const std::string value(payload.substr(valueStart, valueEnd - valueStart));
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

bool ExtractBool(std::string_view payload, std::string_view key, bool& value) {
    const std::string marker = "\"" + std::string(key) + "\":";
    const std::size_t start = payload.find(marker);
    if (start == std::string_view::npos) {
        return false;
    }
    const auto valueStart = start + marker.size();
    if (payload.substr(valueStart, 4) == "true") {
        value = true;
        return true;
    }
    if (payload.substr(valueStart, 5) == "false") {
        value = false;
        return true;
    }
    return false;
}

const wchar_t* StateName(LoginState state) {
    switch (state) {
    case LoginState::Idle:
        return L"Idle";
    case LoginState::WaitingForNetwork:
        return L"WaitingForNetwork";
    case LoginState::CheckingInternet:
        return L"CheckingInternet";
    case LoginState::HttpLogin:
        return L"HttpLogin";
    case LoginState::RequestingBrowserFallback:
        return L"RequestingBrowserFallback";
    case LoginState::BrowserLogin:
        return L"BrowserLogin";
    case LoginState::VerifyingInternet:
        return L"VerifyingInternet";
    case LoginState::Succeeded:
        return L"Succeeded";
    case LoginState::Failed:
        return L"Failed";
    case LoginState::BrowserFallbackDisabled:
        return L"BrowserFallbackDisabled";
    }
    return L"Unknown";
}

std::wstring StatusPayload(const ServiceStatus& status) {
    return std::wstring(L"{\"state\":\"") + StateName(status.state) +
           L"\",\"serviceRunning\":" +
           std::wstring(status.serviceRunning ? L"true" : L"false") +
           L",\"uiConnected\":" +
           std::wstring(status.uiConnected ? L"true" : L"false") +
           L",\"uiAutostart\":" +
           std::wstring(status.uiAutostart ? L"true" : L"false") +
           L",\"internetAvailable\":" +
           std::wstring(status.internetAvailable ? L"true" : L"false") +
           L",\"internetStatusKnown\":" +
           std::wstring(status.internetStatusKnown ? L"true" : L"false") +
           L",\"publicIp\":\"" + JsonEscape(status.publicIp) + L"\"" +
           L",\"message\":\"" + JsonEscape(status.lastMessage) + L"\"}";
}

std::string ToUtf8(std::wstring_view value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::optional<LocalInstallInfo> LoadLocalInstallInfo(
    const std::filesystem::path& path, std::wstring& error) {
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = L"local version.json could not be opened";
        return std::nullopt;
    }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length <= 0 || length > 64 * 1024) {
        error = L"local version.json has an invalid size";
        return std::nullopt;
    }
    input.seekg(0, std::ios::beg);
    std::string json(static_cast<std::size_t>(length), '\0');
    if (!input.read(json.data(), static_cast<std::streamsize>(json.size()))) {
        error = L"local version.json could not be read";
        return std::nullopt;
    }
    LocalInstallInfo local;
    if (!ParseLocalInstallInfo(json, local, error)) {
        return std::nullopt;
    }
    return local;
}

std::filesystem::path ProgramDataPath() {
    PWSTR rawPath = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr,
                                                 &rawPath);
    if (FAILED(result) || rawPath == nullptr) {
        return {};
    }
    std::filesystem::path path(rawPath);
    CoTaskMemFree(rawPath);
    return path;
}

class HttpTransport final : public ILoginTransport {
public:
    explicit HttpTransport(Logger& logger) : client_(&logger) {}

    HttpLoginResult Login(const AppConfig& config,
                          const Credentials& credentials) override {
        return client_.Login(config, credentials);
    }

    InternetStatus CheckInternet(const AppConfig& config) override {
        return client_.CheckInternet(config);
    }

    void Cancel() noexcept { client_.Cancel(); }

private:
    HttpPortalClient client_;
};

class BrowserRequester final : public IBrowserFallbackRequester {
public:
    bool HasInteractiveUser() const override {
        return WTSGetActiveConsoleSessionId() != 0xFFFFFFFF;
    }

    bool RequestBrowserLogin(std::wstring_view) override {
        if (connected_.load()) {
            return true;
        }
        if (launchRequested_.exchange(true)) {
            return true;
        }

        wchar_t modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, modulePath,
                                                ARRAYSIZE(modulePath));
        if (length == 0 || length >= ARRAYSIZE(modulePath)) {
            launchRequested_.store(false);
            return false;
        }
        const std::filesystem::path uiPath =
            std::filesystem::path(modulePath).parent_path() /
            L"CampusNetClient.exe";
        std::wstring error;
        if (!UserSessionLauncher::LaunchInActiveSession(uiPath, error)) {
            launchRequested_.store(false);
            return false;
        }
        return true;
    }

    void SetConnected(bool connected) {
        connected_.store(connected);
        if (!connected) {
            launchRequested_.store(false);
        }
    }

private:
    std::atomic<bool> connected_{false};
    std::atomic<bool> launchRequested_{false};
};

class ServiceRuntime {
public:
    ServiceRuntime()
        : config_(LoadConfig(ConfigPath())),
          logger_(ConfigPath().parent_path() / L"logs" / L"service.log"),
          transport_(logger_),
          login_(config_, credentials_, transport_, browser_),
          updaterDirectory_(ConfigPath().parent_path()),
          stagingDirectory_(ProgramDataPath() / L"CampusNetClient" /
                            L"UpdateStaging"),
          updaterProcessHost_(stagingDirectory_),
          updaterSupervisor_(updaterProcessHost_,
                             updaterDirectory_ / L"CampusNetUpdater.exe"),
          updateApplyProcessHost_(ProgramDataPath() / L"CampusNetClient",
                                  updaterDirectory_ / L"CampusNetUpdater.exe",
                                  stagingDirectory_),
          localInstallInfo_(LoadLocalInstallInfo(
              updaterDirectory_ / L"version.json", updateConfigError_)),
          stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        logger_.Info(L"CampusNetService starting");
    }

    ~ServiceRuntime() {
        Stop();
        if (stopEvent_ != nullptr) {
            CloseHandle(stopEvent_);
        }
    }

    bool Start() {
        const AppConfig internetCheckConfig = config_;
        InitializeUpdaterBroker();
        const bool ipcStarted = ipc_.Start(
            [this](const ProtocolMessage& request,
                   const ServiceIpcClientContext& client) {
            if (client.interactiveUser) {
                browser_.SetConnected(true);
                login_.SetUiConnected(true);
            }
            return Handle(request, client);
        });
        if (!ipcStarted) {
            logger_.Error(L"unable to start local IPC");
            return false;
        }
        if (!network_.Start([this] {
                login_.OnNetworkChanged();
                RequestInternetStatusRefresh();
            })) {
            logger_.Warn(L"network notification registration failed");
        }
        try {
            loginWorkerThread_ = std::thread([this] {
                login_.RunOnce();
                std::unique_lock lock(loginWorkerMutex_);
                while (!loginWorkerStopping_) {
                    loginWorkerCondition_.wait(lock, [this] {
                        return loginWorkerStopping_ || loginRequestPending_;
                    });
                    if (loginWorkerStopping_) {
                        break;
                    }
                    loginRequestPending_ = false;
                    lock.unlock();
                    login_.RunOnce();
                    lock.lock();
                }
            });
        } catch (...) {
            logger_.Error(L"unable to start the login worker");
            return false;
        }
        try {
            internetStatusThread_ = std::thread([this, internetCheckConfig] {
                std::unique_lock lock(internetStatusMutex_);
                while (!internetStatusStopping_) {
                    internetStatusCondition_.wait_for(
                        lock, std::chrono::seconds(30), [this] {
                            return internetStatusStopping_ ||
                                   internetStatusRefreshRequested_;
                        });
                    if (internetStatusStopping_) {
                        break;
                    }
                    internetStatusRefreshRequested_ = false;
                    lock.unlock();

                    const InternetStatus internet =
                        transport_.CheckInternet(internetCheckConfig);
                    login_.SetInternetStatus(internet.reachable,
                                             internet.publicIp,
                                             internet.statusCode);

                    lock.lock();
                }
            });
        } catch (...) {
            logger_.Warn(L"unable to start the internet status worker");
        }
        if (updateBroker_ && !updaterSupervisor_.Start()) {
            logger_.Warn(L"updater agent supervisor did not start; login remains active");
        }
        return true;
    }

    void Stop() {
        {
            std::scoped_lock lock(internetStatusMutex_);
            internetStatusStopping_ = true;
        }
        internetStatusCondition_.notify_all();
        transport_.Cancel();
        {
            std::scoped_lock lock(loginWorkerMutex_);
            loginWorkerStopping_ = true;
        }
        loginWorkerCondition_.notify_all();
        network_.Stop();
        updaterSupervisor_.Stop();
        ipc_.Stop();
        login_.SetUiConnected(false);
        if (loginWorkerThread_.joinable()) {
            loginWorkerThread_.join();
        }
        if (internetStatusThread_.joinable()) {
            internetStatusThread_.join();
        }
    }

    void Wait() const {
        if (stopEvent_ != nullptr) {
            WaitForSingleObject(stopEvent_, INFINITE);
        }
    }

    void RequestStop() const {
        if (stopEvent_ != nullptr) {
            SetEvent(stopEvent_);
        }
    }

private:
    void InitializeUpdaterBroker() {
        if (!localInstallInfo_) {
            logger_.Warn(L"automatic updater is disabled: " + updateConfigError_);
            return;
        }
        if (stagingDirectory_.empty()) {
            logger_.Warn(L"automatic updater is disabled: ProgramData is unavailable");
            return;
        }
        UpdateBrokerCallbacks callbacks;
        callbacks.requestCheck = [this] { return updaterSupervisor_.RequestCheck(); };
        callbacks.hasInteractiveSession = [] {
            return UserSessionLauncher::HasInteractiveUser();
        };
        callbacks.showUpdater = [this] {
            std::wstring error;
            std::filesystem::path gui;
            if (!updateApplyProcessHost_.PrepareInteractiveGui(gui, error)) {
                logger_.Warn(L"updater GUI could not be prepared: " + error);
                return false;
            }
            const auto installRoot = UpdateInstallRoot();
            const std::vector<std::pair<std::wstring, std::wstring>> environment{
                {L"CAMPUSNET_UPDATE_INSTALL_ROOT", installRoot.wstring()},
                {L"CAMPUSNET_UPDATE_CHANNEL",
                 localInstallInfo_->channel == UpdateChannel::Portable
                     ? L"portable" : L"msi"},
            };
            if (!UserSessionLauncher::LaunchInActiveSession(gui, error,
                                                            environment)) {
                logger_.Warn(L"updater GUI could not be launched: " + error);
                return false;
            }
            return true;
        };
        callbacks.applyStagedUpdate = [this](const TrustedUpdatePlan& plan) {
            std::filesystem::path copiedUpdater;
            std::wstring error;
            if (!updateApplyProcessHost_.LaunchApply(
                    plan, UpdateInstallRoot(), copiedUpdater, error)) {
                logger_.Warn(L"update apply worker could not be started: " + error);
                return false;
            }
            return true;
        };
        updateBroker_ = std::make_unique<UpdateBroker>(
            localInstallInfo_->version, localInstallInfo_->channel,
            stagingDirectory_, std::move(callbacks));
        updateAgentIpc_ = std::make_unique<UpdateAgentIpc>(
            updaterSupervisor_, *updateBroker_, stagingDirectory_);
    }

    std::filesystem::path UpdateInstallRoot() const {
        if (localInstallInfo_ &&
            localInstallInfo_->channel == UpdateChannel::Portable) {
            return updaterDirectory_.parent_path();
        }
        return updaterDirectory_;
    }

    ProtocolMessage Handle(const ProtocolMessage& request,
                           const ServiceIpcClientContext& client) {
        if (request.type == MessageType::UpdateAgentStatus) {
            if (updateAgentIpc_) {
                return updateAgentIpc_->Handle(request, client);
            }
            return ErrorReply(request, ERROR_INVALID_STATE);
        }
        if (request.type == MessageType::CheckForUpdates ||
            request.type == MessageType::GetUpdateStatus ||
            request.type == MessageType::ShowUpdater ||
            request.type == MessageType::ApplyStagedUpdate) {
            if (updateBroker_) {
                return updateBroker_->Handle(request, client.interactiveUser);
            }
            return ErrorReply(request, ERROR_FILE_NOT_FOUND);
        }
        switch (request.type) {
        case MessageType::GetStatus:
            return Reply(request, StatusPayload(login_.Status()));
        case MessageType::GetConfig:
            return Reply(request,
                         std::wstring(L"{\"gateway\":\"") + JsonEscape(config_.gateway) +
                             L"\",\"portalUrl\":\"" +
                             JsonEscape(config_.portalUrl) + L"\",\"uiAutostart\":" +
                             std::wstring(config_.uiAutostart ? L"true" : L"false") +
                             L"}");
        case MessageType::SetCredentials: {
            const auto username = ExtractString(request.payload, "username");
            const auto password = ExtractString(request.payload, "password");
            const bool saved = !username.empty() && !password.empty() &&
                               credentials_.Write({username, password});
            if (saved) {
                logger_.SetSecrets({password});
            }
            return Reply(request, saved ? L"{\"ok\":true}"
                                        : L"{\"ok\":false}");
        }
        case MessageType::SetUiAutostart: {
            bool enabled = false;
            const bool parsed = ExtractBool(request.payload, "enabled", enabled);
            if (parsed) {
                config_.uiAutostart = enabled;
                login_.SetConfig(config_);
                SaveConfig(ConfigPath(), config_);
            }
            return Reply(request, parsed ? L"{\"ok\":true}"
                                         : L"{\"ok\":false}");
        }
        case MessageType::LoginNow: {
            QueueLoginRequest();
            return Reply(request, StatusPayload(login_.Status()));
        }
        case MessageType::BrowserLoginResult: {
            bool success = false;
            const bool parsed = ExtractBool(request.payload, "success", success);
            if (parsed) {
                login_.OnBrowserLoginResult(
                    success, ExtractString(request.payload, "message"));
            }
            return Reply(request, StatusPayload(login_.Status()));
        }
        case MessageType::CheckInternet: {
            bool refresh = false;
            if (ExtractBool(request.payload, "refresh", refresh) && refresh) {
                RequestInternetStatusRefresh();
            }
            const auto status = login_.Status();
            return Reply(request,
                         std::wstring(L"{\"reachable\":") +
                             std::wstring(status.internetAvailable ? L"true" : L"false") +
                             L",\"statusCode\":" +
                             std::to_wstring(status.internetStatusCode) +
                             L",\"known\":" +
                             std::wstring(status.internetStatusKnown ? L"true" : L"false") +
                             L",\"publicIp\":\"" +
                             JsonEscape(status.publicIp) + L"\"}");
        }
        case MessageType::ServiceCommand:
            if (request.payload.find("stop") != std::string::npos) {
                RequestStop();
            }
            return Reply(request, L"{\"ok\":true}");
        case MessageType::UpdateConfig:
        case MessageType::GetLogs:
            return Reply(request, L"{\"ok\":true}");
        case MessageType::BrowserLoginRequest: {
            const auto credentials = credentials_.Read();
            if (!credentials.has_value()) {
                return Reply(request, L"{\"ok\":false}");
            }
            return Reply(
                request,
                std::wstring(L"{\"ok\":true,\"username\":\"") +
                    JsonEscape(credentials->username) +
                    L"\",\"password\":\"" +
                    JsonEscape(credentials->password) + L"\"}");
        }
        }
        return Reply(request, L"{\"ok\":false}");
    }

    ProtocolMessage ErrorReply(const ProtocolMessage& request,
                               DWORD errorCode) const {
        return {kProtocolVersion, request.type, request.requestId,
                "{\"errorCode\":" + std::to_string(errorCode) + "}"};
    }

    ProtocolMessage Reply(const ProtocolMessage& request,
                          std::wstring_view payload) const {
        return {kProtocolVersion, request.type, request.requestId,
                ToUtf8(payload)};
    }

    void QueueLoginRequest() {
        {
            std::scoped_lock lock(loginWorkerMutex_);
            if (loginWorkerStopping_) {
                return;
            }
            loginRequestPending_ = true;
        }
        loginWorkerCondition_.notify_one();
    }

    void RequestInternetStatusRefresh() {
        {
            std::scoped_lock lock(internetStatusMutex_);
            if (internetStatusStopping_) {
                return;
            }
            internetStatusRefreshRequested_ = true;
        }
        internetStatusCondition_.notify_one();
    }

    AppConfig config_;
    Logger logger_;
    WindowsCredentialStore credentials_;
    HttpTransport transport_;
    BrowserRequester browser_;
    LoginService login_;
    ServiceIpcServer ipc_;
    NetworkMonitor network_;
    std::filesystem::path updaterDirectory_;
    std::filesystem::path stagingDirectory_;
    WindowsUpdaterProcessHost updaterProcessHost_;
    UpdaterProcessSupervisor updaterSupervisor_;
    WindowsUpdateApplyProcessHost updateApplyProcessHost_;
    std::wstring updateConfigError_;
    std::optional<LocalInstallInfo> localInstallInfo_;
    std::unique_ptr<UpdateBroker> updateBroker_;
    std::unique_ptr<UpdateAgentIpc> updateAgentIpc_;
    std::mutex loginWorkerMutex_;
    std::condition_variable loginWorkerCondition_;
    bool loginWorkerStopping_{false};
    bool loginRequestPending_{false};
    std::thread loginWorkerThread_;
    std::mutex internetStatusMutex_;
    std::condition_variable internetStatusCondition_;
    bool internetStatusStopping_{false};
    bool internetStatusRefreshRequested_{true};
    std::thread internetStatusThread_;
    HANDLE stopEvent_;
};

void UpdateServiceStatus(DWORD state, DWORD win32ExitCode = NO_ERROR,
                         DWORD waitHint = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = win32ExitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted =
        state == SERVICE_RUNNING
            ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
            : 0;
    if (g_statusHandle != nullptr) {
        SetServiceStatus(g_statusHandle, &g_status);
    }
}

ServiceRuntime* g_runtime = nullptr;

VOID WINAPI ServiceHandler(DWORD control) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        if (g_runtime != nullptr) {
            g_runtime->RequestStop();
        }
        UpdateServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerW(L"CampusNetService",
                                                  ServiceHandler);
    if (g_statusHandle == nullptr) {
        return;
    }
    UpdateServiceStatus(SERVICE_START_PENDING, NO_ERROR, 5000);

    ServiceRuntime runtime;
    g_runtime = &runtime;
    if (!runtime.Start()) {
        g_runtime = nullptr;
        UpdateServiceStatus(SERVICE_STOPPED, ERROR_SERVICE_NOT_ACTIVE);
        return;
    }
    UpdateServiceStatus(SERVICE_RUNNING);
    runtime.Wait();
    UpdateServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);
    g_runtime = nullptr;
    UpdateServiceStatus(SERVICE_STOPPED);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(L"CampusNetService"), ServiceMain},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        return static_cast<int>(GetLastError());
    }
    return 0;
}
