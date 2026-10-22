# CampusNetClient 启动器与发布布局设计

状态：用户已审核通过

## 目标

为 CampusNetClient 增加一个方便双击启动的原生 Windows 启动器，并在仓库根目录提供中文 README，说明项目用途、绿色版启动、首次配置、Windows 服务管理、托盘行为、运行依赖和 Visual Studio 构建方法。

Release 发布目录采用以下结构：

```text
build/package/Release/
├── CampusNetLauncher.exe
└── bin/
    ├── CampusNetClient.exe
    ├── CampusNetService.exe
    └── WinUI、WebView2 与资源文件
```

发布目录根部只保留启动器文件及 `bin` 子目录。编译中间文件仍放在 `build/obj`。工作区根目录已有的 `release` 属于原 Python 客户端，始终不修改。

## 方案与决定

采用独立的原生 Win32 GUI 启动器，而不是把 WinUI 主程序移到根目录，或使用 `.cmd`、PowerShell 等脚本。启动器项目位于 `launcher/`，入口使用 `wWinMain`，链接为 Windows 子系统，不显示控制台、不解析 CLI 参数，也不承担服务或登录逻辑。

启动器根据自身路径定位 `bin/CampusNetClient.exe`，通过 Windows Shell 打开目标程序，并将 `bin` 设为目标工作目录。启动器自身按普通用户权限运行；客户端现有 `requireAdministrator` manifest 继续负责 UAC 提示。目标缺失或 Shell 启动失败时，通过原生消息框说明错误，不静默失败。

客户端通过 `bin` 中的同目录依赖运行，包括现有自包含 Windows App SDK 文件、WebView2 loader、状态图片和服务程序。WebView2 Evergreen Runtime 仍是使用 WebView2 兜底流程时的系统依赖，不随此启动器嵌入。绿色目录可整体移动；凭据和服务安装状态不存于发布目录。

## 解决方案与构建

- 将 `CampusNetLauncher` 加入 `CampusNetClient.sln`，配置 x64 的 Debug/Release。
- Release 下启动器输出至 `build/package/Release/`；客户端、服务和运行时文件输出至 `build/package/Release/bin/`。
- Debug 使用平行目录 `build/package/Debug/`，保持启动器对 `bin/CampusNetClient.exe` 的相对定位一致。共享库、测试程序和中间文件继续使用 `build/<Configuration>` 与 `build/obj`。
- Launcher 的 PDB 和 MSBuild 中间文件保存在 `build/obj/CampusNetLauncher/<Configuration>/`，发布根直接只保留启动器 EXE 与 `bin/`。
- 为解决方案中的客户端、服务和启动器启用实际 Build 配置，确保构建完整生成可运行目录。启动器在解决方案构建中依赖客户端和服务项目完成。
- README 指示用户在 Visual Studio 2022 中选择 `Release | x64` 并生成解决方案；不要求通过命令行启动客户端。

## README 使用内容

README 将说明：

1. 项目由 WinUI 3 客户端、Windows Service 和原生 GUI 启动器组成；服务负责 HTTP 门户认证与网络状态监控，UI 提供凭据、服务和 WebView2 兜底管理。
2. 首次运行 `CampusNetLauncher.exe`，在客户端中填写账号密码并保存，再安装并启动服务。安装后的服务配置为自动启动。
3. UI 自动拉起开关控制服务能否在需要 WebView2 兜底时启动 UI；关闭时 HTTP 服务仍运行，但兜底 UI 不会自动拉起。
4. 手动启动客户端会显示窗口；关闭窗口回到托盘，托盘左键打开，右键提供打开、登录、刷新和退出；再次运行启动器会唤醒已有实例。
5. 最低 Windows 版本、WebView2 Runtime 条件、管理员授权说明及常见故障排查。
6. Visual Studio 2022 的 C++ 桌面开发工具和 Windows SDK 构建前置条件。

README 不提供或暗示命令行产品接口。凭据说明以当前实现为准：客户端把保存操作经本机 IPC 交给服务，服务使用 Windows Credential Manager 保存账号凭据。

## 当前 build 清理范围

用户已明确要求清理当前构建目录。检查到唯一目标为仓库父目录下的 `../build`，包含 768 个文件，约 1.45 GB，顶层均为 `bin`、`Debug`、`legacy-tests`、`obj`、`package`、`Release` 等构建/打包目录。`package` 中包含旧绿色版产物、验证用 MSI 和 WiX 元数据。

实施时清空该精确 `build` 目录中的生成内容，再执行测试与 Release 解决方案构建；构建会重新生成测试/编译文件和新发布目录。不得删除仓库源码、`logs`、工作区根目录 `release` 或任何凭据文件。

## 验收

- Release 解决方案构建成功，`build/package/Release/` 根部仅有 `CampusNetLauncher.exe` 与 `bin/` 子目录。
- `bin/` 包含客户端、服务、WinUI 运行时文件、WebView2 loader 和 UI 所需资源。
- 启动器为 Windows GUI 子系统程序；无控制台窗口、无 CLI 参数入口，可定位并启动 `bin/CampusNetClient.exe`。
- README 的运行步骤和依赖与源码、manifest 及构建输出一致。
- C++ 测试、现有 UI 静态检查和新增布局检查通过。
- 清理仅影响上述 workspace-root `build`。

## 不在范围内

- MSI、商店/MSIX、签名、自动更新或 GitHub 发布。
- 修改原 Python 客户端及其 `release` 目录。
- 将服务安装改成计划任务或调整现有 HTTP/WebView2 登录逻辑。
