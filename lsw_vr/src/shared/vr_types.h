#pragma once

#include <cstdint>

// Plain data exchanged between the 32-bit mod and the 64-bit host through the
// frame channel: fixed-size fields only, so the layout matches in both.

// A pose in the host's OpenXR LOCAL space (right-handed, meters; -Z forward).
struct VrPose {
    float orientation[4];  // quaternion x, y, z, w
    float position[3];
};

// Field of view as tangents of the half angles (left/down are negative).
struct VrFovTangents {
    float left;
    float right;
    float up;
    float down;
};

// Host -> game: the latest head pose and the headset's field of view (the
// union of both eyes).
struct VrHeadState {
    VrPose pose;
    VrFovTangents fov;
};

// Touch controller buttons (VrHand::buttons bits).
enum VrButton : uint32_t {
    kVrButtonA = 1u << 0,  // right hand
    kVrButtonB = 1u << 1,  // right hand
    kVrButtonX = 1u << 2,  // left hand
    kVrButtonY = 1u << 3,  // left hand
    kVrButtonMenu = 1u << 4,
    kVrButtonStick = 1u << 5,  // thumbstick click
};

// One controller. Poses are in the same LOCAL space as the head. The grip
// pose sits in the palm (where a held saber's hilt goes); the aim pose points
// forward out of the controller.
struct VrHand {
    int32_t tracked;  // poses valid this frame
    VrPose grip;
    VrPose aim;
    float trigger;  // 0..1
    float squeeze;  // grip button, 0..1
    float stick_x;  // -1..1
    float stick_y;  // -1..1, up positive
    uint32_t buttons;
};

// Host -> game, every headset frame: head and both hands (0 = left, 1 = right).
struct VrTrackingState {
    VrHeadState head;
    VrHand hands[2];
    uint32_t host_tick;  // GetTickCount() when written (system-wide), to detect a stalled host
    VrPose eyes[2];      // each eye's pose (0 left, 1 right), for stereo rendering
};

// Game -> host, per captured frame: the pose and field of view the frame was
// rendered with, so the host can show it world-locked (projection layer).
// has_pose: 0 = plain floating screen (menus, head tracking off), 1 = mono
// (one image for both eyes, rendered from the head), 2 / 3 = the left / right
// eye's image (stereo by alternating eyes), rendered from that eye.
enum VrFrameKind : int32_t {
    kVrFrameScreen = 0,
    kVrFrameMono = 1,
    kVrFrameLeftEye = 2,
    kVrFrameRightEye = 3,
};

struct VrFrameMeta {
    int32_t has_pose;
    VrPose pose;
    VrFovTangents fov;
};

static_assert(sizeof(VrPose) == 28 && sizeof(VrFovTangents) == 16 && sizeof(VrHeadState) == 44 &&
                  sizeof(VrFrameMeta) == 48 && sizeof(VrHand) == 80 && sizeof(VrTrackingState) == 264,
              "VR shared types must have the same layout in 32- and 64-bit builds");
