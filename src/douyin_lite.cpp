#include <windows.h>
#include <psapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <cstdlib>

#include "../minhook/include/MinHook.h"

typedef BOOL(WINAPI* PFN_CreateProcessW)(
    LPCWSTR lpApplicationName,
    LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation
);

typedef LPWSTR(WINAPI* PFN_GetCommandLineW)();

static PFN_CreateProcessW g_origCreateProcessW = nullptr;
static PFN_GetCommandLineW g_origGetCommandLineW = nullptr;
static std::wstring g_cachedCmdLine;
static std::once_flag g_cmdLineOnce;
static wchar_t g_szDllPath[MAX_PATH] = {0};

struct Config {
    DWORD v8HeapMb = 512;
    DWORD rendererLimit = 4;
    SIZE_T totalThresholdMb = 900;
    SIZE_T singleThresholdMb = 400;
    SIZE_T trimTargetMb = 300;
    DWORD cooldownSec = 6;
};

static Config g_cfg;

static std::wstring GetModuleDir() {
    wchar_t dir[MAX_PATH] = {0};
    wcscpy_s(dir, g_szDllPath);
    PathRemoveFileSpecW(dir);
    return std::wstring(dir);
}

static std::wstring GetConfigPath() {
    wchar_t iniPath[MAX_PATH] = {0};
    swprintf_s(iniPath, L"%s\\douyin_lite.ini", GetModuleDir().c_str());
    return std::wstring(iniPath);
}

static const char* g_defaultIni =
    "[mem]\n"
    "v8_heap_mb=512\n"
    "renderer_limit=4\n"
    "total_threshold_mb=900\n"
    "single_threshold_mb=400\n"
    "trim_target_mb=300\n"
    "cooldown_sec=6\n";

static void EnsureConfigFile() {
    std::wstring path = GetConfigPath();
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) return;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    WriteFile(hFile, g_defaultIni, (DWORD)strlen(g_defaultIni), &written, NULL);
    CloseHandle(hFile);
}

static bool ReadIniUint(const std::wstring& iniPath, const wchar_t* key,
                        unsigned long minV, unsigned long maxV, unsigned long* out) {
    wchar_t buf[64] = {0};
    if (GetPrivateProfileStringW(L"mem", key, L"", buf, 64, iniPath.c_str()) <= 0) {
        return false;
    }
    unsigned long v = wcstoul(buf, nullptr, 10);
    if (v < minV || v > maxV) {
        return false;
    }
    *out = v;
    return true;
}

static void LoadConfig() {
    std::wstring iniPath = GetConfigPath();
    EnsureConfigFile();

    unsigned long v = 0;
    if (ReadIniUint(iniPath, L"v8_heap_mb", 64, 4096, &v)) {
        g_cfg.v8HeapMb = (DWORD)v;
    }
    if (ReadIniUint(iniPath, L"renderer_limit", 1, 8, &v)) {
        g_cfg.rendererLimit = (DWORD)v;
    }
    if (ReadIniUint(iniPath, L"total_threshold_mb", 128, 8192, &v)) {
        g_cfg.totalThresholdMb = (SIZE_T)v;
    }
    if (ReadIniUint(iniPath, L"single_threshold_mb", 64, 4096, &v)) {
        g_cfg.singleThresholdMb = (SIZE_T)v;
    }
    if (ReadIniUint(iniPath, L"trim_target_mb", 0, 4096, &v)) {
        g_cfg.trimTargetMb = (SIZE_T)v;
    }
    if (ReadIniUint(iniPath, L"cooldown_sec", 1, 120, &v)) {
        g_cfg.cooldownSec = (DWORD)v;
    }
}

static std::wstring FormatMb(unsigned long mb) {
    wchar_t buf[32] = {0};
    swprintf_s(buf, L"%lu", mb);
    return std::wstring(buf);
}

static const wchar_t* g_mediaCacheFlags =
    L" --disk-cache-size=16777216"
    L" --media-cache-size=16777216";

static std::wstring g_extraMainFlags;

// 渲染进程堆实测上限 256 时仅用 160MB；主进程不解析 --js-flags 的 V8 段，靠 GetCommandLineW hook 覆盖
// --optimize_for_size 放弃：V8 12+ 无此 flag，写入会被忽略
static void BuildMainFlags() {
    g_extraMainFlags =
        L" --js-flags=\"--max-old-space-size=" + FormatMb(g_cfg.v8HeapMb) + L"\""
        L" --disable-background-networking --disable-component-update --disable-breakpad"
        L" --renderer-process-limit=" + FormatMb(g_cfg.rendererLimit) +
        L" --process-per-site-instance" + std::wstring(g_mediaCacheFlags);
}

