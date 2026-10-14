#pragma once

#include "../shared/vr_types.h"

// Head and controller tracking for the current game frame. Latched once per
// frame (at Present) so every system sees the same poses all frame.

// Reads the newest tracking state from lsw_vr_host. Call once per frame.
void VrLatchTracking();

// This frame's tracking, or nullptr when the host has no tracked head.
const VrTrackingState *VrTracking();
