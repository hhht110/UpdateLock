#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}
#pragma warning(push)
#pragma warning(disable: 4458)
#include <gdiplus.h>
#pragma warning(pop)
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <wincrypt.h>
#include <objbase.h>
#include <wuapi.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "resource.h"

namespace {

const wchar_t* const kAppName = L"Win7/10/11自动更新关闭工具v3.3";
const wchar_t* const kMutexName = L"Global\\UpdateLock.SingleInstance.7A64EA9A";
const wchar_t* const kWindowsUpdate = L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate";
const wchar_t* const kAutomaticUpdates = L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU";
const wchar_t* const kCurrentVersion = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
const REGSAM kRegistryView = KEY_WOW64_64KEY;

struct AppError : public std::exception {
    std::wstring message;
    explicit AppError(const std::wstring& value) : message(value) {}
    const char* what() const noexcept override { return "AppError"; }
};

std::wstring Win32Message(DWORD code) {
    wchar_t* text = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD count = FormatMessageW(flags, nullptr, code, 0,
        reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring result = count && text ? text : L"未知错误";
    if (text) LocalFree(text);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) {
        result.pop_back();
    }
    return result;
}

[[noreturn]] void ThrowWin32(const wchar_t* operation, DWORD code = GetLastError()) {
    throw AppError(std::wstring(operation) + L"失败。Win32 " + std::to_wstring(code) +
                   L"：" + Win32Message(code));
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return std::string();
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) ThrowWin32(L"UTF-8 编码");
    std::string result(static_cast<size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), &result[0], count, nullptr, nullptr)) {
        ThrowWin32(L"UTF-8 编码");
    }
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return std::wstring();
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (!count) ThrowWin32(L"UTF-8 解码");
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), &result[0], count)) {
        ThrowWin32(L"UTF-8 解码");
    }
    return result;
}

std::string Base64Encode(const std::wstring& value) {
    const std::string bytes = WideToUtf8(value);
    DWORD count = 0;
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()),
        static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
        nullptr, &count)) ThrowWin32(L"Base64 编码");
    std::string result(count, '\0');
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()),
        static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
        &result[0], &count)) ThrowWin32(L"Base64 编码");
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

std::string Base64EncodeBytes(const std::vector<BYTE>& bytes) {
    if (bytes.empty()) return std::string();
    DWORD count = 0;
    const BYTE* data = bytes.data();
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(bytes.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &count)) {
        ThrowWin32(L"Base64 编码");
    }
    if (count == 0) return std::string();
    std::string result(count, '\0');
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(bytes.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &result[0], &count)) {
        ThrowWin32(L"Base64 编码");
    }
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

std::vector<BYTE> Base64DecodeBytes(const std::string& value) {
    if (value.empty()) return {};
    DWORD count = 0;
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()),
        CRYPT_STRING_BASE64, nullptr, &count, nullptr, nullptr)) {
        throw AppError(L"备份文件包含无效 Base64。" );
    }
    std::vector<BYTE> result(count);
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()),
        CRYPT_STRING_BASE64, result.data(), &count, nullptr, nullptr)) {
        throw AppError(L"备份文件包含无效 Base64。" );
    }
    result.resize(count);
    return result;
}

std::wstring Base64Decode(const std::string& value) {
    DWORD count = 0;
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()),
        CRYPT_STRING_BASE64, nullptr, &count, nullptr, nullptr)) {
        throw AppError(L"备份文件包含无效 Base64。" );
    }
    if (count == 0) return std::wstring();
    std::string bytes(count, '\0');
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()),
        CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(&bytes[0]), &count, nullptr, nullptr)) {
        throw AppError(L"备份文件包含无效 Base64。" );
    }
    bytes.resize(count);
    return Utf8ToWide(bytes);
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
    std::vector<std::string> result;
    size_t start = 0;
    for (;;) {
        const size_t end = value.find(delimiter, start);
        result.push_back(value.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

int32_t ParseInt32(const std::string& text) {
    std::wstring value = Utf8ToWide(text);
    while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && iswspace(value.back())) value.pop_back();
    if (value.empty()) throw AppError(L"备份文件包含无效整数。" );

    wchar_t localeNegative[16] = {};
    wchar_t localePositive[16] = {};
    GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SNEGATIVESIGN,
        localeNegative, static_cast<int>(std::size(localeNegative)));
    GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SPOSITIVESIGN,
        localePositive, static_cast<int>(std::size(localePositive)));
    if (localeNegative[0] && value.compare(0, wcslen(localeNegative), localeNegative) == 0) {
        value.replace(0, wcslen(localeNegative), L"-");
    } else if (localePositive[0] && value.compare(0, wcslen(localePositive), localePositive) == 0) {
        value.replace(0, wcslen(localePositive), L"+");
    }

    wchar_t* end = nullptr;
    errno = 0;
    const long long number = _wcstoi64(value.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != '\0' ||
        number < std::numeric_limits<int32_t>::min() || number > std::numeric_limits<int32_t>::max()) {
        throw AppError(L"备份文件包含无效整数。" );
    }
    return static_cast<int32_t>(number);
}

struct ScopedHandle {
    HANDLE value = nullptr;
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : value(handle) {}
    ~ScopedHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
};

struct ScopedRegKey {
    HKEY value = nullptr;
    ~ScopedRegKey() { if (value) RegCloseKey(value); }
    ScopedRegKey() = default;
    ScopedRegKey(const ScopedRegKey&) = delete;
    ScopedRegKey& operator=(const ScopedRegKey&) = delete;
};

struct ScopedServiceHandle {
    SC_HANDLE value = nullptr;
    ~ScopedServiceHandle() { if (value) CloseServiceHandle(value); }
    ScopedServiceHandle() = default;
    explicit ScopedServiceHandle(SC_HANDLE handle) : value(handle) {}
    ScopedServiceHandle(const ScopedServiceHandle&) = delete;
    ScopedServiceHandle& operator=(const ScopedServiceHandle&) = delete;
};

struct PolicyValue {
    const wchar_t* path;
    const wchar_t* name;
};

struct RawRegistryState {
    const PolicyValue* policy;
    bool exists;
    DWORD type;
    std::vector<BYTE> data;
};

struct PolicyState {
    const PolicyValue* policy;
    bool exists;
    int32_t value;
};

bool SamePolicy(const PolicyValue& a, const std::wstring& path, const std::wstring& name) {
    return path == a.path && name == a.name;
}

RawRegistryState ReadRawRegistryState(const PolicyValue& policy) {
    ScopedRegKey key;
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, policy.path, 0,
        KEY_QUERY_VALUE | kRegistryView, &key.value);
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) {
        return RawRegistryState{&policy, false, 0, {}};
    }
    if (opened != ERROR_SUCCESS) ThrowWin32(L"读取注册表值", opened);

    DWORD type = 0;
    DWORD size = 0;
    LSTATUS queried = RegQueryValueExW(key.value, policy.name, nullptr, &type, nullptr, &size);
    if (queried == ERROR_FILE_NOT_FOUND) return RawRegistryState{&policy, false, 0, {}};
    if (queried != ERROR_SUCCESS) ThrowWin32(L"读取注册表值大小", queried);
    if (size > 1024 * 1024) throw AppError(L"注册表值过大，拒绝读取。" );

    std::vector<BYTE> data(size);
    queried = RegQueryValueExW(key.value, policy.name, nullptr, &type,
        data.empty() ? nullptr : data.data(), &size);
    if (queried != ERROR_SUCCESS) ThrowWin32(L"读取注册表值", queried);
    data.resize(size);
    return RawRegistryState{&policy, true, type, std::move(data)};
}

void SetRawRegistryState(const RawRegistryState& state) {
    if (!state.exists || state.type == 0 || state.data.size() > MAXDWORD) {
        throw AppError(L"要写入的注册表原始状态无效。" );
    }
    ScopedRegKey key;
    DWORD disposition = 0;
    const LSTATUS created = RegCreateKeyExW(HKEY_LOCAL_MACHINE, state.policy->path, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE | kRegistryView, nullptr,
        &key.value, &disposition);
    if (created != ERROR_SUCCESS) ThrowWin32(L"写入注册表值", created);
    const LSTATUS written = RegSetValueExW(key.value, state.policy->name, 0, state.type,
        state.data.data(), static_cast<DWORD>(state.data.size()));
    if (written != ERROR_SUCCESS) ThrowWin32(L"写入注册表值", written);
}

void DeleteRawRegistryValue(const PolicyValue& policy) {
    ScopedRegKey key;
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, policy.path, 0,
        KEY_SET_VALUE | KEY_QUERY_VALUE | kRegistryView, &key.value);
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return;
    if (opened != ERROR_SUCCESS) ThrowWin32(L"打开注册表值", opened);
    const LSTATUS deleted = RegDeleteValueW(key.value, policy.name);
    if (deleted != ERROR_SUCCESS && deleted != ERROR_FILE_NOT_FOUND) {
        ThrowWin32(L"删除注册表值", deleted);
    }
}

void RestoreRawRegistryState(const RawRegistryState& state) {
    if (state.exists) SetRawRegistryState(state);
    else DeleteRawRegistryValue(*state.policy);
}

void VerifyRawRegistryState(const RawRegistryState& expected) {
    const RawRegistryState actual = ReadRawRegistryState(*expected.policy);
    if (actual.exists != expected.exists ||
        (actual.exists && (actual.type != expected.type || actual.data != expected.data))) {
        throw AppError(std::wstring(expected.policy->name) + L" 回读不一致。" );
    }
}

std::wstring RawRegistryString(const RawRegistryState& state) {
    if (!state.exists || state.type != REG_SZ || state.data.size() % sizeof(wchar_t) != 0) {
        return std::wstring();
    }
    std::wstring value(state.data.size() / sizeof(wchar_t), L'\0');
    if (!state.data.empty()) std::memcpy(&value[0], state.data.data(), state.data.size());
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

RawRegistryState MakeRegistryStringState(const PolicyValue& policy, const std::wstring& value) {
    RawRegistryState state{&policy, true, REG_SZ,
        std::vector<BYTE>((value.size() + 1) * sizeof(wchar_t))};
    std::memcpy(state.data.data(), value.c_str(), state.data.size());
    return state;
}

RawRegistryState MakeRegistryDwordState(const PolicyValue& policy, DWORD value) {
    RawRegistryState state{&policy, true, REG_DWORD, std::vector<BYTE>(sizeof(DWORD))};
    std::memcpy(state.data.data(), &value, sizeof(value));
    return state;
}

bool IsRegistryDword(const RawRegistryState& state, DWORD expected) {
    if (!state.exists || state.type != REG_DWORD || state.data.size() != sizeof(DWORD)) return false;
    DWORD value = 0;
    std::memcpy(&value, state.data.data(), sizeof(value));
    return value == expected;
}

PolicyState ReadDwordState(const PolicyValue& policy) {
    ScopedRegKey key;
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, policy.path, 0,
        KEY_QUERY_VALUE | kRegistryView, &key.value);
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) {
        return PolicyState{&policy, false, 0};
    }
    if (opened != ERROR_SUCCESS) ThrowWin32(L"读取注册表策略", opened);
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS queried = RegQueryValueExW(key.value, policy.name, nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &size);
    if (queried == ERROR_FILE_NOT_FOUND) return PolicyState{&policy, false, 0};
    if (queried != ERROR_SUCCESS) ThrowWin32(L"读取注册表策略", queried);
    if (type != REG_DWORD || size != sizeof(DWORD)) {
        throw AppError(std::wstring(L"注册表值不是 REG_DWORD：") + policy.path + L"\\" + policy.name);
    }
    return PolicyState{&policy, true, static_cast<int32_t>(value)};
}

void SetDword(const PolicyValue& policy, int32_t value) {
    ScopedRegKey key;
    DWORD disposition = 0;
    const LSTATUS created = RegCreateKeyExW(HKEY_LOCAL_MACHINE, policy.path, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE | kRegistryView, nullptr,
        &key.value, &disposition);
    if (created != ERROR_SUCCESS) ThrowWin32(L"写入注册表策略", created);
    const DWORD raw = static_cast<DWORD>(value);
    const LSTATUS written = RegSetValueExW(key.value, policy.name, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&raw), sizeof(raw));
    if (written != ERROR_SUCCESS) ThrowWin32(L"写入注册表策略", written);
}

void DeleteDword(const PolicyValue& policy) {
    ScopedRegKey key;
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, policy.path, 0,
        KEY_SET_VALUE | KEY_QUERY_VALUE | kRegistryView, &key.value);
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return;
    if (opened != ERROR_SUCCESS) ThrowWin32(L"打开注册表策略", opened);
    const LSTATUS deleted = RegDeleteValueW(key.value, policy.name);
    if (deleted != ERROR_SUCCESS && deleted != ERROR_FILE_NOT_FOUND) {
        ThrowWin32(L"删除注册表策略", deleted);
    }
}

void RestorePolicy(const PolicyState& state) {
    if (state.exists) SetDword(*state.policy, state.value);
    else DeleteDword(*state.policy);
}

void VerifyPolicy(const PolicyState& expected) {
    const PolicyState actual = ReadDwordState(*expected.policy);
    if (actual.exists != expected.exists || (actual.exists && actual.value != expected.value)) {
        throw AppError(std::wstring(expected.policy->name) + L" 恢复后回读不一致。" );
    }
}

std::wstring BackupDirectory() {
    wchar_t path[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA | CSIDL_FLAG_CREATE,
        nullptr, SHGFP_TYPE_CURRENT, path))) {
        throw AppError(L"无法定位 ProgramData。" );
    }
    return std::wstring(path) + L"\\UpdateLock";
}

void PrepareBackupDirectory() {
    const std::wstring directory = BackupDirectory();
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        ThrowWin32(L"创建备份目录");
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) {
        ThrowWin32(L"创建备份目录权限");
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (!GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) || !present) {
        LocalFree(descriptor);
        throw AppError(L"无法读取备份目录权限。" );
    }
    const DWORD result = SetNamedSecurityInfoW(const_cast<LPWSTR>(directory.c_str()),
        SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl, nullptr);
    LocalFree(descriptor);
    if (result != ERROR_SUCCESS) ThrowWin32(L"设置备份目录权限", result);
}

std::vector<std::string> ReadUtf8Lines(const std::wstring& path) {
    ScopedHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) ThrowWin32(L"打开备份文件");
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file.value, &size)) ThrowWin32(L"读取备份文件大小");
    if (size.QuadPart < 0 || size.QuadPart > 1024 * 1024) throw AppError(L"备份文件大小无效。" );
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!bytes.empty() && (!ReadFile(file.value, &bytes[0], static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())) {
        ThrowWin32(L"读取备份文件");
    }
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    (void)Utf8ToWide(bytes);
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < bytes.size()) {
        size_t end = bytes.find('\n', start);
        std::string line = bytes.substr(start, end == std::string::npos ? end : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (end == std::string::npos) { start = bytes.size(); break; }
        start = end + 1;
    }
    if (start == bytes.size() && !bytes.empty() && bytes.back() == '\n') {
        // File.WriteAllLines ends with CRLF; no synthetic empty line is part of the format.
    }
    return lines;
}

std::wstring UniqueTempPath(const std::wstring& finalPath) {
    return finalPath + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(GetTickCount());
}

void WriteUtf8LinesAtomically(const std::wstring& path, const std::vector<std::string>& lines, bool durable) {
    const std::wstring temp = UniqueTempPath(path);
    ScopedHandle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | (durable ? FILE_FLAG_WRITE_THROUGH : 0), nullptr));
    if (file.value == INVALID_HANDLE_VALUE) ThrowWin32(L"创建临时备份文件");
    std::string bytes;
    for (const std::string& line : lines) bytes += line + "\r\n";
    DWORD written = 0;
    if (!bytes.empty() && (!WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()),
        &written, nullptr) || written != bytes.size())) {
        const DWORD error = GetLastError();
        CloseHandle(file.value);
        file.value = nullptr;
        DeleteFileW(temp.c_str());
        ThrowWin32(L"写入临时备份文件", error);
    }
    if (durable && !FlushFileBuffers(file.value)) {
        const DWORD error = GetLastError();
        CloseHandle(file.value);
        file.value = nullptr;
        DeleteFileW(temp.c_str());
        ThrowWin32(L"刷新临时备份文件", error);
    }
    CloseHandle(file.value);
    file.value = nullptr;
    if (!MoveFileW(temp.c_str(), path.c_str())) {
        const DWORD error = GetLastError();
        DeleteFileW(temp.c_str());
        ThrowWin32(L"提交备份文件", error);
    }
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

enum class StatusLevel { Enabled, PartiallyDisabled, Disabled };
enum class OperationOutcome { Success, PartialSuccess, Failed };

struct UpdateStatus {
    StatusLevel level;
    std::wstring title;
    std::vector<std::wstring> details;
};

struct OperationResult {
    OperationOutcome outcome;
    std::wstring message;
};

struct VersionInfo {
    DWORD major;
    DWORD minor;
    DWORD build;
    BYTE productType;
};

bool IsWindows10Desktop(const VersionInfo& version) {
    return version.productType == VER_NT_WORKSTATION &&
        version.major == 10 && version.minor == 0 && version.build < 22000;
}

bool IsWindows11Desktop(const VersionInfo& version) {
    return version.productType == VER_NT_WORKSTATION &&
        version.major == 10 && version.minor == 0 && version.build >= 22000;
}

bool IsWindowsServer2016(const VersionInfo& version) {
    // Windows Server 2016 uses NT 10.0, build 14393. Keep the allow-list
    // narrow so later server releases are not silently treated as desktop OSes.
    return version.productType != VER_NT_WORKSTATION &&
        version.major == 10 && version.minor == 0 && version.build == 14393;
}

VersionInfo ReadWindowsVersion() {
    typedef LONG (WINAPI* RtlGetVersionFn)(PRTL_OSVERSIONINFOEXW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) ThrowWin32(L"加载系统版本组件");
    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtlGetVersion) throw AppError(L"当前系统不提供可靠版本检测接口。" );
    RTL_OSVERSIONINFOEXW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    const LONG status = rtlGetVersion(&info);
    if (status != 0) throw AppError(L"无法可靠识别当前 Windows 版本。" );
    return VersionInfo{info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber,
                       info.wProductType};
}

