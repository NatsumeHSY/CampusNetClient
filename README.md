# 广航海校园网自动登录

本仓库按平台组织：

- [desktop/](desktop/README.md)：Windows 客户端，包含 WinUI 3 UI、系统服务、启动器、更新器、测试和 MSI 打包工具。当前支持电信套餐，移动套餐正在测试。
- [android/](android/README.md)：Android 客户端预留目录，尚未实现；计划仅支持 Android 12 及以上的 ARM 架构设备。鸿蒙或卓易通环境可能有部分功能不支持或不生效，后续不考虑适配。
- `version.json`：Windows 客户端的远端更新清单，保留在仓库根目录以兼容已发布版本。

Windows 解决方案位于 `desktop/CampusNetClient.sln`，版本来源为 `desktop/version.props`。构建、测试和打包产物统一放在仓库外、工作目录根部的 `build/` 中；Windows 发布目录为 `build/package/Release/`。

Android 后续使用独立的构建和版本配置。内部设计文档保存在仓库外，仓库内的 `docs/` 目录由 Git 忽略。
