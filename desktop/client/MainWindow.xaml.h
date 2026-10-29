#pragma once

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include "MainWindow.g.h"

#include "ClientIpc.h"
#include "ClientViewModel.h"
#include "ServiceManager.h"
#include "StandaloneUpdateRuntime.h"
#include "TrayIcon.h"
#include "UpdateCheckPresentation.h"

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
    void OnWindowShown();

private:
    void RefreshStatus();
    void RequestUpdateCheck();
    void CompleteUpdateCheck(const std::string& phase);
    void ShowUpdateCheckResult(std::string_view phase);
    void EndUpdateOverlay();
    void RefreshLocalNetworkInfo();
    void RequestNetworkStatusRefresh();
    void UpdateLoginButton();
    void StartLoginAttempt(std::wstring requestId);
    void CompleteLoginAttempt(bool succeeded, std::wstring message);
    void SetStatusIndicator(bool connected);
    void SetStatusIllustration(std::wstring_view assetName);
    void StartStatusIndicatorAnimation();
    void HandleTrayCommand(TrayCommand command);
    void StopPolling();
    void RefreshServiceStatus();
    void SetServiceFeedback(std::wstring message);
    std::filesystem::path ResolveServiceBinaryPath() const;
    void ExitApplication();

    ClientViewModel viewModel_;
    ServiceIpcClient ipc_;
    std::unique_ptr<TrayIcon> tray_;
    Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer updateOverlayTimer_{nullptr};
    std::thread pollThread_;
    std::thread loginRequestThread_;
    std::shared_ptr<std::atomic<bool>> lifetimeGuard_{
        std::make_shared<std::atomic<bool>>(true)};
    std::atomic<bool> pollStop_{false};
    std::atomic<bool> updateCheckRequested_{false};
    std::atomic<bool> updateCheckInFlight_{false};
    UpdateCheckPresentation updateCheckPresentation_;
    bool updateCheckTimedOut_{false};
    bool updateResultShowing_{false};
    std::optional<bool> internetReachable_;
    bool loginSucceeded_{false};
    bool loginAttemptPending_{false};
    bool loginAttemptAwaitingRequest_{false};
    bool carrierLoaded_{false};
    bool allowWindowClose_{false};
    std::wstring statusIllustrationAsset_;
    winrt::event_token closingToken_{};
    ServiceManager serviceManager_;
    ServiceCommandGate serviceCommandGate_;
    std::unique_ptr<StandaloneUpdateRuntime> standaloneUpdateRuntime_;
};

} // namespace winrt::CampusNetClient::implementation

namespace winrt::CampusNetClient::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

} // namespace winrt::CampusNetClient::factory_implementation
