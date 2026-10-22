#include "pch.h"
#include "App.xaml.h"

#include "MainWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::CampusNetClient::implementation {

App::App(ClientStartupMode startupMode) : startupMode_(startupMode) {
    InitializeComponent();
}

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    if (window_ == nullptr) {
        window_ = make<MainWindow>();
    }
    if (startupMode_ == ClientStartupMode::Background) {
        window_.AppWindow().Hide();
        return;
    }
    window_.Activate();
    winrt::get_self<MainWindow>(window_)->OnWindowShown();
}

} // namespace winrt::CampusNetClient::implementation
