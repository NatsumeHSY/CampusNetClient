#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

class IUpdaterProcessHost {
public:
    virtual ~IUpdaterProcessHost() = default;
    virtual bool StartRestrictedAgent(const std::filesystem::path& executable,
                                     HANDLE& process,
                                     std::wstring& error) = 0;
    virtual void RequestStop(HANDLE process) = 0;
    virtual bool WaitForExit(HANDLE process,
                             std::chrono::milliseconds timeout) = 0;
    virtual bool IsRunning(HANDLE process) = 0;
    virtual void Terminate(HANDLE process) = 0;
    virtual void Release(HANDLE process) = 0;
    virtual DWORD ProcessId(HANDLE process) = 0;
    virtual bool RequestCheck(HANDLE process) = 0;
};

class WindowsUpdaterProcessHost final : public IUpdaterProcessHost {
public:
    explicit WindowsUpdaterProcessHost(std::filesystem::path stagingDirectory);
    ~WindowsUpdaterProcessHost() override;

    bool StartRestrictedAgent(const std::filesystem::path& executable,
                             HANDLE& process,
                             std::wstring& error) override;
    void RequestStop(HANDLE process) override;
    bool WaitForExit(HANDLE process,
                     std::chrono::milliseconds timeout) override;
    bool IsRunning(HANDLE process) override;
    void Terminate(HANDLE process) override;
    void Release(HANDLE process) override;
    DWORD ProcessId(HANDLE process) override;
    bool RequestCheck(HANDLE process) override;

private:
    struct AgentEvents {
        HANDLE stop = nullptr;
        HANDLE check = nullptr;
    };

    bool PrepareStagingDirectory(std::wstring& error) const;

    std::filesystem::path stagingDirectory_;
    std::mutex mutex_;
    std::map<HANDLE, AgentEvents> events_;
};

class UpdaterProcessSupervisor final {
public:
    using Clock = std::chrono::steady_clock;
    using NowFunction = std::function<Clock::time_point()>;

    UpdaterProcessSupervisor(IUpdaterProcessHost& host,
                             std::filesystem::path executable,
                             std::chrono::milliseconds monitorInterval =
                                 std::chrono::seconds(10),
                             std::chrono::milliseconds stopTimeout =
                                 std::chrono::seconds(5),
                             NowFunction now = [] { return Clock::now(); });
    ~UpdaterProcessSupervisor();

    UpdaterProcessSupervisor(const UpdaterProcessSupervisor&) = delete;
    UpdaterProcessSupervisor& operator=(const UpdaterProcessSupervisor&) = delete;

    bool Start();
    void Stop();
    bool EnsureRunning();
    bool RequestCheck();
    DWORD AgentProcessId() const;
    bool IsExpectedAgentProcess(DWORD processId) const;
    HANDLE AgentProcessHandle() const;

private:
    void MonitorLoop();

    IUpdaterProcessHost& host_;
    std::filesystem::path executable_;
    std::chrono::milliseconds monitorInterval_;
    std::chrono::milliseconds stopTimeout_;
    NowFunction now_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool started_ = false;
    bool stopping_ = false;
    unsigned failureCount_ = 0;
    Clock::time_point retryAfter_{};
    HANDLE agentProcess_ = nullptr;
    std::thread monitorThread_;
};

std::chrono::milliseconds RestartDelay(unsigned failureCount);
