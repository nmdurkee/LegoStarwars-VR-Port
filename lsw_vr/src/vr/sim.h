#pragma once

#include <cstdint>

#include "../shared/vr_types.h"

// Simulation mode for tuning without a headset: enabled by a file
// lsw_vr_sim.txt next to the game. The mod then
//  - fakes Rift CV1 tracking (per-eye field of view, eye spacing, a head pose
//    set by commands), so the camera takes exactly the VR path;
//  - writes what the headset would show to PNG files on request
//    (sim_left.png, sim_right.png, sim_headset.png = each eye cropped to its
//    real field of view, side by side) plus sim_info.txt;
//  - reads commands from lsw_vr_cmd.txt (one per line, executed in order):
//      press <a|b|x|y|start|up|down|left|right> [frames]
//      stick <x> <y> [frames]      left stick, -1..1, y up
//      look <yaw> <pitch>          head direction in degrees (yaw left +)
//      move <x> <y> <z>            head position offset in meters
//      wait <frames>
//      dump
//      scale <v> | stereo <v> | mono | stereo_on | fp <0|1>
// The host process is not launched in this mode.

// True if simulation mode is on (decided once, at startup).
bool SimActive();

// Synthetic tracking for this frame.
void SimTracking(VrTrackingState *tracking);

// Per Present: runs the command queue. Returns the game buttons to hold
// (names resolved by the caller) via SimButtons/SimStick below.
void SimOnPresent(unsigned frame);
// Buttons requested by the current command, as names (bit per name below).
enum SimButton : unsigned {
    kSimA = 1, kSimB = 2, kSimX = 4, kSimY = 8, kSimStart = 16,
    kSimUp = 32, kSimDown = 64, kSimLeft = 128, kSimRight = 256,
};
unsigned SimButtons();
// Left stick override (-1..1, y up); false if none.
bool SimStick(float *x, float *y);

// A captured game frame (BGRA rows, tightly packed) and its metadata.
void SimOnFrame(const uint8_t *pixels, uint32_t width, uint32_t height, const VrFrameMeta &meta);
