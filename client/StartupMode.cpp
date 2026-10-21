#include "StartupMode.h"

ClientStartupMode ParseStartupMode(std::wstring_view value) {
    return value == L"1" || value == L"true" || value == L"TRUE"
               ? ClientStartupMode::Background
               : ClientStartupMode::Interactive;
}

bool ShouldActivateExistingInstance(ClientStartupMode startupMode) {
    return startupMode == ClientStartupMode::Interactive;
}

