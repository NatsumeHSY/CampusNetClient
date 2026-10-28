#include "LauncherPaths.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowTitle[] = L"广航海校园网自动登录";
constexpr std::size_t kMaximumModulePath = 32768;

void ShowLauncherError(const std::wstring& message) {
    MessageBoxW(nullptr, message.c_str(), kWindowTitle, MB_OK | MB_ICONERROR);
}

bool GetLauncherExecutablePath(std::filesystem::path& path) {
    std::vector<wchar_t> buffer(kMaximumModulePath);
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return false;
    }

    path = std::filesystem::path(std::wstring(buffer.data(), length));
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::filesystem::path launcherPath;
    if (!GetLauncherExecutablePath(launcherPath)) {
        ShowLauncherError(L"无法获取启动器路径，因此无法定位客户端程序。");
        return 1;
    }

    const std::filesystem::path clientPath =
        ResolveClientExecutablePath(launcherPath);
    const std::filesystem::path workingDirectory = clientPath.parent_path();
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(clientPath, fileError) || fileError) {
        ShowLauncherError(L"找不到客户端程序：\n" + clientPath.native());
        return 1;
    }

    const HINSTANCE result = ShellExecuteW(
        nullptr,
        L"open",
        clientPath.c_str(),
        nullptr,
        workingDirectory.c_str(),
        SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ShowLauncherError(L"启动客户端失败：\n" + clientPath.native() +
                           L"\n请检查发布目录中的程序文件是否完整。");
        return 1;
    }

    return 0;
}
