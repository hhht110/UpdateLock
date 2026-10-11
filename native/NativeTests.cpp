#define UPDATELOCK_TEST
#include "UpdateLock.cpp"

#include <iostream>

namespace {

void Require(bool condition, const wchar_t* message) {
    if (!condition) throw AppError(message);
}

void RemoveIfPresent(const std::wstring& path) {
    if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) ThrowWin32(L"删除测试文件");
}

RECT ChildClientRect(HWND parent, int controlId) {
    HWND child = GetDlgItem(parent, controlId);
    if (!child) throw AppError(L"界面布局测试缺少控件。" );
    RECT rect = {};
    if (!GetWindowRect(child, &rect)) ThrowWin32(L"读取界面控件位置");
    POINT points[2] = {{rect.left, rect.top}, {rect.right, rect.bottom}};
    if (!MapWindowPoints(HWND_DESKTOP, parent, points, 2)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SUCCESS) ThrowWin32(L"转换界面控件位置", error);
    }
    return RECT{points[0].x, points[0].y, points[1].x, points[1].y};
}

struct InkBounds {
    bool found = false;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

template <typename Draw>
InkBounds MeasureRenderedInk(int canvasSize, Draw draw) {
    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = canvasSize;
    bitmapInfo.bmiHeader.biHeight = -canvasSize;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* pixelData = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS,
        &pixelData, nullptr, 0);
    if (!bitmap || !pixelData) throw AppError(L"无法创建图标像素测试画布。" );
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        DeleteObject(bitmap);
        throw AppError(L"无法创建图标像素测试 DC。" );
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    RECT canvas = {0, 0, canvasSize, canvasSize};
    FillRectColor(dc, canvas, RGB(255, 255, 255));
    draw(dc, canvasSize / 2, canvasSize / 2);
    GdiFlush();

    InkBounds bounds;
    const DWORD* pixels = static_cast<const DWORD*>(pixelData);
    for (int y = 0; y < canvasSize; ++y) {
        for (int x = 0; x < canvasSize; ++x) {
            if ((pixels[static_cast<size_t>(y) * canvasSize + x] & 0x00FFFFFFu) == 0x00FFFFFFu) {
                continue;
            }
            if (!bounds.found) {
                bounds = InkBounds{true, x, y, x, y};
            } else {
                bounds.left = std::min(bounds.left, x);
                bounds.top = std::min(bounds.top, y);
                bounds.right = std::max(bounds.right, x);
                bounds.bottom = std::max(bounds.bottom, y);
            }
        }
    }
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    DeleteObject(bitmap);
    return bounds;
}

void RequireVisibleUnclippedInk(const InkBounds& bounds, int canvasSize,
    const wchar_t* message) {
    Require(bounds.found && bounds.left > 0 && bounds.top > 0 &&
        bounds.right < canvasSize - 1 && bounds.bottom < canvasSize - 1, message);
}

} // namespace

