// Starts lsw_vr_host.exe (the 64-bit OpenXR process) next to the mod's DLL and
// owns the frame channel it reads from.

#include <windows.h>

#include <cstdio>

#include "../log.h"
#include "../shared/frame_channel.h"
#include "vr.h"
#include "sim.h"
#include "vr_channel.h"

namespace {
    FrameChannel g_channel;
    bool g_channel_ok = false;
    HANDLE g_host = nullptr;  // lsw_vr_host.exe, to notice if it dies
    bool g_host_dead_logged = false;

    // Folder of the module containing this code (the game folder for the mod).
    void ModuleFolder(char *path, DWORD size) {
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&ModuleFolder), &module);
        GetModuleFileNameA(module, path, size);
        char *slash = strrchr(path, '\\');
        if (slash != nullptr) {
            slash[1] = '\0';
        }
    }
} // namespace

FrameChannel *VrChannel() {
    if (!g_channel_ok || g_host == nullptr) {
        return nullptr;
    }
    // A host that crashed or was closed can't clear its flags, so don't trust
    // the channel (stale pose, capture requests) once it is gone.
    if (WaitForSingleObject(g_host, 0) != WAIT_TIMEOUT) {
        if (!g_host_dead_logged) {
            Log("vr: lsw_vr_host.exe has exited; VR off");
            g_host_dead_logged = true;
        }
        return nullptr;
    }
    return &g_channel;
}

void VrStart() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;

    const DWORD pid = GetCurrentProcessId();
    g_channel_ok = g_channel.Create(pid);
    if (!g_channel_ok) {
        Log("vr: frame channel creation failed (%lu); VR off", GetLastError());
        return;
    }

    if (SimActive()) {
        Log("vr: simulation mode, not launching lsw_vr_host.exe");
        return;
    }
    char folder[MAX_PATH];
    ModuleFolder(folder, MAX_PATH);
    char host[MAX_PATH];
    sprintf_s(host, "%slsw_vr_host.exe", folder);
    if (GetFileAttributesA(host) == INVALID_FILE_ATTRIBUTES) {
        Log("vr: %s not found; VR off", host);
        return;
    }
    char command_line[MAX_PATH + 32];
    sprintf_s(command_line, "\"%s\" %lu", host, pid);
    STARTUPINFOA startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (!CreateProcessA(host, command_line, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, folder, &startup,
                        &process)) {
        Log("vr: launching %s failed (%lu); VR off", host, GetLastError());
        return;
    }
    CloseHandle(process.hThread);
    g_host = process.hProcess;
    Log("vr: launched lsw_vr_host.exe (pid %lu)", process.dwProcessId);

    // lsw_vr_tuner.exe is no longer started here; run it by hand to tune.
}
