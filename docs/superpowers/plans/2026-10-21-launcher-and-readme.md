# CampusNetClient 启动器与 README 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 提供中文项目 README 和无控制台的原生启动器，并让完整运行时文件统一落在发布目录 `bin/` 下。

**Architecture:** 新建 Win32 GUI 启动器，以自身路径为基准启动 `bin/CampusNetClient.exe`，由客户端 manifest 处理 UAC。客户端与服务的 Debug/Release 运行文件分别放在 `build/package/<Configuration>/bin/`，启动器放在对应发布根目录；解决方案构建依赖确保发布包完整。

**Tech Stack:** C++20、Win32、MSBuild/v143、Visual Studio solution、PowerShell 7 静态验收。

**Spec:** `docs/superpowers/specs/2026-10-21-launcher-and-release-layout-design.md`

## Global Constraints

- 只为 x64 配置 Debug 和 Release。
- 发布目录为 workspace-root `build/package/<Configuration>`；启动器在根部，运行时文件在 `bin/`。
- 编译中间文件位于 workspace-root `build/obj`；共享库与测试输出保持 `build/<Configuration>`。
- Launcher 使用 `wWinMain` 和 Windows 子系统；没有 CLI 参数接口或控制台窗口。
- Launcher 自身不提权；客户端现有 `requireAdministrator` manifest 负责 UAC。
- 只清理 workspace-root `build`；绝不触碰 workspace-root `release`、源码、`logs` 或凭据。
- 保留当前工作树中已有的单实例唤醒未提交修改；不做 Git commit 或 push。

## Review Focus

1. 启动器所在路径或便携目录名称包含空格、中文时，仍应解析到同级 `bin/CampusNetClient.exe`；由 `LauncherPaths` 单元测试覆盖。
2. 客户端文件缺失时，Launcher 应显示可理解的图形错误，不创建控制台或静默退出；由 Launcher 源码/项目静态检查覆盖。
3. UAC 由客户端 manifest 触发而非启动器自身提前触发；由发布检查同时核对两个 manifest 和 Shell `open` 启动方式。
4. 部分发布目录缺少客户端、服务、WebView2 loader 或 WinUI 资源时，布局验收必须失败；由 `LauncherPackagingTests.ps1` 覆盖。
5. 重复启动时已有客户端实例仍被唤醒，隐藏服务启动仍不弹窗；保留并运行 `ClientManagementTests` 中的启动模式和命名事件回归测试。

---

### Task 1: 清理当前旧 build 产物

**Files:**
- Generated output only: workspace-root `build/`

- [ ] **Step 1: 再次确认并清理精确目标**

解析 `../build` 并断言它位于仓库父目录的工作区根目录。断言通过后，仅枚举并删除该目录的直接子项；不要删除 build 目录本身，也不要将清理作用于 `release`、repo 根目录或任意变量未解析的路径。该操作包含旧验证 MSI、WiX 元数据和 `CampusNetClient-green` 生成目录。

- [ ] **Step 2: 核验清理边界**

确认 workspace-root `build` 为空；确认 workspace-root `release` 及仓库源码仍存在，且 Git 中的源文件状态没有因清理发生变化。

### Task 2: 加入可测试的启动器路径解析

**Files:**
- Create: `launcher/LauncherPaths.h`
- Create: `launcher/LauncherPaths.cpp`
- Create: `tests/LauncherTests.cpp`
- Modify: `tests/CampusNetTests.vcxproj`
- Modify: `tests/ConfigTests.cpp`

**Interfaces:**
- Produces: `std::filesystem::path ResolveClientExecutablePath(const std::filesystem::path& launcherExecutable)`，返回启动器目录下 `bin/CampusNetClient.exe` 的完整组合路径。

- [ ] **Step 1: 先写失败的路径测试**

在 `LauncherTests.cpp` 中添加 `RunLauncherTests()`，断言：

  - `C:\Portable Folder\CampusNetLauncher.exe` 解析为 `C:\Portable Folder\bin\CampusNetClient.exe`。
  - `D:\测试目录\校园网\CampusNetLauncher.exe` 保留 Unicode 路径并解析为同级 `bin` 目标。

