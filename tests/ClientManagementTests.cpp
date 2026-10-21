#include "ClientViewModel.h"
#include "ServiceManager.h"
#include "SingleInstanceActivation.h"
#include "StartupMode.h"

#include <cassert>

int RunClientManagementTests() {
    ClientViewModel viewModel(LoadDefaultConfig());
    assert(viewModel.UiAutostart());
    assert(viewModel.WebViewFallbackEnabled());
    viewModel.SetUiAutostart(false);
    assert(!viewModel.UiAutostart());
    assert(!viewModel.WebViewFallbackEnabled());
    assert(viewModel.WebViewFallbackNotice().find(L"WebView2") !=
           std::wstring::npos);

    assert(MapServiceState(SERVICE_STOPPED) == ManagedServiceState::Stopped);
    assert(MapServiceState(SERVICE_RUNNING) == ManagedServiceState::Running);
    assert(MapServiceState(SERVICE_START_PENDING) ==
           ManagedServiceState::StartPending);
    assert(MapServiceState(SERVICE_STOP_PENDING) ==
           ManagedServiceState::StopPending);

    ServiceCommandGate gate;
    assert(gate.TryAcquire());
    assert(!gate.TryAcquire());
    gate.Release();
    assert(gate.TryAcquire());
    gate.Release();

    assert(ParseStartupMode(L"") == ClientStartupMode::Interactive);
    assert(ParseStartupMode(L"0") == ClientStartupMode::Interactive);
    assert(ParseStartupMode(L"1") == ClientStartupMode::Background);
    assert(ParseStartupMode(L"true") == ClientStartupMode::Background);
    assert(ShouldActivateExistingInstance(ClientStartupMode::Interactive));
    assert(!ShouldActivateExistingInstance(ClientStartupMode::Background));

    const std::wstring activationEventName =
        L"Local\\CampusNetClient.Test.Activate." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64());
    HANDLE activationEvent =
        CreateSingleInstanceActivationEvent(activationEventName.c_str());
    assert(activationEvent != nullptr);
    assert(SignalSingleInstanceActivation(activationEventName.c_str(), 0));
    assert(WaitForSingleObject(activationEvent, 0) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(activationEvent, 0) == WAIT_TIMEOUT);
    CloseHandle(activationEvent);
    return 0;
}
