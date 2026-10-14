// Device hooks. The game is forced windowed (see ForceWindowed) and
// Ctrl+Shift+F12 kills it. Stage 1: prove injection with a corner marker and survey the
// vertex shader constants the game uploads, to locate its view/projection
// matrices.

#include "hooks.h"

#include <windows.h>
#include <d3d9.h>

#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <map>

#include "log.h"
#include "camera.h"
#include "game.h"
#include "skeleton.h"
#include "vr/vr.h"
#include "vr/vr_controls.h"
#include "vr/sim.h"
#include "vr/vr_frame.h"
#include "vr/vr_force.h"
#include "vr/vr_interaction.h"

namespace {
    // COM vtable slots (d3d9.h declaration order).
    constexpr int kD3D9CreateDevice = 16;
    constexpr int kDeviceReset = 16;
    constexpr int kDevicePresent = 17;
    constexpr int kDeviceSetTransform = 44;
    constexpr int kDeviceSetVertexShaderConstantF = 94;
    constexpr int kDeviceSetViewport = 47;
    // Speed blur (frame blending) ghosts badly in VR, especially with stereo by
    // alternating eyes, and the game rewrites its config, so hold it off here:
    // config handlers 006db700 / 006db720 set these bytes.
    constexpr uintptr_t kSpeedBlurSupported = 0x8264c5;
    constexpr uintptr_t kSpeedBlurEnabled = 0x8264c6;