static const std::vector<std::wstring> g_blockedProcesses = {
    L"tt_crash_reporter.exe",
    L"dump_reporter.exe",
    L"douyin_doctor.exe",
    L"douyin_game_widget.exe",
    L"douyin_guard.exe",
    L"parfait_crash_handler.exe"
};

static bool IsProcessBlocked(LPCWSTR appName, LPCWSTR cmdLine) {
    std::wstring target;
    if (appName) {
        target += appName;
    }
    if (cmdLine) {
        if (!target.empty()) target += L" ";
        target += cmdLine;
    }

    for (const auto& blocked : g_blockedProcesses) {
        if (StrStrIW(target.c_str(), blocked.c_str()) != nullptr) {
            return true;
        }
    }
    return false;
}

static void InjectDllToProcess(HANDLE hProcess) {
    if (!hProcess || g_szDllPath[0] == L'\0') return;

    SIZE_T sz = (wcslen(g_szDllPath) + 1) * sizeof(wchar_t);
    LPVOID pRemoteMem = VirtualAllocEx(hProcess, NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemoteMem) return;

    if (WriteProcessMemory(hProcess, pRemoteMem, g_szDllPath, sz, NULL)) {
        LPTHREAD_START_ROUTINE pLoadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"),
            "LoadLibraryW"
        );
        if (pLoadLib) {
            HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, pLoadLib, pRemoteMem, 0, NULL);
            if (hThread) {
                WaitForSingleObject(hThread, 2000);
                CloseHandle(hThread);
            }
        }
    }
    VirtualFreeEx(hProcess, pRemoteMem, 0, MEM_RELEASE);
}

// 将命令行中 v8 堆上限改写为配置值，不依赖具体版本号
static void PatchV8Heap(std::wstring& cmdLine) {
    const std::wstring prefix = L"--v8_max_old_space_size_mb=";
    size_t pos = cmdLine.find(prefix);
    if (pos == std::wstring::npos) return;

    size_t numStart = pos + prefix.length();
    size_t numEnd = numStart;
    while (numEnd < cmdLine.length() && cmdLine[numEnd] >= L'0' && cmdLine[numEnd] <= L'9') {
        ++numEnd;
    }
    if (numEnd == numStart) return;

    cmdLine.replace(numStart, numEnd - numStart, FormatMb(g_cfg.v8HeapMb));
}

