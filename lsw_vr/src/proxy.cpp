// Proxy d3d9.dll: loads the real system d3d9.dll and forwards the three
// entry points the game imports. Direct3DCreate9 also installs device hooks.

#include <windows.h>
#include <d3d9.h>

#include "hooks.h"
#include "log.h"

namespace {
    HMODULE g_real_d3d9 = nullptr;

    using Direct3DCreate9Fn = IDirect3D9 *(WINAPI *)(UINT);
    using D3DPERF_BeginEventFn = int(WINAPI *)(D3DCOLOR, LPCWSTR);
    using D3DPERF_EndEventFn = int(WINAPI *)();

    FARPROC RealProc(const char *name) {
        if (g_real_d3d9 == nullptr) {
            char path[MAX_PATH];
            GetSystemDirectoryA(path, MAX_PATH);  // SysWOW64 for a 32-bit process
            strcat_s(path, "\\d3d9.dll");
            g_real_d3d9 = LoadLibraryA(path);
            Log("loaded real d3d9 from %s -> %p", path, g_real_d3d9);
        }
        return g_real_d3d9 != nullptr ? GetProcAddress(g_real_d3d9, name) : nullptr;
    }
} // namespace

extern "C" IDirect3D9 *WINAPI Proxy_Direct3DCreate9(UINT sdk_version) {
    auto real = reinterpret_cast<Direct3DCreate9Fn>(RealProc("Direct3DCreate9"));
    IDirect3D9 *d3d = real != nullptr ? real(sdk_version) : nullptr;
    Log("Direct3DCreate9(sdk=%u) -> %p", sdk_version, d3d);
    if (d3d != nullptr) {
        InstallD3D9Hooks(d3d);
    }
    return d3d;
}

extern "C" int WINAPI Proxy_D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) {
    auto real = reinterpret_cast<D3DPERF_BeginEventFn>(RealProc("D3DPERF_BeginEvent"));
    return real != nullptr ? real(color, name) : 0;
}

extern "C" int WINAPI Proxy_D3DPERF_EndEvent() {
    auto real = reinterpret_cast<D3DPERF_EndEventFn>(RealProc("D3DPERF_EndEvent"));
    return real != nullptr ? real() : 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // The game isn't DPI-aware, so on a scaled monitor (e.g. 150%) Windows
        // stretches its window as a bitmap, which blurs it. Declare awareness
        // before the game creates its window (this DLL loads with the exe).
        using SetContextFn = BOOL(WINAPI *)(HANDLE);
        HMODULE user32 = GetModuleHandleA("user32.dll");
        const auto set_context =
            user32 != nullptr ? reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
                              : nullptr;
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        if (set_context == nullptr || !set_context(reinterpret_cast<HANDLE>(-4))) {
            SetProcessDPIAware();
        }
    }
    return TRUE;
}
