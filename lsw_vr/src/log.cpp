#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <share.h>

namespace {
    CRITICAL_SECTION g_lock;
    FILE *g_file = nullptr;
    const char *g_file_name = "lsw_vr.log";

    void OpenLog() {
        InitializeCriticalSection(&g_lock);
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char *slash = strrchr(path, '\\');
        if (slash != nullptr) {
            strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), g_file_name);
        }
        // Keep the previous session's log (name.prev.log) for diagnosis.
        char previous[MAX_PATH];
        strcpy_s(previous, path);
        char *dot = strrchr(previous, '.');
        if (dot != nullptr) {
            strcpy_s(dot, MAX_PATH - (dot - previous), ".prev.log");
            MoveFileExA(path, previous, MOVEFILE_REPLACE_EXISTING);
        }
        // Shared, so the log can be read while the game runs.
        g_file = _fsopen(path, "w", _SH_DENYNO);
    }
} // namespace

void LogSetFileName(const char *name) {
    g_file_name = name;
}

void Log(const char *format, ...) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(
        &once, [](PINIT_ONCE, PVOID, PVOID *) -> BOOL {
            OpenLog();
            return TRUE;
        },
        nullptr, nullptr);
    if (g_file == nullptr) {
        return;
    }

    EnterCriticalSection(&g_lock);
    SYSTEMTIME now;
    GetLocalTime(&now);
    std::fprintf(g_file, "%02d:%02d:%02d.%03d ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    va_list args;
    va_start(args, format);
    std::vfprintf(g_file, format, args);
    va_end(args);
    std::fputc('\n', g_file);
    std::fflush(g_file);
    LeaveCriticalSection(&g_lock);
}