在测试工程中登记该测试文件，在 `ConfigTests.cpp` 声明并调用 `RunLauncherTests()`。

- [ ] **Step 2: 运行测试并确认失败**

运行 Debug `CampusNetTests` 构建。预期因 `LauncherPaths.h`/接口尚不存在而编译失败。

- [ ] **Step 3: 实现最小路径 helper**

在 `LauncherPaths.h/.cpp` 定义上述签名；实现只组合 `launcherExecutable.parent_path() / L"bin" / L"CampusNetClient.exe"`，不检查文件是否存在。

- [ ] **Step 4: 运行测试确认通过**

将 `LauncherPaths.cpp` 加入测试工程并补齐 `launcher` include 路径；构建并运行 `CampusNetTests.exe`，预期退出码为 0。

### Task 3: 新建无控制台 Win32 启动器

**Files:**
- Create: `launcher/CampusNetLauncher.vcxproj`
- Create: `launcher/LauncherMain.cpp`
- Create: `launcher/launcher.manifest`
- Modify: `launcher/LauncherPaths.h/.cpp`（复用 Task 2 接口）

**Interfaces:**
- Consumes: Task 2 的 `ResolveClientExecutablePath(...)`。
- Produces: `CampusNetLauncher.exe`，Windows GUI 子系统入口为 `wWinMain`。

- [ ] **Step 1: 创建 launcher 项目并配置 Windows 子系统**

配置 v143、Unicode、C++20、x64 Debug/Release、`<SubSystem>Windows</SubSystem>`、`launcher.manifest`（`asInvoker`），引用 Shell32。提供项目级默认属性 `CampusNetBuildRoot=$(SolutionDir)..\build` 和 `CampusNetPackageRoot=$(CampusNetBuildRoot)\package\$(Configuration)`，确保独立构建时输出属性有效；Task 4 会让客户端和服务使用相同值。把 `Link.ProgramDatabaseFile` 定向到 `$(IntDir)$(TargetName).pdb`，并关闭增量链接，避免 PDB 或 `.ilk` 文件落入发布根目录。

- [ ] **Step 2: 实现启动和图形错误路径**

`LauncherMain.cpp` 从 `GetModuleFileNameW` 取得启动器路径，通过 Task 2 helper 定位客户端；用 `ShellExecuteW` 的 `open` verb、空参数、`bin` 工作目录启动目标。目标不存在或 Shell 返回失败时显示 `MessageBoxW`，成功时直接退出。不得调用 console API 或解析命令行参数。

- [ ] **Step 3: 构建并检查 GUI 子系统**

构建 launcher Debug/Release。用 `dumpbin /headers` 或等价 PE 检查确认 Subsystem 为 Windows GUI；构建日志不得出现该项目使用 Console 子系统的配置。

### Task 4: 配置完整解决方案发布布局

**Files:**
- Modify: `CampusNetClient.sln`
- Modify: `client/CampusNetClient.vcxproj`
- Modify: `service/CampusNetService.vcxproj`
- Modify: `launcher/CampusNetLauncher.vcxproj`

**Interfaces:**
- Consumes: Task 3 的 Launcher 项目。
- Produces: `build/package/<Configuration>/CampusNetLauncher.exe` 和 `build/package/<Configuration>/bin/` 运行文件夹。

- [ ] **Step 1: 定义统一包路径属性**

