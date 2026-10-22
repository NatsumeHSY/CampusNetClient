# MSI 构建

使用 `build-msi.ps1` 构建非 MSIX 的传统 Windows Installer 包。脚本从仓库根目录 `version.props` 读取唯一 SemVer，接收已构建的 Release `bin` 目录，并在 `build\package` 生成 MSI 和同目录 `*.msi.files.json` 安装文件哈希清单。

```powershell
pwsh -File installer\build-msi.ps1 `
  -PackageBinDir build\package\Release\bin
```

需要 WiX Toolset v4 的 `wix.exe` 可用；也可以通过 `-WixPath` 指向其完整路径。`-OutputPath` 可覆盖 MSI 输出位置；若目标文件已经存在，默认拒绝覆盖，只有显式传入 `-Force` 才会覆盖。临时 MSI payload 会放在目标 `build\package` 的唯一子目录，完成后只清理脚本创建的临时目录。

脚本把 SemVer 的 major/minor/patch 映射为 MSI ProductVersion（major/minor 最大 255，patch 最大 65535），预发布与构建元数据不参与 MSI 版本比较。ProductCode 从 MSI 数值版本稳定派生，使同一 MSI 数值版本能够进行 `REINSTALL=ALL REINSTALLMODE=vomus` 修复；WiX 的 `AllowDowngrades` 允许更新器按仓库清单回退到较低版本。此策略只适用于由该脚本构建且共享 UpgradeCode 的 CampusNetClient MSI。

脚本构建的安装内容包括 WinUI 客户端文件、服务、更新器、MSI 渠道 `version.json` 和服务安装元数据。MSI 由 SYSTEM apply helper 通过 Windows Installer API 应用；不会运行 `msiexec.exe` 命令行。开发机不应直接执行安装、降级或修复测试；这些场景应在干净 Windows VM 中验收。