    using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                       D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
    using ResetFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
    using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND,
                                                  const RGNDATA *);
    using SetTransformFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *);
    using SetVSConstFFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, const float *, UINT);

    CreateDeviceFn g_real_create_device = nullptr;
    ResetFn g_real_reset = nullptr;
    PresentFn g_real_present = nullptr;
    SetTransformFn g_real_set_transform = nullptr;
    SetVSConstFFn g_real_set_vs_const = nullptr;
    using SetViewportFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
    SetViewportFn g_real_set_viewport = nullptr;
    // Distinct viewports seen (diagnostic: does the 3D view fill the buffer?).
    D3DVIEWPORT9 g_viewports_seen[16] = {};
    int g_viewport_count = 0;

    // Render size override: the game only offers monitor modes, but the
    // headset's per-eye view is taller than wide (tangents 0.966 h / 1.111 v),
    // so the back buffer is replaced by one of that shape. The game still
    // thinks in its own mode size (g_game_w x g_game_h); viewports it sets in
    // those units are scaled to the real buffer.
    constexpr UINT kVrRenderWidth = 1392;
    constexpr UINT kVrRenderHeight = 1600;
    UINT g_game_w = 0, g_game_h = 0;  // size the game asked for
    UINT g_real_w = 0, g_real_h = 0;  // size actually created

    HRESULT STDMETHODCALLTYPE HookSetViewport(IDirect3DDevice9 *device, const D3DVIEWPORT9 *viewport) {
        D3DVIEWPORT9 scaled;
        if (viewport != nullptr && g_game_w != 0 && (g_game_w != g_real_w || g_game_h != g_real_h)) {
            IDirect3DSurface9 *target = nullptr;
            D3DSURFACE_DESC desc = {};
            if (SUCCEEDED(device->GetRenderTarget(0, &target)) && target != nullptr) {
                target->GetDesc(&desc);
                target->Release();
            }
            // Only viewports on a back-buffer-sized target given in game units.
            if (desc.Width == g_real_w && desc.Height == g_real_h && viewport->X + viewport->Width <= g_game_w &&
                viewport->Y + viewport->Height <= g_game_h &&
                !(viewport->Width == g_real_w && viewport->Height == g_real_h)) {
                scaled = *viewport;
                scaled.X = viewport->X * g_real_w / g_game_w;
                scaled.Y = viewport->Y * g_real_h / g_game_h;
                scaled.Width = viewport->Width * g_real_w / g_game_w;
                scaled.Height = viewport->Height * g_real_h / g_game_h;
                viewport = &scaled;
            }
        }
        if (viewport != nullptr && g_viewport_count < 16) {
            bool seen = false;
            for (int i = 0; i < g_viewport_count; ++i) {
                const D3DVIEWPORT9 &v = g_viewports_seen[i];
                seen = seen || (v.X == viewport->X && v.Y == viewport->Y && v.Width == viewport->Width &&
                                v.Height == viewport->Height);
            }
            if (!seen) {
                g_viewports_seen[g_viewport_count++] = *viewport;
                IDirect3DSurface9 *target = nullptr;
                D3DSURFACE_DESC desc = {};
                if (SUCCEEDED(device->GetRenderTarget(0, &target)) && target != nullptr) {
                    target->GetDesc(&desc);
                    target->Release();
                }
                Log("viewport %lu,%lu %lux%lu on a %ux%u render target", viewport->X, viewport->Y, viewport->Width,
                    viewport->Height, desc.Width, desc.Height);
            }
        }
        return g_real_set_viewport(device, viewport);
    }

    HWND g_window = nullptr;  // focus window from CreateDevice, reused on Reset

    // The game pauses when its window loses focus, which gets in the way in
    // VR (tuner window, desktop). Its window procedure (006de1c0) keeps
    // WM_ACTIVATE's state in one flag; the game runs while it is set. All
    // messages still reach the game (its keyboard comes from WM_KEYDOWN, and
    // other handlers watch focus), and the flag is set again every frame.
    constexpr uintptr_t kWindowActiveFlag = 0x827610;

    unsigned g_frame = 0;
    DWORD g_fps_tick = 0;
    unsigned g_fps_frames = 0;

    // Per-frame survey of 4-register constant uploads: register -> upload count.
    std::map<UINT, unsigned> g_vs_matrix_uploads;
    unsigned g_set_transform_calls = 0;
    constexpr unsigned kSurveyEvery = 300;

    void *VTableEntry(void *object, int slot) {
        return (*reinterpret_cast<void ***>(object))[slot];
    }

    bool Hook(void *target, void *detour, void **original, const char *name) {
        MH_STATUS status = MH_CreateHook(target, detour, original);
        if (status == MH_OK) {
            status = MH_EnableHook(target);
        }
        Log("hook %-26s %p -> %s", name, target, MH_StatusToString(status));
        return status == MH_OK;
    }

    void LogMatrix(const char *label, const float *m) {
        Log("  %s", label);
        for (int row = 0; row < 4; ++row) {
            Log("    [% .4f % .4f % .4f % .4f]", m[row * 4 + 0], m[row * 4 + 1], m[row * 4 + 2], m[row * 4 + 3]);
        }
    }

    HRESULT STDMETHODCALLTYPE HookSetVSConst(IDirect3DDevice9 *device, UINT start, const float *data, UINT count) {
        if (start == 12 && count >= 4 && data != nullptr) {
            CameraOnShaderView(data);  // c12 = view (shader parameter table)
        }
        if (count >= 4) {
            ++g_vs_matrix_uploads[start];
            // On survey frames, dump the first upload to each register block.
            if (g_frame % kSurveyEvery == 0 && g_vs_matrix_uploads[start] == 1 && data != nullptr) {
                char label[64];
                sprintf_s(label, "c%u (count %u)", start, count);
                LogMatrix(label, data);
            }
        }
        return g_real_set_vs_const(device, start, data, count);
    }

    HRESULT STDMETHODCALLTYPE HookSetTransform(IDirect3DDevice9 *device, D3DTRANSFORMSTATETYPE state,
                                               const D3DMATRIX *matrix) {
        ++g_set_transform_calls;
        return g_real_set_transform(device, state, matrix);
    }

    void DrawMarker(IDirect3DDevice9 *device) {
        // A solid square in the top-left corner: visible proof the mod loaded.
        // It cycles green/blue so a frozen frame is obvious.
        const D3DRECT rect = {8, 8, 40, 40};
        const D3DCOLOR color = ((g_frame / 60) % 2 == 0) ? D3DCOLOR_XRGB(0, 200, 0) : D3DCOLOR_XRGB(0, 80, 255);
        device->Clear(1, &rect, D3DCLEAR_TARGET, color, 1.0f, 0);
    }

    // Full-screen PC view (for recording): the window covers its monitor
    // without borders and Present shows a crop of the (tall, left-eye) frame
    // with the monitor's shape. Needs the copy swap effect (source rects).
    bool g_swap_copy = false;
    bool g_fullscreen_applied = false;
    RECT g_windowed_rect = {};
    constexpr DWORD kWindowedStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;

    void ApplyPcFullscreen(bool on) {
        if (g_window == nullptr) {
            return;
        }
        if (on) {
            GetWindowRect(g_window, &g_windowed_rect);
            MONITORINFO info = {sizeof(info)};
            GetMonitorInfoA(MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST), &info);
            const RECT &m = info.rcMonitor;
            SetWindowLongPtrA(g_window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(g_window, HWND_NOTOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            Log("window: full screen %ldx%ld (left eye, cropped to the monitor's shape)", m.right - m.left,
                m.bottom - m.top);
        } else {
            SetWindowLongPtrA(g_window, GWL_STYLE, kWindowedStyle);
            const RECT &r = g_windowed_rect;
            if (r.right > r.left) {
                SetWindowPos(g_window, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                             SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            }
            Log("window: normal");
        }
        g_fullscreen_applied = on;
    }

    // The part of the back buffer shown full screen: centred, with the
    // window's aspect ratio.
    bool FullscreenSourceRect(IDirect3DDevice9 *device, RECT *source) {
        RECT client;
        if (g_window == nullptr || !GetClientRect(g_window, &client) || client.right <= 0 || client.bottom <= 0) {
            return false;
        }
        IDirect3DSurface9 *back_buffer = nullptr;
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer))) {
            return false;
        }
        D3DSURFACE_DESC desc;
        back_buffer->GetDesc(&desc);
        back_buffer->Release();
        LONG width = static_cast<LONG>(desc.Width);
        LONG height = width * client.bottom / client.right;
        if (height > static_cast<LONG>(desc.Height)) {
            height = static_cast<LONG>(desc.Height);
            width = height * client.right / client.bottom;
        }
        source->left = (static_cast<LONG>(desc.Width) - width) / 2;
        source->top = (static_cast<LONG>(desc.Height) - height) / 2;
        source->right = source->left + width;
        source->bottom = source->top + height;
        return true;
    }

    HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9 *device, const RECT *src, const RECT *dst, HWND window,
                                          const RGNDATA *dirty) {
        *GameAddress<unsigned char>(kSpeedBlurSupported) = 0;
        *GameAddress<unsigned char>(kSpeedBlurEnabled) = 0;
        *GameAddress<int>(kWindowActiveFlag) = 1;  // keep running when unfocused
        VrCaptureFrame(device, CameraTakeFrameMeta());  // before the marker: the headset gets the clean frame
        DrawMarker(device);
        SimOnPresent(g_frame);
        VrLatchTracking();
        CameraOnPresent(g_frame);
        VrInteractionOnPresent(g_frame);
        VrForceOnPresent(g_frame);
        VrControlsOnPresent(g_frame);
        SkeletonOnPresent();
        CameraEndFrame();

        if (g_frame % kSurveyEvery == 0) {
            Log("frame %u survey: SetTransform calls=%u, 4x4+ VS constant uploads by start register:", g_frame,
                g_set_transform_calls);
            for (const auto &[reg, uploads] : g_vs_matrix_uploads) {
                Log("  c%-4u x%u", reg, uploads);
            }
        }
        g_vs_matrix_uploads.clear();
        g_set_transform_calls = 0;

        ++g_fps_frames;
        const DWORD now = GetTickCount();
        if (now - g_fps_tick >= 5000) {
            Log("fps %.1f (frame %u)", g_fps_frames * 1000.0 / (now - g_fps_tick), g_frame);
            g_fps_tick = now;
            g_fps_frames = 0;
        }
        ++g_frame;
        const bool fullscreen = g_swap_copy && CameraPcFullscreen();
        if (fullscreen != g_fullscreen_applied) {
            ApplyPcFullscreen(fullscreen);
        }
        RECT source;
        if (fullscreen && src == nullptr && dst == nullptr && FullscreenSourceRect(device, &source)) {
            return g_real_present(device, &source, nullptr, window, dirty);
        }
        return g_real_present(device, src, dst, window, dirty);
    }

    // Exclusive fullscreen locks up the desktop (no Alt-Tab), and VR needs a
    // desktop window anyway, so always run windowed: a normal (movable,
    // non-topmost) window whose client area is the back buffer size. The
    // first time it is centred on the monitor under the mouse (the one the
    // game was launched from) rather than the primary one, which may be a
    // display the user can't see; after that it stays where the user put it.
    void ForceWindowed(D3DPRESENT_PARAMETERS *params, HWND window) {
        if (!params->Windowed) {
            Log("forcing windowed (game asked for fullscreen %ux%u @%uHz, interval 0x%x)", params->BackBufferWidth,
                params->BackBufferHeight, params->FullScreen_RefreshRateInHz, params->PresentationInterval);
            params->Windowed = TRUE;
            params->FullScreen_RefreshRateInHz = 0;
        }
        // Copy swap effect, so Present can show part of the frame (full-screen
        // PC view). Not possible with multisampling.
        Log("swap: effect %d, back buffers %u, msaa %d", params->SwapEffect, params->BackBufferCount,
            params->MultiSampleType);
        if (params->MultiSampleType == D3DMULTISAMPLE_NONE) {
            params->SwapEffect = D3DSWAPEFFECT_COPY;
            params->BackBufferCount = 1;
            g_swap_copy = true;
        } else {
            g_swap_copy = false;
        }
        // The window style is reset below; full screen is re-applied next frame.
        g_fullscreen_applied = false;
        // Windowed mode only supports these intervals.
        if (params->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE &&
            params->PresentationInterval != D3DPRESENT_INTERVAL_DEFAULT) {
            params->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        }
        if (params->BackBufferWidth != 0 && params->BackBufferHeight != 0) {
            g_game_w = params->BackBufferWidth;
            g_game_h = params->BackBufferHeight;
            params->BackBufferWidth = kVrRenderWidth;
            params->BackBufferHeight = kVrRenderHeight;
            g_real_w = kVrRenderWidth;
            g_real_h = kVrRenderHeight;
            Log("render size %ux%u (game mode %ux%u)", g_real_w, g_real_h, g_game_w, g_game_h);
        }
        if (params->hDeviceWindow != nullptr) {
            window = params->hDeviceWindow;
        }
        if (window == nullptr) {
            window = g_window;
        }
        g_window = window;
        if (window != nullptr) {
            const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
            SetWindowLongPtrA(window, GWL_STYLE, style);
            SetWindowLongPtrA(window, GWL_EXSTYLE, GetWindowLongPtrA(window, GWL_EXSTYLE) & ~WS_EX_TOPMOST);
            // Client area: the buffer's shape, at most 85% of the monitor's
            // height (Present scales the buffer to it).
            LONG client_w = static_cast<LONG>(params->BackBufferWidth);
            LONG client_h = static_cast<LONG>(params->BackBufferHeight);
            {
                // The monitor it will be on: under the mouse the first time.
                static bool placed_before = false;
                POINT cursor = {};
                GetCursorPos(&cursor);
                MONITORINFO info = {sizeof(info)};
                GetMonitorInfoA(placed_before ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
                                              : MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY),
                                &info);
                placed_before = true;
                const LONG limit = (info.rcWork.bottom - info.rcWork.top) * 85 / 100;
                if (client_h > limit && limit > 100) {
                    client_w = client_w * limit / client_h;
                    client_h = limit;
                }
            }
            RECT frame = {0, 0, client_w, client_h};
            AdjustWindowRect(&frame, style, FALSE);
            const int width = frame.right - frame.left;
            const int height = frame.bottom - frame.top;
            static bool placed = false;
            if (!placed) {
                POINT cursor = {};
                GetCursorPos(&cursor);
                MONITORINFO monitor = {sizeof(monitor)};
                GetMonitorInfoA(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
                const RECT &work = monitor.rcWork;
                const int x = work.left + std::max(0, static_cast<int>(work.right - work.left - width) / 2);
                const int y = work.top + std::max(0, static_cast<int>(work.bottom - work.top - height) / 2);
                SetWindowPos(window, HWND_NOTOPMOST, x, y, width, height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                placed = true;
            } else {
                SetWindowPos(window, HWND_NOTOPMOST, 0, 0, width, height,
                             SWP_NOMOVE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            }
        }
    }

    // Emergency exit: Ctrl+Shift+F12 kills the game from a separate thread, so it
    // works even if the render thread is stuck.
    DWORD WINAPI PanicKeyThread(LPVOID) {
        for (;;) {
            if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                (GetAsyncKeyState(VK_F12) & 0x8000)) {
                Log("panic key pressed: terminating");
                ClipCursor(nullptr);
                TerminateProcess(GetCurrentProcess(), 1);
            }
            Sleep(50);
        }
    }

    HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *params) {
        VrOnDeviceReset();
        ForceWindowed(params, nullptr);
        HRESULT result = g_real_reset(device, params);
        Log("Reset %ux%u windowed=%d -> 0x%08lx", params->BackBufferWidth, params->BackBufferHeight, params->Windowed,
            result);
        return result;
    }

    void HookDevice(IDirect3DDevice9 *device) {
        static bool hooked = false;
        if (hooked) {
            return;
        }
        hooked = true;
        Hook(VTableEntry(device, kDevicePresent), &HookPresent, reinterpret_cast<void **>(&g_real_present),
             "Device::Present");
        Hook(VTableEntry(device, kDeviceReset), &HookReset, reinterpret_cast<void **>(&g_real_reset),
             "Device::Reset");
        Hook(VTableEntry(device, kDeviceSetTransform), &HookSetTransform,
             reinterpret_cast<void **>(&g_real_set_transform), "Device::SetTransform");
        Hook(VTableEntry(device, kDeviceSetVertexShaderConstantF), &HookSetVSConst,
             reinterpret_cast<void **>(&g_real_set_vs_const), "Device::SetVertexShaderConstantF");
        Hook(VTableEntry(device, kDeviceSetViewport), &HookSetViewport, reinterpret_cast<void **>(&g_real_set_viewport),
             "Device::SetViewport");
        g_fps_tick = GetTickCount();
        VrStart();
    }

    HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND window,
                                               DWORD flags, D3DPRESENT_PARAMETERS *params,
                                               IDirect3DDevice9 **device) {
        ForceWindowed(params, window);
        HRESULT result = g_real_create_device(d3d, adapter, type, window, flags, params, device);
        {
            // Diagnostic for desktop blur: is the window DPI-aware (sharp) or
            // being stretched by Windows?
            using GetDpiFn = UINT(WINAPI *)(HWND);
            const auto get_dpi =
                reinterpret_cast<GetDpiFn>(GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow"));
            Log("dpi: process aware=%d, window dpi=%u (96 = unscaled view; 144 at 150%% means sharp)",
                IsProcessDPIAware(), get_dpi != nullptr && window != nullptr ? get_dpi(window) : 0);
        }
        Log("CreateDevice adapter=%u flags=0x%lx %ux%u windowed=%d fmt=%d -> 0x%08lx", adapter, flags,
            params->BackBufferWidth, params->BackBufferHeight, params->Windowed, params->BackBufferFormat, result);
        if (SUCCEEDED(result) && device != nullptr && *device != nullptr) {
            HookDevice(*device);
        }
        return result;
    }
} // namespace

void InstallD3D9Hooks(IDirect3D9 *d3d) {
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;
    CloseHandle(CreateThread(nullptr, 0, &PanicKeyThread, nullptr, 0, nullptr));
    MH_STATUS status = MH_Initialize();
    Log("MinHook init: %s", MH_StatusToString(status));
    InstallCameraHook();
    InstallSkeletonHook();
    InstallVrControlsHook();
    InstallVrInteraction();
    InstallVrForce();
    Hook(VTableEntry(d3d, kD3D9CreateDevice), &HookCreateDevice, reinterpret_cast<void **>(&g_real_create_device),
         "IDirect3D9::CreateDevice");
}
