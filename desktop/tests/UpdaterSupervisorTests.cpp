#include "UpdaterProcessSupervisor.h"

#include <cassert>
#include <map>

namespace {

class FakeProcessHost final : public IUpdaterProcessHost {
public:
    bool StartAgent(const std::filesystem::path&, HANDLE& process,
                    std::wstring& error) override {
        ++startCalls;
        if (failStarts > 0) {
            --failStarts;
            error = L"simulated create-process failure";
            return false;
        }
        const auto value = nextHandle++;
        process = reinterpret_cast<HANDLE>(value);
        running[value] = true;
        error.clear();
        return true;
    }

    void RequestStop(HANDLE process) override {
        ++stopRequests;
        const auto value = reinterpret_cast<std::uintptr_t>(process);
        if (!ignoreStop) {
            running[value] = false;
        }
    }

    bool WaitForExit(HANDLE process, std::chrono::milliseconds) override {
        ++waitCalls;
        const auto value = reinterpret_cast<std::uintptr_t>(process);
        return !running[value];
    }

    bool IsRunning(HANDLE process) override {
        const auto value = reinterpret_cast<std::uintptr_t>(process);
        return running[value];
    }

    void Terminate(HANDLE process) override {
        ++terminateCalls;
        const auto value = reinterpret_cast<std::uintptr_t>(process);
        running[value] = false;
    }

    void Release(HANDLE process) override {
        ++releaseCalls;
        running.erase(reinterpret_cast<std::uintptr_t>(process));
    }

    DWORD ProcessId(HANDLE process) override {
        return static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(process));
    }

    bool RequestCheck(HANDLE process) override {
        return IsRunning(process);
    }

    void Crash(HANDLE process) {
        running[reinterpret_cast<std::uintptr_t>(process)] = false;
    }

    unsigned failStarts = 0;
    unsigned startCalls = 0;
    unsigned stopRequests = 0;
    unsigned waitCalls = 0;
    unsigned terminateCalls = 0;
    unsigned releaseCalls = 0;
    bool ignoreStop = false;

private:
    std::uintptr_t nextHandle = 1;
    std::map<std::uintptr_t, bool> running;
};

} // namespace

int RunUpdaterSupervisorTests() {
    assert(RestartDelay(0) == std::chrono::seconds(1));
    assert(RestartDelay(1) == std::chrono::seconds(1));
    assert(RestartDelay(2) == std::chrono::seconds(2));
    assert(RestartDelay(100) <= std::chrono::minutes(5));

    auto now = std::chrono::steady_clock::time_point{};
    FakeProcessHost failedHost;
    failedHost.failStarts = 1;
    UpdaterProcessSupervisor failedSupervisor(
        failedHost, L"CampusNetUpdater.exe", std::chrono::hours(1),
        std::chrono::seconds(2), [&] { return now; });
    assert(failedSupervisor.Start());
    assert(failedHost.startCalls == 1);
    assert(!failedSupervisor.EnsureRunning());
    now += RestartDelay(1);
    assert(failedSupervisor.EnsureRunning());
    assert(failedHost.startCalls == 2);
    failedSupervisor.Stop();
    assert(failedHost.stopRequests == 1);
    assert(failedHost.waitCalls == 1);
    assert(failedHost.releaseCalls == 1);

    now = std::chrono::steady_clock::time_point{};
    FakeProcessHost crashHost;
    UpdaterProcessSupervisor crashSupervisor(
        crashHost, L"CampusNetUpdater.exe", std::chrono::hours(1),
        std::chrono::seconds(2), [&] { return now; });
    assert(crashSupervisor.Start());
    const HANDLE first = crashSupervisor.AgentProcessHandle();
    crashHost.Crash(first);
    assert(!crashSupervisor.EnsureRunning());
    now += RestartDelay(1);
    assert(crashSupervisor.EnsureRunning());
    assert(crashHost.startCalls == 2);
    crashSupervisor.Stop();

    FakeProcessHost stopTimeoutHost;
    stopTimeoutHost.ignoreStop = true;
    UpdaterProcessSupervisor stopTimeoutSupervisor(
        stopTimeoutHost, L"CampusNetUpdater.exe", std::chrono::hours(1),
        std::chrono::milliseconds(1));
    assert(stopTimeoutSupervisor.Start());
    stopTimeoutSupervisor.Stop();
    assert(stopTimeoutHost.terminateCalls == 1);
    assert(stopTimeoutHost.waitCalls == 2);

    return 0;
}
