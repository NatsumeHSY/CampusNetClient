# CampusNetClient 使用说明

## 安装

运行图形化 MSI 安装包。安装过程会注册 `CampusNetService` 为自动启动的 Windows Service；不需要用户打开命令行或执行脚本。安装目录必须同时包含 `CampusNetClient.exe` 和 `CampusNetService.exe`。

首次运行客户端后，在托盘菜单打开设置，填写校园网账号和密码并保存。密码通过 Windows Credential Manager 写入，不写入日志；服务通过本机受限 Named Pipe 接收必要的配置操作。

## 工作方式

- Service 负责网络变化监听、HTTP portal 登录、联网验证和重试。
- UI 默认隐藏到系统托盘，由 Service 在需要 WebView2 兜底时拉起到当前交互用户会话。
- WebView2 只在用户会话内运行；Service 不创建窗口，也不跨 Session 直接操作桌面。
- UI 自启动关闭后，HTTP 登录仍可运行，但 Service 不会自动启动 WebView2 兜底。

## 卸载

使用 Windows“已安装的应用”中的卸载入口。MSI 会先停止并移除 Service，再移除程序文件。真实校园网登录仍需要授权的测试账号和可访问的测试 portal 才能验证。

