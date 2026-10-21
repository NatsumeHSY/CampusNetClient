#include "SingleInstanceActivation.h"

HANDLE CreateSingleInstanceActivationEvent(LPCWSTR eventName) {
    return CreateEventW(nullptr, FALSE, FALSE, eventName);
}

HANDLE OpenSingleInstanceActivationEvent(LPCWSTR eventName) {
    return OpenEventW(SYNCHRONIZE, FALSE, eventName);
}

bool SignalSingleInstanceActivation(LPCWSTR eventName,
                                    DWORD retryMilliseconds) {
    const ULONGLONG deadline = GetTickCount64() + retryMilliseconds;
    for (;;) {
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
        if (event != nullptr) {
            const bool signaled = SetEvent(event) == TRUE;
            CloseHandle(event);
            return signaled;
        }

        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND || GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(20);
    }
}
