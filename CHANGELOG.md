# 版本说明

## 3.3

- 增加 Windows 11 版本识别，支持 24H2、25H2 和 26H2；26H2 支持 `DisplayVersion=26H2` 和 Build `26300` 系列兜底。
- 将“Windows 更新已完全关闭”改为“Windows 更新策略已完全关闭”，明确说明 Win10/11 的 Windows Update 服务仍由系统管理。
- 保持现有更新策略、注册表、服务控制、备份恢复和按钮行为不变。

## 3.2

- 版本统一为“Win7/10/11自动更新关闭工具v3.2”，文件及产品版本统一为 `3.2.0.0`。
- 窗口支持最大化和拖动调整大小；启动时按当前屏幕工作区选择初始尺寸。
- UI 会根据窗口客户区自动整体缩放标题、字体、卡片、按钮、Badge 和图标；极小窗口仍保留垂直滚动兜底。
- 调整窗口期间实时更新布局，并降低子控件重绘和字体重建频率，改善拖动缩放卡顿。
- 不改变 Windows Update 策略、Windows 11 升级策略、注册表、服务、状态判断、备份恢复或按钮行为。

## 3.1

- 当前版本统一命名为“Win7/10/11自动更新关闭工具v3.1”，文件及产品版本、程序清单版本统一为 `3.1.0.0`。
- 包含此前完成的状态卡片、状态徽章、抗锯齿图标和按钮优化；本次更名不改变更新策略、备份恢复或按钮行为。

## 3.0.1

- 状态标题和关键检查项增加 `√` / `×` 标记，完整、部分和未关闭状态更直观。
- 调整自动更新区与 Windows 11 升级区的垂直间距，将各自状态、详情和按钮归入独立面板。
- 新增 Windows Server 2016（Build 14393）识别与更新策略控制；两项仅支持 Server 2019/Win10 1809 以后的通知策略在 Server 2016 上不写入并显示“不适用”。
- Windows 11 升级锁定在 Server 上保持不可用且底层零写入。
- 其他未验证的 Windows Server 版本继续拒绝运行，避免错误套用桌面系统策略。
- 不采用加壳或混淆规避杀软；保留透明原生单 EXE，建议使用受信任 Authenticode 证书签名并提交误报复核。

## 3.0.0

- 统一程序名称为“Win7/10/11自动更新关闭3.0工具”，文件版本更新为 `3.0.0.0`。
- 新增仅适用于 Windows 10 的“禁止升级到 Windows 11”与独立恢复功能。
- 自动识别当前真实 Windows 10 功能版本，按 Build、`DisplayVersion`、`ReleaseId` 交叉校验；未知 Build 拒绝写入。
- 新功能仅写入 `ProductVersion`、`TargetReleaseVersion`、`TargetReleaseVersionInfo`，不改动现有自动更新策略或服务控制。
- 新增独立备份 `%ProgramData%\UpdateLock\windows11-upgrade-backup-v1.txt`，保留原始存在性、类型和值，恢复成功后才删除备份。
- Windows 7 和 Windows 11 上新功能按钮灰显，底层调用同样零写入。
- 界面删除重复的“Windows 11 升级控制”标题，直接显示“Windows 11 升级状态”。

## 2.1.0

在 2.0 基础上加强 Windows 10/11 的手动更新入口封锁，不重复改动既有控制逻辑：

- 新增 `SetDisableUXWUAccess=1`，关闭 Windows Update 页面中的手动检查、下载和安装入口。
- 状态检测新增“Windows Update 手动访问”项目，只有原有自动更新策略和访问封锁全部生效才显示完全关闭。
- 新增 `%ProgramData%\UpdateLock\windows-update-access-backup-v1.txt`，独立保存并恢复访问策略原值。
- 原六项 `policy-backup-v1.txt` 格式保持不变，继续兼容 2.0 备份。
- 新增访问策略备份解析、恢复和状态组合测试。
- 程序文件版本和产品版本统一修正为 `2.1.0.0`。

注意：本版仍禁止 Windows Update 驱动更新，新硬件可能需要手动安装厂商驱动。

## 2.0.0

首个 Win7/10/11 原生统一版本：

- 从 C#/.NET 迁移为原生 C++17/Win32 单文件 EXE。
- 使用 `/MT` 静态 CRT，无需安装 .NET Framework 或 VC++ Redistributable。
- 自动识别 Windows 7、Windows 10、Windows 11，并选择对应 Controller。
- Windows 7 新增 `NoAutoUpdate`、`DisableWindowsUpdateAccess` 和 `wuauserv` 控制。
- Windows 10/11 保留六项自动更新、通知、自动重启和驱动更新策略。
- 使用 Win32 Registry API 与 SCM API，不依赖脚本或外部命令。
- 保留原始状态备份、一键恢复、回读验证、失败回滚、UAC 和单实例保护。
- 兼容旧 `policy-backup-v1.txt`，并为 Win7 使用独立原始状态备份。
- 统一图标、中文界面和作者信息“啊常用户”。

