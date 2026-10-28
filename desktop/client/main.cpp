#include "pch.h"

#include "App.xaml.h"
#include "SingleInstanceActivation.h"
#include "StartupMode.h"

#include <string>

namespace {

constexpr wchar_t kSingleInstanceMutexName[] =
    L"Local\\CampusNetClient.SingleInstance.v1";

HANDLE AcquireSingleInstanceMutex() {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
    if (mutex == nullptr) {
        return nullptr;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return nullptr;
    }
    return mutex;
}

ClientStartupMode StartupModeFromEnvironment() {
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(
        L"CAMPUSNET_UI_START_HIDDEN", value, ARRAYSIZE(value));
    return ParseStartupMode(length == 0 ? std::wstring_view{} :
                                               std::wstring_view(value, length));
}

} // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    const ClientStartupMode startupMode = StartupModeFromEnvironment();
    const HANDLE singleInstanceMutex = AcquireSingleInstanceMutex();
    if (singleInstanceMutex == nullptr) {
        if (ShouldActivateExistingInstance(startupMode)) {
            SignalSingleInstanceActivation();
        }
        return 0;
    }

    const HANDLE activationEvent = CreateSingleInstanceActivationEvent();
    winrt::Microsoft::UI::Xaml::Application::Start([startupMode](auto&&) {
        // The startup mode is captured below instead of exposed as a public
        // command-line switch. The service is the only caller that sets the
        // environment marker for a hidden UI handoff.
        winrt::make<winrt::CampusNetClient::implementation::App>(startupMode);
    });
    if (activationEvent != nullptr) {
        CloseHandle(activationEvent);
    }
    CloseHandle(singleInstanceMutex);
    return 0;
}
