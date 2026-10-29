#include "pch.h"
#include "MainWindow.xaml.h"
#include "SingleInstanceActivation.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#if __has_include("MainWindow.xaml.g.hpp")
#include "MainWindow.xaml.g.hpp"
#endif
#include "Protocol.h"
#include "resource.h"
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <utility>
#include <winsock2.h>
#include <IPTypes.h>
#include <iphlpapi.h>
#include <string_view>
#include <vector>
#include <ws2tcpip.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace winrt::CampusNetClient::implementation {
namespace {
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
std::wstring FromUtf8(std::string_view value) {
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
std::string EscapeJson(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\n':
            escaped += "\\n";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}
std::optional<bool> ExtractJsonBool(std::string_view payload,
                                    std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\":";
    const auto start = payload.find(marker);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    const auto value = payload.substr(start + marker.size());
    if (value.starts_with("true")) {
        return true;
    }
    if (value.starts_with("false")) {
        return false;
    }
    return std::nullopt;
}
bool HasState(std::string_view payload, std::string_view state) {
    return payload.find("\"state\":\"" + std::string(state) + "\"") !=
           std::string_view::npos;
}
std::wstring StatusAssetUri(std::wstring_view assetName) {
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(
        nullptr, modulePath, static_cast<DWORD>(_countof(modulePath)));
    if (length == 0 || length >= _countof(modulePath)) {
        return {};
    }
    const auto assetPath =
        std::filesystem::path(std::wstring(modulePath, length)).parent_path() /
        L"assets" / L"status" / std::wstring(assetName);
    return std::wstring(L"file:///") + assetPath.generic_wstring();
}
struct LocalNetworkInfo {
    std::wstring ipv4{L"未检测到"};
    std::wstring mac{L"未检测到"};
    std::wstring computerName{L"未检测到"};
};
std::wstring FormatMacAddress(const BYTE* address, ULONG length) {
    if (address == nullptr || length == 0) {
        return L"未检测到";
    }
    std::wstring result;
    result.reserve(length * 3);
    for (ULONG index = 0; index < length; ++index) {
        if (index != 0) {
            result.push_back(L'-');
        }
        wchar_t byteText[3]{};
        swprintf_s(byteText, ARRAYSIZE(byteText), L"%02X", address[index]);
        result.append(byteText);
    }
    return result;
}
bool ContainsInsensitive(const wchar_t* value, std::wstring_view marker) {
    if (value == nullptr || marker.empty()) {
        return false;
    }
    const std::wstring_view text(value);
    if (text.size() < marker.size()) {
        return false;
    }
    for (std::size_t offset = 0;
         offset + marker.size() <= text.size(); ++offset) {
        bool matches = true;
        for (std::size_t index = 0; index < marker.size(); ++index) {
            if (std::towlower(text[offset + index]) !=
                std::towlower(marker[index])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}
bool IsVirtualAdapter(PIP_ADAPTER_ADDRESSES adapter) {
    return adapter == nullptr ||
           ContainsInsensitive(adapter->FriendlyName, L"WSL") ||
           ContainsInsensitive(adapter->FriendlyName, L"Hyper-V") ||
           ContainsInsensitive(adapter->FriendlyName, L"VMware") ||
           ContainsInsensitive(adapter->FriendlyName, L"Virtual") ||
           ContainsInsensitive(adapter->FriendlyName, L"VPN") ||
           ContainsInsensitive(adapter->FriendlyName, L"TAP") ||
           ContainsInsensitive(adapter->FriendlyName, L"Teredo") ||
           ContainsInsensitive(adapter->FriendlyName, L"Tunnel") ||
           ContainsInsensitive(adapter->FriendlyName, L"Docker") ||
           ContainsInsensitive(adapter->Description, L"WSL") ||
           ContainsInsensitive(adapter->Description, L"Hyper-V") ||
           ContainsInsensitive(adapter->Description, L"VMware") ||
           ContainsInsensitive(adapter->Description, L"Virtual") ||
           ContainsInsensitive(adapter->Description, L"VPN") ||
           ContainsInsensitive(adapter->Description, L"TAP") ||
           ContainsInsensitive(adapter->Description, L"Teredo") ||
           ContainsInsensitive(adapter->Description, L"Tunnel") ||
           ContainsInsensitive(adapter->Description, L"Docker");
}
bool HasIpv4Gateway(PIP_ADAPTER_ADDRESSES adapter) {
    for (auto* gateway = adapter->FirstGatewayAddress; gateway != nullptr;
         gateway = gateway->Next) {
        if (gateway->Address.lpSockaddr != nullptr &&
            gateway->Address.lpSockaddr->sa_family == AF_INET) {
            return true;
        }
    }
    return false;
}
std::optional<DWORD> QueryBestInterfaceIndex() {
    IN_ADDR destination{};
    if (InetPtonW(AF_INET, L"8.8.8.8", &destination) != 1) {
        return std::nullopt;
    }
    DWORD bestInterface = 0;
    if (GetBestInterface(destination.S_un.S_addr, &bestInterface) !=
        NO_ERROR) {
        return std::nullopt;
    }
    return bestInterface;
}
bool TryGetIpv4(PIP_ADAPTER_ADDRESSES adapter, std::wstring& ipv4) {
    for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
         unicast = unicast->Next) {
        if (unicast->Address.lpSockaddr == nullptr ||
            unicast->Address.lpSockaddr->sa_family != AF_INET) {
            continue;
        }
        const auto* address = reinterpret_cast<const sockaddr_in*>(
            unicast->Address.lpSockaddr);
        const ULONG hostAddress = ntohl(address->sin_addr.S_un.S_addr);
        if (hostAddress == 0 ||
            (hostAddress & 0xFF000000UL) == 0x7F000000UL ||
            (hostAddress & 0xFFFF0000UL) == 0xA9FE0000UL) {
            continue;
        }
        wchar_t addressText[INET_ADDRSTRLEN]{};
        if (InetNtopW(AF_INET, const_cast<IN_ADDR*>(&address->sin_addr),
                      addressText, ARRAYSIZE(addressText)) != nullptr) {
            ipv4 = addressText;
            return true;
        }
    }
    return false;
}
LocalNetworkInfo QueryLocalNetworkInfo() {
    LocalNetworkInfo info;
    wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD computerNameLength = ARRAYSIZE(computerName);
    if (GetComputerNameW(computerName, &computerNameLength)) {
        info.computerName.assign(computerName, computerNameLength);
    }
    ULONG bufferLength = 15 * 1024;
    std::vector<BYTE> buffer(bufferLength);
    auto* adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    ULONG result = GetAdaptersAddresses(
        AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, adapters, &bufferLength);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufferLength);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        result = GetAdaptersAddresses(
            AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                            GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters, &bufferLength);
    }
    if (result != NO_ERROR) {
        return info;
    }
    const auto bestInterface = QueryBestInterfaceIndex();
    PIP_ADAPTER_ADDRESSES gatewayFallback = nullptr;
    std::wstring gatewayFallbackIpv4;
    ULONG gatewayFallbackMetric = ULONG_MAX;
    PIP_ADAPTER_ADDRESSES physicalFallback = nullptr;
    std::wstring physicalFallbackIpv4;
    for (auto* adapter = adapters; adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
            adapter->IfType == IF_TYPE_TUNNEL ||
            adapter->PhysicalAddressLength == 0 ||
            IsVirtualAdapter(adapter)) {
            continue;
        }
        std::wstring ipv4;
        if (!TryGetIpv4(adapter, ipv4)) {
            continue;
        }
        const bool hasGateway = HasIpv4Gateway(adapter);
        if (hasGateway && bestInterface.has_value() &&
            adapter->IfIndex == *bestInterface) {
            info.ipv4 = std::move(ipv4);
            info.mac = FormatMacAddress(adapter->PhysicalAddress,
                                        adapter->PhysicalAddressLength);
            return info;
        }
        if (hasGateway &&
            (gatewayFallback == nullptr ||
             adapter->Ipv4Metric < gatewayFallbackMetric)) {
            gatewayFallback = adapter;
            gatewayFallbackIpv4 = ipv4;
            gatewayFallbackMetric = adapter->Ipv4Metric;
        } else if (physicalFallback == nullptr) {
            physicalFallback = adapter;
            physicalFallbackIpv4 = std::move(ipv4);
        }
    }
    auto* selected = gatewayFallback != nullptr ? gatewayFallback :
                                                   physicalFallback;
    if (selected != nullptr) {
        info.ipv4 = gatewayFallback != nullptr
                        ? std::move(gatewayFallbackIpv4)
                        : std::move(physicalFallbackIpv4);
        info.mac = FormatMacAddress(selected->PhysicalAddress,
                                    selected->PhysicalAddressLength);
    }
    return info;
}
std::optional<std::string> ExtractJsonString(std::string_view payload,
                                             std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\":\"";
    const std::size_t start = payload.find(marker);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    std::string value;
    bool escaped = false;
    for (std::size_t i = start + marker.size(); i < payload.size(); ++i) {
        const char ch = payload[i];
        if (escaped) {
            switch (ch) {
            case '"':
                value.push_back('"');
                break;
            case '\\':
                value.push_back('\\');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            default:
                value.push_back(ch);
                break;
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            return value;
        } else {
            value.push_back(ch);
        }
    }
    return std::nullopt;
}
const wchar_t* ServiceStateName(ManagedServiceState state) {
    switch (state) {
    case ManagedServiceState::Missing:
        return L"未安装";
    case ManagedServiceState::Stopped:
        return L"已停止";
    case ManagedServiceState::StartPending:
        return L"正在启动";
    case ManagedServiceState::Running:
        return L"运行中";
    case ManagedServiceState::StopPending:
        return L"正在停止";
    case ManagedServiceState::Failed:
        return L"状态异常";
    }
    return L"未知";
}
const wchar_t* ServiceInstallationName(ManagedServiceState state) {
    switch (state) {
    case ManagedServiceState::Missing:
        return L"未安装";
    case ManagedServiceState::Stopped:
    case ManagedServiceState::StartPending:
    case ManagedServiceState::Running:
    case ManagedServiceState::StopPending:
        return L"已安装";
    case ManagedServiceState::Failed:
        return L"状态未知";
    }
    return L"状态未知";
}
HICON LoadCampusNetIcon(int width, int height) {
    auto* module = GetModuleHandleW(nullptr);
    return reinterpret_cast<HICON>(LoadImageW(
        module, MAKEINTRESOURCEW(IDI_CAMPUSNETCLIENT), IMAGE_ICON, width,
        height, LR_DEFAULTCOLOR | LR_SHARED));
}
void ApplyWindowIcons(HWND handle) {
    if (handle == nullptr) {
        return;
    }
    const auto smallIcon = LoadCampusNetIcon(
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    const auto largeIcon = LoadCampusNetIcon(
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    if (smallIcon != nullptr) {
        SendMessageW(handle, WM_SETICON, ICON_SMALL,
                     reinterpret_cast<LPARAM>(smallIcon));
    }
    if (largeIcon != nullptr) {
        SendMessageW(handle, WM_SETICON, ICON_BIG,
                     reinterpret_cast<LPARAM>(largeIcon));
    }
}
} // namespace
MainWindow::MainWindow()
    : viewModel_(LoadDefaultConfig()) {
    InitializeComponent();
    SetStatusIllustration(L"service-connecting.png");
    StartStatusIndicatorAnimation();
    RefreshLocalNetworkInfo();
    AppWindow().Title(L"广航海校园网自动登录");
    AppWindow().Resize({1000, 720});
    closingToken_ = AppWindow().Closing([this](auto const&, auto const& args) {
        if (!allowWindowClose_) {
            args.Cancel(true);
            HideToTray();
        }
    });
    auto windowNative = this->try_as<::IWindowNative>();
    HWND handle = nullptr;
    if (windowNative != nullptr && SUCCEEDED(windowNative->get_WindowHandle(&handle))) {
        ApplyWindowIcons(handle);
        tray_ = std::make_unique<TrayIcon>(handle, [this](TrayCommand command) {
            HandleTrayCommand(command);
        });
        tray_->Initialize();
    }
    ServicePathText().Text(winrt::hstring(
        L"服务程序：" + ResolveServiceBinaryPath().wstring()));
    std::wstring serviceQueryError;
    if (serviceManager_.Query(serviceQueryError) == ManagedServiceState::Missing) {
        standaloneUpdateRuntime_ = std::make_unique<StandaloneUpdateRuntime>(
            ResolveServiceBinaryPath().parent_path());
        std::wstring updaterError;
        if (!standaloneUpdateRuntime_->Start(updaterError)) {
            SetServiceFeedback(std::move(updaterError));
            standaloneUpdateRuntime_.reset();
        }
    }
    RefreshServiceStatus();
    dispatcher_ =
        Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    if (dispatcher_ != nullptr) {
        updateOverlayTimer_ = dispatcher_.CreateTimer();
        updateOverlayTimer_.Interval(std::chrono::seconds(40));
        updateOverlayTimer_.IsRepeating(false);
        updateOverlayTimer_.Tick([this](auto const&, auto const&) {
            if (updateResultShowing_) {
                EndUpdateOverlay();
            } else {
                updateCheckPresentation_.OnTimeout();
                updateCheckTimedOut_ = true;
                ShowUpdateCheckResult({});
            }
        });
        const HANDLE activationEvent = OpenSingleInstanceActivationEvent();
        const auto lifetimeGuard = lifetimeGuard_;
        pollThread_ = std::thread([this, activationEvent, lifetimeGuard] {
            while (!pollStop_.load()) {
                DWORD waitResult = WAIT_TIMEOUT;
                if (activationEvent != nullptr) {
                    waitResult = WaitForSingleObject(activationEvent, 2000);
                    if (waitResult == WAIT_FAILED) {
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        waitResult = WAIT_TIMEOUT;
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                }
                if (pollStop_.load()) {
                    break;
                }
                if (updateCheckRequested_.exchange(false)) {
                    std::string completedPhase;
                    std::wstring error;
                    ipc_.Call({kProtocolVersion, MessageType::CheckForUpdates,
                               L"ui-update-check", "{}"}, 2500, error);
                    const auto updateDeadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds(40);
                    while (!pollStop_.load() &&
                           std::chrono::steady_clock::now() < updateDeadline) {
                        auto status = ipc_.Call(
                            {kProtocolVersion, MessageType::GetUpdateStatus,
                             L"ui-update-status", "{}"}, 750, error);
                        if (status) {
                            const auto phase = ExtractJsonString(
                                status->payload, "phase");
                            if (phase && (*phase == "upToDate" ||
                                          *phase == "available" ||
                                          *phase == "downloading" ||
                                          *phase == "failed" ||
                                          *phase == "ready" ||
                                          *phase == "applying")) {
                                completedPhase = *phase;
                                break;
                            }
                        }
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(250));
                    }
                    dispatcher_.TryEnqueue([this, lifetimeGuard, completedPhase] {
                        if (lifetimeGuard->load()) {
                            updateCheckInFlight_.store(false);
                            CompleteUpdateCheck(completedPhase);
                        }
                    });
                }
                if (waitResult == WAIT_OBJECT_0) {
                    dispatcher_.TryEnqueue([this] {
                        ShowFromTray();
                        RefreshStatus();
                    });
                } else if (waitResult == WAIT_TIMEOUT) {
                    dispatcher_.TryEnqueue([this] { RefreshStatus(); });
                }
            }
            if (activationEvent != nullptr) {
                CloseHandle(activationEvent);
            }
        });
    }
}
MainWindow::~MainWindow() {
    lifetimeGuard_->store(false);
    StopPolling();
    if (loginRequestThread_.joinable()) {
        loginRequestThread_.join();
    }
}
void MainWindow::LoginButton_Click(IInspectable const&, RoutedEventArgs const&) {
    StartLoginAttempt(L"ui-login");
}
void MainWindow::RefreshButton_Click(IInspectable const&, RoutedEventArgs const&) {
    RefreshStatus();
    RefreshLocalNetworkInfo();
    RefreshServiceStatus();
    RequestNetworkStatusRefresh();
    RequestUpdateCheck();
}
void MainWindow::SaveButton_Click(IInspectable const&, RoutedEventArgs const&) {
    const std::string carrier = CarrierBox().SelectedIndex() == 1 ? "mobile" : "telecom";
    const std::string startup = UiAutostartToggle().IsOn() ? "true" : "false";
    std::wstring settingsError;
    const auto saved = ipc_.Call({kProtocolVersion, MessageType::UpdateConfig,
        L"ui-carrier", "{\"carrier\":\"" + carrier + "\",\"uiAutostart\":" + startup + "}"}, 1500, settingsError);
    if (!saved || !ExtractJsonBool(saved->payload, "ok").value_or(false)) {
        SetServiceFeedback(L"套餐保存失败，请确认服务已启动。");
        return;
    }
    std::wstring error;
    const std::string username = ToUtf8(UsernameBox().Text().c_str());
    const std::string password = ToUtf8(PasswordBox().Password().c_str());
    if (!username.empty() && !password.empty()) {
        ipc_.Call({kProtocolVersion, MessageType::SetCredentials,
                   L"ui-credentials",
                   "{\"username\":\"" + EscapeJson(username) +
                       "\",\"password\":\"" + EscapeJson(password) +
                       "\"}"},
                  1500, error);
    }
}
void MainWindow::ShowFromTray() {
    AppWindow().Show(true);
    OnWindowShown();
    RefreshLocalNetworkInfo();
}
void MainWindow::HideToTray() {
    AppWindow().Hide();
    updateCheckPresentation_.OnWindowHidden();
}
void MainWindow::OnWindowShown() {
    const auto action = updateCheckPresentation_.OnWindowShown();
    if (action.requestCheck) {
        RequestUpdateCheck();
    }
}
void MainWindow::RequestUpdateCheck() {
    if (updateResultShowing_) return;
    bool expected = false;
    if (updateCheckInFlight_.compare_exchange_strong(expected, true)) {
        updateCheckTimedOut_ = false;
        MainContentScrollViewer().IsEnabled(false);
        UpdateCheckProgressRing().Visibility(Visibility::Visible);
        UpdateCheckProgressRing().IsActive(true);
        UpdateResultIcon().Visibility(Visibility::Collapsed);
        UpdateCheckResultText().Text(L"正在检查更新，请稍候…");
        UpdateCheckOverlay().Visibility(Visibility::Visible);
        if (updateOverlayTimer_ != nullptr) {
            updateOverlayTimer_.Stop();
            updateOverlayTimer_.Interval(std::chrono::seconds(40));
            updateOverlayTimer_.Start();
        }
        updateCheckRequested_.store(true);
    }
}
void MainWindow::CompleteUpdateCheck(const std::string& phase) {
    updateCheckPresentation_.OnCheckCompleted();
    if (!updateCheckTimedOut_) ShowUpdateCheckResult(phase);
}
void MainWindow::ShowUpdateCheckResult(std::string_view phase) {
    const auto result = ClassifyUpdateCheckResult(phase);
    UpdateCheckProgressRing().IsActive(false);
    UpdateCheckProgressRing().Visibility(Visibility::Collapsed);
    auto icon = UpdateResultIcon();
    const wchar_t* colorResource = L"SystemFillColorCautionBrush";
    switch (result) {
    case UpdateCheckResult::UpToDate:
        icon.Glyph(L"\uE73E");
        colorResource = L"SystemFillColorSuccessBrush";
        UpdateCheckResultText().Text(L"没有新版本，当前已是最新版本。");
        break;
    case UpdateCheckResult::Available:
        icon.Glyph(L"\uE896");
        colorResource = L"SystemFillColorAttentionBrush";
        UpdateCheckResultText().Text(L"发现新版本，请查看更新器窗口。");
        break;
    case UpdateCheckResult::Failed:
        icon.Glyph(L"\uEA39");
        colorResource = L"SystemFillColorCriticalBrush";
        UpdateCheckResultText().Text(L"更新检查失败，请稍后重试。");
        break;
    default:
        icon.Glyph(L"\uE823");
        UpdateCheckResultText().Text(L"更新检查超时，请稍后重试。");
        break;
    }
    icon.Foreground(Application::Current().Resources().Lookup(winrt::box_value(colorResource))
        .as<Microsoft::UI::Xaml::Media::Brush>());
    icon.Visibility(Visibility::Visible);
    updateResultShowing_ = true;
    if (updateOverlayTimer_ != nullptr) {
        updateOverlayTimer_.Stop();
        updateOverlayTimer_.Interval(std::chrono::milliseconds(1500));
        updateOverlayTimer_.Start();
    }
}
void MainWindow::EndUpdateOverlay() {
    if (updateOverlayTimer_ != nullptr) {
        updateOverlayTimer_.Stop();
    }
    UpdateCheckOverlay().Visibility(Visibility::Collapsed);
    updateResultShowing_ = false;
    MainContentScrollViewer().IsEnabled(true);
}
void MainWindow::RefreshStatus() {
    std::wstring serviceError;
    const auto serviceState = serviceManager_.Query(serviceError);
    if (serviceState != ManagedServiceState::Running) carrierLoaded_ = false;
    if (!carrierLoaded_ && serviceState == ManagedServiceState::Running) {
        std::wstring configError;
        const auto config = ipc_.Call({kProtocolVersion, MessageType::GetConfig,
            L"ui-config", "{}"}, 1500, configError);
        const auto carrier = config ? ExtractJsonString(config->payload, "carrier") : std::nullopt;
        if (config && carrier && (*carrier == "mobile" || *carrier == "telecom")) {
            CarrierBox().SelectedIndex(*carrier == "mobile" ? 1 : 0);
            UiAutostartToggle().IsOn(ExtractJsonBool(config->payload, "uiAutostart").value_or(true));
            carrierLoaded_ = true;
        }
    }
    StatusText().Text(L"\u6B63\u5728\u8FDE\u63A5\u5230\u670D\u52A1");
    std::wstring error;
    const auto response = ipc_.Call(
        {kProtocolVersion, MessageType::GetStatus, L"ui-status", "{}"}, 1500,
        error);
    if (!response.has_value() || serviceState != ManagedServiceState::Running ||
        !ExtractJsonBool(response->payload, "serviceRunning").value_or(false)) {
        StatusText().Text(L"\u672A\u8FDE\u63A5\u5230\u670D\u52A1");
        PublicIpText().Text(L"\u5916\u7F51 IP\uFF1A\u670D\u52A1\u672A\u8FDE\u63A5");
        internetReachable_.reset();
        if (loginAttemptPending_ && !loginAttemptAwaitingRequest_) {
            CompleteLoginAttempt(
                false, error.empty() ? L"登录服务未连接。" : error);
        }
        UpdateLoginButton();
        SetStatusIndicator(false);
        SetStatusIllustration(L"service-disconnected.png");
        return;
    }
    StatusText().Text(L"\u5DF2\u8FDE\u63A5\u5230\u670D\u52A1");
    SetStatusIndicator(true);
    const bool internetAvailable =
        ExtractJsonBool(response->payload, "internetAvailable").value_or(false);
    const bool internetStatusKnown =
        ExtractJsonBool(response->payload, "internetStatusKnown").value_or(false);
    internetReachable_ = internetStatusKnown
                             ? std::optional<bool>(internetAvailable)
                             : std::nullopt;
    const auto publicIp = ExtractJsonString(response->payload, "publicIp");
    const std::wstring publicIpValue =
        !internetStatusKnown
            ? L"\u68C0\u6D4B\u4E2D"
            : (publicIp.has_value() && !publicIp->empty())
                  ? FromUtf8(*publicIp)
                  : L"\u672A\u83B7\u53D6";
    PublicIpText().Text(winrt::hstring(
        L"\u5916\u7F51 IP\uFF1A" + publicIpValue));
    loginSucceeded_ = HasState(response->payload, "Succeeded");
    if (loginAttemptPending_ && !loginAttemptAwaitingRequest_) {
        if (loginSucceeded_) {
            CompleteLoginAttempt(true, {});
        } else if (HasState(response->payload, "Failed")) {
            const auto message = ExtractJsonString(response->payload, "message");
            CompleteLoginAttempt(
                false, message.has_value() ? FromUtf8(*message)
                                           : L"登录失败，请检查网络和账号密码。");
        }
    }
    UpdateLoginButton();
    if (HasState(response->payload, "Succeeded")) {
        SetStatusIllustration(L"network-authenticated.png");
    } else if (HasState(response->payload, "WaitingForNetwork") ||
               HasState(response->payload, "CheckingInternet") ||
               HasState(response->payload, "HttpLogin") ||
               HasState(response->payload, "VerifyingInternet")) {
        SetStatusIllustration(L"network-authenticating.png");
    } else if (HasState(response->payload, "Failed")) {
        SetStatusIllustration(L"network-authentication-failed.png");
    } else if (HasState(response->payload, "Idle") || !internetAvailable) {
        SetStatusIllustration(L"network-unauthenticated.png");
    } else {
        SetStatusIllustration(L"service-connected.png");
    }
}
void MainWindow::RefreshLocalNetworkInfo() {
    const auto info = QueryLocalNetworkInfo();
    LocalIpText().Text(winrt::hstring(
        L"\u672C\u673A IPv4\uFF1A" + info.ipv4));
    LocalMacText().Text(winrt::hstring(
        L"\u672C\u673A MAC\uFF1A" + info.mac));
    ComputerNameText().Text(winrt::hstring(
        L"\u8BA1\u7B97\u673A\u540D\uFF1A" + info.computerName));
}
void MainWindow::RequestNetworkStatusRefresh() {
    std::wstring error;
    ipc_.Call({kProtocolVersion, MessageType::CheckInternet,
               L"ui-network-refresh", "{\"refresh\":true}"},
              1500, error);
}
void MainWindow::UpdateLoginButton() {
    LoginButton().IsEnabled(viewModel_.CanRequestLogin(
        internetReachable_, loginSucceeded_, loginAttemptPending_));
    LoginProgressRing().IsActive(loginAttemptPending_);
    LoginProgressRing().Visibility(loginAttemptPending_ ? Visibility::Visible
                                                       : Visibility::Collapsed);
}
void MainWindow::StartLoginAttempt(std::wstring requestId) {
    if (!viewModel_.CanRequestLogin(internetReachable_, loginSucceeded_,
                                    loginAttemptPending_)) {
        return;
    }
    loginAttemptPending_ = true;
    loginAttemptAwaitingRequest_ = true;
    LoginFailureInfoBar().IsOpen(false);
    UpdateLoginButton();
    if (loginRequestThread_.joinable()) {
        loginRequestThread_.join();
    }
    try {
        const auto lifetimeGuard = lifetimeGuard_;
        loginRequestThread_ = std::thread(
            [this, lifetimeGuard, requestId = std::move(requestId)] {
                std::wstring error;
                const auto response = ipc_.Call(
                    {kProtocolVersion, MessageType::LoginNow, requestId, "{}"},
                    1500, error);
                const bool requestAccepted = response.has_value();
                dispatcher_.TryEnqueue(
                    [this, lifetimeGuard, requestAccepted,
                     error = std::move(error)]() mutable {
                        if (!lifetimeGuard->load()) {
                            return;
                        }
                        if (!requestAccepted) {
                            CompleteLoginAttempt(
                                false, error.empty()
                                           ? L"无法连接到登录服务。"
                                           : std::move(error));
                            return;
                        }
                        loginAttemptAwaitingRequest_ = false;
                        RefreshStatus();
                    });
            });
    } catch (...) {
        CompleteLoginAttempt(false, L"无法启动登录请求。");
    }
}
void MainWindow::CompleteLoginAttempt(bool succeeded, std::wstring message) {
    loginAttemptPending_ = false;
    loginAttemptAwaitingRequest_ = false;
    loginSucceeded_ = succeeded;
    if (succeeded) {
        LoginFailureInfoBar().IsOpen(false);
    } else {
        if (message.empty()) {
            message = L"登录失败，请检查网络和账号密码。";
        }
        LoginFailureInfoBar().Message(winrt::hstring(message));
        LoginFailureInfoBar().IsOpen(true);
    }
    UpdateLoginButton();
}
void MainWindow::SetStatusIndicator(bool connected) {
    const auto color = connected
                           ? Microsoft::UI::ColorHelper::FromArgb(255, 16, 124, 16)
                           : Microsoft::UI::ColorHelper::FromArgb(255, 209, 52, 56);
    StatusIndicator().Fill(
        Microsoft::UI::Xaml::Media::SolidColorBrush(color));
}
void MainWindow::SetStatusIllustration(std::wstring_view assetName) {
    if (assetName == statusIllustrationAsset_) {
        return;
    }
    const auto uri = StatusAssetUri(assetName);
    if (uri.empty()) {
        return;
    }
    try {
        auto bitmap = Microsoft::UI::Xaml::Media::Imaging::BitmapImage();
        bitmap.UriSource(Windows::Foundation::Uri(winrt::hstring(uri)));
        StatusIllustration().Source(bitmap);
        statusIllustrationAsset_.assign(assetName);
    } catch (...) {
        // A missing/corrupt optional illustration must not prevent the client
        // from showing the service status text and controls.
    }
}
void MainWindow::StartStatusIndicatorAnimation() {
    const auto resource =
        RootGrid().Resources().Lookup(box_value(L"StatusIndicatorBreathing"));
    if (resource != nullptr) {
        resource.as<Microsoft::UI::Xaml::Media::Animation::Storyboard>().Begin();
    }
}
void MainWindow::HandleTrayCommand(TrayCommand command) {
    switch (command) {
    case TrayCommand::Open:
        ShowFromTray();
        RefreshStatus();
        break;
    case TrayCommand::Login:
        StartLoginAttempt(L"tray-login");
        break;
    case TrayCommand::Refresh:
        RefreshStatus();
        break;
    case TrayCommand::Exit:
        ExitApplication();
        break;
    }
}
void MainWindow::InstallServiceButton_Click(
    IInspectable const&, RoutedEventArgs const&) {
    if (!serviceCommandGate_.TryAcquire()) {
        return;
    }
    std::wstring error;
    const auto servicePath = ResolveServiceBinaryPath();
    bool success = false;
    if (servicePath.empty() || !std::filesystem::exists(servicePath)) {
        error = L"未找到 CampusNetService.exe，请将服务程序放在 UI 同目录后重试。";
    } else {
        success = serviceManager_.Install(
            servicePath.wstring(), L"CampusNetService", error);
    }
    serviceCommandGate_.Release();
    if (success && standaloneUpdateRuntime_) {
        standaloneUpdateRuntime_->Stop();
        standaloneUpdateRuntime_.reset();
    }
    SetServiceFeedback(success ? L"服务安装成功。" : error);
    RefreshServiceStatus();
}
void MainWindow::StartServiceButton_Click(
    IInspectable const&, RoutedEventArgs const&) {
    if (!serviceCommandGate_.TryAcquire()) {
        return;
    }
    std::wstring error;
    const bool success = serviceManager_.Start(error);
    serviceCommandGate_.Release();
    SetServiceFeedback(success ? L"服务启动成功。" : error);
    RefreshServiceStatus();
}
void MainWindow::StopServiceButton_Click(
    IInspectable const&, RoutedEventArgs const&) {
    if (!serviceCommandGate_.TryAcquire()) {
        return;
    }
    std::wstring error;
    const bool success = serviceManager_.Stop(error);
    serviceCommandGate_.Release();
    SetServiceFeedback(success ? L"服务停止成功。" : error);
    RefreshServiceStatus();
}
void MainWindow::UninstallServiceButton_Click(
    IInspectable const&, RoutedEventArgs const&) {
    if (!serviceCommandGate_.TryAcquire()) {
        return;
    }
    std::wstring error;
    const bool success = serviceManager_.Uninstall(error);
    serviceCommandGate_.Release();
    std::wstring feedback = success ? L"服务卸载成功。" : error;
    if (success && !standaloneUpdateRuntime_) {
        standaloneUpdateRuntime_ = std::make_unique<StandaloneUpdateRuntime>(
            ResolveServiceBinaryPath().parent_path());
        std::wstring updaterError;
        if (!standaloneUpdateRuntime_->Start(updaterError)) {
            feedback = std::move(updaterError);
            standaloneUpdateRuntime_.reset();
        }
    }
    SetServiceFeedback(std::move(feedback));
    RefreshServiceStatus();
}
void MainWindow::RefreshServiceStatus() {
    std::wstring error;
    const auto state = serviceManager_.Query(error);
    ServiceInstallStatusText().Text(winrt::hstring(ServiceInstallationName(state)));
    ServiceStatusText().Text(winrt::hstring(
        std::wstring(L"服务状态：") + ServiceStateName(state)));
    if (!error.empty() && state != ManagedServiceState::Missing) {
        SetServiceFeedback(error);
    }
}
void MainWindow::SetServiceFeedback(std::wstring message) {
    ServiceFeedbackText().Text(winrt::hstring(std::move(message)));
}
std::filesystem::path MainWindow::ResolveServiceBinaryPath() const {
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(
        nullptr, modulePath, ARRAYSIZE(modulePath));
    if (length == 0 || length >= ARRAYSIZE(modulePath)) {
        return {};
    }
    return std::filesystem::path(modulePath).parent_path() /
           L"CampusNetService.exe";
}
void MainWindow::ExitApplication() {
    allowWindowClose_ = true;
    StopPolling();
    tray_.reset();
    Close();
}
void MainWindow::StopPolling() {
    pollStop_.store(true);
    if (pollThread_.joinable()) {
        pollThread_.join();
    }
}
} // namespace winrt::CampusNetClient::implementation
