# Win7/10/11自动更新关闭工具v3.3

一个原生 C++/Win32 单文件工具，自动识别 Windows 7、Windows 10、Windows 11 或 Windows Server 2016，并使用对应策略关闭 Windows Update。

## 主要特性

- 原生 Win32 x86 EXE，无 .NET、无额外运行时依赖。
- 使用 `/MT` 静态 CRT，可在 Win7 SP1、Win10、Win11、Windows Server 2016 上直接运行。
- 注册表通过 Win32 Registry API 操作；Win7 的 `wuauserv` 通过 SCM API 操作。
- 不调用 PowerShell、CMD、`reg.exe` 或 `sc.exe`。
- 修改前保存原始状态，支持一键恢复、写入后回读验证和失败回滚。
- UAC 管理员权限和单实例保护。

## 系统策略

### Windows 7

只处理：

- `NoAutoUpdate`
- `DisableWindowsUpdateAccess`
- `wuauserv` 停止并禁用

不会处理 BITS、CryptSvc、TrustedInstaller、SoftwareDistribution、Catroot2 或系统文件。

原始状态保存到：

```text
%ProgramData%\UpdateLock\windows7-original-state-v1.txt
```

### Windows 10 / Windows 11

处理原六项策略：

- `ExcludeWUDriversInQualityUpdate`
- `SetUpdateNotificationLevel`
- `UpdateNotificationLevel`
- `NoAutoUpdate`
- `NoAutoRebootWithLoggedOnUsers`
- `AUOptions`（关闭时删除，恢复时还原原状态）

2.1 额外设置：

- `SetDisableUXWUAccess=1`

该策略会隐藏或禁用 Windows Update 页面中的手动检查、下载和安装入口。Win10/11 不停止 `wuauserv`、BITS、UsoSvc、WaaSMedicSvc 或 Update Orchestrator。

备份文件：

```text
%ProgramData%\UpdateLock\policy-backup-v1.txt
%ProgramData%\UpdateLock\windows-update-access-backup-v1.txt
```

原六项备份格式保持兼容，2.1 的访问封锁使用独立备份文件。

### Windows Server 2016

仅对真实版本 `10.0 / Build 14393 / Server` 启用，复用现代系统的可恢复注册表控制，但不会把 Server 当成桌面 Windows 10：

- 设置 `NoAutoUpdate`、`NoAutoRebootWithLoggedOnUsers`、`ExcludeWUDriversInQualityUpdate` 和 `SetDisableUXWUAccess`。
- 删除 `AUOptions`，恢复时按原始备份还原。
- 不写入 Server 2016 不支持的 `SetUpdateNotificationLevel` 和 `UpdateNotificationLevel`，界面显示“当前系统不适用”。
- 不停止 `wuauserv`、BITS、UsoSvc、WaaSMedicSvc 或 Update Orchestrator。

Windows 11 升级锁定在 Server 2016 上不可用，底层调用同样拒绝写入。其他尚未验证的 Windows Server 版本不会被自动放行。

### Windows 10 禁止升级到 Windows 11

Windows 10 界面另外提供“禁止升级到 Windows 11”和“恢复允许升级到 Windows 11”。该功能与自动更新控制独立，只写入：

- `ProductVersion=Windows 10`
- `TargetReleaseVersion=1`
- `TargetReleaseVersionInfo=当前实际 Windows 10 功能版本`

功能版本按 `RtlGetVersion` 的真实 Build 与 `DisplayVersion`、`ReleaseId` 交叉校验，支持 1507 至 22H2 的已知 Build；未知 Build 会拒绝写入。首次修改前的原始存在性、注册表类型和值保存到：

```text
%ProgramData%\UpdateLock\windows11-upgrade-backup-v1.txt
```

Windows 7、Windows 11 和 Windows Server 2016 上该组按钮不可用，底层调用也会拒绝写入。该功能不会禁用更新服务、修改 TPM/Secure Boot 或删除系统文件，也不会把旧版 Windows 10 自动升级到 22H2。

Windows 11 功能版本显示支持 24H2、25H2 和 26H2；当注册表 `DisplayVersion` 不可用时，会按真实 Build 识别，26H2 对应 Build 26300 系列。

## 重要影响

关闭 Windows Update 驱动更新后，新接入的 USB 或其他硬件可能无法自动在线获取驱动。此时可从硬件厂商官网下载并离线安装驱动，或先恢复更新、安装驱动后再重新关闭。

不要在系统正在安装或卸载更新时运行关闭或恢复操作。

## 构建

需要 Visual Studio Build Tools、MSVC v141 工具链和 Windows 10 SDK 19041。在 `native` 目录运行：

```bat
build-native.cmd
```

构建输出位于 `native\build`。脚本同时生成 `UpdateLock.exe` 和只使用临时目录的原生测试程序。

发布包只分发一个文件：`Win7-10-11自动更新关闭工具v3.3.exe`。`NativeTests.exe`、目标文件和资源文件均不放入发布包。

## 下载与校验

普通用户请从 GitHub Releases 下载单文件 EXE，并使用 Release 中公布的 SHA-256 校验文件完整性。

当前 v3.3 EXE 的作者元数据为“啊常用户”，但尚未使用受信任的 Authenticode 证书签名，因此 Windows 仍可能提示“未知发布者”或显示 SmartScreen 警告。加壳或混淆不会建立发布者信誉，反而可能提高安全软件的启发式风险；正式分发应使用一致的受信任 Authenticode 证书签名，并对明确误报逐版本提交厂商复核。

## 验证边界

已完成原生构建、单元测试和二进制依赖检查。不同补丁级别和定制系统可能存在行为差异，真实机器操作前应确保重要数据已有备份。

本次发布已检查 Windows 10 运行界面；Windows 7、Windows 11 和 Windows Server 2016 未在本次发布中重新完成实机禁止、重启及恢复全流程验证。自动化测试不能替代这些实机验证。

窗口支持最大化和拖动调整大小，启动时会根据当前屏幕工作区选择初始尺寸；窗口变化时标题、字体、卡片、按钮、Badge 和图标会按客户区整体缩放，当可用高度仍不足时再显示垂直滚动条，确保底部说明和操作按钮仍可访问。状态显示表示所检查策略达到目标值，不代表所有后台更新通道永久停止。

