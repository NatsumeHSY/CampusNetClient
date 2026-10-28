# 广航海校园网自动登录

CampusNetClient 是面向 Windows 的校园网自动登录客户端，由 WinUI 3 客户端、Windows 服务、原生 Win32 启动器，以及独立更新器组成。客户端和更新器都没有公开 CLI；绿色版从根目录的 `CampusNetLauncher.exe` 启动。

## 发布包与首次使用

绿色版目录保持完整：

```text
CampusNetClient-Release/
├── CampusNetLauncher.exe
└── bin/
    ├── CampusNetClient.exe
    ├── CampusNetService.exe
    ├── CampusNetUpdater.exe
    ├── version.json
    ├── WebView2Loader.dll
    └── WinUI / Windows App SDK 运行文件、资源与 assets/status/
```

将完整目录解压到可写位置，双击根目录启动器。在“登录凭据”卡片保存校园网账号密码，再在“Windows 服务”卡片安装并启动服务。服务使用 Windows Credential Manager 保存凭据；服务安装/管理操作需要管理员授权。MSI 版由 Windows Installer 安装到 Program Files，并安装自动启动的 `CampusNetService`。

HTTP 认证流程按仓库内的成功脚本对齐：服务先检查门户状态，再提交认证请求；只有门户返回成功才报告认证成功。浏览器/WebView2 是 HTTP 认证失败时的兜底流程，要求系统安装 Evergreen WebView2 Runtime。状态卡片显示服务连接状态、本机网络信息、公网出口 IP 和外网可达状态。

支持电信套餐，以及移动套餐（正在测试）。在“登录凭据”的“校园网套餐”中选择电信或移动，然后点击“保存凭据与设置”。默认电信；保存时需要服务已启动，选择由服务保存，重启客户端和服务后会恢复。电信沿用 eportal 认证，移动读取门户表单及隐藏字段并提交到 webauth.do，再检查外网是否可达。移动套餐仍需在实际校园网环境验证。

## 自动更新


仓库根目录的 `version.json` 是远端发布清单；它与应用目录中的本地 `version.json` 作用不同。便携版本地标记位于 `bin/version.json`，MSI 本地标记位于安装根目录。`version.props` 是项目版本号的唯一来源，生成器会把同一版本写入便携包、MSI 和远端清单。

### 更新链路

1. `CampusNetService` 由 Service Control Manager 在开机时启动。服务启动后启动一个位于 Session 0 的受限 updater agent；它立即检查一次，之后约每 6 小时检查并加入随机抖动，失败后按退避间隔重试。服务监视 agent，agent 意外退出时会重新拉起。
2. agent 通过 HTTPS 读取 GitHub 仓库根目录的 raw `version.json`。清单只接受固定仓库的 GitHub Release URL，并记录 SemVer、公告、两个渠道的下载 URL、大小、SHA-256 和每个受管文件的 SHA-256。
3. 只要远端 SemVer 与本地字符串不完全相等就执行更新，远端较低版本也会更新到清单指定版本；版本相同但受管文件哈希不符时，也会重新下载修复。下载由 Session 0 agent 完成，先写入受限的 ProgramData 暂存目录，再核验长度和 SHA-256。
4. 如果有交互式用户会话，服务会从受保护的 ProgramData 位置启动普通用户权限的原生 Win32 updater GUI。GUI 展示本地/目标版本、渠道、公告、下载进度、速度和错误。客户端第一次显示窗口时也会请求检查，并在最多 10 秒内显示不可操作的检查遮罩；发现更新时由 updater GUI 接管展示。
5. 下载完成后，GUI 通过本机受 ACL 保护的 IPC 请求服务应用更新。服务启动一个从安装目录复制到 ProgramData 的 SYSTEM apply helper，所以不会锁住正被替换的 updater 文件。helper 再次读取远端清单并核验版本、渠道、下载大小、SHA-256 和暂存文件。
6. 绿色版先在安装目录同卷解包到受保护的旁路目录，校验 ZIP 路径、条目、文件列表、每个文件的 SHA-256 和版本标记；然后停止服务、关闭客户端进程并交换目录。新服务启动失败时会恢复旧目录。
7. MSI 版通过 Windows Installer API 应用已校验 MSI，不调用 `msiexec` 命令行。它核验本机 MSI 注册信息、安装位置和本地版本标记。远端版本可以高于或低于本地；相同 Windows Installer 数值版本则执行 `REINSTALL=ALL REINSTALLMODE=vomus` 修复。MSI 版本采用 SemVer 的前三个数字字段，预发布/构建元数据不进入 MSI 数值版本。
8. 更新完成后，updater GUI 使用 `runas` 启动主客户端。主客户端 manifest 声明 `requireAdministrator`，因此这一步可能出现 UAC；用户取消时更新仍已安装，手动打开客户端即可。

