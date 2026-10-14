#include "vr_frame.h"

#include "../log.h"
#include "../shared/frame_channel.h"
#include "sim.h"
#include "vr_channel.h"

#include <windows.h>

namespace {
    // Tracking older than this means the host stalled or died: drop it, so
    // nothing (a held button, a pushed stick) stays applied forever.
    constexpr DWORD kStaleMs = 150;
    // A read can collide with the host writing; keep the last good state for
    // a few frames instead of dropping tracking for one.
    constexpr int kKeepOnFailedRead = 3;

    VrTrackingState g_tracking = {};
    bool g_valid = false;
    int g_failed_reads = 0;
} // namespace

void VrLatchTracking() {
    if (SimActive()) {
        SimTracking(&g_tracking);
        g_valid = true;
        return;
    }
    const FrameChannel *channel = VrChannel();
    const bool was_valid = g_valid;
    VrTrackingState read;
    if (channel != nullptr && channel->ReadTracking(&read)) {
        g_tracking = read;
        g_valid = true;
        g_failed_reads = 0;
    } else if (channel != nullptr && was_valid && ++g_failed_reads <= kKeepOnFailedRead) {
        // keep the previous state
    } else {
        g_valid = false;
    }
    if (g_valid && GetTickCount() - g_tracking.host_tick > kStaleMs) {
        g_valid = false;
    }
    if (g_valid != was_valid) {
        Log("vr: tracking %s", g_valid ? "available" : "lost");
    }
}

const VrTrackingState *VrTracking() {
    return g_valid ? &g_tracking : nullptr;
}
