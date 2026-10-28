#include "StandaloneUpdateRuntime.h"

#include "../updater/UpdateManifest.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cwchar>
#include <fstream>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path ProgramDataDirectory() {
    std::wstring value(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"ProgramData", value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size()) {
        return {};
    }
    value.resize(length);
    return std::filesystem::path(value);
}

bool ReadInstallInfo(const std::filesystem::path& path,
                     LocalInstallInfo& installInfo, std::wstring& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = L"无法读取本地 version.json，更新代理未启动。";
        return false;
    }
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size <= 0 || size > 64 * 1024) {
        error = L"本地 version.json 大小无效，更新代理未启动。";
        return false;
    }
    input.seekg(0, std::ios::beg);
    std::string json(static_cast<std::size_t>(size), '\0');
    if (!input.read(json.data(), static_cast<std::streamsize>(json.size())) ||
        !ParseLocalInstallInfo(json, installInfo, error)) {
        if (error.empty()) {
            error = L"本地 version.json 格式无效，更新代理未启动。";
        }
        return false;
    }
    return true;
}

bool BuildEnvironment(
    const std::vector<std::pair<std::wstring, std::wstring>>& overrides,
    std::vector<wchar_t>& block) {
    LPWCH raw = GetEnvironmentStringsW();
    if (raw == nullptr) {
        return false;
    }
    std::vector<std::wstring> entries;
    for (const wchar_t* current = raw; *current != L'\0';
         current += wcslen(current) + 1) {
        entries.emplace_back(current);
    }
    FreeEnvironmentStringsW(raw);

    for (const auto& [name, value] : overrides) {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [&](const std::wstring& entry) {
                const auto equals = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
                return equals != std::wstring::npos &&
                    CompareStringOrdinal(entry.data(), static_cast<int>(equals),
                        name.data(), static_cast<int>(name.size()), TRUE) ==
                        CSTR_EQUAL;
            }), entries.end());
        entries.push_back(name + L"=" + value);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left,
                                                  const auto& right) {
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) ==
               CSTR_LESS_THAN;
    });
    block.clear();
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (entries.empty()) {
        block.push_back(L'\0');
    }
    return true;
}

ProtocolMessage ErrorReply(const ProtocolMessage& request, DWORD code) {
    return {kProtocolVersion, request.type, request.requestId,
            "{\"errorCode\":" + std::to_string(code) + "}"};
}

} // namespace

StandaloneUpdateRuntime::StandaloneUpdateRuntime(
    std::filesystem::path updaterDirectory)
    : updaterDirectory_(std::move(updaterDirectory)),
      updaterExecutable_(updaterDirectory_ / L"CampusNetUpdater.exe"),
      installRoot_(updaterDirectory_),
      stagingDirectory_(ProgramDataDirectory() / L"CampusNetClient" /
                        L"UpdateStaging"),
      updaterProcessHost_(stagingDirectory_),
      updaterSupervisor_(updaterProcessHost_, updaterExecutable_),
      updateApplyProcessHost_(ProgramDataDirectory() / L"CampusNetClient",
                              updaterExecutable_, stagingDirectory_) {}

StandaloneUpdateRuntime::~StandaloneUpdateRuntime() { Stop(); }