enum class Windows10VersionSource { DisplayVersion, ReleaseId, BuildMapping };

struct Windows10VersionInfo {
    bool isWindows10 = false;
    bool known = false;
    DWORD build = 0;
    DWORD revision = 0;
    std::wstring functionalVersion;
    Windows10VersionSource source = Windows10VersionSource::BuildMapping;
    std::wstring error;
};

struct Windows10BuildMapping {
    DWORD build;
    const wchar_t* functionalVersion;
};

const std::array<Windows10BuildMapping, 14> kWindows10BuildMappings = {{
    {10240, L"1507"}, {10586, L"1511"}, {14393, L"1607"}, {15063, L"1703"},
    {16299, L"1709"}, {17134, L"1803"}, {17763, L"1809"}, {18362, L"1903"},
    {18363, L"1909"}, {19041, L"2004"}, {19042, L"20H2"}, {19043, L"21H1"},
    {19044, L"21H2"}, {19045, L"22H2"}
}};

const Windows10BuildMapping* FindWindows10BuildMapping(DWORD build) {
    for (const Windows10BuildMapping& mapping : kWindows10BuildMappings) {
        if (mapping.build == build) return &mapping;
    }
    return nullptr;
}

bool IsDigits(const std::wstring& value) {
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

bool IsDisplayVersionFormat(const std::wstring& value) {
    return value.size() == 4 && value[2] == L'H' &&
        value[0] >= L'0' && value[0] <= L'9' && value[1] >= L'0' && value[1] <= L'9' &&
        (value[3] == L'1' || value[3] == L'2');
}

bool IsReleaseIdFormat(const std::wstring& value) {
    return value.size() == 4 && IsDigits(value);
}

std::wstring ReadOptionalRegistryString(const wchar_t* name) {
    const PolicyValue policy{kCurrentVersion, name};
    return RawRegistryString(ReadRawRegistryState(policy));
}

DWORD ReadOptionalRegistryDword(const wchar_t* name) {
    const PolicyValue policy{kCurrentVersion, name};
    const RawRegistryState state = ReadRawRegistryState(policy);
    if (!state.exists || state.type != REG_DWORD || state.data.size() != sizeof(DWORD)) return 0;
    DWORD value = 0;
    std::memcpy(&value, state.data.data(), sizeof(value));
    return value;
}

class Windows10VersionDetector {
public:
    static Windows10VersionInfo Resolve(const VersionInfo& version,
        const std::wstring& displayVersion, const std::wstring& releaseId, DWORD revision) {
        Windows10VersionInfo result;
        result.isWindows10 = IsWindows10Desktop(version);
        result.build = version.build;
        result.revision = revision;
        if (!result.isWindows10) return result;

        const Windows10BuildMapping* mapping = FindWindows10BuildMapping(version.build);
        if (!mapping) {
            result.error = L"已确认当前系统为 Windows 10，但当前 Build 无法映射到已知功能版本。\r\nBuild：" +
                std::to_wstring(version.build) + L"\r\n为避免写入错误目标版本，本次未修改系统。";
            return result;
        }

        if (IsDisplayVersionFormat(displayVersion) && displayVersion == mapping->functionalVersion) {
            result.known = true;
            result.functionalVersion = displayVersion;
            result.source = Windows10VersionSource::DisplayVersion;
        } else if (!IsDisplayVersionFormat(displayVersion) && IsReleaseIdFormat(releaseId) &&
                   releaseId == mapping->functionalVersion) {
            result.known = true;
            result.functionalVersion = releaseId;
            result.source = Windows10VersionSource::ReleaseId;
        } else {
            result.known = true;
            result.functionalVersion = mapping->functionalVersion;
            result.source = Windows10VersionSource::BuildMapping;
        }
        return result;
    }

    static Windows10VersionInfo Detect(const VersionInfo& version) {
        return Resolve(version, ReadOptionalRegistryString(L"DisplayVersion"),
            ReadOptionalRegistryString(L"ReleaseId"), ReadOptionalRegistryDword(L"UBR"));
    }

    static const wchar_t* SourceName(Windows10VersionSource source) {
        switch (source) {
        case Windows10VersionSource::DisplayVersion: return L"DisplayVersion";
        case Windows10VersionSource::ReleaseId: return L"ReleaseId";
        case Windows10VersionSource::BuildMapping: return L"Build 识别";
        }
        return L"Build 识别";
    }
};

enum class Windows11VersionSource { DisplayVersion, BuildMapping };

struct Windows11VersionInfo {
    bool isWindows11 = false;
    bool known = false;
    DWORD build = 0;
    DWORD revision = 0;
    std::wstring functionalVersion;
    Windows11VersionSource source = Windows11VersionSource::BuildMapping;
    std::wstring error;
};

struct Windows11BuildMapping {
    DWORD firstBuild;
    DWORD lastBuild;
    const wchar_t* functionalVersion;
};

const std::array<Windows11BuildMapping, 6> kWindows11BuildMappings = {{
    {22000, 22620, L"21H2"}, {22621, 22630, L"22H2"},
    {22631, 26099, L"23H2"}, {26100, 26199, L"24H2"},
    {26200, 26299, L"25H2"}, {26300, 26399, L"26H2"}
}};

const Windows11BuildMapping* FindWindows11BuildMapping(DWORD build) {
    for (const Windows11BuildMapping& mapping : kWindows11BuildMappings) {
        if (build >= mapping.firstBuild && build <= mapping.lastBuild) return &mapping;
    }
    return nullptr;
}

class Windows11VersionDetector {
public:
    static Windows11VersionInfo Resolve(const VersionInfo& version,
        const std::wstring& displayVersion, DWORD revision) {
        Windows11VersionInfo result;
        result.isWindows11 = IsWindows11Desktop(version);
        result.build = version.build;
        result.revision = revision;
        if (!result.isWindows11) return result;

        const Windows11BuildMapping* mapping = FindWindows11BuildMapping(version.build);
        if (IsDisplayVersionFormat(displayVersion) &&
            (!mapping || displayVersion == mapping->functionalVersion)) {
            result.known = true;
            result.functionalVersion = displayVersion;
            result.source = Windows11VersionSource::DisplayVersion;
        } else if (mapping) {
            result.known = true;
            result.functionalVersion = mapping->functionalVersion;
            result.source = Windows11VersionSource::BuildMapping;
        } else {
            result.error = L"已确认当前系统为 Windows 11，但当前 Build 无法映射到已知功能版本。\r\nBuild：" +
                std::to_wstring(version.build);
        }
        return result;
    }

    static Windows11VersionInfo Detect(const VersionInfo& version) {
        return Resolve(version, ReadOptionalRegistryString(L"DisplayVersion"),
            ReadOptionalRegistryDword(L"UBR"));
    }
};

class IUpdateController {
public:
    virtual ~IUpdateController() = default;
    virtual std::wstring DisplayName() const = 0;
    virtual std::wstring Description() const = 0;
    virtual std::wstring Warning() const = 0;
    virtual std::wstring Confirmation() const = 0;
    virtual std::wstring DisableButtonText() const = 0;
    virtual bool CanRestore() const = 0;
    virtual UpdateStatus GetStatus() const = 0;
    virtual OperationResult Disable() = 0;
    virtual OperationResult Restore() = 0;
};

class ModernUpdateController : public IUpdateController {
public:
    explicit ModernUpdateController(const wchar_t* displayName, bool supportsNotificationPolicy = true)
        : displayName_(displayName), backupPath_(BackupDirectory() + L"\\policy-backup-v1.txt"),
          accessBackupPath_(BackupDirectory() + L"\\windows-update-access-backup-v1.txt"),
          supportsNotificationPolicy_(supportsNotificationPolicy) {}

    std::wstring DisplayName() const override { return displayName_; }
    std::wstring Description() const override {
        if (!supportsNotificationPolicy_) {
            return L"关闭 Windows Server 自动更新、更新自动重启和 Windows Update 驱动更新，并禁止手动下载或安装更新。";
        }
        return L"关闭 Windows 自动更新、更新通知、更新自动重启和 Windows Update 驱动更新，并禁止通过 Windows Update 手动下载或安装更新。Windows Update 服务保持系统管理状态。";
    }
    std::wstring Warning() const override {
        return L"注意：关闭后 Windows 不会按正常自动更新流程获取系统安全补丁和 Windows Update 驱动更新。\r\n"
               L"Windows Update 页面将不再提供手动检查、下载或安装更新的入口。\r\n"
               L"“恢复”只恢复本软件第一次运行前保存的本机更新策略状态。";
    }
    std::wstring Confirmation() const override {
        if (!supportsNotificationPolicy_) {
            return L"这会关闭 Windows Server 自动更新、更新自动重启和 Windows Update 驱动更新。\r\n"
                   L"同时禁用 Windows Update 的手动检查、下载和安装入口。";
        }
        return L"这会关闭 Windows 自动更新、更新通知、更新自动重启和 Windows Update 驱动更新。\r\n"
               L"同时禁用 Windows Update 的手动检查、下载和安装入口。";
    }
    std::wstring DisableButtonText() const override { return L"彻底关闭 Windows 更新"; }
    bool CanRestore() const override { return FileExists(backupPath_); }
#ifdef UPDATELOCK_TEST
    std::vector<PolicyState> TestReadBackup() const { return ReadBackup(); }
    const std::wstring& TestBackupPath() const { return backupPath_; }
    void TestSetBackupPath(const std::wstring& path) { backupPath_ = path; }
    PolicyState TestReadAccessBackup() const { return ReadAccessBackup(); }
    const std::wstring& TestAccessBackupPath() const { return accessBackupPath_; }
    void TestSetAccessBackupPath(const std::wstring& path) { accessBackupPath_ = path; }
#endif

    UpdateStatus GetStatus() const override {
        const bool autoBlocked = IsValue(kPolicies[3], 1);
        const bool noRestart = IsValue(kPolicies[4], 1);
        const bool notificationsBlocked = supportsNotificationPolicy_
            ? IsValue(kPolicies[1], 1) && IsValue(kPolicies[2], 2)
            : true;
        const bool driverBlocked = IsValue(kPolicies[0], 1);
        const bool accessBlocked = IsValue(kDisableWindowsUpdateAccess, 1);
        return BuildStatus(autoBlocked, notificationsBlocked, noRestart, driverBlocked, accessBlocked,
            supportsNotificationPolicy_);
    }

    static UpdateStatus BuildStatus(bool autoBlocked, bool notificationsBlocked,
                                    bool noRestart, bool driverBlocked, bool accessBlocked,
                                    bool notificationsApplicable = true) {
        StatusLevel level;
        std::wstring title;
        std::wstring summary;
        if (!autoBlocked) {
            level = StatusLevel::Enabled;
            title = L"× Windows 更新未完全关闭";
            summary = L"核心自动更新策略未生效。";
        } else if ((!notificationsApplicable || notificationsBlocked) &&
                   noRestart && driverBlocked && accessBlocked) {
            level = StatusLevel::Disabled;
            title = L"√ Windows 更新策略已完全关闭";
            summary = L"Windows Update 手动检查、下载和安装：已禁用；Windows Update 服务仍由系统管理。";
        } else {
            level = StatusLevel::PartiallyDisabled;
            title = L"× Windows 自动更新已关闭，但部分封锁策略未生效";
            summary = L"部分附加或访问封锁策略未生效。";
        }
        return UpdateStatus{level, title, {
            std::wstring(autoBlocked ? L"√ " : L"× ") + L"自动更新：" +
                (autoBlocked ? L"已关闭" : L"未关闭") + L"    " +
                (notificationsApplicable ? (notificationsBlocked ? L"√ " : L"× ") : L"— ") +
                L"更新通知：" + (notificationsApplicable
                    ? (notificationsBlocked ? L"已关闭" : L"未关闭")
                    : L"当前系统不适用"),
            std::wstring(noRestart ? L"√ " : L"× ") + L"更新自动重启：" +
                (noRestart ? L"已限制" : L"未限制") + L"    " +
                (driverBlocked ? L"√ " : L"× ") + L"Windows Update 驱动更新：" +
                (driverBlocked ? L"已关闭" : L"未关闭"),
            std::wstring(accessBlocked ? L"√ " : L"× ") + L"Windows Update 手动访问：" +
                (accessBlocked ? L"已禁用" : L"未禁用"),
            summary}};
    }

    OperationResult Disable() override {
        std::vector<PolicyState> before;
        PolicyState accessBefore{};
        try { before = Capture(); }
        catch (const AppError& error) {
            return {OperationOutcome::Failed, L"读取当前策略失败，系统未修改：\r\n" + error.message};
        }
        try { accessBefore = ReadDwordState(kDisableWindowsUpdateAccess); }
        catch (const AppError& error) {
            return {OperationOutcome::Failed, L"读取 Windows Update 访问策略失败，系统未修改：\r\n" + error.message};
        }
        try {
            SaveOriginalIfNeeded(before);
            SaveAccessOriginalIfNeeded(accessBefore);
            SetDword(kPolicies[3], 1);
            SetDword(kPolicies[4], 1);
            DeleteDword(kPolicies[5]);
            SetDword(kPolicies[0], 1);
            if (supportsNotificationPolicy_) {
                SetDword(kPolicies[1], 1);
                SetDword(kPolicies[2], 2);
            }
            SetDword(kDisableWindowsUpdateAccess, 1);
            if (GetStatus().level != StatusLevel::Disabled) {
                throw AppError(L"自动更新、附加策略或 Windows Update 访问封锁写入后回读未全部达到目标状态。" );
            }
            return {OperationOutcome::Success,
                L"Windows 更新策略已完全关闭。\r\n\r\n"
                L"Windows Update 的手动检查、下载和安装入口已禁用。"};
        } catch (const AppError& error) {
            return DisableFailure(before, accessBefore, error.message);
        } catch (...) {
            return DisableFailure(before, accessBefore, L"发生未预期的原生运行时错误。");
        }
    }

    OperationResult Restore() override {
        std::vector<PolicyState> before;
        PolicyState accessBefore{};
        try { before = Capture(); }
        catch (const AppError& error) {
            return {OperationOutcome::Failed, L"读取当前策略失败，系统未修改：\r\n" + error.message};
        }
        try { accessBefore = ReadDwordState(kDisableWindowsUpdateAccess); }
        catch (const AppError& error) {
            return {OperationOutcome::Failed, L"读取 Windows Update 访问策略失败，系统未修改：\r\n" + error.message};
        }
        try {
            const std::vector<PolicyState> original = ReadBackup();
            RestoreStates(original);
            VerifyStates(original);
            if (FileExists(accessBackupPath_)) {
                const PolicyState originalAccess = ReadAccessBackup();
                RestorePolicy(originalAccess);
                VerifyPolicy(originalAccess);
            }
            if (!DeleteFileW(backupPath_.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
                return {OperationOutcome::PartialSuccess,
                    L"原始策略已经恢复并验证，但无法删除备份文件：\r\n" +
                    Win32Message(GetLastError())};
            }
            if (!DeleteFileW(accessBackupPath_.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
                return {OperationOutcome::PartialSuccess,
                    L"原始策略已经恢复并验证，但无法删除 Windows Update 访问策略备份：\r\n" +
                    Win32Message(GetLastError())};
            }
            return {OperationOutcome::Success, L"已恢复为本软件首次运行前的策略状态。"};
        } catch (const AppError& error) {
            return RestoreFailure(before, accessBefore, error.message);
        } catch (...) {
            return RestoreFailure(before, accessBefore, L"发生未预期的原生运行时错误。");
        }
    }

private:
    static const std::array<PolicyValue, 6> kPolicies;
    static const PolicyValue kDisableWindowsUpdateAccess;
    std::wstring displayName_;
    std::wstring backupPath_;
    std::wstring accessBackupPath_;
    bool supportsNotificationPolicy_;

    static bool IsValue(const PolicyValue& policy, int32_t expected) {
        const PolicyState state = ReadDwordState(policy);
        return state.exists && state.value == expected;
    }

    static const PolicyValue* FindPolicy(const std::wstring& path, const std::wstring& name) {
        for (const PolicyValue& policy : kPolicies) {
            if (SamePolicy(policy, path, name)) return &policy;
        }
        return nullptr;
    }

    static std::vector<PolicyState> Capture() {
        std::vector<PolicyState> result;
        result.reserve(kPolicies.size());
        for (const PolicyValue& policy : kPolicies) result.push_back(ReadDwordState(policy));
        return result;
    }

    void SaveOriginalIfNeeded(const std::vector<PolicyState>& original) {
        PrepareBackupDirectory();
        if (FileExists(backupPath_)) { (void)ReadBackup(); return; }
        std::vector<std::string> lines;
        for (const PolicyState& state : original) {
            lines.push_back(Base64Encode(state.policy->path) + "|" +
                Base64Encode(state.policy->name) + "|" + (state.exists ? "1" : "0") +
                "|" + std::to_string(state.value));
        }
        WriteUtf8LinesAtomically(backupPath_, lines, false);
    }

    void SaveAccessOriginalIfNeeded(const PolicyState& original) {
        PrepareBackupDirectory();
        if (FileExists(accessBackupPath_)) { (void)ReadAccessBackup(); return; }
        WriteUtf8LinesAtomically(accessBackupPath_, {
            "UpdateLock-ModernAccess|1",
            Base64Encode(original.policy->path) + "|" + Base64Encode(original.policy->name) + "|" +
                (original.exists ? "1" : "0") + "|" + std::to_string(original.value)}, false);
    }

    PolicyState ReadAccessBackup() const {
        const std::vector<std::string> lines = ReadUtf8Lines(accessBackupPath_);
        if (lines.size() != 2 || lines[0] != "UpdateLock-ModernAccess|1") {
            throw AppError(L"Windows Update 访问策略备份格式无效。" );
        }
        const std::vector<std::string> parts = Split(lines[1], '|');
        if (parts.size() != 4 || (parts[2] != "0" && parts[2] != "1") ||
            !SamePolicy(kDisableWindowsUpdateAccess, Base64Decode(parts[0]), Base64Decode(parts[1]))) {
            throw AppError(L"Windows Update 访问策略备份包含未允许项或无效值。" );
        }
        return PolicyState{&kDisableWindowsUpdateAccess, parts[2] == "1", ParseInt32(parts[3])};
    }

    std::vector<PolicyState> ReadBackup() const {
        const std::vector<std::string> lines = ReadUtf8Lines(backupPath_);
        if (lines.size() != kPolicies.size()) throw AppError(L"策略备份不完整。" );
        std::vector<PolicyState> parsed;
        for (const std::string& line : lines) {
            const std::vector<std::string> parts = Split(line, '|');
            if (parts.size() != 4) throw AppError(L"策略备份文件格式无效。" );
            const std::wstring path = Base64Decode(parts[0]);
            const std::wstring name = Base64Decode(parts[1]);
            const PolicyValue* allowed = FindPolicy(path, name);
            if (!allowed || (parts[2] != "0" && parts[2] != "1")) {
                throw AppError(L"策略备份包含未允许项或无效值。" );
            }
            for (const PolicyState& existing : parsed) {
                if (existing.policy == allowed) throw AppError(L"策略备份包含重复项。" );
            }
            parsed.push_back(PolicyState{allowed, parts[2] == "1", ParseInt32(parts[3])});
        }
        std::vector<PolicyState> ordered;
        for (const PolicyValue& policy : kPolicies) {
            const auto found = std::find_if(parsed.begin(), parsed.end(),
                [&policy](const PolicyState& state) { return state.policy == &policy; });
            if (found == parsed.end()) throw AppError(L"策略备份缺少必要项。" );
            ordered.push_back(*found);
        }
        return ordered;
    }

    static void RestoreStates(const std::vector<PolicyState>& states) {
        if (states.size() != kPolicies.size()) throw AppError(L"策略状态不完整。" );
        for (const PolicyState& state : states) {
            bool allowed = false;
            for (const PolicyValue& policy : kPolicies) if (state.policy == &policy) allowed = true;
            if (!allowed) throw AppError(L"策略状态包含未允许项。" );
            RestorePolicy(state);
        }
    }

    static void VerifyStates(const std::vector<PolicyState>& states) {
        for (const PolicyState& state : states) VerifyPolicy(state);
    }

    static std::wstring TryRestore(const std::vector<PolicyState>& states, const PolicyState& access) {
        try { RestoreStates(states); VerifyStates(states); RestorePolicy(access); VerifyPolicy(access); return std::wstring(); }
        catch (const AppError& error) { return error.message; }
        catch (...) { return L"回滚时发生未预期的原生运行时错误。"; }
    }

    static OperationResult DisableFailure(const std::vector<PolicyState>& before, const PolicyState& accessBefore,
        const std::wstring& error) {
        const std::wstring rollback = TryRestore(before, accessBefore);
        return rollback.empty()
            ? OperationResult{OperationOutcome::Failed,
                L"更新策略未完成写入，已恢复到操作前状态：\r\n" + error}
            : OperationResult{OperationOutcome::PartialSuccess,
                L"更新策略写入失败且回滚不完整。\r\n原始错误：" + error +
                L"\r\n回滚错误：" + rollback};
    }

    static OperationResult RestoreFailure(const std::vector<PolicyState>& before, const PolicyState& accessBefore,
        const std::wstring& error) {
        const std::wstring rollback = TryRestore(before, accessBefore);
        return rollback.empty()
            ? OperationResult{OperationOutcome::Failed,
                L"恢复失败，已返回到本次操作前状态：\r\n" + error}
            : OperationResult{OperationOutcome::PartialSuccess,
                L"恢复失败且无法完整返回到本次操作前状态。\r\n原始错误：" +
                error + L"\r\n回滚错误：" + rollback};
    }
};

const std::array<PolicyValue, 6> ModernUpdateController::kPolicies = {{
    {kWindowsUpdate, L"ExcludeWUDriversInQualityUpdate"},
    {kWindowsUpdate, L"SetUpdateNotificationLevel"},
    {kWindowsUpdate, L"UpdateNotificationLevel"},
    {kAutomaticUpdates, L"NoAutoUpdate"},
    {kAutomaticUpdates, L"NoAutoRebootWithLoggedOnUsers"},
    {kAutomaticUpdates, L"AUOptions"}
}};
const PolicyValue ModernUpdateController::kDisableWindowsUpdateAccess =
    {kWindowsUpdate, L"SetDisableUXWUAccess"};

struct ServiceState {
    DWORD startupType;
    bool delayed;
    DWORD status;
};

ScopedServiceHandle OpenWuauserv(DWORD access) {
    ScopedServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager.value) ThrowWin32(L"OpenSCManager");
    SC_HANDLE raw = OpenServiceW(manager.value, L"wuauserv", access);
    if (!raw) ThrowWin32(L"OpenService(wuauserv)");
    return ScopedServiceHandle(raw);
}

DWORD QueryServiceStatusStable(DWORD timeoutMs) {
    ScopedServiceHandle service = OpenWuauserv(SERVICE_QUERY_STATUS);
    const DWORD start = GetTickCount();
    for (;;) {
        SERVICE_STATUS_PROCESS status = {};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) ThrowWin32(L"QueryServiceStatusEx");
        if (status.dwCurrentState == SERVICE_RUNNING || status.dwCurrentState == SERVICE_STOPPED ||
            status.dwCurrentState == SERVICE_PAUSED) return status.dwCurrentState;
        if (GetTickCount() - start >= timeoutMs) throw AppError(L"等待 wuauserv 稳定状态超时。" );
        Sleep(200);
    }
}

ServiceState ReadServiceState(DWORD timeoutMs) {
    const DWORD status = QueryServiceStatusStable(timeoutMs);
    if (status != SERVICE_RUNNING && status != SERVICE_STOPPED) {
        throw AppError(L"wuauserv 处于不支持的暂停状态。" );
    }
    ScopedServiceHandle service = OpenWuauserv(SERVICE_QUERY_CONFIG);
    DWORD needed = 0;
    QueryServiceConfigW(service.value, nullptr, 0, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !needed) ThrowWin32(L"QueryServiceConfig 大小");
    std::vector<BYTE> buffer(needed);
    auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service.value, config, needed, &needed)) ThrowWin32(L"QueryServiceConfig");
    SERVICE_DELAYED_AUTO_START_INFO delayed = {};
    if (!QueryServiceConfig2W(service.value, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
        reinterpret_cast<BYTE*>(&delayed), sizeof(delayed), &needed)) ThrowWin32(L"QueryServiceConfig2");
    return ServiceState{config->dwStartType, delayed.fDelayedAutostart != FALSE, status};
}

