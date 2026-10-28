# Windows Installer 安装包

CampusNetClient MSI 使用传统 Windows Installer 技术，不采用 MSIX。安装包包含 WinUI 客户端、校园网服务及自动更新组件。

安装时需要管理员授权。安装程序会注册 `CampusNetService`，使其能够随 Windows 启动并执行网络监测和自动登录。校园网账号密码由服务保存到 Windows Credential Manager。

更新和修复由 Windows Installer 应用；卸载可通过 Windows“已安装的应用”完成。服务安装与系统配置变更需要管理员权限。
