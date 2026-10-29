#include "ClientViewModel.h"
#include "ServiceManager.h"
#include "SingleInstanceActivation.h"
#include "StartupMode.h"

#include <cassert>
#include <optional>

template <typename ViewModel>
bool LoginAttemptAvailabilityBehavesCorrectly(const ViewModel& viewModel) {
    if constexpr (requires {
                      viewModel.CanRequestLogin(std::optional<bool>{}, false,
                                                true);
                  }) {
        return !viewModel.CanRequestLogin(false, false, true) &&
               !viewModel.CanRequestLogin(std::nullopt, false, true) &&
               !viewModel.CanRequestLogin(false, true, false) &&
               viewModel.CanRequestLogin(false, false, false) &&
               viewModel.CanRequestLogin(std::nullopt, false, false) &&
               !viewModel.CanRequestLogin(true, false, false);
    }
    return false;
}

int RunClientManagementTests() {
    ClientViewModel viewModel(LoadDefaultConfig());

    assert(viewModel.CanRequestLogin(std::nullopt, false));
    assert(!viewModel.CanRequestLogin(std::nullopt, true));
    assert(!viewModel.CanRequestLogin(true, false));
    assert(!viewModel.CanRequestLogin(true, true));
    assert(viewModel.CanRequestLogin(false, false));
    assert(!viewModel.CanRequestLogin(false, true));
    assert(LoginAttemptAvailabilityBehavesCorrectly(viewModel));

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
