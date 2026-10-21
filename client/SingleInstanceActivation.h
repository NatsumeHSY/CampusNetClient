#pragma once

#include <windows.h>

inline constexpr wchar_t kSingleInstanceActivationEventName[] =
    L"Local\\CampusNetClient.Activate.v1";

HANDLE CreateSingleInstanceActivationEvent(LPCWSTR eventName =
                                               kSingleInstanceActivationEventName);
HANDLE OpenSingleInstanceActivationEvent(LPCWSTR eventName =
                                              kSingleInstanceActivationEventName);
bool SignalSingleInstanceActivation(LPCWSTR eventName =
                                        kSingleInstanceActivationEventName,
                                    DWORD retryMilliseconds = 1000);
