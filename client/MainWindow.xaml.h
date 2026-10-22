#pragma once

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include "MainWindow.g.h"

#include "ClientIpc.h"
#include "ClientViewModel.h"
#include "ServiceManager.h"
#include "TrayIcon.h"
#include "WebViewLogin.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <atomic>
#include <thread>

namespace winrt::CampusNetClient::implementation {

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    ~MainWindow();

    void LoginButton_Click(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void RefreshButton_Click(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void SaveButton_Click(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void InstallServiceButton_Click(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void StartServiceButton_Click(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void StopServiceButton_Click(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void UninstallServiceButton_Click(
        Windows::Foundation::IInspectable const& sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& args);

    void ShowFromTray();
    void HideToTray();

private:
    void RefreshStatus();
    void RefreshLocalNetworkInfo();
    void RequestNetworkStatusRefresh();
    void UpdateLoginButton();
    void StartLoginAttempt(std::wstring requestId);
    void CompleteLoginAttempt(bool succeeded, std::wstring message);
    void SetStatusIndicator(bool connected);
    void SetStatusIllustration(std::wstring_view assetName);
    void StartStatusIndicatorAnimation();
    void HandleTrayCommand(TrayCommand command);
    void MaybeStartBrowserFallback(std::string_view statusPayload);
    void SendBrowserLoginResult(const WebViewLoginResult& result);
    void StopPolling();
    void RefreshServiceStatus();
    void SetServiceFeedback(std::wstring message);
    std::filesystem::path ResolveServiceBinaryPath() const;
    void ExitApplication();

    ClientViewModel viewModel_;
    ServiceIpcClient ipc_;
    std::unique_ptr<TrayIcon> tray_;
    std::unique_ptr<WebView2LoginHost> webView_;
    Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{nullptr};
    std::thread pollThread_;
    std::thread loginRequestThread_;
    std::shared_ptr<std::atomic<bool>> lifetimeGuard_{
        std::make_shared<std::atomic<bool>>(true)};
    std::atomic<bool> pollStop_{false};
    std::optional<bool> internetReachable_;
    bool loginSucceeded_{false};
    bool loginAttemptPending_{false};
    bool loginAttemptAwaitingRequest_{false};
    bool browserFlowStarted_{false};
    bool allowWindowClose_{false};
    std::wstring statusIllustrationAsset_;
    winrt::event_token closingToken_{};
    ServiceManager serviceManager_;
    ServiceCommandGate serviceCommandGate_;
};

} // namespace winrt::CampusNetClient::implementation

namespace winrt::CampusNetClient::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

} // namespace winrt::CampusNetClient::factory_implementation
