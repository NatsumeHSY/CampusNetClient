# 广航海校园网自动登录

## 项目简介

CampusNetClient 是一个面向 Windows 的校园网自动登录客户端。它由 WinUI 3 桌面界面、负责 HTTP 认证与网络状态监测的 Windows 服务，以及用于启动客户端的原生 Win32 图形启动器组成。

启动器不提供命令行启动接口。日常使用时直接打开发布目录根部的 `CampusNetLauncher.exe`。

## 发布包结构

解压后保持目录结构完整，不要单独移动或删除 `bin` 中的文件：

```text
Release/
├── CampusNetLauncher.exe
└── bin/
    ├── CampusNetClient.exe
    ├── CampusNetService.exe
    ├── WebView2Loader.dll
    ├── WinUI / Windows App SDK 运行文件与本地化资源
    └── assets/status/        状态贴图
```

`CampusNetLauncher.exe` 会按自身所在目录定位 `bin\CampusNetClient.exe`。客户端和服务所需的运行时文件都在 `bin` 中；发布包必须整体复制或解压到目标机器。

## 运行方法

1. 将完整的 Release 文件夹解压到一个可写目录。
2. 双击 `CampusNetLauncher.exe`。启动器本身不请求管理员权限；客户端启动时会通过 Windows UAC 请求管理员权限，这是服务管理所需的权限。
3. 在“登录凭据”卡片填写校园网账号和密码，选择“保存凭据与设置”。凭据由服务保存到 Windows Credential Manager。
4. 在“Windows 服务”卡片选择“安装服务”，再选择“启动服务”。安装的服务启动类型为自动，后续可在该卡片查看安装状态及运行状态。
5. 保持服务运行。服务会通过 HTTP 方式尝试校园网认证，并在网络变化时重新检查；需要浏览器兜底时，可由服务在当前交互会话启动 UI 和 WebView2 流程。

首次安装服务、启动或卸载服务时，如果 Windows 显示 UAC 提示，请确认操作来自本程序后再允许。

## 系统要求

- Windows 11 x64（项目最低目标版本为 Windows 10.0.22000）。
- 发布包包含自包含的 Windows App SDK / WinUI 运行文件。
- 如果需要 WebView2 浏览器兜底登录，系统还需安装 WebView2 Runtime（Microsoft Edge WebView2 Evergreen Runtime）。没有该 Runtime 时，服务的 HTTP 登录仍可工作，但 WebView2 兜底流程不可用。
- 校园网门户的认证方式和网络策略可能因学校或网络环境变化；本项目不保证适用于未经验证的门户。

## 服务管理与自动登录

服务负责后台 HTTP 门户认证、网络状态监测和本地 UI 通信。服务安装后配置为 Windows 自动启动；客户端窗口关闭或退出，不会自动停止服务。

HTTP 认证流程按当前校园网门户脚本对齐：服务最多等待 90 秒，确认本机有 IPv4 且认证门户可达；先检查门户首页是否已经登录，再向 `/eportal/InterFace.do?method=login` 提交双重 URL 编码的认证请求。只有门户响应包含 `result: success` 才判定认证成功；3 秒后的在线页复查用于状态记录，不会推翻门户已接受的结果。认证参数使用项目配置的门户地址和脚本中的网络参数，账号密码只从 Windows Credential Manager 读取。

网卡信息优先使用名称为“以太网”的接口；未找到时按脚本规则回退到非 APIPA、非回环且非 `vEthernet*` 的 IPv4 地址，并跳过 Hyper-V 描述的 MAC 候选。

“后台行为”中的开关表示是否允许服务在需要浏览器兜底时拉起 UI，并非 Windows 登录启动项。关闭后服务仍可以运行并尝试 HTTP 登录，但 WebView2 自动兜底登录不会启动。需要使用兜底功能时，请保持此开关开启。

服务卡片显示服务安装状态与运行状态，并提供安装、启动、停止和卸载操作。服务状态异常时，可先选择“刷新状态”，再根据卡片中的反馈检查服务是否已安装并运行。

运行状态卡片会显示由 HTTPS 公共 API 查询到的公网出口 IP，并由服务后台每 30 秒检查外网连通性。检测到外网可达时“立即登录”按钮会禁用；检测明确为不可达时重新启用。IP API 不可用时显示“未获取”，不会单独覆盖现有连通性探测结果。

## 系统托盘与单实例

- 手动打开启动器时，客户端显示主窗口。
- 关闭窗口会将客户端隐藏到系统托盘，进程继续运行。
- 左键单击托盘图标可重新打开窗口；右键菜单可打开设置、立即登录、刷新状态或退出客户端。
- 再次手动打开启动器时，会唤醒已有客户端实例，而不会重复创建窗口。服务以隐藏方式拉起 UI 时，重复的隐藏启动不会强行显示窗口。
- 从托盘菜单选择“退出”才会结束客户端进程；这不会卸载或停止 Windows 服务。

## 构建方法

使用 Visual Studio 2022 打开 `CampusNetClient.sln`，安装“使用 C++ 的桌面开发”工作负载、v143 工具集和 Windows SDK 10.0.26100.0。确保 NuGet 依赖还原成功，然后选择 `x64` 与 `Release`，执行“生成解决方案”。

构建完成后的 Release 便携包位于仓库目录的上一级 `build\package\Release\`；Debug 包位于 `build\package\Debug\`。两种配置的包根部都只包含启动器和 `bin` 目录；编译中间文件位于工作目录根部的 `build\obj\`。

## 常见问题

### 提示缺少 WebView2Loader.dll

请确认使用完整发布目录中的 `CampusNetLauncher.exe` 启动，并检查 `bin\WebView2Loader.dll` 是否存在。不要只复制根部启动器或客户端 EXE。

### WebView2 兜底流程没有启动

确认系统已安装 Microsoft Edge WebView2 Evergreen Runtime，并在客户端设置中开启“允许服务自动拉起 UI”。该选项关闭时，服务不会启动 WebView2 窗口。

### 服务显示未安装或无法启动

打开客户端并查看“Windows 服务”卡片。安装服务需要管理员权限；安装后还需启动服务。若操作失败，先刷新状态并查看界面返回的 Windows 错误信息。

### 校园网仍未认证

确认账号密码已保存、Windows 服务正在运行，并检查当前网络是否能访问校园网认证门户。校园网的门户地址、认证字段或网络策略发生变化时，可能需要更新客户端适配；不要将一次构建或其他网络环境中的成功视为当前网络已验证。

### 构建时报 MDM2025 或 Project.winmd 路径无效

当前 Windows App SDK / C++/WinRT 的元数据合并工具可能无法处理包含中文的源码或中间输出路径。若出现此错误，请将仓库临时放在不含中文的本地目录中再构建；这属于构建工具的路径限制，不是启动器的命令行或运行限制。
