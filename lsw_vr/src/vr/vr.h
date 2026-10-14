#pragma once

#include "../shared/vr_types.h"

struct IDirect3DDevice9;

// VR output, stage 1 ("big screen"): the game's frame is shown on a floating
// screen in the headset. OpenXR runs in lsw_vr_host.exe, a separate 64-bit
// process (Meta's 32-bit OpenXR runtime crashes in xrCreateSession, and a
// separate process keeps runtime crashes out of the game). Frames go to it
// through shared memory (src/shared/frame_channel.h).

// Creates the frame channel and launches lsw_vr_host.exe from the mod's folder
// (idempotent). Without the host, VR is simply off.
void VrStart();

// Called from Present, before anything is drawn over the frame: copies the
// back buffer for the headset, with the pose/field of view it was rendered
// with. Does nothing until the host wants frames.
void VrCaptureFrame(IDirect3DDevice9 *device, const VrFrameMeta &meta);

// Called before IDirect3DDevice9::Reset, which requires default-pool
// resources to be released.
void VrOnDeviceReset();