void ChangeServiceStartup(DWORD startupType) {
    ScopedServiceHandle service = OpenWuauserv(SERVICE_CHANGE_CONFIG);
    if (!ChangeServiceConfigW(service.value, SERVICE_NO_CHANGE, startupType, SERVICE_NO_CHANGE,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) ThrowWin32(L"ChangeServiceConfig");
}

void ChangeServiceDelayed(bool delayed) {
    ScopedServiceHandle service = OpenWuauserv(SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG);
    SERVICE_DELAYED_AUTO_START_INFO info = {delayed ? TRUE : FALSE};
    if (!ChangeServiceConfig2W(service.value, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &info)) {
        ThrowWin32(L"ChangeServiceConfig2");
    }
}

void WaitForService(DWORD wanted, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        if (QueryServiceStatusStable(timeoutMs) == wanted) return;
        Sleep(200);
    }
    throw AppError(L"等待 wuauserv 状态超时。" );
}

void StopServiceSafe(DWORD timeoutMs) {
    const DWORD state = QueryServiceStatusStable(timeoutMs);
    if (state == SERVICE_STOPPED) return;
    if (state != SERVICE_RUNNING) throw AppError(L"wuauserv 不处于可安全停止状态。" );
    ScopedServiceHandle service = OpenWuauserv(SERVICE_STOP | SERVICE_QUERY_STATUS);
    SERVICE_STATUS status = {};
    if (!ControlService(service.value, SERVICE_CONTROL_STOP, &status)) ThrowWin32(L"停止 wuauserv");
    WaitForService(SERVICE_STOPPED, timeoutMs);
}

