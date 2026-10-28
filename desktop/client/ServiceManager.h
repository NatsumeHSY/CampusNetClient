#pragma once

#include <windows.h>

#include <atomic>
#include <string>

enum class ManagedServiceState {
    Missing,
    Stopped,
    StartPending,
    Running,
    StopPending,
    Failed,
};

ManagedServiceState MapServiceState(DWORD state);

class ServiceCommandGate {
public:
    bool TryAcquire();
    void Release();

private:
    std::atomic<bool> active_{false};
};

class ServiceManager {
public:
    explicit ServiceManager(std::wstring serviceName = L"CampusNetService");

    bool Install(const std::wstring& binaryPath, const std::wstring& displayName,
                 std::wstring& error) const;
    bool Start(std::wstring& error) const;
    bool Stop(std::wstring& error) const;
    bool Uninstall(std::wstring& error) const;
    ManagedServiceState Query(std::wstring& error) const;

private:
    static std::wstring LastErrorMessage(DWORD error);
    std::wstring serviceName_;
};
