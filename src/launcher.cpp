#include <windows.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <string>

#pragma comment(lib, "shlwapi.lib")

static std::wstring GetExecutableDirectory() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return std::wstring(path);
}

static bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

static void ShowError(const std::wstring& message) {
    MessageBoxW(NULL, message.c_str(), L"douyin_lite", MB_OK | MB_ICONERROR);
}

static const wchar_t* g_usage =
    L"用法:\n"
    L"  douyin_lite.exe [--dll <DLL路径>] [--exe <douyin.exe路径>]\n"
    L"\n"
    L"参数可省略，默认使用当前目录下的 douyin_lite.dll 与 douyin.exe";

static const wchar_t* g_mutexName = L"Local\\douyin_lite_instance";

// 重复启动时把已运行的抖音主窗口拉到前台
static void ActivateExistingWindow() {
    struct EnumData {
        DWORD pid;
        HWND hwnd;
    } data = {0};

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnapshot, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"douyin.exe") == 0) {
                    data.pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(hSnapshot, &pe));
        }
        CloseHandle(hSnapshot);
    }
    if (!data.pid) return;

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        EnumData* d = reinterpret_cast<EnumData*>(lParam);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == d->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == NULL) {
            wchar_t title[256] = {0};
            GetWindowTextW(hwnd, title, 256);
            if (title[0]) {
                d->hwnd = hwnd;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));

    if (data.hwnd) {
        if (IsIconic(data.hwnd)) {
            ShowWindow(data.hwnd, SW_RESTORE);
        }
        SetForegroundWindow(data.hwnd);
    }
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, g_mutexName);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        ActivateExistingWindow();
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    std::wstring appDir = GetExecutableDirectory();
    std::wstring dllPath = appDir + L"\\douyin_lite.dll";
    std::wstring targetExe = appDir + L"\\douyin.exe";

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if ((arg == L"--dll" || arg == L"/dll") && i + 1 < argc) {
            dllPath = argv[++i];
        } else if ((arg == L"--exe" || arg == L"/exe") && i + 1 < argc) {
            targetExe = argv[++i];
        } else if (arg == L"--help" || arg == L"/?") {
            MessageBoxW(NULL, g_usage, L"douyin_lite", MB_OK | MB_ICONINFORMATION);
            LocalFree(argv);
            return 0;
        }
    }

    if (!FileExists(dllPath)) {
        ShowError(std::wstring(L"未找到 DLL 文件:\n") + dllPath + L"\n\n" + g_usage);
        LocalFree(argv);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    if (!FileExists(targetExe)) {
        ShowError(std::wstring(L"未找到目标程序:\n") + targetExe + L"\n\n" + g_usage);
        LocalFree(argv);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    wchar_t fullDllPath[MAX_PATH] = {0};
    GetFullPathNameW(dllPath.c_str(), MAX_PATH, fullDllPath, NULL);

    wchar_t workDir[MAX_PATH] = {0};
    wcscpy_s(workDir, targetExe.c_str());
    PathRemoveFileSpecW(workDir);

    std::wstring cmdLine = L"\"" + targetExe + L"\"";

    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {0};

    if (!CreateProcessW(targetExe.c_str(), &cmdLine[0],
                        NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, workDir, &si, &pi)) {
        ShowError(L"启动 douyin.exe 失败，错误码: " + std::to_wstring(GetLastError()));
        LocalFree(argv);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    SIZE_T pathSize = (wcslen(fullDllPath) + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(pi.hProcess, NULL, pathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool injected = false;

    if (remoteMem) {
        if (WriteProcessMemory(pi.hProcess, remoteMem, fullDllPath, pathSize, NULL)) {
            LPTHREAD_START_ROUTINE pfnLoadLibraryW = (LPTHREAD_START_ROUTINE)GetProcAddress(
                GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
            if (pfnLoadLibraryW) {
                HANDLE hRemoteThread = CreateRemoteThread(pi.hProcess, NULL, 0,
                                                          pfnLoadLibraryW, remoteMem, 0, NULL);
                if (hRemoteThread) {
                    WaitForSingleObject(hRemoteThread, 5000);
                    CloseHandle(hRemoteThread);
                    injected = true;
                }
            }
        }
        VirtualFreeEx(pi.hProcess, remoteMem, 0, MEM_RELEASE);
    }

    if (!injected) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ShowError(L"DLL 注入失败，错误码: " + std::to_wstring(GetLastError()));
        LocalFree(argv);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LocalFree(argv);
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