void StartServiceSafe(DWORD timeoutMs) {
    if (QueryServiceStatusStable(timeoutMs) == SERVICE_RUNNING) return;
    ScopedServiceHandle service = OpenWuauserv(SERVICE_START | SERVICE_QUERY_STATUS);
    if (!StartServiceW(service.value, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        ThrowWin32(L"启动 wuauserv");
    }
    WaitForService(SERVICE_RUNNING, timeoutMs);
}

void StopAndDisableWuauserv() {
    const DWORD state = QueryServiceStatusStable(30000);
    if (state != SERVICE_RUNNING && state != SERVICE_STOPPED) throw AppError(L"wuauserv 状态不稳定。" );
    StopServiceSafe(30000);
    ChangeServiceStartup(SERVICE_DISABLED);
    const ServiceState verified = ReadServiceState(30000);
    if (verified.startupType != SERVICE_DISABLED || verified.status != SERVICE_STOPPED) {
        throw AppError(L"wuauserv 停止或禁用后的 SCM 回读校验失败。" );
    }
}

void RestoreWuauserv(const ServiceState& original) {
    if (original.startupType < SERVICE_AUTO_START || original.startupType > SERVICE_DISABLED) {
        throw AppError(L"服务备份包含无效启动类型。" );
    }
    const DWORD current = QueryServiceStatusStable(30000);
    if (current != SERVICE_RUNNING && current != SERVICE_STOPPED) throw AppError(L"wuauserv 状态不稳定。" );
    if (original.status == SERVICE_RUNNING) {
        if (current == SERVICE_STOPPED) {
            ChangeServiceStartup(original.startupType == SERVICE_DISABLED ? SERVICE_DEMAND_START : original.startupType);
            StartServiceSafe(30000);
        }
        ChangeServiceStartup(original.startupType);
        ChangeServiceDelayed(original.delayed);
    } else {
        if (current == SERVICE_RUNNING) StopServiceSafe(30000);
        ChangeServiceStartup(original.startupType);
        ChangeServiceDelayed(original.delayed);
    }
}

bool IsWindowsUpdateInstallerBusy() {
    IUpdateSession* session = nullptr;
    const HRESULT created = CoCreateInstance(__uuidof(UpdateSession), nullptr,
        CLSCTX_INPROC_SERVER, __uuidof(IUpdateSession), reinterpret_cast<void**>(&session));
    if (FAILED(created) || !session) throw AppError(L"无法创建 Windows Update Agent 会话。" );
    IUpdateInstaller* installer = nullptr;
    const HRESULT madeInstaller = session->CreateUpdateInstaller(&installer);
    session->Release();
    if (FAILED(madeInstaller) || !installer) throw AppError(L"无法创建 Windows Update 安装器对象。" );
    VARIANT_BOOL busy = VARIANT_FALSE;
    const HRESULT queried = installer->get_IsBusy(&busy);
    installer->Release();
    if (FAILED(queried)) throw AppError(L"无法读取 Windows Update 安装状态。" );
    return busy != VARIANT_FALSE;
}

class Windows7UpdateController : public IUpdateController {
public:
    Windows7UpdateController() : backupPath_(BackupDirectory() + L"\\windows7-original-state-v1.txt") {}
    std::wstring DisplayName() const override { return L"Windows 7"; }
    std::wstring Description() const override { return L"通过 Windows 7 专用更新策略和 Windows Update 服务禁止自动更新与更新访问。"; }
    std::wstring Warning() const override { return L"Win7 模式仅处理 NoAutoUpdate、DisableWindowsUpdateAccess 和 wuauserv。\r\n不会修改 BITS、CryptSvc、TrustedInstaller、系统文件或更新缓存目录权限。"; }
    std::wstring Confirmation() const override { return L"这会设置 Windows 7 更新策略，并停止、禁用 Windows Update 服务 wuauserv。"; }
    std::wstring DisableButtonText() const override { return L"关闭所有 Windows 更新"; }
    bool CanRestore() const override { return FileExists(backupPath_); }
#ifdef UPDATELOCK_TEST
    void TestReadBackup() const { (void)ReadBackup(); }
    const std::wstring& TestBackupPath() const { return backupPath_; }
    void TestSetBackupPath(const std::wstring& path) { backupPath_ = path; }
#endif

    UpdateStatus GetStatus() const override {
        const bool automatic = IsValue(kNoAutoUpdate, 1);
        const bool access = IsValue(kDisableAccess, 1);
        const ServiceState service = ReadServiceState(15000);
        const bool startupDisabled = service.startupType == SERVICE_DISABLED;
        const bool stopped = service.status == SERVICE_STOPPED;
        const int count = automatic + access + startupDisabled + stopped;
        const StatusLevel level = count == 4 ? StatusLevel::Disabled : count == 0 ? StatusLevel::Enabled : StatusLevel::PartiallyDisabled;
        const std::wstring title = level == StatusLevel::Disabled ? L"√ Windows Update 已禁用" :
            level == StatusLevel::PartiallyDisabled ? L"× Windows Update 部分禁用" : L"× Windows Update 未禁用";
        return {level, title, {
            std::wstring(automatic ? L"√ " : L"× ") + L"自动更新策略：" +
                (automatic ? L"已禁用" : L"未禁用") + L"    " +
                (access ? L"√ " : L"× ") + L"Windows Update 访问：" +
                (access ? L"已禁用" : L"未禁用"),
            std::wstring(startupDisabled ? L"√ " : L"× ") + L"Windows Update 服务启动：" +
                (startupDisabled ? L"已禁用" : StartupName(service.startupType)),
            std::wstring(stopped ? L"√ " : L"× ") + L"Windows Update 服务状态：" +
                (stopped ? L"已停止" : L"正在运行")}};
    }

    OperationResult Disable() override {
        Win7State before;
        try {
            before = Capture();
            if (before.service.status == SERVICE_RUNNING && IsWindowsUpdateInstallerBusy()) {
                return {OperationOutcome::Failed, L"Windows Update 正在安装或卸载更新。为避免中断系统维护，本次未修改任何设置。"};
            }
        } catch (const AppError& e) { return {OperationOutcome::Failed, L"读取 Win7 当前状态失败，系统未修改：\r\n" + e.message}; }
        int completed = 0;
        try {
            SaveOriginalIfNeeded(before);
            SetDword(kNoAutoUpdate, 1); VerifyValue(kNoAutoUpdate, 1); ++completed;
            SetDword(kDisableAccess, 1); VerifyValue(kDisableAccess, 1); ++completed;
            StopAndDisableWuauserv(); ++completed;
            if (GetStatus().level != StatusLevel::Disabled) throw AppError(L"Win7 禁用后状态回读未全部满足。" );
            return {OperationOutcome::Success, L"Windows 7 更新策略和 wuauserv 已全部禁用，并已逐项回读确认。"};
        } catch (const AppError& e) {
            return DisableFailure(before, completed, e.message);
        } catch (...) {
            return DisableFailure(before, completed, L"发生未预期的原生运行时错误。");
        }
    }

    OperationResult Restore() override {
        Win7State before;
        try {
            before = Capture();
            if (before.service.status == SERVICE_RUNNING && IsWindowsUpdateInstallerBusy()) return {OperationOutcome::Failed, L"Windows Update 正在安装或卸载更新。为避免中断系统维护，本次未修改任何设置。"};
        } catch (const AppError& e) { return {OperationOutcome::Failed, L"读取 Win7 当前状态失败，系统未修改：\r\n" + e.message}; }
        try {
            const Win7State original = ReadBackup(); Apply(original); Verify(original);
            if (!DeleteFileW(backupPath_.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return {OperationOutcome::PartialSuccess, L"Windows 7 原始状态已恢复并验证，但无法删除备份文件：\r\n" + Win32Message(GetLastError())};
            return {OperationOutcome::Success, L"Windows 7 更新策略和 wuauserv 已恢复到第一次禁用前的原始状态。"};
        } catch (const AppError& e) {
            return RestoreFailure(before, e.message);
        } catch (...) {
            return RestoreFailure(before, L"发生未预期的原生运行时错误。");
        }
    }

private:
    struct Win7State { PolicyState noAuto; PolicyState access; ServiceState service; };
    static const PolicyValue kNoAutoUpdate;
    static const PolicyValue kDisableAccess;
    std::wstring backupPath_;
    static bool IsValue(const PolicyValue& p, int32_t v) { const auto s=ReadDwordState(p); return s.exists && s.value==v; }
    static std::wstring StartupName(DWORD type) { return type==SERVICE_AUTO_START?L"自动":type==SERVICE_DEMAND_START?L"手动":type==SERVICE_DISABLED?L"已禁用":L"未知"; }
    static Win7State Capture() { return {ReadDwordState(kNoAutoUpdate), ReadDwordState(kDisableAccess), ReadServiceState(30000)}; }
    static void VerifyValue(const PolicyValue& p, int32_t v) { if(!IsValue(p,v)) throw AppError(std::wstring(p.name)+L" 写入后回读失败。" ); }
    void SaveOriginalIfNeeded(const Win7State& s) {
        PrepareBackupDirectory(); if(FileExists(backupPath_)){(void)ReadBackup();return;}
        std::vector<std::string> lines={"UpdateLock-Windows7|1",PolicyLine(s.noAuto),PolicyLine(s.access),
            "SERVICE|wuauserv|"+std::to_string(s.service.startupType)+"|"+(s.service.status==SERVICE_RUNNING?"1":"0")+"|"+(s.service.delayed?"1":"0")};
        WriteUtf8LinesAtomically(backupPath_,lines,true); (void)ReadBackup();
    }
    static std::string PolicyLine(const PolicyState& s){return "DWORD|"+Base64Encode(s.policy->path)+"|"+Base64Encode(s.policy->name)+"|"+(s.exists?"1":"0")+"|"+std::to_string(s.value);}
    static PolicyState ParsePolicy(const std::string& line,const PolicyValue& allowed){auto p=Split(line,'|');if(p.size()!=5||p[0]!="DWORD"||Base64Decode(p[1])!=allowed.path||Base64Decode(p[2])!=allowed.name||(p[3]!="0"&&p[3]!="1"))throw AppError(L"Win7 策略备份包含未允许项或无效值。" );return {&allowed,p[3]=="1",ParseInt32(p[4])};}
    Win7State ReadBackup() const {
        auto l=ReadUtf8Lines(backupPath_);if(l.size()!=4||l[0]!="UpdateLock-Windows7|1")throw AppError(L"Win7 原始状态备份格式或版本无效。" );
        auto service=Split(l[3],'|');if(service.size()!=5||service[0]!="SERVICE"||service[1]!="wuauserv"||(service[3]!="0"&&service[3]!="1")||(service[4]!="0"&&service[4]!="1"))throw AppError(L"Win7 服务备份格式无效。" );
        int32_t start=ParseInt32(service[2]);if(start<2||start>4)throw AppError(L"Win7 服务备份格式无效。" );
        const DWORD savedStatus = service[3] == "1" ? static_cast<DWORD>(SERVICE_RUNNING) : static_cast<DWORD>(SERVICE_STOPPED);
        return {ParsePolicy(l[1],kNoAutoUpdate),ParsePolicy(l[2],kDisableAccess),{static_cast<DWORD>(start),service[4]=="1",savedStatus}};
    }
    static void Apply(const Win7State& s){RestorePolicy(s.noAuto);RestorePolicy(s.access);RestoreWuauserv(s.service);}
    static void Verify(const Win7State& s){VerifyPolicy(s.noAuto);VerifyPolicy(s.access);auto a=ReadServiceState(30000);if(a.startupType!=s.service.startupType||a.delayed!=s.service.delayed||a.status!=s.service.status)throw AppError(L"wuauserv 恢复后状态与原始备份不一致。" );}
    static std::wstring TryApply(const Win7State& s){try{Apply(s);Verify(s);return L"";}catch(const AppError&e){return e.message;}catch(...){return L"回滚时发生未预期的原生运行时错误。";}}
    static OperationResult DisableFailure(const Win7State& before,int completed,const std::wstring& error){const std::wstring rollback=TryApply(before);if(!rollback.empty())return{OperationOutcome::PartialSuccess,L"Win7 禁用部分失败且回滚不完整。\r\n原始错误："+error+L"\r\n回滚错误："+rollback};if(completed>0)return{OperationOutcome::PartialSuccess,L"Win7 禁用执行到第 "+std::to_wstring(completed)+L" 个已验证步骤后失败；已完整恢复到操作前状态，当前没有保留部分禁用效果。\r\n失败原因："+error};return{OperationOutcome::Failed,L"Win7 禁用未完成，已确认系统仍为操作前状态：\r\n"+error};}
    static OperationResult RestoreFailure(const Win7State& before,const std::wstring& error){const std::wstring rollback=TryApply(before);return rollback.empty()?OperationResult{OperationOutcome::Failed,L"Win7 恢复失败，已返回到本次操作前状态：\r\n"+error}:OperationResult{OperationOutcome::PartialSuccess,L"Win7 恢复失败且无法完整返回到本次操作前状态。\r\n原始错误："+error+L"\r\n回滚错误："+rollback};}
};

const PolicyValue Windows7UpdateController::kNoAutoUpdate={kAutomaticUpdates,L"NoAutoUpdate"};
const PolicyValue Windows7UpdateController::kDisableAccess={kWindowsUpdate,L"DisableWindowsUpdateAccess"};

class Windows11UpgradeBlocker {
public:
    Windows11UpgradeBlocker() : backupPath_(BackupDirectory() + L"\\windows11-upgrade-backup-v1.txt") {}

    std::wstring Description() const {
        return L"仅在 Windows 10 上锁定当前功能版本，阻止正常 Windows Update 路径升级到 Windows 11。";
    }

    std::wstring Warning() const {
        return L"Windows 11 升级控制只写入 ProductVersion、TargetReleaseVersion 和 TargetReleaseVersionInfo。\r\n"
               L"不会禁用更新服务，不修改 TPM、Secure Boot 或硬件兼容性检查。";
    }

    std::wstring Confirmation() const {
        return L"这会把当前 Windows 10 锁定在当前实际功能版本，阻止正常 Windows Update 路径升级到 Windows 11。\r\n"
               L"不会自动把旧版 Windows 10 升级到 22H2，也不会修改 Windows 自动更新控制。";
    }

    std::wstring BlockButtonText() const { return L"禁止升级到 Windows 11"; }
    std::wstring RestoreButtonText() const { return L"恢复允许升级到 Windows 11"; }
    bool CanRestore(const VersionInfo& version) const {
        return IsWindows10(version) && FileExists(backupPath_);
    }

#ifdef UPDATELOCK_TEST
    const std::wstring& TestBackupPath() const { return backupPath_; }
    void TestSetBackupPath(const std::wstring& path) { backupPath_ = path; }
    std::array<RawRegistryState, 3> TestReadBackup() const { return ReadBackup(); }
    static UpdateStatus TestBuildStatus(const Windows10VersionInfo& version, bool anyValues,
        bool productTarget, bool targetTarget, bool infoTarget, const std::wstring& configuredInfo) {
        return BuildStatus(version, anyValues, productTarget, targetTarget, infoTarget, configuredInfo);
    }
#endif

    UpdateStatus GetStatus(const VersionInfo& version) const {
        if (!IsWindows10(version)) {
            if (IsWindows11Desktop(version)) {
                return UpdateStatus{StatusLevel::Enabled,
                    L"Windows 11 升级状态：当前系统已是 Windows 11",
                    {L"当前系统已是 Windows 11；本功能不适用。"}};
            }
            return UpdateStatus{StatusLevel::Enabled,
                L"Windows 11 升级状态：当前系统不适用",
                {L"本功能只适用于 Windows 10，当前系统不会读取或写入升级锁定策略。"}};
        }

        const Windows10VersionInfo detected = Windows10VersionDetector::Detect(version);
        if (!detected.known) {
            return UpdateStatus{StatusLevel::PartiallyDisabled,
                L"Windows 11 升级状态：无法确认当前功能版本",
                {L"当前系统：Windows 10", L"OS Build：" + std::to_wstring(detected.build) +
                    (detected.revision ? L"." + std::to_wstring(detected.revision) : L""), detected.error}};
        }

        const std::array<RawRegistryState, 3> states = Capture();
        const bool productTarget = IsStringTarget(states[0], L"Windows 10");
        const bool targetTarget = IsRegistryDword(states[1], 1);
        const std::wstring configuredInfo = RawRegistryString(states[2]);
        const bool infoTarget = states[2].exists && states[2].type == REG_SZ &&
            configuredInfo == detected.functionalVersion;
        const bool anyValues = states[0].exists || states[1].exists || states[2].exists;
        return BuildStatus(detected, anyValues, productTarget, targetTarget, infoTarget, configuredInfo);
    }

    OperationResult Block(const VersionInfo& version) {
        const Windows10VersionInfo detected = Windows10VersionDetector::Detect(version);
        if (!detected.isWindows10) {
            return {OperationOutcome::Failed,
                L"“禁止升级到 Windows 11”只适用于 Windows 10，当前系统未修改。"};
        }
        if (!detected.known) return {OperationOutcome::Failed, detected.error};

        const std::array<RawRegistryState, 3> before = Capture();
        try {
            SaveOriginalIfNeeded(before);
            ApplyTarget(detected);
            const UpdateStatus status = GetStatus(version);
            if (status.title != L"Windows 11 升级状态：已阻止") {
                throw AppError(L"Windows 11 升级锁定写入后整体回读未达到目标状态。" );
            }
            return {OperationOutcome::Success,
                L"已将当前 Windows 10 锁定在功能版本 " + detected.functionalVersion + L"。\r\n\r\n"
                L"Windows 11 正常升级路径已阻止。"};
        } catch (const AppError& error) {
            return BlockFailure(before, error.message);
        } catch (...) {
            return BlockFailure(before, L"发生未预期的原生运行时错误。");
        }
    }

    OperationResult Restore(const VersionInfo& version) {
        if (!IsWindows10(version)) {
            return {OperationOutcome::Failed,
                L"“恢复允许升级到 Windows 11”只适用于 Windows 10，当前系统未修改。"};
        }
        const std::array<RawRegistryState, 3> before = Capture();
        try {
            const std::array<RawRegistryState, 3> original = ReadBackup();
            RestoreStates(original);
            VerifyStates(original);
            if (!DeleteFileW(backupPath_.c_str())) {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND) {
                    return {OperationOutcome::PartialSuccess,
                        L"Windows 11 升级策略已经恢复并验证，但无法删除备份文件：\r\n" +
                        Win32Message(error)};
                }
            }
            return {OperationOutcome::Success, L"已恢复允许升级到 Windows 11，并还原首次操作前的原始策略状态。"};
        } catch (const AppError& error) {
            return RestoreFailure(before, error.message);
        } catch (...) {
            return RestoreFailure(before, L"发生未预期的原生运行时错误。");
        }
    }

private:
    static const std::array<PolicyValue, 3> kPolicies;
    std::wstring backupPath_;

    static bool IsWindows10(const VersionInfo& version) {
        return IsWindows10Desktop(version);
    }

    static std::array<RawRegistryState, 3> Capture() {
        return {ReadRawRegistryState(kPolicies[0]), ReadRawRegistryState(kPolicies[1]),
                ReadRawRegistryState(kPolicies[2])};
    }

    static bool IsStringTarget(const RawRegistryState& state, const std::wstring& expected) {
        return state.exists && state.type == REG_SZ && RawRegistryString(state) == expected;
    }

    static UpdateStatus BuildStatus(const Windows10VersionInfo& version, bool anyValues,
        bool productTarget, bool targetTarget, bool infoTarget, const std::wstring& configuredInfo) {
        if (!anyValues) {
            return UpdateStatus{StatusLevel::Enabled, L"Windows 11 升级状态：允许", {
                L"当前系统：Windows 10 " + version.functionalVersion,
                L"OS Build：" + std::to_wstring(version.build) +
                    (version.revision ? L"." + std::to_wstring(version.revision) : L""),
                L"目标产品：未配置"}};
        }
        if (productTarget && targetTarget && infoTarget) {
            return UpdateStatus{StatusLevel::Disabled, L"Windows 11 升级状态：已阻止", {
                L"当前系统：Windows 10 " + version.functionalVersion,
                L"OS Build：" + std::to_wstring(version.build) +
                    (version.revision ? L"." + std::to_wstring(version.revision) : L""),
                L"锁定版本：Windows 10 " + version.functionalVersion}};
        }
        if (productTarget && targetTarget && !configuredInfo.empty() && !infoTarget) {
            return UpdateStatus{StatusLevel::PartiallyDisabled,
                L"Windows 11 升级状态：目标版本与当前系统不一致", {
                    L"当前系统：Windows 10 " + version.functionalVersion,
                    L"配置目标：Windows 10 " + configuredInfo,
                    L"请重新点击“禁止升级到 Windows 11”更新当前目标版本。"}};
        }
        return UpdateStatus{StatusLevel::PartiallyDisabled, L"Windows 11 升级状态：部分配置", {
            L"当前系统：Windows 10 " + version.functionalVersion,
            std::wstring(L"ProductVersion：") + (productTarget ? L"Windows 10" : L"未达到目标"),
            std::wstring(L"TargetReleaseVersion：") + (targetTarget ? L"1" : L"未达到目标"),
            std::wstring(L"TargetReleaseVersionInfo：") + (infoTarget ? version.functionalVersion : L"未达到目标")}};
    }

    void SaveOriginalIfNeeded(const std::array<RawRegistryState, 3>& original) {
        PrepareBackupDirectory();
        if (FileExists(backupPath_)) { (void)ReadBackup(); return; }
        std::vector<std::string> lines{"UpdateLock-Windows11Upgrade|1"};
        for (const RawRegistryState& state : original) {
            lines.push_back("RAW|" + Base64Encode(state.policy->path) + "|" +
                Base64Encode(state.policy->name) + "|" + (state.exists ? "1" : "0") + "|" +
                std::to_string(state.type) + "|" + Base64EncodeBytes(state.data));
        }
        WriteUtf8LinesAtomically(backupPath_, lines, false);
    }

    std::array<RawRegistryState, 3> ReadBackup() const {
        const std::vector<std::string> lines = ReadUtf8Lines(backupPath_);
        if (lines.size() != 4 || lines[0] != "UpdateLock-Windows11Upgrade|1") {
            throw AppError(L"Windows 11 升级备份格式无效。" );
        }
        std::array<RawRegistryState, 3> result{};
        for (size_t i = 0; i < kPolicies.size(); ++i) {
            const std::vector<std::string> parts = Split(lines[i + 1], '|');
            if (parts.size() != 6 || parts[0] != "RAW" || (parts[3] != "0" && parts[3] != "1")) {
                throw AppError(L"Windows 11 升级备份包含无效项目。" );
            }
            const std::wstring path = Base64Decode(parts[1]);
            const std::wstring name = Base64Decode(parts[2]);
            const PolicyValue* allowed = nullptr;
            for (const PolicyValue& policy : kPolicies) {
                if (SamePolicy(policy, path, name)) allowed = &policy;
            }
            if (!allowed) throw AppError(L"Windows 11 升级备份包含未允许项目。" );
            for (size_t j = 0; j < i; ++j) {
                if (result[j].policy == allowed) throw AppError(L"Windows 11 升级备份包含重复项目。" );
            }
            const int32_t type = ParseInt32(parts[4]);
            const std::vector<BYTE> data = Base64DecodeBytes(parts[5]);
            if (type < 0 || (parts[3] == "0" && (type != 0 || !data.empty())) ||
                (parts[3] == "1" && type == 0)) {
                throw AppError(L"Windows 11 升级备份值类型无效。" );
            }
            result[i] = RawRegistryState{allowed, parts[3] == "1", static_cast<DWORD>(type), data};
        }
        for (const PolicyValue& policy : kPolicies) {
            bool found = false;
            for (const RawRegistryState& state : result) if (state.policy == &policy) found = true;
            if (!found) throw AppError(L"Windows 11 升级备份缺少必要项目。" );
        }
        return result;
    }

    static void RestoreStates(const std::array<RawRegistryState, 3>& states) {
        for (const RawRegistryState& state : states) RestoreRawRegistryState(state);
    }

    static void VerifyStates(const std::array<RawRegistryState, 3>& states) {
        for (const RawRegistryState& state : states) VerifyRawRegistryState(state);
    }

    static void ApplyTarget(const Windows10VersionInfo& version) {
        const RawRegistryState product = MakeRegistryStringState(kPolicies[0], L"Windows 10");
        SetRawRegistryState(product); VerifyRawRegistryState(product);
        const RawRegistryState target = MakeRegistryDwordState(kPolicies[1], 1);
        SetRawRegistryState(target); VerifyRawRegistryState(target);
        const RawRegistryState info = MakeRegistryStringState(kPolicies[2], version.functionalVersion);
        SetRawRegistryState(info); VerifyRawRegistryState(info);
    }

    static std::wstring TryRestore(const std::array<RawRegistryState, 3>& states) {
        try { RestoreStates(states); VerifyStates(states); return std::wstring(); }
        catch (const AppError& error) { return error.message; }
        catch (...) { return L"回滚时发生未预期的原生运行时错误。"; }
    }

    static OperationResult BlockFailure(const std::array<RawRegistryState, 3>& before,
        const std::wstring& error) {
        const std::wstring rollback = TryRestore(before);
        return rollback.empty()
            ? OperationResult{OperationOutcome::Failed, L"Windows 11 升级锁定未完成，已恢复到操作前状态：\r\n" + error}
            : OperationResult{OperationOutcome::PartialSuccess,
                L"Windows 11 升级锁定失败且回滚不完整。\r\n原始错误：" + error +
                L"\r\n回滚错误：" + rollback};
    }

    static OperationResult RestoreFailure(const std::array<RawRegistryState, 3>& before,
        const std::wstring& error) {
        const std::wstring rollback = TryRestore(before);
        return rollback.empty()
            ? OperationResult{OperationOutcome::Failed, L"Windows 11 升级恢复失败，已返回到本次操作前状态：\r\n" + error}
            : OperationResult{OperationOutcome::PartialSuccess,
                L"Windows 11 升级恢复失败且回滚不完整。\r\n原始错误：" + error +
                L"\r\n回滚错误：" + rollback};
    }
};

const std::array<PolicyValue, 3> Windows11UpgradeBlocker::kPolicies = {{
    {kWindowsUpdate, L"ProductVersion"},
    {kWindowsUpdate, L"TargetReleaseVersion"},
    {kWindowsUpdate, L"TargetReleaseVersionInfo"}
}};

struct UiContext {
    std::unique_ptr<IUpdateController> controller;
    Windows11UpgradeBlocker upgradeBlocker;
    VersionInfo version{};
    HWND title = nullptr;
    HWND author = nullptr;
    HWND system = nullptr;
    HWND description = nullptr;
    HWND status = nullptr;
    HWND disableButton = nullptr;
    HWND restoreButton = nullptr;
    HWND upgradeStatus = nullptr;
    HWND upgradeDisableButton = nullptr;
    HWND upgradeRestoreButton = nullptr;
    HWND warning = nullptr;
    HFONT normalFont = nullptr;
    HFONT titleFont = nullptr;
    HFONT cardTitleFont = nullptr;
    HFONT stateFont = nullptr;
    HFONT badgeFont = nullptr;
    HFONT buttonFont = nullptr;
    HFONT authorFont = nullptr;
    std::vector<HFONT> retiredFonts;
    HBRUSH backgroundBrush = nullptr;
    UpdateStatus updateState{StatusLevel::Enabled, L"", {}};
    UpdateStatus upgradeState{StatusLevel::Enabled, L"", {}};
    bool updateReadFailed = false;
    bool upgradeReadFailed = false;
    int dpi = 96;
    int baseDpi = 96;
    int fontDpi = 96;
    int contentOriginX = 0;
    int scrollOffset = 0;
    bool scrollBarVisible = false;
    bool layingOut = false;
    bool inSizeMove = false;
};

class GdiplusSession {
public:
    GdiplusSession() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token_, &input, nullptr) != Gdiplus::Ok) {
            throw AppError(L"无法初始化 Windows GDI+ 图形组件。" );
        }
    }

    ~GdiplusSession() {
        if (token_) Gdiplus::GdiplusShutdown(token_);
    }

    GdiplusSession(const GdiplusSession&) = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;

private:
    ULONG_PTR token_ = 0;
};

int ScaleForDpi(int value, int dpi) {
    return MulDiv(value, dpi, 96);
}