int wmain() {
    try {
        GdiplusSession gdiplus;
        wchar_t tempRoot[MAX_PATH] = {};
        if (!GetTempPathW(MAX_PATH, tempRoot)) ThrowWin32(L"获取测试临时目录");
        const std::wstring testDirectory = std::wstring(tempRoot) + L"UpdateLock-NativeTests-" +
            std::to_wstring(GetCurrentProcessId());
        if (!CreateDirectoryW(testDirectory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            ThrowWin32(L"创建测试临时目录");
        }
        Require(Base64Encode(L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate") ==
            "U09GVFdBUkVcUG9saWNpZXNcTWljcm9zb2Z0XFdpbmRvd3NcV2luZG93c1VwZGF0ZQ==",
            L"Base64 与 C# UTF-8 格式不一致。" );
        Require(Base64Decode("Tm9BdXRvVXBkYXRl") == L"NoAutoUpdate", L"Base64 解码不兼容。" );
        Require(Base64Decode("").empty(), L"空 Base64 处理错误。" );
        Require(ParseInt32("  +42 ") == 42 && ParseInt32("-2147483648") == INT32_MIN,
            L"旧 C# Int32 文本兼容解析错误。" );

        const VersionInfo win10_22h2{10, 0, 19045, VER_NT_WORKSTATION};
        const Windows10VersionInfo displayVersion = Windows10VersionDetector::Resolve(
            win10_22h2, L"22H2", L"2004", 4046);
        Require(displayVersion.isWindows10 && displayVersion.known &&
            displayVersion.functionalVersion == L"22H2" &&
            displayVersion.source == Windows10VersionSource::DisplayVersion,
            L"DisplayVersion 版本识别错误。" );

        const Windows10VersionInfo releaseVersion = Windows10VersionDetector::Resolve(
            VersionInfo{10, 0, 18363, VER_NT_WORKSTATION}, L"", L"1909", 844);
        Require(releaseVersion.known && releaseVersion.functionalVersion == L"1909" &&
            releaseVersion.source == Windows10VersionSource::ReleaseId,
            L"ReleaseId 版本兜底错误。" );

        const Windows10VersionInfo invalidDisplayReleaseVersion = Windows10VersionDetector::Resolve(
            VersionInfo{10, 0, 18363, VER_NT_WORKSTATION}, L"not-a-version", L"1909", 844);
        Require(invalidDisplayReleaseVersion.known &&
            invalidDisplayReleaseVersion.functionalVersion == L"1909" &&
            invalidDisplayReleaseVersion.source == Windows10VersionSource::ReleaseId,
            L"DisplayVersion 异常时未使用 ReleaseId 兜底。" );

        for (const Windows10BuildMapping& mapping : kWindows10BuildMappings) {
            const Windows10VersionInfo mapped = Windows10VersionDetector::Resolve(
                VersionInfo{10, 0, mapping.build, VER_NT_WORKSTATION}, L"", L"", 1);
            Require(mapped.known && mapped.functionalVersion == mapping.functionalVersion &&
                mapped.source == Windows10VersionSource::BuildMapping,
                L"Build 映射表存在未覆盖项。" );
        }

        const Windows10VersionInfo mismatchedVersion = Windows10VersionDetector::Resolve(
            VersionInfo{10, 0, 18363, VER_NT_WORKSTATION}, L"22H2", L"1909", 418);
        Require(mismatchedVersion.known && mismatchedVersion.functionalVersion == L"1909" &&
            mismatchedVersion.source == Windows10VersionSource::BuildMapping,
            L"DisplayVersion 与真实 Build 冲突时未使用 Build 映射。" );

        const Windows10VersionInfo unknownVersion = Windows10VersionDetector::Resolve(
            VersionInfo{10, 0, 19046, VER_NT_WORKSTATION}, L"22H2", L"", 1);
        Require(unknownVersion.isWindows10 && !unknownVersion.known && !unknownVersion.error.empty(),
            L"未知 Windows 10 Build 未被拒绝。" );

        const VersionInfo server2016{10, 0, 14393, VER_NT_SERVER};
        const Windows10VersionInfo serverVersion = Windows10VersionDetector::Resolve(
            server2016, L"1607", L"1607", 1);
        Require(IsWindowsServer2016(server2016) && !serverVersion.isWindows10,
            L"Windows Server 2016 与 Windows 10 桌面版本未正确隔离。" );

        const Windows11VersionInfo win11_26h2 = Windows11VersionDetector::Resolve(
            VersionInfo{10, 0, 26300, VER_NT_WORKSTATION}, L"26H2", 9550);
        Require(win11_26h2.isWindows11 && win11_26h2.known &&
            win11_26h2.functionalVersion == L"26H2" &&
            win11_26h2.source == Windows11VersionSource::DisplayVersion,
            L"Windows 11 26H2 DisplayVersion 识别错误。" );
        const Windows11VersionInfo win11_26h2Mapped = Windows11VersionDetector::Resolve(
            VersionInfo{10, 0, 26340, VER_NT_WORKSTATION}, L"", 9616);
        Require(win11_26h2Mapped.known && win11_26h2Mapped.functionalVersion == L"26H2" &&
            win11_26h2Mapped.source == Windows11VersionSource::BuildMapping,
            L"Windows 11 26H2 Build 兜底识别错误。" );
        std::unique_ptr<IUpdateController> serverController = CreateController(server2016);
        Require(serverController->DisplayName() == L"Windows Server 2016",
            L"Windows Server 2016 Controller 路由错误。" );
        bool server2019Rejected = false;
        try { (void)CreateController(VersionInfo{10, 0, 17763, VER_NT_SERVER}); }
        catch (const AppError&) { server2019Rejected = true; }
        Require(server2019Rejected, L"未验证的 Windows Server 版本不应被静默放行。" );

        const UpdateStatus upgradeAllowed = Windows11UpgradeBlocker::TestBuildStatus(
            displayVersion, false, false, false, false, L"");
        const UpdateStatus upgradeBlocked = Windows11UpgradeBlocker::TestBuildStatus(
            displayVersion, true, true, true, true, L"22H2");
        const UpdateStatus upgradeMismatch = Windows11UpgradeBlocker::TestBuildStatus(
            displayVersion, true, true, true, false, L"21H2");
        const UpdateStatus upgradePartial = Windows11UpgradeBlocker::TestBuildStatus(
            displayVersion, true, true, false, false, L"");
        Require(upgradeAllowed.title == L"Windows 11 升级状态：允许" &&
            upgradeBlocked.title == L"Windows 11 升级状态：已阻止" &&
            upgradeMismatch.title == L"Windows 11 升级状态：目标版本与当前系统不一致" &&
            upgradePartial.title == L"Windows 11 升级状态：部分配置",
            L"upgrade-status" );

        const UpdateStatus all = ModernUpdateController::BuildStatus(true, true, true, true, true);
        const UpdateStatus partial = ModernUpdateController::BuildStatus(true, true, true, true, false);
        const UpdateStatus coreFailed = ModernUpdateController::BuildStatus(false, true, true, true, true);
        const UpdateStatus serverAll = ModernUpdateController::BuildStatus(true, false, true, true, true, false);
        Require(all.level == StatusLevel::Disabled && all.title == L"√ Windows 更新策略已完全关闭" &&
            all.details[0].find(L"√ 自动更新") == 0, L"现代完整状态语义错误。" );
        Require(partial.level == StatusLevel::PartiallyDisabled &&
            partial.title == L"× Windows 自动更新已关闭，但部分封锁策略未生效" &&
            partial.details[2].find(L"× Windows Update 手动访问") == 0,
            L"现代访问封锁状态语义错误。" );
        Require(coreFailed.level == StatusLevel::Enabled && coreFailed.title == L"× Windows 更新未完全关闭" &&
            coreFailed.details[0].find(L"× 自动更新") == 0, L"现代核心状态语义错误。" );
        Require(serverAll.level == StatusLevel::Disabled &&
            serverAll.details[0].find(L"更新通知：当前系统不适用") != std::wstring::npos,
            L"Windows Server 2016 不适用的通知策略被错误计入状态。" );

        UiContext presentation;
        presentation.version = win10_22h2;
        presentation.updateState = all;
        Require(UpdateHeaderState(presentation) == L"策略已完全关闭" &&
            UpdateVisual(presentation) == StatusVisual::Success,
            L"完整关闭状态的 UI 映射错误。" );
        const std::vector<StatusRow> allRows = BuildUpdateRows(presentation.updateState);
        Require(allRows.size() == 5 && allRows[0].label == L"自动更新" &&
            allRows[4].label == L"Windows Update 手动检查、下载和安装" &&
            allRows[4].value == L"已禁用" && allRows[4].visual == StatusVisual::Success,
            L"更新子状态没有正确映射为五行右侧徽章。" );
        presentation.updateState = partial;
        Require(UpdateHeaderState(presentation) == L"部分关闭" &&
            UpdateVisual(presentation) == StatusVisual::Warning,
            L"部分关闭状态的 UI 映射错误。" );
        presentation.updateState = coreFailed;
        Require(UpdateHeaderState(presentation) == L"未关闭" &&
            UpdateVisual(presentation) == StatusVisual::Error,
            L"未关闭状态的 UI 映射错误。" );

        presentation.upgradeState = upgradeBlocked;
        Require(UpgradeHeaderState(presentation) == L"已阻止" &&
            UpgradeVisual(presentation) == StatusVisual::Success,
            L"Windows 11 已阻止状态的 UI 映射错误。" );
        const std::vector<StatusRow> blockedRows = BuildUpgradeRows(presentation);
        Require(blockedRows.size() == 3 && blockedRows[2].label == L"锁定版本" &&
            blockedRows[2].visual == StatusVisual::Success,
            L"Windows 11 锁定版本的成功徽章映射错误。" );
        presentation.upgradeState = upgradeAllowed;
        const std::vector<StatusRow> allowedRows = BuildUpgradeRows(presentation);
        Require(UpgradeHeaderState(presentation) == L"未阻止" &&
            UpgradeVisual(presentation) == StatusVisual::Error &&
            allowedRows.size() == 3 && allowedRows[2].label == L"锁定版本" &&
            allowedRows[2].value == L"未锁定" && allowedRows[2].visual == StatusVisual::Error,
            L"Windows 11 未阻止状态的 UI 映射错误。" );
        presentation.upgradeState = upgradePartial;
        Require(UpgradeHeaderState(presentation) == L"部分配置" &&
            UpgradeVisual(presentation) == StatusVisual::Warning,
            L"Windows 11 部分配置状态的 UI 映射错误。" );

        for (const int layoutDpi : {96, 120, 144}) {
            const UiLayout layout = BuildUiLayout(layoutDpi);
            Require(layout.updateCard.bottom <= layout.disableButton.top &&
                layout.restoreButton.top == layout.disableButton.top &&
                layout.restoreButton.bottom == layout.disableButton.bottom,
                L"Windows 更新卡片与按钮在 DPI 缩放后错位。" );
            Require(layout.updatePanel.bottom - layout.updatePanel.top >= ScaleForDpi(265, layoutDpi) &&
                layout.upgradePanel.bottom - layout.upgradePanel.top >= ScaleForDpi(230, layoutDpi),
                L"状态卡片没有保留加高后的纵向空间。" );
            Require(layout.disableButton.bottom - layout.disableButton.top == ScaleForDpi(44, layoutDpi) &&
                layout.upgradeDisableButton.bottom - layout.upgradeDisableButton.top == ScaleForDpi(44, layoutDpi),
                L"操作按钮没有保持 44px DPI 缩放高度。" );
            Require(layout.upgradeCard.top - layout.disableButton.bottom >= ScaleForDpi(35, layoutDpi) &&
                layout.upgradeCard.bottom <= layout.upgradeDisableButton.top,
                L"两个功能区在 DPI 缩放后层级分隔不足。" );
            Require(layout.upgradeRestoreButton.top == layout.upgradeDisableButton.top &&
                layout.upgradeRestoreButton.bottom == layout.upgradeDisableButton.bottom &&
                layout.warning.top - layout.upgradeDisableButton.bottom >= ScaleForDpi(24, layoutDpi),
                L"Windows 11 按钮或底部说明在 DPI 缩放后错位。" );
            Require(layout.warning.bottom <= ScaleForDpi(kUiClientHeight, layoutDpi),
                L"底部说明超出 DPI 缩放后的客户区。" );

            const int canvasSize = ScaleForDpi(48, layoutDpi);
            const int badgeIconSize = ScaleForDpi(18, layoutDpi);
            const InkBounds successInk = MeasureRenderedInk(canvasSize,
                [badgeIconSize](HDC dc, int x, int y) {
                    DrawStatusIcon(dc, x, y, badgeIconSize, StatusVisual::Success);
                });
            RequireVisibleUnclippedInk(successInk, canvasSize,
                L"成功状态图标在 DPI 缩放后为空或被裁切。" );
            Require(std::abs((successInk.right - successInk.left) -
                (successInk.bottom - successInk.top)) <= 2,
                L"成功状态图标在 DPI 缩放后不是正圆。" );
            const InkBounds errorInk = MeasureRenderedInk(canvasSize,
                [badgeIconSize](HDC dc, int x, int y) {
                    DrawStatusIcon(dc, x, y, badgeIconSize, StatusVisual::Error);
                });
            RequireVisibleUnclippedInk(errorInk, canvasSize,
                L"失败状态图标在 DPI 缩放后为空或被裁切。" );
            const InkBounds warningInk = MeasureRenderedInk(canvasSize,
                [badgeIconSize](HDC dc, int x, int y) {
                    DrawStatusIcon(dc, x, y, badgeIconSize, StatusVisual::Warning);
                });
            RequireVisibleUnclippedInk(warningInk, canvasSize,
                L"警告状态图标在 DPI 缩放后为空或被裁切。" );

            const int identityIconSize = ScaleForDpi(36, layoutDpi);
            const InkBounds shieldInk = MeasureRenderedInk(canvasSize,
                [identityIconSize](HDC dc, int x, int y) {
                    DrawUpdateShield(dc, x, y, identityIconSize, StatusVisual::Success);
                });
            RequireVisibleUnclippedInk(shieldInk, canvasSize,
                L"Windows Update 盾牌在 DPI 缩放后为空或被裁切。" );
            const InkBounds windowsInk = MeasureRenderedInk(canvasSize,
                [identityIconSize](HDC dc, int x, int y) {
                    DrawWindowsMark(dc, x, y, identityIconSize);
                });
            RequireVisibleUnclippedInk(windowsInk, canvasSize,
                L"Windows 标志在 DPI 缩放后为空或被裁切。" );

            const int buttonIconSize = ScaleForDpi(20, layoutDpi);
            const InkBounds restoreInk = MeasureRenderedInk(canvasSize,
                [buttonIconSize](HDC dc, int x, int y) {
                    DrawButtonActionIcon(dc, x, y, buttonIconSize,
                        RGB(35, 50, 69), true);
                });
            RequireVisibleUnclippedInk(restoreInk, canvasSize,
                L"恢复箭头在 DPI 缩放后为空或被裁切。" );

            const InkBounds blockInk = MeasureRenderedInk(canvasSize,
                [buttonIconSize](HDC dc, int x, int y) {
                    DrawButtonActionIcon(dc, x, y, buttonIconSize,
                        RGB(35, 50, 69), false);
                });
            RequireVisibleUnclippedInk(blockInk, canvasSize,
                L"禁止图标在 DPI 缩放后为空或被裁切。" );
        }

        for (const int baseDpi : {96, 120, 144}) {
            const int compactDpi = ChooseRenderDpi(baseDpi, 590, 689, true);
            Require(compactDpi >= MinimumRenderDpi(baseDpi) &&
                ScaleForDpi(kUiClientWidth, compactDpi) <= 590 &&
                ScaleForDpi(kUiClientHeight, compactDpi) <= 689,
                L"125%/150% DPI 下的紧凑客户区仍然超出可用空间。" );
            Require(!(ScaleForDpi(kUiClientHeight, compactDpi) > 689),
                L"紧凑客户区错误地需要垂直滚动条。" );

            int previousDpi = MinimumRenderDpi(baseDpi);
            for (int availableHeight = 420; availableHeight <= 900; availableHeight += 17) {
                const int candidate = ChooseRenderDpi(baseDpi, 900, availableHeight, true);
                Require(candidate >= previousDpi,
                    L"可用高度增加时渲染 DPI 不应反向下降。" );
                previousDpi = candidate;
            }
        }

        ModernUpdateController modern(L"Windows 10");
        modern.TestSetBackupPath(testDirectory + L"\\policy-backup-v1.txt");
        RemoveIfPresent(modern.TestBackupPath());
        const std::vector<std::string> modernLines = {
            Base64Encode(kWindowsUpdate) + "|" + Base64Encode(L"UpdateNotificationLevel") + "|1|2",
            Base64Encode(kAutomaticUpdates) + "|" + Base64Encode(L"AUOptions") + "|0|0",
            Base64Encode(kWindowsUpdate) + "|" + Base64Encode(L"ExcludeWUDriversInQualityUpdate") + "|1|1",
            Base64Encode(kAutomaticUpdates) + "|" + Base64Encode(L"NoAutoUpdate") + "|0|-7",
            Base64Encode(kWindowsUpdate) + "|" + Base64Encode(L"SetUpdateNotificationLevel") + "|1|1",
            Base64Encode(kAutomaticUpdates) + "|" + Base64Encode(L"NoAutoRebootWithLoggedOnUsers") + "|1|1"
        };
        WriteUtf8LinesAtomically(modern.TestBackupPath(), modernLines, true);
        const std::vector<PolicyState> parsed = modern.TestReadBackup();
        Require(parsed.size() == 6, L"现代旧备份行数解析错误。" );
        Require(std::wstring(parsed[0].policy->name) == L"ExcludeWUDriversInQualityUpdate", L"现代备份未按原白名单顺序重排。" );
        Require(std::wstring(parsed[3].policy->name) == L"NoAutoUpdate" && !parsed[3].exists && parsed[3].value == -7,
            L"现代备份存在性或 Int32 值解析错误。" );
        RemoveIfPresent(modern.TestBackupPath());

        modern.TestSetAccessBackupPath(testDirectory + L"\\windows-update-access-backup-v1.txt");
        RemoveIfPresent(modern.TestAccessBackupPath());

        Windows11UpgradeBlocker upgradeBlocker;
        upgradeBlocker.TestSetBackupPath(testDirectory + L"\\windows11-upgrade-backup-v1.txt");
        RemoveIfPresent(upgradeBlocker.TestBackupPath());
        const std::vector<BYTE> originalStringBytes = {
            'o', 0, 'l', 0, 'd', 0, 0, 0};
        WriteUtf8LinesAtomically(upgradeBlocker.TestBackupPath(), {
            "UpdateLock-Windows11Upgrade|1",
            "RAW|" + Base64Encode(kWindowsUpdate) + "|" + Base64Encode(L"ProductVersion") + "|1|1|" +
                Base64EncodeBytes(originalStringBytes),
            "RAW|" + Base64Encode(kWindowsUpdate) + "|" + Base64Encode(L"TargetReleaseVersion") + "|1|4|" +
                Base64EncodeBytes(std::vector<BYTE>{7, 0, 0, 0}),
            "RAW|" + Base64Encode(kWindowsUpdate) + "|" + Base64Encode(L"TargetReleaseVersionInfo") + "|0|0|"}, true);
        const std::array<RawRegistryState, 3> upgradeBackup = upgradeBlocker.TestReadBackup();
        Require(upgradeBackup[0].exists && upgradeBackup[0].type == REG_SZ &&
            upgradeBackup[0].data == originalStringBytes && upgradeBackup[1].type == REG_DWORD &&
            upgradeBackup[1].data == std::vector<BYTE>({7, 0, 0, 0}) && !upgradeBackup[2].exists,
            L"upgrade-backup" );
        RemoveIfPresent(upgradeBlocker.TestBackupPath());

        const OperationResult win7BlockAttempt = upgradeBlocker.Block(
            VersionInfo{6, 1, 7601, VER_NT_WORKSTATION});
        const OperationResult serverBlockAttempt = upgradeBlocker.Block(server2016);
        const OperationResult win11RestoreAttempt = upgradeBlocker.Restore(
            VersionInfo{10, 0, 22000, VER_NT_WORKSTATION});
        Require(win7BlockAttempt.outcome == OperationOutcome::Failed &&
            serverBlockAttempt.outcome == OperationOutcome::Failed &&
            win11RestoreAttempt.outcome == OperationOutcome::Failed &&
            !FileExists(upgradeBlocker.TestBackupPath()),
            L"upgrade-zero-write" );
        WriteUtf8LinesAtomically(modern.TestAccessBackupPath(), {
            "UpdateLock-ModernAccess|1",
            Base64Encode(kWindowsUpdate) + "|" + Base64Encode(L"SetDisableUXWUAccess") + "|0|9"}, true);
        const PolicyState accessBackup = modern.TestReadAccessBackup();
        Require(std::wstring(accessBackup.policy->name) == L"SetDisableUXWUAccess" &&
            !accessBackup.exists && accessBackup.value == 9, L"Windows Update 访问策略备份解析错误。" );
        RemoveIfPresent(modern.TestAccessBackupPath());

        Windows7UpdateController win7;
        win7.TestSetBackupPath(testDirectory + L"\\windows7-original-state-v1.txt");
        RemoveIfPresent(win7.TestBackupPath());
        const std::vector<std::string> win7Lines = {
            "UpdateLock-Windows7|1",
            "DWORD|" + Base64Encode(kAutomaticUpdates) + "|" + Base64Encode(L"NoAutoUpdate") + "|0|0",
            "DWORD|" + Base64Encode(kWindowsUpdate) + "|" + Base64Encode(L"DisableWindowsUpdateAccess") + "|1|5",
            "SERVICE|wuauserv|3|1|0"
        };
        WriteUtf8LinesAtomically(win7.TestBackupPath(), win7Lines, true);
        win7.TestReadBackup();
        RemoveIfPresent(win7.TestBackupPath());

        bool rejected = false;
        WriteUtf8LinesAtomically(win7.TestBackupPath(), {
            "UpdateLock-Windows7|1",
            "DWORD|" + Base64Encode(kAutomaticUpdates) + "|" + Base64Encode(L"NoAutoUpdate") + "|0|0",
            "DWORD|" + Base64Encode(kWindowsUpdate) + "|" + Base64Encode(L"Unexpected") + "|1|5",
            "SERVICE|wuauserv|3|1|0"}, true);
        try { win7.TestReadBackup(); } catch (const AppError&) { rejected = true; }
        RemoveIfPresent(win7.TestBackupPath());
        Require(rejected, L"Win7 备份白名单未拒绝越界项目。" );

        const VersionInfo currentVersion = ReadWindowsVersion();
        Require(currentVersion.productType == VER_NT_WORKSTATION,
            L"当前测试机不是 Windows 工作站。" );
        std::unique_ptr<IUpdateController> current = CreateController(currentVersion);
        const UpdateStatus liveStatus = current->GetStatus();
        Require(!liveStatus.title.empty() && !liveStatus.details.empty(),
            L"当前系统只读状态检查失败。" );

        UiContext ui;
        ui.version = currentVersion;
        ui.controller = CreateController(currentVersion);
        WNDCLASSEXW windowClass = {sizeof(windowClass)};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = L"AchangUser.UpdateLock.NativeTests";
        if (!RegisterClassExW(&windowClass)) ThrowWin32(L"注册界面测试窗口类");
        const DWORD testWindowStyle = WS_OVERLAPPEDWINDOW | WS_VSCROLL;
        RECT testWindowRect = {0, 0, ScaleForDpi(kUiClientWidth, ui.dpi),
            ScaleForDpi(500, ui.dpi)};
        AdjustWindowRectEx(&testWindowRect, testWindowStyle, FALSE, 0);
        HWND testWindow = CreateWindowExW(0, windowClass.lpszClassName, kAppName,
            testWindowStyle, 0, 0,
            testWindowRect.right - testWindowRect.left, testWindowRect.bottom - testWindowRect.top,
            nullptr, nullptr, windowClass.hInstance, &ui);
        if (!testWindow) ThrowWin32(L"创建界面布局测试窗口");
        Require(ui.dpi < ui.baseDpi, L"小窗口没有触发 UI 整体缩放。" );
        const RECT updateCard = ChildClientRect(testWindow, IDC_STATUS);
        const RECT updateButton = ChildClientRect(testWindow, IDC_DISABLE);
        const RECT upgradeCard = ChildClientRect(testWindow, IDC_UPGRADE_STATUS);
        const RECT upgradeButton = ChildClientRect(testWindow, IDC_UPGRADE_DISABLE);
        const RECT warningCard = ChildClientRect(testWindow, IDC_WARNING);
        RECT clientRect = {};
        GetClientRect(testWindow, &clientRect);
        Require(updateCard.bottom <= updateButton.top &&
            updateButton.top - updateCard.bottom <= ScaleForDpi(14, ui.dpi),
            L"自动更新状态与操作按钮距离过大。" );
        Require(upgradeCard.top - updateButton.bottom >= ScaleForDpi(35, ui.dpi),
            L"自动更新与 Windows 11 升级区域分隔不足。" );
        Require(upgradeCard.bottom <= upgradeButton.top &&
            upgradeButton.top - upgradeCard.bottom <= ScaleForDpi(14, ui.dpi),
            L"Windows 11 升级状态与操作按钮距离过大。" );
        Require(warningCard.top - upgradeButton.bottom >= ScaleForDpi(24, ui.dpi) &&
            warningCard.bottom > clientRect.bottom && ui.scrollBarVisible,
            L"底部说明区域间距或滚动区域判断错误。" );
        SendMessageW(testWindow, WM_VSCROLL, MAKEWPARAM(SB_BOTTOM, 0), 0);
        const RECT warningCardAtBottom = ChildClientRect(testWindow, IDC_WARNING);
        Require(ui.scrollOffset > 0 && warningCardAtBottom.bottom <= clientRect.bottom,
            L"垂直滚动未将底部说明区域滚动到可见范围。" );
        RECT largeWindowRect = {0, 0, ScaleForDpi(1000, ui.baseDpi),
            ScaleForDpi(1000, ui.baseDpi)};
        AdjustWindowRectEx(&largeWindowRect, testWindowStyle, FALSE, 0);
        if (!SetWindowPos(testWindow, nullptr, 0, 0,
            largeWindowRect.right - largeWindowRect.left,
            largeWindowRect.bottom - largeWindowRect.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
            ThrowWin32(L"调整界面放大测试窗口");
        }
        if (!(ui.dpi > ui.baseDpi && !ui.scrollBarVisible)) {
            throw AppError(L"大窗口缩放结果异常：renderDpi=" + std::to_wstring(ui.dpi) +
                L"，baseDpi=" + std::to_wstring(ui.baseDpi) +
                L"，scrollBarVisible=" + (ui.scrollBarVisible ? L"true" : L"false"));
        }
        DestroyWindow(testWindow);
        UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        if (!RemoveDirectoryW(testDirectory.c_str())) ThrowWin32(L"删除测试临时目录");

        std::wcout << L"ALL_NATIVE_TESTS_PASSED" << std::endl;
        return 0;
    } catch (const AppError& error) {
        std::cerr << "TEST_FAILED: " << WideToUtf8(error.message) << std::endl;
        return 1;
    }
}