开机启动的 Windows 服务以及 Session 0 受限 updater agent 本身不显示 UAC；普通用户权限的 updater GUI 也不请求提升。更新完成后通过 `runas` 重新启动 `requireAdministrator` 主客户端时可能出现 UAC；用户取消不会撤销已应用的更新。

UAC/会话边界：服务的浏览器兜底路径目前通过 `CreateProcessAsUserW` 使用活动用户 token 启动主客户端，而主客户端 manifest 要求 `requireAdministrator`。这不是 `runas` 提权调用，不会主动显示交互式 UAC 同意框；在默认 UAC 配置下，进程可能因活动用户 token 未提升而无法启动。该路径必须在启用默认 UAC 的干净 Windows 虚拟机中验证；在验证完成前，不能保证“开机后 UI 一定无提示启动”。服务与 updater agent 的自动检查/下载仍可在没有 UI 的情况下运行。更新检查或下载失败不会阻止校园网服务继续运行。

更新完整性依赖 HTTPS、固定仓库 URL allowlist 和 SHA-256 清单校验。SHA-256 能检测下载损坏或与清单不一致，但不是发布者数字签名；正式公开分发仍建议对二进制和 MSI 进行 Authenticode 签名。

## 托盘和实例行为

- 手动打开启动器会显示主窗口；关闭窗口会隐藏到托盘，进程继续运行。
- 点击托盘图标重新打开窗口；右键菜单可打开窗口或退出客户端。
- 重复手动启动会唤醒已有实例；服务的隐藏启动不会强行显示已有 UI。
- 退出 UI 不会停止 Windows 服务。

## 构建

需要 Visual Studio 2022 C++ 工作负载、v143、Windows SDK 10.0.26100.0、Windows App SDK NuGet 依赖，以及 WiX Toolset v4（MSI 构建）。所有构建产物放在工作区根目录 `build`，不要放入原 Python 项目的 `release`。

WinUI 的元数据合并工具在部分环境下对含非 ASCII 字符的路径不稳定；可将工作区临时映射为 ASCII 盘符，再调用 MSBuild。请在包含 `CampusNetClient` 子目录的工作区根目录执行，并按本机 Visual Studio 版本调整 MSBuild 路径：

```powershell
$workspaceRoot = (Get-Location).Path
$msbuild = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
subst T: $workspaceRoot
try {
    & $msbuild `
      T:\CampusNetClient\CampusNetClient.sln /m /p:Configuration=Release /p:Platform=x64
} finally {
    subst T: /d
}
```

## 验证边界与常见问题

- 当前实现通过单元/静态测试和本地构建验证；真实服务安装、Session 0 到交互桌面的行为、UAC 交互、MSI 安装/修复/降级和绿色版回滚仍应在干净 Windows 虚拟机中验收，不要在唯一日用机上尝试首次 MSI 更新。
- 缺少 `WebView2Loader.dll` 时，检查是否从完整绿色包根目录启动且 `bin\WebView2Loader.dll` 存在。
- WebView2 兜底未启动时，确认 Evergreen Runtime 已安装并允许服务拉起 UI。
- 更新检查遮罩最多等待 10 秒，随后在遮罩中央显示检查结果及对应图标，停留 1500ms 后关闭并恢复窗口操作。结果会区分没有新版本、检查超时、检查失败和发现新版本。
- 公网 IP API 暂时不可用时显示未获取，不会把未知状态误判为外网可达。