constexpr int kUiClientWidth = 780;
constexpr int kUiClientHeight = 858;
constexpr int kUiMinimumClientWidth = kUiClientWidth;
constexpr int kUiMinimumClientHeight = 420;
constexpr int kUiMinimumRenderDpi = 72;
constexpr int kUiMaximumScalePercent = 125;

struct UiLayout {
    RECT title;
    RECT author;
    RECT system;
    RECT description;
    RECT updatePanel;
    RECT updateCard;
    RECT disableButton;
    RECT restoreButton;
    RECT upgradePanel;
    RECT upgradeCard;
    RECT upgradeDisableButton;
    RECT upgradeRestoreButton;
    RECT warning;
};

RECT ScaleRect(int left, int top, int right, int bottom, int dpi) {
    return RECT{ScaleForDpi(left, dpi), ScaleForDpi(top, dpi),
        ScaleForDpi(right, dpi), ScaleForDpi(bottom, dpi)};
}

int MinimumRenderDpi(int baseDpi) {
    return std::min(baseDpi, kUiMinimumRenderDpi);
}

int MaximumRenderDpi(int baseDpi) {
    return std::max(baseDpi, MulDiv(baseDpi, kUiMaximumScalePercent, 100));
}

int ChooseRenderDpi(int baseDpi, int availableWidth, int availableHeight,
    bool allowEnlarge) {
    const int widthDpi = MulDiv(std::max(1, availableWidth), 96, kUiClientWidth);
    const int heightDpi = MulDiv(std::max(1, availableHeight), 96, kUiClientHeight);
    const int upperBound = allowEnlarge ? MaximumRenderDpi(baseDpi) : baseDpi;
    int result = std::max(MinimumRenderDpi(baseDpi),
        std::min(upperBound, std::min(widthDpi, heightDpi)));
    const int minimum = MinimumRenderDpi(baseDpi);
    while (result > minimum &&
        (ScaleForDpi(kUiClientWidth, result) > availableWidth ||
         ScaleForDpi(kUiClientHeight, result) > availableHeight)) {
        --result;
    }
    return result;
}

UiLayout BuildUiLayout(int dpi) {
    return UiLayout{
        ScaleRect(28, 14, 570, 52, dpi),
        ScaleRect(590, 23, 752, 47, dpi),
        ScaleRect(24, 58, 756, 100, dpi),
        ScaleRect(30, 108, 750, 144, dpi),
        ScaleRect(20, 154, 760, 432, dpi),
        ScaleRect(36, 170, 744, 362, dpi),
        ScaleRect(36, 374, 378, 418, dpi),
        ScaleRect(402, 374, 744, 418, dpi),
        ScaleRect(20, 454, 760, 696, dpi),
        ScaleRect(36, 470, 744, 626, dpi),
        ScaleRect(36, 638, 378, 682, dpi),
        ScaleRect(402, 638, 744, 682, dpi),
        ScaleRect(20, 718, 760, 846, dpi)};
}

void OffsetUiLayout(UiLayout& layout, int offsetX, int offsetY) {
    OffsetRect(&layout.title, offsetX, offsetY);
    OffsetRect(&layout.author, offsetX, offsetY);
    OffsetRect(&layout.system, offsetX, offsetY);
    OffsetRect(&layout.description, offsetX, offsetY);
    OffsetRect(&layout.updatePanel, offsetX, offsetY);
    OffsetRect(&layout.updateCard, offsetX, offsetY);
    OffsetRect(&layout.disableButton, offsetX, offsetY);
    OffsetRect(&layout.restoreButton, offsetX, offsetY);
    OffsetRect(&layout.upgradePanel, offsetX, offsetY);
    OffsetRect(&layout.upgradeCard, offsetX, offsetY);
    OffsetRect(&layout.upgradeDisableButton, offsetX, offsetY);
    OffsetRect(&layout.upgradeRestoreButton, offsetX, offsetY);
    OffsetRect(&layout.warning, offsetX, offsetY);
}

enum class StatusVisual { Neutral, Success, Warning, Error };

struct VisualPalette {
    COLORREF foreground;
    COLORREF background;
    COLORREF border;
    COLORREF panel;
};

VisualPalette PaletteFor(StatusVisual visual) {
    switch (visual) {
    case StatusVisual::Success:
        return {RGB(0, 118, 68), RGB(229, 247, 237), RGB(184, 222, 199), RGB(244, 251, 247)};
    case StatusVisual::Warning:
        return {RGB(183, 96, 0), RGB(255, 243, 218), RGB(240, 202, 132), RGB(255, 250, 240)};
    case StatusVisual::Error:
        return {RGB(185, 32, 48), RGB(254, 232, 235), RGB(241, 187, 195), RGB(255, 246, 247)};
    case StatusVisual::Neutral:
    default:
        return {RGB(69, 85, 103), RGB(239, 243, 248), RGB(208, 218, 228), RGB(245, 249, 253)};
    }
}

StatusVisual VisualForLevel(StatusLevel level) {
    if (level == StatusLevel::Disabled) return StatusVisual::Success;
    if (level == StatusLevel::PartiallyDisabled) return StatusVisual::Warning;
    return StatusVisual::Error;
}

StatusVisual UpdateVisual(const UiContext& ui) {
    return ui.updateReadFailed ? StatusVisual::Error : VisualForLevel(ui.updateState.level);
}

StatusVisual UpgradeVisual(const UiContext& ui) {
    if (ui.upgradeReadFailed) return StatusVisual::Error;
    return IsWindows10Desktop(ui.version) ? VisualForLevel(ui.upgradeState.level) : StatusVisual::Neutral;
}

std::wstring UpdateHeaderState(const UiContext& ui) {
    if (ui.updateReadFailed) return L"读取失败";
    if (ui.updateState.level == StatusLevel::Disabled) return L"策略已完全关闭";
    if (ui.updateState.level == StatusLevel::PartiallyDisabled) return L"部分关闭";
    return L"未关闭";
}

std::wstring UpgradeHeaderState(const UiContext& ui) {
    if (ui.upgradeReadFailed) return L"读取失败";
    if (!IsWindows10Desktop(ui.version)) {
        const size_t separator = ui.upgradeState.title.find(L'：');
        return separator == std::wstring::npos ? ui.upgradeState.title :
            ui.upgradeState.title.substr(separator + 1);
    }
    if (ui.upgradeState.level == StatusLevel::Disabled) return L"已阻止";
    if (ui.upgradeState.level == StatusLevel::PartiallyDisabled) return L"部分配置";
    return L"未阻止";
}

struct StatusRow {
    std::wstring label;
    std::wstring value;
    StatusVisual visual = StatusVisual::Neutral;
    bool badge = true;
};

std::wstring TrimDisplayText(const std::wstring& text) {
    size_t first = 0;
    while (first < text.size() && iswspace(text[first])) ++first;
    size_t last = text.size();
    while (last > first && iswspace(text[last - 1])) --last;
    return text.substr(first, last - first);
}

bool IsStatusMarker(wchar_t value) {
    return value == L'√' || value == L'×' || value == L'—';
}

StatusVisual VisualForMarker(wchar_t marker) {
    if (marker == L'√') return StatusVisual::Success;
    if (marker == L'×') return StatusVisual::Error;
    return StatusVisual::Neutral;
}

std::vector<StatusRow> BuildUpdateRows(const UpdateStatus& status) {
    std::vector<StatusRow> rows;
    for (const std::wstring& line : status.details) {
        size_t marker = 0;
        while (marker < line.size()) {
            while (marker < line.size() && !IsStatusMarker(line[marker])) ++marker;
            if (marker >= line.size()) break;
            size_t next = marker + 1;
            while (next < line.size() && !IsStatusMarker(line[next])) ++next;
            std::wstring item = TrimDisplayText(line.substr(marker + 1, next - marker - 1));
            const size_t separator = item.find(L'：');
            if (separator != std::wstring::npos) {
                StatusRow row;
                row.label = TrimDisplayText(item.substr(0, separator));
                row.value = TrimDisplayText(item.substr(separator + 1));
                row.visual = VisualForMarker(line[marker]);
                if (row.label == L"Windows Update 手动访问") {
                    row.label = L"Windows Update 手动检查、下载和安装";
                    if (row.value == L"未禁用") row.value = L"可用";
                }
                rows.push_back(row);
            }
            marker = next;
        }
    }
    return rows;
}

std::vector<StatusRow> BuildUpgradeRows(const UiContext& ui) {
    std::vector<StatusRow> rows;
    for (const std::wstring& detail : ui.upgradeState.details) {
        StatusRow row;
        const size_t separator = detail.find(L'：');
        if (separator == std::wstring::npos) {
            row.label = detail;
            row.badge = false;
            row.visual = ui.upgradeReadFailed ? StatusVisual::Error : UpgradeVisual(ui);
            rows.push_back(row);
            continue;
        }
        row.label = TrimDisplayText(detail.substr(0, separator));
        row.value = TrimDisplayText(detail.substr(separator + 1));
        row.badge = row.label == L"锁定版本" || row.label == L"目标产品" ||
            row.value == L"未达到目标";
        row.visual = StatusVisual::Neutral;
        if (row.label == L"目标产品") {
            row.label = L"锁定版本";
            if (row.value == L"未配置") row.value = L"未锁定";
            row.badge = true;
            row.visual = StatusVisual::Error;
        } else if (row.label == L"锁定版本") {
            row.visual = ui.upgradeState.level == StatusLevel::Disabled ?
                StatusVisual::Success : StatusVisual::Error;
        } else if (row.value == L"未达到目标") {
            row.visual = StatusVisual::Error;
        }
        rows.push_back(row);
    }
    return rows;
}

struct SystemDisplayInfo {
    std::wstring system;
    std::wstring feature;
    std::wstring build;
};

SystemDisplayInfo BuildSystemDisplayInfo(const VersionInfo& version,
    const std::wstring& displayName) {
    SystemDisplayInfo result{displayName, L"不适用", std::to_wstring(version.build)};
    if (IsWindows10Desktop(version)) {
        try {
            const Windows10VersionInfo detected = Windows10VersionDetector::Detect(version);
            if (detected.known) {
                result.feature = detected.functionalVersion;
                if (detected.source == Windows10VersionSource::BuildMapping) result.feature += L"（Build 识别）";
            } else {
                result.feature = L"无法识别";
            }
        } catch (const AppError&) {
            result.feature = L"无法识别";
        }
    } else if (IsWindows11Desktop(version)) {
        try {
            const Windows11VersionInfo detected = Windows11VersionDetector::Detect(version);
            if (detected.known) {
                result.feature = detected.functionalVersion;
                if (detected.source == Windows11VersionSource::BuildMapping) result.feature += L"（Build 识别）";
            } else {
                result.feature = L"无法识别";
            }
        } catch (const AppError&) {
            result.feature = L"无法识别";
        }
    }
    return result;
}

std::wstring BuildSystemLabel(const VersionInfo& version, const std::wstring& displayName) {
    const SystemDisplayInfo info = BuildSystemDisplayInfo(version, displayName);
    return L"当前系统：" + info.system + L"；功能版本：" + info.feature + L"；Build：" + info.build;
}

void FillRoundedPanel(HDC dc, const RECT& rect, COLORREF fill, COLORREF border,
    int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void FillRectColor(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

Gdiplus::Color GdiplusColor(COLORREF color) {
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

void ConfigureIconGraphics(Gdiplus::Graphics& graphics) {
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
}

void AddRoundedRectPath(Gdiplus::GraphicsPath& path, const RECT& rect, int radius) {
    const Gdiplus::REAL x = static_cast<Gdiplus::REAL>(rect.left);
    const Gdiplus::REAL y = static_cast<Gdiplus::REAL>(rect.top);
    const Gdiplus::REAL width = static_cast<Gdiplus::REAL>(rect.right - rect.left);
    const Gdiplus::REAL height = static_cast<Gdiplus::REAL>(rect.bottom - rect.top);
    const Gdiplus::REAL diameter = static_cast<Gdiplus::REAL>(
        std::max(1, std::min(radius * 2, static_cast<int>(std::min(width, height)))));
    path.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(x + width - diameter, y + height - diameter,
        diameter, diameter, 0.0f, 90.0f);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRoundedBadge(HDC dc, const RECT& rect, COLORREF fill,
    COLORREF border, int radius) {
    Gdiplus::Graphics graphics(dc);
    ConfigureIconGraphics(graphics);
    Gdiplus::GraphicsPath path;
    AddRoundedRectPath(path, rect, radius);
    Gdiplus::SolidBrush brush(GdiplusColor(fill));
    Gdiplus::Pen pen(GdiplusColor(border), 1.0f);
    graphics.FillPath(&brush, &path);
    graphics.DrawPath(&pen, &path);
}

void DrawStatusIcon(HDC dc, int centerX, int centerY, int size,
    StatusVisual visual) {
    const VisualPalette palette = PaletteFor(visual);
    Gdiplus::Graphics graphics(dc);
    ConfigureIconGraphics(graphics);
    const Gdiplus::REAL iconSize = static_cast<Gdiplus::REAL>(size);
    const Gdiplus::REAL half = iconSize / 2.0f;
    const Gdiplus::REAL left = static_cast<Gdiplus::REAL>(centerX) - half;
    const Gdiplus::REAL top = static_cast<Gdiplus::REAL>(centerY) - half;
    Gdiplus::SolidBrush shapeBrush(GdiplusColor(palette.foreground));
    if (visual == StatusVisual::Warning) {
        const Gdiplus::PointF points[3] = {
            Gdiplus::PointF(static_cast<Gdiplus::REAL>(centerX), top),
            Gdiplus::PointF(left, top + iconSize),
            Gdiplus::PointF(left + iconSize, top + iconSize)};
        graphics.FillPolygon(&shapeBrush, points, static_cast<INT>(std::size(points)));
    } else {
        graphics.FillEllipse(&shapeBrush, left, top, iconSize, iconSize);
    }

    const Gdiplus::REAL stroke = std::max(1.6f, iconSize * 0.11f);
    Gdiplus::Pen markPen(Gdiplus::Color(255, 255, 255), stroke);
    markPen.SetStartCap(Gdiplus::LineCapRound);
    markPen.SetEndCap(Gdiplus::LineCapRound);
    markPen.SetLineJoin(Gdiplus::LineJoinRound);
    const Gdiplus::REAL cx = static_cast<Gdiplus::REAL>(centerX);
    const Gdiplus::REAL cy = static_cast<Gdiplus::REAL>(centerY);
    if (visual == StatusVisual::Success) {
        const Gdiplus::PointF check[3] = {
            Gdiplus::PointF(cx - iconSize * 0.25f, cy + iconSize * 0.01f),
            Gdiplus::PointF(cx - iconSize * 0.07f, cy + iconSize * 0.19f),
            Gdiplus::PointF(cx + iconSize * 0.29f, cy - iconSize * 0.22f)};
        graphics.DrawLines(&markPen, check, static_cast<INT>(std::size(check)));
    } else if (visual == StatusVisual::Error) {
        const Gdiplus::REAL arm = iconSize * 0.22f;
        graphics.DrawLine(&markPen, cx - arm, cy - arm, cx + arm, cy + arm);
        graphics.DrawLine(&markPen, cx + arm, cy - arm, cx - arm, cy + arm);
    } else if (visual == StatusVisual::Warning) {
        graphics.DrawLine(&markPen, cx, cy - iconSize * 0.22f,
            cx, cy + iconSize * 0.13f);
        Gdiplus::SolidBrush markBrush(Gdiplus::Color(255, 255, 255));
        const Gdiplus::REAL dot = std::max(1.8f, iconSize * 0.12f);
        graphics.FillEllipse(&markBrush, cx - dot / 2.0f,
            cy + iconSize * 0.28f - dot / 2.0f, dot, dot);
    } else {
        graphics.DrawLine(&markPen, cx - iconSize * 0.24f, cy,
            cx + iconSize * 0.24f, cy);
    }
}

void DrawButtonActionIcon(HDC dc, int centerX, int centerY, int size,
    COLORREF color, bool restore) {
    Gdiplus::Graphics graphics(dc);
    ConfigureIconGraphics(graphics);
    const Gdiplus::REAL iconSize = static_cast<Gdiplus::REAL>(size);
    const Gdiplus::REAL cx = static_cast<Gdiplus::REAL>(centerX);
    const Gdiplus::REAL cy = static_cast<Gdiplus::REAL>(centerY);
    const Gdiplus::REAL stroke = std::max(1.8f, iconSize * 0.095f);
    Gdiplus::Pen pen(GdiplusColor(color), stroke);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    if (restore) {
        const Gdiplus::PointF tip(cx - iconSize * 0.40f, cy - iconSize * 0.16f);
        Gdiplus::GraphicsPath curve;
        curve.AddBezier(cx - iconSize * 0.08f, cy + iconSize * 0.36f,
            cx + iconSize * 0.56f, cy + iconSize * 0.40f,
            cx + iconSize * 0.56f, cy - iconSize * 0.16f,
            cx + iconSize * 0.08f, cy - iconSize * 0.16f);
        curve.AddLine(Gdiplus::PointF(cx + iconSize * 0.08f,
            cy - iconSize * 0.16f), tip);
        graphics.DrawPath(&pen, &curve);
        const Gdiplus::PointF arrow[3] = {
            Gdiplus::PointF(cx - iconSize * 0.17f, cy - iconSize * 0.39f),
            tip,
            Gdiplus::PointF(cx - iconSize * 0.17f, cy + iconSize * 0.07f)};
        graphics.DrawLines(&pen, arrow, static_cast<INT>(std::size(arrow)));
    } else {
        const Gdiplus::REAL inset = stroke / 2.0f + 0.5f;
        graphics.DrawEllipse(&pen, cx - iconSize / 2.0f + inset,
            cy - iconSize / 2.0f + inset, iconSize - inset * 2.0f,
            iconSize - inset * 2.0f);
        graphics.DrawLine(&pen, cx - iconSize * 0.29f, cy + iconSize * 0.29f,
            cx + iconSize * 0.29f, cy - iconSize * 0.29f);
    }
}

void FillAntialiasedButton(HDC dc, const RECT& rect, COLORREF fill, COLORREF border) {
    Gdiplus::Graphics graphics(dc);
    ConfigureIconGraphics(graphics);
    const Gdiplus::REAL height = static_cast<Gdiplus::REAL>(rect.bottom - rect.top);
    const Gdiplus::REAL stroke = std::max(1.0f, height / 44.0f);
    const Gdiplus::REAL inset = stroke / 2.0f;
    const Gdiplus::REAL x = static_cast<Gdiplus::REAL>(rect.left) + inset;
    const Gdiplus::REAL y = static_cast<Gdiplus::REAL>(rect.top) + inset;
    const Gdiplus::REAL width = static_cast<Gdiplus::REAL>(rect.right - rect.left) - stroke - 1.0f;
    const Gdiplus::REAL pathHeight = height - stroke - 1.0f;
    const Gdiplus::REAL diameter = std::max(6.0f, height / 5.0f);
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(x + width - diameter, y + pathHeight - diameter,
        diameter, diameter, 0.0f, 90.0f);
    path.AddArc(x, y + pathHeight - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(GdiplusColor(fill));
    Gdiplus::Pen pen(GdiplusColor(border), stroke);
    graphics.FillPath(&brush, &path);
    graphics.DrawPath(&pen, &path);
}

void DrawOwnerButton(const DRAWITEMSTRUCT& item) {
    wchar_t text[160] = {};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    const bool primary = item.CtlID == IDC_DISABLE || item.CtlID == IDC_UPGRADE_DISABLE;
    const bool restore = item.CtlID == IDC_RESTORE || item.CtlID == IDC_UPGRADE_RESTORE;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    COLORREF fill = primary ? RGB(0, 103, 192) : RGB(255, 255, 255);
    COLORREF border = primary ? RGB(0, 103, 192) : RGB(178, 190, 203);
    COLORREF foreground = primary ? RGB(255, 255, 255) : RGB(35, 50, 69);
    if (pressed && !disabled) {
        fill = primary ? RGB(0, 83, 158) : RGB(235, 240, 246);
    }
    if (disabled) {
        fill = RGB(240, 243, 247);
        border = RGB(211, 218, 226);
        foreground = RGB(145, 153, 164);
    }
    RECT rect = item.rcItem;
    const int buttonHeight = static_cast<int>(rect.bottom - rect.top);
    FillAntialiasedButton(item.hDC, rect, fill, border);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, foreground);
    HGDIOBJ oldFont = SelectObject(item.hDC,
        reinterpret_cast<HGDIOBJ>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0)));
    SIZE textSize = {};
    GetTextExtentPoint32W(item.hDC, text, static_cast<int>(wcslen(text)), &textSize);
    const int iconSize = std::max(16, buttonHeight / 2 - 2);
    const int gap = iconSize / 2;
    const int groupWidth = iconSize + gap + textSize.cx;
    const int offset = pressed && !disabled ? 1 : 0;
    const int iconX = rect.left + (rect.right - rect.left - groupWidth) / 2 + iconSize / 2 + offset;
    const int centerY = (rect.top + rect.bottom) / 2 + offset;
    DrawButtonActionIcon(item.hDC, iconX, centerY, iconSize, foreground, restore);
    RECT textRect = {iconX + iconSize / 2 + gap, rect.top + offset,
        rect.right, rect.bottom + offset};
    DrawTextW(item.hDC, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, oldFont);
    if ((item.itemState & ODS_FOCUS) && !disabled) {
        RECT focus = rect;
        InflateRect(&focus, -5, -5);
        DrawFocusRect(item.hDC, &focus);
    }
}

void SetControlFont(HWND control, HFONT font, bool redraw = true) {
    if (control && font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), redraw ? TRUE : FALSE);
    }
}

