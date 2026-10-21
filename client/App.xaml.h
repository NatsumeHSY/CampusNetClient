#pragma once

#include "App.xaml.g.h"
#include "StartupMode.h"

#include <memory>

namespace winrt::CampusNetClient::implementation {

struct App : AppT<App> {
    explicit App(ClientStartupMode startupMode = ClientStartupMode::Interactive);

    void OnLaunched(
        Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

private:
    Microsoft::UI::Xaml::Window window_{nullptr};
    ClientStartupMode startupMode_{ClientStartupMode::Interactive};
};

} // namespace winrt::CampusNetClient::implementation