在客户端和服务项目中为 `CampusNetBuildRoot` 提供默认值 `$(SolutionDir)..\build`；`CampusNetPackageRoot` 默认值为 `$(CampusNetBuildRoot)\package\$(Configuration)`。客户端和服务 `OutDir` 设为 `$(CampusNetPackageRoot)\bin\`；启动器沿用 Task 3 中相同属性并将 `OutDir` 设为 `$(CampusNetPackageRoot)\`。三个项目的 `IntDir` 均为 `$(CampusNetBuildRoot)\obj\$(ProjectName)\$(Configuration)\`。

- [ ] **Step 2: 移除过时的服务复制目标**

客户端与服务直接输出到同一个 `bin` 后，移除客户端中从 `build/<Configuration>/CampusNetService.exe` 复制服务的旧 post-build target，避免复制旧路径或复制自身。

- [ ] **Step 3: 登记 solution 构建配置和顺序**

在 `.sln` 加入 Launcher 的 Debug/Release x64 映射；为 client、service、launcher 两个配置都登记 `Build.0`。添加 launcher 对 client 和 service 的 solution dependencies，使构建完成时其运行文件均已生成。保留 shared/tests 现有配置。

- [ ] **Step 4: 运行布局验收**

在 Task 1 清理旧输出后构建 Release solution。断言发布根直接文件只有 `CampusNetLauncher.exe`，并且 `bin` 存在 `CampusNetClient.exe`、`CampusNetService.exe`、`WebView2Loader.dll`、客户端状态资源以及 Windows App SDK runtime 文件。检查 `obj` 仍留在 workspace-root `build/obj`。

### Task 5: 编写中文 README 与发布静态检查

**Files:**
- Create: `README.md`
- Create: `tests/LauncherPackagingTests.ps1`

**Interfaces:**
- Consumes: Task 4 确定的实际输出目录和文件布局。

- [ ] **Step 1: 先写失败的发布静态检查**

添加 PowerShell 7 脚本，验证 README 有项目介绍、运行步骤、依赖、服务管理、托盘和构建章节；验证 Launcher 项目为 Windows subsystem/`wWinMain`、客户端 manifest 为 `requireAdministrator`、launcher manifest 为 `asInvoker`，并验证发布目录根部及 `bin` 中的文件清单。

- [ ] **Step 2: 运行检查并确认失败**

在 README 和 launcher 项目未完成时运行脚本，预期因目标文件或章节不存在而失败。

- [ ] **Step 3: 编写 README**

依据已审规格和源码，用中文说明项目组成、便携目录结构、首次启动/UAC、保存账号、安装并启动自动启动服务、UI 自动拉起/WebView2 兜底开关、托盘操作、单实例唤醒、Windows 11 x64 与 Evergreen WebView2 Runtime 条件、常见问题及 VS2022 Release x64 构建。明确产品没有 CLI 启动接口；不要加入 shell 命令作为用户启动步骤，也不要声称未经验证的校园环境兼容性。

- [ ] **Step 4: 运行发布静态检查并确认通过**

Release 布局生成后，以 PowerShell 7 运行 `tests/LauncherPackagingTests.ps1`，预期 `LauncherPackagingTests: PASS`。

### Task 6: 完成干净构建验证

**Files:**
- Verification: `tests/CampusNetTests.vcxproj`、现有 UI 检查和 Task 5 检查

- [ ] **Step 1: 从已清理状态构建并运行 Debug C++ 测试**

使用 Visual Studio 2022 MSBuild 构建 `tests/CampusNetTests.vcxproj` 的 Debug x64 并运行生成的 `CampusNetTests.exe`。预期退出码 0，覆盖现有协议/服务测试、单实例激活测试和 LauncherPaths 测试。

- [ ] **Step 2: 运行全部静态检查与 diff 检查**

用 PowerShell 7 运行 `StatusIllustrationTests.ps1`、`MainWindowUiLayoutTests.ps1`、`LauncherPackagingTests.ps1`；运行 `git diff --check`。逐项检查发布根、`bin` 和 `obj` 实际位置。不得提交或推送。

## 实施约束

- 不覆盖或回滚当前未提交的 `SingleInstanceActivation` 相关修改。
- 清理旧 build 后重新生成必要测试和发布产物；旧 MSI 与旧 `CampusNetClient-green` 包属于此次明确清理范围。
- 不修改 workspace-root `release` 及其凭据/配置；不修改 `logs`。
- 所有命令均为开发构建/验证步骤；发布给用户的 Launcher 没有 CLI。