void ReapRetiredFonts(UiContext& ui) {
    auto it = ui.retiredFonts.begin();
    while (it != ui.retiredFonts.end()) {
        if (DeleteObject(*it)) it = ui.retiredFonts.erase(it);
        else ++it;
    }
}

void RetireUiFont(UiContext& ui, HFONT font) {
    if (!font) return;
    if (!DeleteObject(font)) ui.retiredFonts.push_back(font);
}

void CreateUiFonts(UiContext& ui, int dpi) {
    HFONT normalFont = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT titleFont = CreateFontW(-MulDiv(17, dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT cardTitleFont = CreateFontW(-MulDiv(13, dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT stateFont = CreateFontW(-MulDiv(18, dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT badgeFont = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT buttonFont = CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT authorFont = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (!normalFont || !titleFont || !cardTitleFont || !stateFont ||
        !badgeFont || !buttonFont || !authorFont) {
        if (normalFont) DeleteObject(normalFont);
        if (titleFont) DeleteObject(titleFont);
        if (cardTitleFont) DeleteObject(cardTitleFont);
        if (stateFont) DeleteObject(stateFont);
        if (badgeFont) DeleteObject(badgeFont);
        if (buttonFont) DeleteObject(buttonFont);
        if (authorFont) DeleteObject(authorFont);
        ThrowWin32(L"创建界面字体");
    }
    const HFONT oldNormalFont = ui.normalFont;
    const HFONT oldTitleFont = ui.titleFont;
    const HFONT oldCardTitleFont = ui.cardTitleFont;
    const HFONT oldStateFont = ui.stateFont;
    const HFONT oldBadgeFont = ui.badgeFont;
    const HFONT oldButtonFont = ui.buttonFont;
    const HFONT oldAuthorFont = ui.authorFont;
    ui.normalFont = normalFont;
    ui.titleFont = titleFont;
    ui.cardTitleFont = cardTitleFont;
    ui.stateFont = stateFont;
    ui.badgeFont = badgeFont;
    ui.buttonFont = buttonFont;
    ui.authorFont = authorFont;

    for (HWND control : {ui.system, ui.description, ui.status,
         ui.disableButton, ui.restoreButton, ui.upgradeStatus,
         ui.upgradeDisableButton, ui.upgradeRestoreButton, ui.warning}) {
        SetControlFont(control, ui.normalFont, false);
    }
    SetControlFont(ui.title, ui.titleFont, false);
    SetControlFont(ui.author, ui.authorFont, false);
    for (HWND button : {ui.disableButton, ui.restoreButton,
         ui.upgradeDisableButton, ui.upgradeRestoreButton}) {
        SetControlFont(button, ui.buttonFont, false);
    }
    RetireUiFont(ui, oldNormalFont);
    RetireUiFont(ui, oldTitleFont);
    RetireUiFont(ui, oldCardTitleFont);
    RetireUiFont(ui, oldStateFont);
    RetireUiFont(ui, oldBadgeFont);
    RetireUiFont(ui, oldButtonFont);
    RetireUiFont(ui, oldAuthorFont);
    ReapRetiredFonts(ui);
}

int MeasureTextWidth(HDC dc, const std::wstring& text) {
    SIZE size = {};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    return size.cx;
}

enum class CardIdentityIcon { UpdateShield, Windows };

void DrawUpdateShield(HDC dc, int centerX, int centerY, int size,
    StatusVisual visual) {
    const COLORREF color = PaletteFor(visual).foreground;
    Gdiplus::Graphics graphics(dc);
    ConfigureIconGraphics(graphics);
    const Gdiplus::REAL iconSize = static_cast<Gdiplus::REAL>(size);
    const Gdiplus::REAL cx = static_cast<Gdiplus::REAL>(centerX);
    const Gdiplus::REAL cy = static_cast<Gdiplus::REAL>(centerY);
    const Gdiplus::PointF shield[8] = {
        Gdiplus::PointF(cx, cy - iconSize * 0.50f),
        Gdiplus::PointF(cx + iconSize * 0.46f, cy - iconSize * 0.31f),
        Gdiplus::PointF(cx + iconSize * 0.40f, cy + iconSize * 0.17f),
        Gdiplus::PointF(cx + iconSize * 0.22f, cy + iconSize * 0.39f),
        Gdiplus::PointF(cx, cy + iconSize * 0.52f),
        Gdiplus::PointF(cx - iconSize * 0.22f, cy + iconSize * 0.39f),
        Gdiplus::PointF(cx - iconSize * 0.40f, cy + iconSize * 0.17f),
        Gdiplus::PointF(cx - iconSize * 0.46f, cy - iconSize * 0.31f)};
    Gdiplus::GraphicsPath path;
    path.AddClosedCurve(shield, static_cast<INT>(std::size(shield)), 0.14f);
    Gdiplus::SolidBrush brush(GdiplusColor(color));
    graphics.FillPath(&brush, &path);

    const Gdiplus::REAL stroke = std::max(2.2f, iconSize * 0.085f);
    Gdiplus::Pen checkPen(Gdiplus::Color(255, 255, 255), stroke);
    checkPen.SetStartCap(Gdiplus::LineCapRound);
    checkPen.SetEndCap(Gdiplus::LineCapRound);
    checkPen.SetLineJoin(Gdiplus::LineJoinRound);
    const Gdiplus::PointF check[3] = {
        Gdiplus::PointF(cx - iconSize * 0.23f, cy + iconSize * 0.01f),
        Gdiplus::PointF(cx - iconSize * 0.05f, cy + iconSize * 0.19f),
        Gdiplus::PointF(cx + iconSize * 0.28f, cy - iconSize * 0.20f)};
    graphics.DrawLines(&checkPen, check, static_cast<INT>(std::size(check)));
}

void DrawWindowsMark(HDC dc, int centerX, int centerY, int size) {
    Gdiplus::Graphics graphics(dc);
    ConfigureIconGraphics(graphics);
    const int gap = std::max(2, size / 9);
    const int tile = (size - gap) / 2;
    const int markSize = tile * 2 + gap;
    const int left = centerX - markSize / 2;
    const int top = centerY - markSize / 2;
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, 0, 120, 215));
    graphics.FillRectangle(&brush, left, top, tile, tile);
    graphics.FillRectangle(&brush, left + tile + gap, top, tile, tile);
    graphics.FillRectangle(&brush, left, top + tile + gap, tile, tile);
    graphics.FillRectangle(&brush, left + tile + gap, top + tile + gap, tile, tile);
}

void DrawCardHeader(HDC dc, const UiContext& ui, const RECT& bounds,
    const std::wstring& prefix, const std::wstring& state, StatusVisual visual,
    CardIdentityIcon identity) {
    const int pad = ScaleForDpi(10, ui.dpi);
    const int identitySize = ScaleForDpi(36, ui.dpi);
    const int statusIconSize = ScaleForDpi(22, ui.dpi);
    const int centerY = (bounds.top + bounds.bottom) / 2;
    const int identityX = bounds.left + pad + identitySize / 2;
    if (identity == CardIdentityIcon::UpdateShield) {
        DrawUpdateShield(dc, identityX, centerY, identitySize, StatusVisual::Success);
    } else {
        DrawWindowsMark(dc, identityX, centerY, identitySize);
    }

    HGDIOBJ oldFont = SelectObject(dc, ui.cardTitleFont);
    SetBkMode(dc, TRANSPARENT);
    TEXTMETRICW metrics = {};
    GetTextMetricsW(dc, &metrics);
    const int labelY = bounds.top + (bounds.bottom - bounds.top - metrics.tmHeight) / 2;
    int x = identityX + identitySize / 2 + ScaleForDpi(14, ui.dpi);
    SetTextColor(dc, RGB(24, 35, 50));
    TextOutW(dc, x, labelY, prefix.c_str(), static_cast<int>(prefix.size()));
    x += MeasureTextWidth(dc, prefix);

    SelectObject(dc, ui.stateFont);
    GetTextMetricsW(dc, &metrics);
    const int stateY = bounds.top + (bounds.bottom - bounds.top - metrics.tmHeight) / 2;
    const VisualPalette palette = PaletteFor(visual);
    SetTextColor(dc, palette.foreground);
    x += ScaleForDpi(7, ui.dpi);
    TextOutW(dc, x, stateY, state.c_str(), static_cast<int>(state.size()));
    x += MeasureTextWidth(dc, state) + ScaleForDpi(11, ui.dpi) + statusIconSize / 2;
    x = std::min(x, static_cast<int>(bounds.right) - pad - statusIconSize / 2);
    DrawStatusIcon(dc, x, centerY, statusIconSize, visual);
    SelectObject(dc, oldFont);
}

void DrawStatusBadge(HDC dc, const UiContext& ui, const RECT& bounds,
    const std::wstring& text, StatusVisual visual) {
    const VisualPalette palette = PaletteFor(visual);
    const int badgeHeight = static_cast<int>(bounds.bottom - bounds.top);
    FillRoundedBadge(dc, bounds, palette.background, palette.border,
        badgeHeight / 2);
    HGDIOBJ oldFont = SelectObject(dc, ui.badgeFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, palette.foreground);
    const int iconSize = std::min(ScaleForDpi(18, ui.dpi),
        badgeHeight - ScaleForDpi(6, ui.dpi));
    const int gap = ScaleForDpi(8, ui.dpi);
    const int textWidth = MeasureTextWidth(dc, text);
    const int contentWidth = textWidth + gap + iconSize;
    const int contentLeft = bounds.left + ((bounds.right - bounds.left) - contentWidth) / 2;
    const int iconX = contentLeft + textWidth + gap + iconSize / 2;
    RECT textRect = {contentLeft, bounds.top,
        contentLeft + textWidth, bounds.bottom};
    DrawTextW(dc, text.c_str(), -1, &textRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawStatusIcon(dc, iconX, (bounds.top + bounds.bottom) / 2, iconSize, visual);
    SelectObject(dc, oldFont);
}

void DrawSystemChip(HDC dc, const UiContext& ui, const RECT& bounds,
    const std::wstring& label, const std::wstring& value) {
    FillRoundedPanel(dc, bounds, RGB(248, 250, 253), RGB(218, 225, 233),
        ScaleForDpi(8, ui.dpi));
    HGDIOBJ oldFont = SelectObject(dc, ui.normalFont);
    SetBkMode(dc, TRANSPARENT);
    const int x = bounds.left + ScaleForDpi(12, ui.dpi);
    RECT textRect = {x, bounds.top, bounds.right - ScaleForDpi(10, ui.dpi), bounds.bottom};
    SetTextColor(dc, RGB(91, 103, 117));
    DrawTextW(dc, label.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    textRect.left += MeasureTextWidth(dc, label) + ScaleForDpi(5, ui.dpi);
    SetTextColor(dc, RGB(31, 45, 62));
    DrawTextW(dc, value.c_str(), -1, &textRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

void DrawSystemInfo(const DRAWITEMSTRUCT& item, const UiContext& ui) {
    const SystemDisplayInfo info = BuildSystemDisplayInfo(ui.version, ui.controller->DisplayName());
    const RECT client = item.rcItem;
    FillRectColor(item.hDC, client, RGB(247, 249, 252));
    const int top = ScaleForDpi(2, ui.dpi);
    const int bottom = client.bottom - ScaleForDpi(2, ui.dpi);
    const int gap = ScaleForDpi(10, ui.dpi);
    const int firstWidth = ScaleForDpi(246, ui.dpi);
    const int secondWidth = ScaleForDpi(220, ui.dpi);
    const int thirdWidth = ScaleForDpi(206, ui.dpi);
    RECT first = {0, top, firstWidth, bottom};
    RECT second = {first.right + gap, top, first.right + gap + secondWidth, bottom};
    RECT third = {second.right + gap, top, second.right + gap + thirdWidth, bottom};
    DrawSystemChip(item.hDC, ui, first, L"当前系统：", info.system);
    DrawSystemChip(item.hDC, ui, second, L"功能版本：", info.feature);
    DrawSystemChip(item.hDC, ui, third, L"Build：", info.build);
}

void DrawUpdateStatusCard(const DRAWITEMSTRUCT& item, const UiContext& ui) {
    const StatusVisual visual = UpdateVisual(ui);
    const VisualPalette palette = PaletteFor(visual);
    FillRectColor(item.hDC, item.rcItem, palette.panel);
    RECT header = item.rcItem;
    header.bottom = header.top + ScaleForDpi(52, ui.dpi);
    DrawCardHeader(item.hDC, ui, header, L"Windows 更新状态：",
        UpdateHeaderState(ui), visual, CardIdentityIcon::UpdateShield);

    std::vector<StatusRow> rows = BuildUpdateRows(ui.updateState);
    if (rows.empty()) {
        rows.push_back(StatusRow{ui.updateState.details.empty() ? L"无法读取状态详情" :
            ui.updateState.details.front(), L"", StatusVisual::Error, false});
    }
    if (rows.size() > 5) rows.resize(5);
    RECT body = {item.rcItem.left + ScaleForDpi(6, ui.dpi), header.bottom + ScaleForDpi(2, ui.dpi),
        item.rcItem.right - ScaleForDpi(6, ui.dpi), item.rcItem.bottom};
    FillRoundedPanel(item.hDC, body, RGB(255, 255, 255), RGB(228, 234, 240), ScaleForDpi(8, ui.dpi));
    const int rowHeight = ScaleForDpi(27, ui.dpi);
    const int badgeWidth = ScaleForDpi(124, ui.dpi);
    const int badgeHeight = ScaleForDpi(26, ui.dpi);
    const int rowsHeight = rowHeight * static_cast<int>(rows.size());
    const int rowsTop = body.top + std::max(0,
        (static_cast<int>(body.bottom - body.top) - rowsHeight) / 2);
    const int badgeRight = body.right - ScaleForDpi(30, ui.dpi);
    HGDIOBJ oldFont = SelectObject(item.hDC, ui.normalFont);
    SetBkMode(item.hDC, TRANSPARENT);
    for (size_t i = 0; i < rows.size(); ++i) {
        const int top = rowsTop + static_cast<int>(i) * rowHeight;
        const int bottom = top + rowHeight;
        const int badgeTop = top + (rowHeight - badgeHeight) / 2;
        RECT badge = {badgeRight - badgeWidth, badgeTop,
            badgeRight, badgeTop + badgeHeight};
        RECT labelRect = {body.left + ScaleForDpi(14, ui.dpi), top,
            badge.left - ScaleForDpi(12, ui.dpi), bottom};
        SetTextColor(item.hDC, RGB(35, 49, 66));
        DrawTextW(item.hDC, rows[i].label.c_str(), -1, &labelRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (rows[i].badge) {
            DrawStatusBadge(item.hDC, ui, badge, rows[i].value, rows[i].visual);
        }
    }
    SelectObject(item.hDC, oldFont);
}

void DrawUpgradeStatusCard(const DRAWITEMSTRUCT& item, const UiContext& ui) {
    const StatusVisual visual = UpgradeVisual(ui);
    const VisualPalette palette = PaletteFor(visual);
    FillRectColor(item.hDC, item.rcItem, palette.panel);
    RECT header = item.rcItem;
    header.bottom = header.top + ScaleForDpi(52, ui.dpi);
    DrawCardHeader(item.hDC, ui, header, L"Windows 11 升级状态：",
        UpgradeHeaderState(ui), visual, CardIdentityIcon::Windows);

    std::vector<StatusRow> rows = BuildUpgradeRows(ui);
    if (rows.empty()) rows.push_back(StatusRow{L"暂无状态详情", L"", StatusVisual::Neutral, false});
    if (rows.size() > 4) rows.resize(4);
    RECT body = {item.rcItem.left + ScaleForDpi(6, ui.dpi), header.bottom + ScaleForDpi(2, ui.dpi),
        item.rcItem.right - ScaleForDpi(6, ui.dpi), item.rcItem.bottom};
    FillRoundedPanel(item.hDC, body, RGB(255, 255, 255), RGB(228, 234, 240), ScaleForDpi(8, ui.dpi));
    const int rowHeight = std::min(ScaleForDpi(28, ui.dpi),
        (static_cast<int>(body.bottom - body.top) - ScaleForDpi(6, ui.dpi)) /
        static_cast<int>(rows.size()));
    const int badgeHeight = ScaleForDpi(24, ui.dpi);
    const int rowsHeight = rowHeight * static_cast<int>(rows.size());
    const int rowsTop = body.top + std::max(0,
        (static_cast<int>(body.bottom - body.top) - rowsHeight) / 2);
    const int badgeRight = body.right - ScaleForDpi(30, ui.dpi);
    HGDIOBJ measureFont = SelectObject(item.hDC, ui.badgeFont);
    int badgeWidth = ScaleForDpi(124, ui.dpi);
    for (const StatusRow& row : rows) {
        if (row.badge) badgeWidth = std::max(badgeWidth,
            MeasureTextWidth(item.hDC, row.value) + ScaleForDpi(46, ui.dpi));
    }
    SelectObject(item.hDC, measureFont);
    const int valueLeft = badgeRight - badgeWidth + ScaleForDpi(10, ui.dpi);
    HGDIOBJ oldFont = SelectObject(item.hDC, ui.normalFont);
    SetBkMode(item.hDC, TRANSPARENT);
    for (size_t i = 0; i < rows.size(); ++i) {
        const int top = rowsTop + static_cast<int>(i) * rowHeight;
        const int bottom = top + rowHeight;
        if (rows[i].value.empty()) {
            RECT lineRect = {body.left + ScaleForDpi(14, ui.dpi), top,
                body.right - ScaleForDpi(14, ui.dpi), bottom};
            SetTextColor(item.hDC, rows[i].visual == StatusVisual::Error ?
                PaletteFor(StatusVisual::Error).foreground : RGB(74, 88, 104));
            DrawTextW(item.hDC, rows[i].label.c_str(), -1, &lineRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            RECT labelRect = {body.left + ScaleForDpi(14, ui.dpi), top,
                body.left + ScaleForDpi(145, ui.dpi), bottom};
            SetTextColor(item.hDC, RGB(75, 88, 104));
            DrawTextW(item.hDC, rows[i].label.c_str(), -1, &labelRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (rows[i].badge) {
                const int badgeTop = top + (rowHeight - badgeHeight) / 2;
                RECT badge = {badgeRight - badgeWidth, badgeTop,
                    badgeRight, badgeTop + badgeHeight};
                DrawStatusBadge(item.hDC, ui, badge, rows[i].value, rows[i].visual);
            } else {
                RECT valueRect = {valueLeft, top,
                    body.right - ScaleForDpi(12, ui.dpi), bottom};
                SetTextColor(item.hDC, RGB(31, 45, 62));
                DrawTextW(item.hDC, rows[i].value.c_str(), -1, &valueRect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
    }
    SelectObject(item.hDC, oldFont);
}

const wchar_t* const kUiWarningText =
    L"注意：\r\n"
    L"1. 关闭后将禁用自动更新、更新通知、驱动更新和手动更新入口。\r\n"
    L"2. 如需更新系统，请先点击“恢复到运行前状态”。\r\n"
    L"3. 禁止升级到 Windows 11 仅对 Windows 10 生效。\r\n"
    L"4. 本工具不修改 TPM、Secure Boot 或硬件兼容性检查。";

void DrawWarningCard(const DRAWITEMSTRUCT& item, const UiContext& ui) {
    FillRoundedPanel(item.hDC, item.rcItem, RGB(255, 249, 235), RGB(235, 196, 102),
        ScaleForDpi(9, ui.dpi));
    const int left = item.rcItem.left + ScaleForDpi(18, ui.dpi);
    const int titleTop = item.rcItem.top + ScaleForDpi(10, ui.dpi);
    DrawStatusIcon(item.hDC, left + ScaleForDpi(9, ui.dpi), titleTop + ScaleForDpi(10, ui.dpi),
        ScaleForDpi(18, ui.dpi), StatusVisual::Warning);
    HGDIOBJ oldFont = SelectObject(item.hDC, ui.badgeFont);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, RGB(164, 87, 0));
    RECT titleRect = {left + ScaleForDpi(27, ui.dpi), titleTop,
        item.rcItem.right - ScaleForDpi(14, ui.dpi), titleTop + ScaleForDpi(22, ui.dpi)};
    DrawTextW(item.hDC, L"注意：", -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, ui.normalFont);
    SetTextColor(item.hDC, RGB(91, 67, 25));
    const std::array<const wchar_t*, 4> lines = {{
        L"1. 关闭后将禁用自动更新、更新通知、驱动更新和手动更新入口。",
        L"2. 如需更新系统，请先点击“恢复到运行前状态”。",
        L"3. 禁止升级到 Windows 11 仅对 Windows 10 生效。",
        L"4. 本工具不修改 TPM、Secure Boot 或硬件兼容性检查。"}};
    for (size_t i = 0; i < lines.size(); ++i) {
        const int top = item.rcItem.top + ScaleForDpi(33 + static_cast<int>(i) * 22, ui.dpi);
        RECT lineRect = {left + ScaleForDpi(27, ui.dpi), top,
            item.rcItem.right - ScaleForDpi(14, ui.dpi), top + ScaleForDpi(22, ui.dpi)};
        DrawTextW(item.hDC, lines[i], -1, &lineRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    SelectObject(item.hDC, oldFont);
}

bool DrawOwnerItemContent(const DRAWITEMSTRUCT& item, const UiContext& ui) {
    switch (item.CtlID) {
    case IDC_DISABLE:
    case IDC_RESTORE:
    case IDC_UPGRADE_DISABLE:
    case IDC_UPGRADE_RESTORE:
        DrawOwnerButton(item);
        return true;
    case IDC_SYSTEM:
        DrawSystemInfo(item, ui);
        return true;
    case IDC_STATUS:
        DrawUpdateStatusCard(item, ui);
        return true;
    case IDC_UPGRADE_STATUS:
        DrawUpgradeStatusCard(item, ui);
        return true;
    case IDC_WARNING:
        DrawWarningCard(item, ui);
        return true;
    default:
        return false;
    }
}

COLORREF OwnerItemBackground(const DRAWITEMSTRUCT& item, const UiContext& ui) {
    if (item.CtlID == IDC_DISABLE || item.CtlID == IDC_RESTORE) {
        return PaletteFor(UpdateVisual(ui)).panel;
    }
    if (item.CtlID == IDC_UPGRADE_DISABLE || item.CtlID == IDC_UPGRADE_RESTORE) {
        return PaletteFor(UpgradeVisual(ui)).panel;
    }
    return RGB(247, 249, 252);
}

bool DrawOwnerItemBuffered(const DRAWITEMSTRUCT& item, const UiContext& ui) {
    const int width = item.rcItem.right - item.rcItem.left;
    const int height = item.rcItem.bottom - item.rcItem.top;
    if (width <= 0 || height <= 0) return false;
    HDC bufferDc = CreateCompatibleDC(item.hDC);
    HBITMAP bufferBitmap = bufferDc ? CreateCompatibleBitmap(item.hDC, width, height) : nullptr;
    if (!bufferDc || !bufferBitmap) {
        if (bufferBitmap) DeleteObject(bufferBitmap);
        if (bufferDc) DeleteDC(bufferDc);
        return DrawOwnerItemContent(item, ui);
    }
    HGDIOBJ oldBitmap = SelectObject(bufferDc, bufferBitmap);
    RECT bufferRect = {0, 0, width, height};
    FillRectColor(bufferDc, bufferRect, OwnerItemBackground(item, ui));
    DRAWITEMSTRUCT bufferedItem = item;
    bufferedItem.hDC = bufferDc;
    bufferedItem.rcItem = bufferRect;
    const bool handled = DrawOwnerItemContent(bufferedItem, ui);
    if (handled) {
        BitBlt(item.hDC, item.rcItem.left, item.rcItem.top, width, height,
            bufferDc, 0, 0, SRCCOPY);
    }
    SelectObject(bufferDc, oldBitmap);
    DeleteObject(bufferBitmap);
    DeleteDC(bufferDc);
    return handled;
}

void MoveChildToRect(HWND child, const RECT& rect) {
    if (!child) return;
    SetWindowPos(child, nullptr, rect.left, rect.top,
        rect.right - rect.left, rect.bottom - rect.top,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
}

struct ChildPlacement {
    HWND window = nullptr;
    RECT rect{};
};

void MoveChildren(const std::array<ChildPlacement, 11>& placements) {
    HDWP deferred = BeginDeferWindowPos(static_cast<int>(placements.size()));
    bool batchFailed = deferred == nullptr;
    if (!batchFailed) {
        for (const ChildPlacement& placement : placements) {
            if (!placement.window) continue;
            HDWP next = DeferWindowPos(deferred, placement.window, nullptr,
                placement.rect.left, placement.rect.top,
                placement.rect.right - placement.rect.left,
                placement.rect.bottom - placement.rect.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
            if (!next) {
                batchFailed = true;
                break;
            }
            deferred = next;
        }
    }
    if (!batchFailed) {
        if (EndDeferWindowPos(deferred)) return;
        deferred = nullptr;
    } else if (deferred) {
        EndDeferWindowPos(deferred);
    }
    for (const ChildPlacement& placement : placements) {
        MoveChildToRect(placement.window, placement.rect);
    }
}

void LayoutControls(HWND window, UiContext& ui, bool updateRenderDpi = true) {
    if (ui.layingOut) return;
    ui.layingOut = true;

    RECT client = {};
    GetClientRect(window, &client);
    int clientWidth = std::max(0, static_cast<int>(client.right - client.left));
    int clientHeight = std::max(0, static_cast<int>(client.bottom - client.top));
    int renderDpi = ui.dpi;
    bool needsScrollbar = ui.scrollBarVisible;
    if (updateRenderDpi) {
        for (int attempt = 0; attempt < 4; ++attempt) {
            GetClientRect(window, &client);
            clientWidth = std::max(0, static_cast<int>(client.right - client.left));
            clientHeight = std::max(0, static_cast<int>(client.bottom - client.top));
            renderDpi = ChooseRenderDpi(ui.baseDpi, clientWidth, clientHeight, true);
            needsScrollbar = ScaleForDpi(kUiClientHeight, renderDpi) > clientHeight;
            if (needsScrollbar == ui.scrollBarVisible) break;
            ui.scrollBarVisible = needsScrollbar;
            ShowScrollBar(window, SB_VERT, needsScrollbar ? TRUE : FALSE);
        }
        ShowScrollBar(window, SB_VERT, ui.scrollBarVisible ? TRUE : FALSE);
        GetClientRect(window, &client);
        clientWidth = std::max(0, static_cast<int>(client.right - client.left));
        clientHeight = std::max(0, static_cast<int>(client.bottom - client.top));
        renderDpi = ChooseRenderDpi(ui.baseDpi, clientWidth, clientHeight, true);
    }

    if (renderDpi != ui.dpi) {
        ui.dpi = renderDpi;
    }
    if (ui.fontDpi != ui.dpi && !ui.inSizeMove) {
        CreateUiFonts(ui, ui.dpi);
        ui.fontDpi = ui.dpi;
    }
    const int contentWidth = ScaleForDpi(kUiClientWidth, ui.dpi);
    const int contentHeight = ScaleForDpi(kUiClientHeight, ui.dpi);
    const int maxScroll = std::max(0, contentHeight - clientHeight);
    ui.scrollOffset = std::max(0, std::min(ui.scrollOffset, maxScroll));
    ui.contentOriginX = std::max(0, (clientWidth - contentWidth) / 2);

    SCROLLINFO scrollInfo = {sizeof(scrollInfo)};
    scrollInfo.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scrollInfo.nMin = 0;
    scrollInfo.nMax = std::max(0, contentHeight - 1);
    scrollInfo.nPage = static_cast<UINT>(clientHeight);
    scrollInfo.nPos = ui.scrollOffset;
    SetScrollInfo(window, SB_VERT, &scrollInfo, TRUE);

    UiLayout layout = BuildUiLayout(ui.dpi);
    OffsetUiLayout(layout, ui.contentOriginX, -ui.scrollOffset);
    MoveChildren({{
        {ui.title, layout.title},
        {ui.author, layout.author},
        {ui.system, layout.system},
        {ui.description, layout.description},
        {ui.status, layout.updateCard},
        {ui.disableButton, layout.disableButton},
        {ui.restoreButton, layout.restoreButton},
        {ui.upgradeStatus, layout.upgradeCard},
        {ui.upgradeDisableButton, layout.upgradeDisableButton},
        {ui.upgradeRestoreButton, layout.upgradeRestoreButton},
        {ui.warning, layout.warning}
    }});
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    ui.layingOut = false;
}

void ScrollTo(HWND window, UiContext& ui, int position) {
    RECT client = {};
    GetClientRect(window, &client);
    const int contentHeight = ScaleForDpi(kUiClientHeight, ui.dpi);
    const int maxScroll = std::max(0, contentHeight -
        static_cast<int>(client.bottom - client.top));
    const int clamped = std::max(0, std::min(position, maxScroll));
    if (clamped == ui.scrollOffset) return;
    ui.scrollOffset = clamped;
    LayoutControls(window, ui);
    InvalidateRect(window, nullptr, FALSE);
}

std::wstring BuildAccessibleStatusText(const std::wstring& header,
    const std::vector<StatusRow>& rows) {
    std::wstring result = header;
    for (const StatusRow& row : rows) {
        result += L"；" + row.label;
        if (!row.value.empty()) result += L"：" + row.value;
    }
    return result;
}

void RefreshUi(HWND window, UiContext& ui) {
    try {
        ui.updateState = ui.controller->GetStatus();
        ui.updateReadFailed = false;
    } catch (const AppError& error) {
        ui.updateState = UpdateStatus{StatusLevel::Enabled, L"状态读取失败", {error.message}};
        ui.updateReadFailed = true;
    }
    EnableWindow(ui.restoreButton, ui.controller->CanRestore() ? TRUE : FALSE);
    SetWindowTextW(ui.status, BuildAccessibleStatusText(
        L"Windows 更新状态：" + UpdateHeaderState(ui), BuildUpdateRows(ui.updateState)).c_str());

    try {
        ui.upgradeState = ui.upgradeBlocker.GetStatus(ui.version);
        ui.upgradeReadFailed = false;
    } catch (const AppError& error) {
        ui.upgradeState = UpdateStatus{StatusLevel::Enabled, L"Windows 11 升级状态：读取失败", {error.message}};
        ui.upgradeReadFailed = true;
    }
    EnableWindow(ui.upgradeDisableButton, IsWindows10Desktop(ui.version) ? TRUE : FALSE);
    EnableWindow(ui.upgradeRestoreButton, ui.upgradeBlocker.CanRestore(ui.version) ? TRUE : FALSE);
    SetWindowTextW(ui.upgradeStatus, BuildAccessibleStatusText(
        L"Windows 11 升级状态：" + UpgradeHeaderState(ui), BuildUpgradeRows(ui)).c_str());
    InvalidateRect(ui.status, nullptr, FALSE);
    InvalidateRect(ui.upgradeStatus, nullptr, FALSE);
    InvalidateRect(window, nullptr, FALSE);
}

void ShowOperationResult(HWND owner, const OperationResult& result) {
    const UINT icon = result.outcome == OperationOutcome::Success ? MB_ICONINFORMATION :
                      result.outcome == OperationOutcome::PartialSuccess ? MB_ICONWARNING : MB_ICONERROR;
    const wchar_t* title = result.outcome == OperationOutcome::Success ? L"操作完成" :
                           result.outcome == OperationOutcome::PartialSuccess ? L"操作部分完成" : L"操作失败";
    MessageBoxW(owner, result.message.c_str(), title, MB_OK | icon);
}

LRESULT WindowProcImpl(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    UiContext* ui = reinterpret_cast<UiContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ui = reinterpret_cast<UiContext*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
        HDC windowDc = GetDC(window);
        if (!windowDc) ThrowWin32(L"读取窗口 DPI");
        const int dpi = GetDeviceCaps(windowDc, LOGPIXELSY);
        ReleaseDC(window, windowDc);
        ui->baseDpi = dpi;
        ui->dpi = dpi;
        ui->backgroundBrush = CreateSolidBrush(RGB(247, 249, 252));
        if (!ui->backgroundBrush) ThrowWin32(L"创建界面画刷");
        const UiLayout layout = BuildUiLayout(dpi);
        CreateUiFonts(*ui, dpi);
        ui->fontDpi = dpi;

        const auto width = [](const RECT& rect) { return rect.right - rect.left; };
        const auto height = [](const RECT& rect) { return rect.bottom - rect.top; };
        ui->title = CreateWindowExW(0, L"STATIC", kAppName, WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
            layout.title.left, layout.title.top, width(layout.title), height(layout.title),
            window, nullptr, nullptr, nullptr);
        ui->author = CreateWindowExW(0, L"STATIC", L"作者：啊常用户",
            WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_NOPREFIX,
            layout.author.left, layout.author.top, width(layout.author), height(layout.author),
            window, nullptr, nullptr, nullptr);
        ui->system = CreateWindowExW(0, L"STATIC",
            BuildSystemLabel(ui->version, ui->controller->DisplayName()).c_str(),
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            layout.system.left, layout.system.top, width(layout.system), height(layout.system),
            window, reinterpret_cast<HMENU>(IDC_SYSTEM), nullptr, nullptr);
        ui->description = CreateWindowExW(0, L"STATIC", ui->controller->Description().c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            layout.description.left, layout.description.top, width(layout.description), height(layout.description),
            window, reinterpret_cast<HMENU>(IDC_DESCRIPTION), nullptr, nullptr);
        ui->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            layout.updateCard.left, layout.updateCard.top, width(layout.updateCard), height(layout.updateCard),
            window, reinterpret_cast<HMENU>(IDC_STATUS), nullptr, nullptr);
        ui->disableButton = CreateWindowExW(0, L"BUTTON", ui->controller->DisableButtonText().c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            layout.disableButton.left, layout.disableButton.top,
            width(layout.disableButton), height(layout.disableButton),
            window, reinterpret_cast<HMENU>(IDC_DISABLE), nullptr, nullptr);
        ui->restoreButton = CreateWindowExW(0, L"BUTTON", L"恢复到运行本软件前的状态",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            layout.restoreButton.left, layout.restoreButton.top,
            width(layout.restoreButton), height(layout.restoreButton),
            window, reinterpret_cast<HMENU>(IDC_RESTORE), nullptr, nullptr);
        ui->upgradeStatus = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            layout.upgradeCard.left, layout.upgradeCard.top,
            width(layout.upgradeCard), height(layout.upgradeCard),
            window, reinterpret_cast<HMENU>(IDC_UPGRADE_STATUS), nullptr, nullptr);
        ui->upgradeDisableButton = CreateWindowExW(0, L"BUTTON", ui->upgradeBlocker.BlockButtonText().c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            layout.upgradeDisableButton.left, layout.upgradeDisableButton.top,
            width(layout.upgradeDisableButton), height(layout.upgradeDisableButton),
            window, reinterpret_cast<HMENU>(IDC_UPGRADE_DISABLE), nullptr, nullptr);
        ui->upgradeRestoreButton = CreateWindowExW(0, L"BUTTON", ui->upgradeBlocker.RestoreButtonText().c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            layout.upgradeRestoreButton.left, layout.upgradeRestoreButton.top,
            width(layout.upgradeRestoreButton), height(layout.upgradeRestoreButton),
            window, reinterpret_cast<HMENU>(IDC_UPGRADE_RESTORE), nullptr, nullptr);
        ui->warning = CreateWindowExW(0, L"STATIC", kUiWarningText,
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            layout.warning.left, layout.warning.top, width(layout.warning), height(layout.warning),
            window, reinterpret_cast<HMENU>(IDC_WARNING), nullptr, nullptr);
        for (HWND control : {ui->system, ui->description, ui->status,
             ui->disableButton, ui->restoreButton, ui->upgradeStatus,
             ui->upgradeDisableButton, ui->upgradeRestoreButton, ui->warning}) {
            SetControlFont(control, ui->normalFont);
        }
        SetControlFont(ui->title, ui->titleFont);
        SetControlFont(ui->author, ui->authorFont);
        for (HWND button : {ui->disableButton, ui->restoreButton,
             ui->upgradeDisableButton, ui->upgradeRestoreButton}) {
            SetControlFont(button, ui->buttonFont);
        }
        LayoutControls(window, *ui);
        RefreshUi(window, *ui);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(window, &paint);
        RECT client = {};
        GetClientRect(window, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        HDC bufferDc = CreateCompatibleDC(dc);
        HBITMAP bufferBitmap = bufferDc ? CreateCompatibleBitmap(dc, width, height) : nullptr;
        if (bufferDc && bufferBitmap) {
            HGDIOBJ oldBitmap = SelectObject(bufferDc, bufferBitmap);
            FillRect(bufferDc, &client, ui && ui->backgroundBrush ? ui->backgroundBrush :
                reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            if (ui) {
                UiLayout layout = BuildUiLayout(ui->dpi);
                OffsetUiLayout(layout, ui->contentOriginX, -ui->scrollOffset);
                const VisualPalette updatePalette = PaletteFor(UpdateVisual(*ui));
                const VisualPalette upgradePalette = PaletteFor(UpgradeVisual(*ui));
                FillRoundedPanel(bufferDc, layout.updatePanel, updatePalette.panel,
                    updatePalette.border, ScaleForDpi(10, ui->dpi));
                FillRoundedPanel(bufferDc, layout.upgradePanel, upgradePalette.panel,
                    upgradePalette.border, ScaleForDpi(10, ui->dpi));
            }
            BitBlt(dc, 0, 0, width, height, bufferDc, 0, 0, SRCCOPY);
            SelectObject(bufferDc, oldBitmap);
        } else {
            FillRect(dc, &client, ui && ui->backgroundBrush ? ui->backgroundBrush :
                reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            if (ui) {
                UiLayout layout = BuildUiLayout(ui->dpi);
                OffsetUiLayout(layout, ui->contentOriginX, -ui->scrollOffset);
                const VisualPalette updatePalette = PaletteFor(UpdateVisual(*ui));
                const VisualPalette upgradePalette = PaletteFor(UpgradeVisual(*ui));
                FillRoundedPanel(dc, layout.updatePanel, updatePalette.panel,
                    updatePalette.border, ScaleForDpi(10, ui->dpi));
                FillRoundedPanel(dc, layout.upgradePanel, upgradePalette.panel,
                    upgradePalette.border, ScaleForDpi(10, ui->dpi));
            }
        }
        if (bufferBitmap) DeleteObject(bufferBitmap);
        if (bufferDc) DeleteDC(bufferDc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_GETMINMAXINFO:
        if (ui) {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            const int minimumDpi = MinimumRenderDpi(ui->baseDpi);
            RECT minimum = {0, 0, ScaleForDpi(kUiMinimumClientWidth, minimumDpi),
                ScaleForDpi(kUiMinimumClientHeight, minimumDpi)};
            AdjustWindowRectEx(&minimum, WS_OVERLAPPEDWINDOW | WS_VSCROLL, FALSE, 0);
            limits->ptMinTrackSize.x = minimum.right - minimum.left;
            limits->ptMinTrackSize.y = minimum.bottom - minimum.top;
            return 0;
        }
        break;
    case WM_SIZE:
        if (ui) {
            LayoutControls(window, *ui, true);
            return 0;
        }
        break;
    case WM_ENTERSIZEMOVE:
        if (ui) {
            ui->inSizeMove = true;
            return 0;
        }
        break;
    case WM_EXITSIZEMOVE:
        if (ui) {
            ui->inSizeMove = false;
            LayoutControls(window, *ui, true);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_VSCROLL:
        if (ui) {
            SCROLLINFO scrollInfo = {sizeof(scrollInfo)};
            scrollInfo.fMask = SIF_ALL;
            GetScrollInfo(window, SB_VERT, &scrollInfo);
            int position = scrollInfo.nPos;
            const int line = ScaleForDpi(40, ui->dpi);
            switch (LOWORD(wParam)) {
            case SB_LINEUP: position -= line; break;
            case SB_LINEDOWN: position += line; break;
            case SB_PAGEUP: position -= static_cast<int>(scrollInfo.nPage); break;
            case SB_PAGEDOWN: position += static_cast<int>(scrollInfo.nPage); break;
            case SB_THUMBPOSITION:
            case SB_THUMBTRACK: position = scrollInfo.nTrackPos; break;
            case SB_TOP: position = scrollInfo.nMin; break;
            case SB_BOTTOM: position = scrollInfo.nMax - static_cast<int>(scrollInfo.nPage) + 1; break;
            default: return 0;
            }
            ScrollTo(window, *ui, position);
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        if (ui) {
            const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (wheelDelta != 0) {
                ScrollTo(window, *ui, ui->scrollOffset -
                    (wheelDelta / WHEEL_DELTA) * ScaleForDpi(64, ui->dpi));
            }
            return 0;
        }
        break;
    case WM_COMMAND:
        if (!ui) break;
        if (LOWORD(wParam) == IDC_DISABLE && HIWORD(wParam) == BN_CLICKED) {
            const std::wstring prompt = ui->controller->Confirmation() + L"\r\n\r\n是否继续？";
            if (MessageBoxW(window, prompt.c_str(), L"确认关闭更新", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return 0;
            EnableWindow(ui->disableButton, FALSE); EnableWindow(ui->restoreButton, FALSE);
            EnableWindow(ui->upgradeDisableButton, FALSE); EnableWindow(ui->upgradeRestoreButton, FALSE);
            try {
                const OperationResult result = ui->controller->Disable();
                ShowOperationResult(window, result);
            } catch (...) {
                MessageBoxW(window, L"发生未预期错误。操作未能完成，请重新检查当前状态。", L"操作失败", MB_OK | MB_ICONERROR);
            }
            RefreshUi(window, *ui);
            EnableWindow(ui->disableButton, TRUE); return 0;
        }
        if (LOWORD(wParam) == IDC_RESTORE && HIWORD(wParam) == BN_CLICKED) {
            if (MessageBoxW(window, L"这会恢复本软件第一次修改前保存的原始状态。\r\n\r\n是否继续？",
                L"确认恢复", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return 0;
            EnableWindow(ui->disableButton, FALSE); EnableWindow(ui->restoreButton, FALSE);
            EnableWindow(ui->upgradeDisableButton, FALSE); EnableWindow(ui->upgradeRestoreButton, FALSE);
            try {
                const OperationResult result = ui->controller->Restore();
                ShowOperationResult(window, result);
            } catch (...) {
                MessageBoxW(window, L"发生未预期错误。恢复未能完成，请重新检查当前状态。", L"操作失败", MB_OK | MB_ICONERROR);
            }
            RefreshUi(window, *ui);
            EnableWindow(ui->disableButton, TRUE); return 0;
        }
        if (LOWORD(wParam) == IDC_UPGRADE_DISABLE && HIWORD(wParam) == BN_CLICKED) {
            const std::wstring prompt = ui->upgradeBlocker.Confirmation() + L"\r\n\r\n是否继续？";
            if (MessageBoxW(window, prompt.c_str(), L"确认禁止升级到 Windows 11",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return 0;
            EnableWindow(ui->disableButton, FALSE); EnableWindow(ui->restoreButton, FALSE);
            EnableWindow(ui->upgradeDisableButton, FALSE); EnableWindow(ui->upgradeRestoreButton, FALSE);
            try {
                const OperationResult result = ui->upgradeBlocker.Block(ui->version);
                ShowOperationResult(window, result);
            } catch (...) {
                MessageBoxW(window, L"发生未预期错误。操作未能完成，请重新检查当前状态。",
                    L"操作失败", MB_OK | MB_ICONERROR);
            }
            RefreshUi(window, *ui);
            EnableWindow(ui->disableButton, TRUE); return 0;
        }
        if (LOWORD(wParam) == IDC_UPGRADE_RESTORE && HIWORD(wParam) == BN_CLICKED) {
            if (MessageBoxW(window,
                L"这会恢复 Windows 11 升级策略到本软件第一次修改前的原始状态。\r\n\r\n是否继续？",
                L"确认恢复 Windows 11 升级允许状态",
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return 0;
            EnableWindow(ui->disableButton, FALSE); EnableWindow(ui->restoreButton, FALSE);
            EnableWindow(ui->upgradeDisableButton, FALSE); EnableWindow(ui->upgradeRestoreButton, FALSE);
            try {
                const OperationResult result = ui->upgradeBlocker.Restore(ui->version);
                ShowOperationResult(window, result);
            } catch (...) {
                MessageBoxW(window, L"发生未预期错误。恢复未能完成，请重新检查当前状态。",
                    L"操作失败", MB_OK | MB_ICONERROR);
            }
            RefreshUi(window, *ui);
            EnableWindow(ui->disableButton, TRUE); return 0;
        }
        break;
    case WM_DRAWITEM:
        if (ui) {
            const DRAWITEMSTRUCT& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (DrawOwnerItemBuffered(item, *ui)) return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if (ui) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, control == ui->author ? RGB(105, 116, 129) :
                control == ui->description ? RGB(73, 86, 101) : RGB(24, 35, 50));
            SetBkColor(dc, RGB(247, 249, 252));
            return reinterpret_cast<LRESULT>(ui->backgroundBrush);
        }
        break;
    case WM_DESTROY:
        if (ui) {
            if (ui->normalFont) DeleteObject(ui->normalFont);
            if (ui->titleFont) DeleteObject(ui->titleFont);
            if (ui->cardTitleFont) DeleteObject(ui->cardTitleFont);
            if (ui->stateFont) DeleteObject(ui->stateFont);
            if (ui->badgeFont) DeleteObject(ui->badgeFont);
            if (ui->buttonFont) DeleteObject(ui->buttonFont);
            if (ui->authorFont) DeleteObject(ui->authorFont);
            ReapRetiredFonts(*ui);
            for (HFONT font : ui->retiredFonts) DeleteObject(font);
            ui->retiredFonts.clear();
            if (ui->backgroundBrush) DeleteObject(ui->backgroundBrush);
        }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    try {
        return WindowProcImpl(window, message, wParam, lParam);
    } catch (const AppError& error) {
        MessageBoxW(window, error.message.c_str(), kAppName, MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxW(window, L"窗口处理发生未预期错误。系统设置不会被继续修改。",
            kAppName, MB_OK | MB_ICONERROR);
    }
    return message == WM_CREATE ? static_cast<LRESULT>(-1) : 0;
}

std::unique_ptr<IUpdateController> CreateController(const VersionInfo& version) {
    if (version.productType == VER_NT_WORKSTATION && version.major == 6 && version.minor == 1) {
        return std::unique_ptr<IUpdateController>(new Windows7UpdateController());
    }
    if (IsWindowsServer2016(version)) {
        return std::unique_ptr<IUpdateController>(new ModernUpdateController(L"Windows Server 2016", false));
    }
    if (IsWindows11Desktop(version)) {
        return std::unique_ptr<IUpdateController>(new ModernUpdateController(L"Windows 11"));
    }
    if (IsWindows10Desktop(version)) {
        return std::unique_ptr<IUpdateController>(new ModernUpdateController(L"Windows 10"));
    }
    throw AppError(L"本工具仅支持 Windows 7、Windows 10、Windows 11 和 Windows Server 2016。当前系统不会被修改。" );
}

} // namespace

#ifndef UPDATELOCK_TEST
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        MessageBoxW(nullptr, L"无法初始化 Windows 组件。", kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }
    PSECURITY_DESCRIPTOR mutexDescriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &mutexDescriptor, nullptr)) {
        MessageBoxW(nullptr, L"无法创建跨会话锁权限。系统不会被修改。", kAppName, MB_OK | MB_ICONERROR);
        CoUninitialize(); return 1;
    }
    SECURITY_ATTRIBUTES mutexSecurity = {sizeof(mutexSecurity), mutexDescriptor, FALSE};
    ScopedHandle mutex(CreateMutexW(&mutexSecurity, TRUE, kMutexName));
    const DWORD mutexError = GetLastError();
    LocalFree(mutexDescriptor);
    if (!mutex.value) {
        MessageBoxW(nullptr, (L"无法建立跨会话操作锁：\r\n" + Win32Message(mutexError)).c_str(), kAppName, MB_OK | MB_ICONERROR);
        CoUninitialize(); return 1;
    }
    if (mutexError == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"更新控制工具已在运行。请关闭另一个窗口后再试。", kAppName, MB_OK | MB_ICONINFORMATION);
        CoUninitialize(); return 0;
    }
    try {
        GdiplusSession gdiplus;
        UiContext ui;
        ui.version = ReadWindowsVersion();
        ui.controller = CreateController(ui.version);
        INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
        wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"AchangUser.UpdateLock.Native";
        if (!RegisterClassExW(&wc)) ThrowWin32(L"注册窗口类");
        HDC screenDc = GetDC(nullptr);
        if (!screenDc) ThrowWin32(L"读取屏幕 DPI");
        const int windowDpi = GetDeviceCaps(screenDc, LOGPIXELSY);
        ReleaseDC(nullptr, screenDc);
        const DWORD windowStyle = WS_OVERLAPPEDWINDOW | WS_VSCROLL;
        RECT workArea = {};
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) ||
            workArea.right <= workArea.left || workArea.bottom <= workArea.top) {
            workArea = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        }
        const int workWidth = workArea.right - workArea.left;
        const int workHeight = workArea.bottom - workArea.top;
        const int initialDpi = ChooseRenderDpi(windowDpi,
            workWidth - ScaleForDpi(24, windowDpi),
            workHeight - ScaleForDpi(72, windowDpi), false);
        const int clientWidth = ScaleForDpi(kUiClientWidth, initialDpi);
        const int clientHeight = ScaleForDpi(kUiClientHeight, initialDpi);
        RECT rect = {0, 0, clientWidth, clientHeight};
        AdjustWindowRectEx(&rect, windowStyle, FALSE, 0);
        const int windowWidth = rect.right - rect.left;
        const int windowHeight = rect.bottom - rect.top;
        const int windowX = workArea.left + std::max(0, (workWidth - windowWidth) / 2);
        const int windowY = workArea.top + std::max(0, (workHeight - windowHeight) / 2);
        HWND window = CreateWindowExW(0, wc.lpszClassName, kAppName,
            windowStyle, windowX, windowY, windowWidth, windowHeight,
            nullptr, nullptr, instance, &ui);
        if (!window) ThrowWin32(L"创建主窗口");
        HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        ShowWindow(window, showCommand);
        UpdateWindow(window);
        MSG message = {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
    } catch (const AppError& error) {
        MessageBoxW(nullptr, error.message.c_str(), kAppName, MB_OK | MB_ICONERROR);
        CoUninitialize(); return 1;
    } catch (...) {
        MessageBoxW(nullptr, L"发生未预期错误。系统设置不会被继续修改。", kAppName, MB_OK | MB_ICONERROR);
        CoUninitialize(); return 1;
    }
    CoUninitialize();
    return 0;
}
#endif