BOOL WINAPI Hooked_CreateProcessW(
    LPCWSTR lpApplicationName,
    LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation
) {
    if (IsProcessBlocked(lpApplicationName, lpCommandLine)) {
        if (lpProcessInformation) {
            ZeroMemory(lpProcessInformation, sizeof(PROCESS_INFORMATION));
        }
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    std::vector<wchar_t> modifiedCmdLine;
    if (lpCommandLine && *lpCommandLine) {
        std::wstring tmp = lpCommandLine;
        PatchV8Heap(tmp);
        if (tmp.find(L"--disk-cache-size") == std::wstring::npos) {
            tmp += g_mediaCacheFlags;
        }
        modifiedCmdLine.assign(tmp.begin(), tmp.end());
        modifiedCmdLine.push_back(L'\0');
    }

    bool needResume = false;
    DWORD flags = dwCreationFlags;
    if (!(flags & CREATE_SUSPENDED)) {
        flags |= CREATE_SUSPENDED;
        needResume = true;
    }

    BOOL ret = g_origCreateProcessW(
        lpApplicationName,
        modifiedCmdLine.empty() ? lpCommandLine : modifiedCmdLine.data(),
        lpProcessAttributes,
        lpThreadAttributes,
        bInheritHandles,
        flags,
        lpEnvironment,
        lpCurrentDirectory,
        lpStartupInfo,
        lpProcessInformation
    );

    if (ret && lpProcessInformation && lpProcessInformation->hProcess) {
        InjectDllToProcess(lpProcessInformation->hProcess);

        if (needResume && lpProcessInformation->hThread) {
            ResumeThread(lpProcessInformation->hThread);
        }
    }

    return ret;
}

LPWSTR WINAPI Hooked_GetCommandLineW() {
    std::call_once(g_cmdLineOnce, []() {
        LPWSTR raw = g_origGetCommandLineW();
        g_cachedCmdLine = raw ? raw : L"";
        if (g_cachedCmdLine.find(L"--js-flags") == std::wstring::npos) {
            g_cachedCmdLine += g_extraMainFlags;
        }
    });
    return const_cast<LPWSTR>(g_cachedCmdLine.c_str());
}

static bool IsRelatedProcess(const PROCESSENTRY32W& pe, DWORD currentPid, const wchar_t* currentBase) {
    return pe.th32ParentProcessID == currentPid ||
           (StrStrIW(pe.szExeFile, L"douyin") != nullptr) ||
           _wcsicmp(pe.szExeFile, currentBase) == 0;
}

static void GetCurrentProcessBaseName(wchar_t* out, DWORD size) {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    const wchar_t* slash = wcsrchr(path, L'\\');
    wcscpy_s(out, size, slash ? slash + 1 : path);
}

// 遍历进程树并按回调处理每个关联进程，回调返回 false 时提前终止
template <typename Fn>
static void ForEachRelatedProcess(Fn&& fn) {
    DWORD currentPid = GetCurrentProcessId();
    wchar_t currentBase[MAX_PATH] = {0};
    GetCurrentProcessBaseName(currentBase, MAX_PATH);

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            if (IsRelatedProcess(pe, currentPid, currentBase)) {
                if (!fn(pe)) break;
            }
        } while (Process32NextW(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
}

// 硬上限：内核持续按 LRU 挤冷页、保留热页。douyin 子进程出厂配额 max 仅约 1.4MB，
// 小于目标值，故不能拿配额 max 做幂等判断，须比对实际工作集并无条件重设
static void ClampProcessWorkingSet(HANDLE hProc, SIZE_T maxBytes) {
    PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
    if (!GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) return;
    if (pmc.WorkingSetSize <= maxBytes) return;

    SIZE_T minWs = 0, maxWs = 0;
    if (!GetProcessWorkingSetSize(hProc, &minWs, &maxWs) || maxWs == maxBytes) return;
    SetProcessWorkingSetSizeEx(hProc, minWs, maxBytes, QUOTA_LIMITS_HARDWS_MAX_ENABLE);
}

static void ApplyTrimToRelatedProcesses() {
    SIZE_T maxBytes = g_cfg.trimTargetMb * 1024 * 1024;
    ForEachRelatedProcess([&](const PROCESSENTRY32W& pe) {
        HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (hProc) {
            ClampProcessWorkingSet(hProc, maxBytes);
            CloseHandle(hProc);
        }
        return true;
    });

    ClampProcessWorkingSet(GetCurrentProcess(), maxBytes);
}

void WorkingSetTrimWorker() {
    // 启动即压工作集会令渲染进程预热缺页
    std::this_thread::sleep_for(std::chrono::seconds(5));
    ApplyTrimToRelatedProcesses();

    auto lastTrim = std::chrono::steady_clock::now();

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastTrim).count() < g_cfg.cooldownSec) {
            continue;
        }

        SIZE_T totalTreeWorkingSet = 0;
        bool singleProcExceed = false;
        ForEachRelatedProcess([&](const PROCESSENTRY32W& pe) {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (hProc) {
                PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
                if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                    totalTreeWorkingSet += pmc.WorkingSetSize;
                    if (pmc.WorkingSetSize > g_cfg.singleThresholdMb * 1024 * 1024) {
                        singleProcExceed = true;
                    }
                }
                CloseHandle(hProc);
            }
            return !singleProcExceed;
        });

        if (totalTreeWorkingSet > g_cfg.totalThresholdMb * 1024 * 1024 || singleProcExceed) {
            ApplyTrimToRelatedProcesses();
            lastTrim = now;
        }
    }
}

void InitHooks() {
    LoadConfig();
    BuildMainFlags();

    if (MH_Initialize() != MH_OK) {
        return;
    }

    MH_CreateHook(
        reinterpret_cast<LPVOID>(&CreateProcessW),
        reinterpret_cast<LPVOID>(&Hooked_CreateProcessW),
        reinterpret_cast<LPVOID*>(&g_origCreateProcessW)
    );

    MH_CreateHook(
        reinterpret_cast<LPVOID>(&GetCommandLineW),
        reinterpret_cast<LPVOID>(&Hooked_GetCommandLineW),
        reinterpret_cast<LPVOID*>(&g_origGetCommandLineW)
    );

    MH_EnableHook(MH_ALL_HOOKS);

    std::thread(WorkingSetTrimWorker).detach();
}

void UninitHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        GetModuleFileNameW(hModule, g_szDllPath, MAX_PATH);
        InitHooks();
        break;
    case DLL_PROCESS_DETACH:
        UninitHooks();
        break;
    }
    return TRUE;
}
