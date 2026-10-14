// lsw_vr_host.exe <game pid>: the 64-bit OpenXR side of the mod. Launched by
// the mod's d3d9.dll; reads frames from the game through the shared frame
// channel and shows them in the headset. Exits when the game exits. If the
// session can't start (no headset, runtime not running) it retries.

#include <windows.h>

#include <cstdlib>

#include "../src/log.h"
#include "../src/shared/frame_channel.h"
#include "xr_screen.h"

namespace {
    constexpr DWORD kRetryMs = 10000;

    volatile bool g_quit = false;
    HANDLE g_game = nullptr;

    DWORD WINAPI WatchGame(LPVOID) {
        WaitForSingleObject(g_game, INFINITE);
        g_quit = true;
        return 0;
    }
} // namespace

int main(int argc, char **argv) {
    LogSetFileName("lsw_vr_host.log");
    const DWORD pid = argc > 1 ? static_cast<DWORD>(strtoul(argv[1], nullptr, 10)) : 0;
    g_game = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (g_game == nullptr) {
        Log("host: game process %lu not found", pid);
        return 1;
    }
    FrameChannel channel;
    if (!channel.Open(pid)) {
        Log("host: frame channel for pid %lu not found", pid);
        return 1;
    }
    Log("host: attached to game pid %lu", pid);
    CloseHandle(CreateThread(nullptr, 0, &WatchGame, nullptr, 0, nullptr));

    while (!g_quit) {
        RunXrScreen(channel, g_quit);
        if (WaitForSingleObject(g_game, kRetryMs) == WAIT_OBJECT_0) {
            break;
        }
    }
    Log("host: game exited");
    return 0;
}