bool StandaloneUpdateRuntime::Start(std::wstring& error) {
    std::scoped_lock lock(mutex_);
    if (started_) {
        error.clear();
        return true;
    }
    if (updaterDirectory_.empty() || !updaterDirectory_.is_absolute() ||
        stagingDirectory_.empty() || !stagingDirectory_.is_absolute() ||
        !ReadInstallInfo(updaterDirectory_ / L"version.json",
                         localInstallInfo_, error)) {
        if (error.empty()) {
            error = L"更新代理路径无效，自动更新暂不可用。";
        }
        return false;
    }
    if (localInstallInfo_.channel == UpdateChannel::Portable) {
        installRoot_ = updaterDirectory_.parent_path();
    }

    UpdateBrokerCallbacks callbacks;
    callbacks.requestCheck = [this] { return RequestAgentCheck(); };
    callbacks.hasInteractiveSession = [] { return true; };
    callbacks.showUpdater = [this] { return ShowUpdater(); };
    callbacks.applyStagedUpdate = [this](const TrustedUpdatePlan& plan) {
        return ApplyStagedUpdate(plan);
    };
    broker_ = std::make_unique<UpdateBroker>(
        localInstallInfo_.version, localInstallInfo_.channel, stagingDirectory_,
        std::move(callbacks));
    agentIpc_ = std::make_unique<UpdateAgentIpc>(
        updaterSupervisor_, *broker_, stagingDirectory_);
    if (!ipc_.Start([this](const ProtocolMessage& request,
                           const ServiceIpcClientContext& client) {
            return Handle(request, client);
        })) {
        agentIpc_.reset();
        broker_.reset();
        error = L"无法启动本地更新代理通信通道。";
        return false;
    }
    started_ = true;
    error.clear();
    return true;
}

void StandaloneUpdateRuntime::Stop() {
    {
        std::scoped_lock lock(mutex_);
        if (!started_) {
            return;
        }
        started_ = false;
    }
    ipc_.Stop();
    updaterSupervisor_.Stop();
    std::scoped_lock lock(mutex_);
    agentStarted_ = false;
    agentIpc_.reset();
    broker_.reset();
}

bool StandaloneUpdateRuntime::RequestAgentCheck() {
    std::scoped_lock lock(mutex_);
    if (!started_) {
        return false;
    }
    if (!agentStarted_) {
        agentStarted_ = updaterSupervisor_.Start();
        return agentStarted_;
    }
    return updaterSupervisor_.RequestCheck();
}

bool StandaloneUpdateRuntime::ShowUpdater() {
    std::filesystem::path copiedUpdater;
    std::wstring error;
    if (!updateApplyProcessHost_.PrepareInteractiveGui(copiedUpdater, error)) {
        return false;
    }
    std::vector<wchar_t> environment;
    const std::vector<std::pair<std::wstring, std::wstring>> variables{
        {L"CAMPUSNET_UPDATE_INSTALL_ROOT", installRoot_.wstring()},
        {L"CAMPUSNET_UPDATE_CHANNEL",
         localInstallInfo_.channel == UpdateChannel::Portable ? L"portable"
                                                             : L"msi"},
    };
    if (!BuildEnvironment(variables, environment)) {
        return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + copiedUpdater.wstring() + L"\"";
    if (!CreateProcessW(copiedUpdater.c_str(), command.data(), nullptr, nullptr,
                        FALSE, CREATE_UNICODE_ENVIRONMENT, environment.data(),
                        copiedUpdater.parent_path().c_str(), &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool StandaloneUpdateRuntime::ApplyStagedUpdate(
    const TrustedUpdatePlan& plan) {
    std::filesystem::path copiedUpdater;
    std::wstring error;
    return updateApplyProcessHost_.LaunchApply(plan, installRoot_,
                                               copiedUpdater, error);
}

ProtocolMessage StandaloneUpdateRuntime::Handle(
    const ProtocolMessage& request, const ServiceIpcClientContext& client) {
    if (request.type == MessageType::UpdateAgentStatus) {
        if (agentIpc_) {
            return agentIpc_->Handle(request, client);
        }
        return ErrorReply(request, ERROR_INVALID_STATE);
    }
    switch (request.type) {
    case MessageType::CheckForUpdates:
    case MessageType::GetUpdateStatus:
    case MessageType::ShowUpdater:
    case MessageType::ApplyStagedUpdate:
        return broker_ ? broker_->Handle(request, client.interactiveUser)
                       : ErrorReply(request, ERROR_INVALID_STATE);
    default:
        return ErrorReply(request, ERROR_SERVICE_NOT_ACTIVE);
    }
}
