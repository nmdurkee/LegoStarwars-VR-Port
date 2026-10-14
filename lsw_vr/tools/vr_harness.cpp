// Standalone test for the VR output path, no game needed: a 32-bit D3D9 window
// (like the game's device: windowed, A8R8G8B8, 1280x800) draws a moving
// pattern and feeds it through the same capture code the mod uses, to
// lsw_vr_host.exe (which must be in the same folder). Run it with the headset
// on; the pattern should appear on a floating screen.
// Exits after the given number of seconds (default 30) or when the window closes.

#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstdlib>

#include "../src/log.h"
#include "../src/vr/vr.h"

namespace {
    constexpr UINT kWidth = 1280;
    constexpr UINT kHeight = 800;

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcA(window, message, wparam, lparam);
    }
} // namespace

int main(int argc, char **argv) {
    const DWORD seconds = argc > 1 ? static_cast<DWORD>(atoi(argv[1])) : 30;

    WNDCLASSA window_class = {};
    window_class.lpfnWndProc = &WindowProc;
    window_class.hInstance = GetModuleHandleA(nullptr);
    window_class.lpszClassName = "vr_harness";
    RegisterClassA(&window_class);
    RECT rect = {0, 0, kWidth, kHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowA("vr_harness", "lsw_vr harness", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                                CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                window_class.hInstance, nullptr);

    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS params = {};
    params.BackBufferWidth = kWidth;
    params.BackBufferHeight = kHeight;
    params.BackBufferFormat = D3DFMT_A8R8G8B8;
    params.BackBufferCount = 1;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = window;
    params.Windowed = TRUE;
    params.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9 *device = nullptr;
    if (d3d == nullptr || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                                   D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device))) {
        std::printf("FAIL: D3D9 device\n");
        return 1;
    }
    Log("harness: D3D9 device ready");
    VrStart();

    const DWORD start = GetTickCount();
    unsigned frame = 0;
    MSG message;
    for (;;) {
        while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                goto done;
            }
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if (GetTickCount() - start > seconds * 1000) {
            break;
        }
        // Dark blue background, a white bar sweeping left to right once per
        // second, and a red square in the top-left corner to check orientation.
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(20, 30, 80), 1.0f, 0);
        const LONG x = static_cast<LONG>((frame % 60) * (kWidth - 80) / 59);
        const D3DRECT bar = {x, 0, x + 80, static_cast<LONG>(kHeight)};
        device->Clear(1, &bar, D3DCLEAR_TARGET, D3DCOLOR_XRGB(240, 240, 240), 1.0f, 0);
        const D3DRECT corner = {40, 40, 200, 200};
        device->Clear(1, &corner, D3DCLEAR_TARGET, D3DCOLOR_XRGB(220, 20, 20), 1.0f, 0);
        VrCaptureFrame(device, VrFrameMeta{});  // no pose: floating screen
        device->Present(nullptr, nullptr, nullptr, nullptr);
        ++frame;
    }
done:
    Log("harness: %u frames in %lu ms", frame, GetTickCount() - start);
    device->Release();
    d3d->Release();
    // The VR thread is still running; ExitProcess ends it.
    ExitProcess(0);
}
