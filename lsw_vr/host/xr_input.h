#pragma once

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "../src/shared/vr_types.h"

// Touch controller input through an OpenXR action set: grip/aim poses,
// trigger, grip button, thumbstick and face buttons for both hands.
class XrInput {
public:
    ~XrInput();

    // Destroys the action spaces and action set; must run before the session
    // and instance are destroyed. Idempotent.
    void Shutdown();

    // Creates the actions, suggests Oculus Touch bindings and attaches the
    // action set to the session. Call once, after xrCreateSession.
    bool Init(XrInstance instance, XrSession session);

    // Reads both hands for display_time (poses in `space`). Hands are left
    // untracked unless the session is focused (OpenXR only delivers input then).
    void Read(XrSession session, XrSpace space, XrTime display_time, bool focused, VrHand hands[2]);

private:
    XrInstance instance_ = XR_NULL_HANDLE;
    XrActionSet action_set_ = XR_NULL_HANDLE;
    XrPath hand_paths_[2] = {};
    XrAction grip_pose_ = XR_NULL_HANDLE;
    XrAction aim_pose_ = XR_NULL_HANDLE;
    XrAction trigger_ = XR_NULL_HANDLE;
    XrAction squeeze_ = XR_NULL_HANDLE;
    XrAction stick_ = XR_NULL_HANDLE;
    XrAction stick_click_ = XR_NULL_HANDLE;
    XrAction button_a_ = XR_NULL_HANDLE;
    XrAction button_b_ = XR_NULL_HANDLE;
    XrAction button_x_ = XR_NULL_HANDLE;
    XrAction button_y_ = XR_NULL_HANDLE;
    XrAction menu_ = XR_NULL_HANDLE;
    XrSpace grip_spaces_[2] = {};
    XrSpace aim_spaces_[2] = {};
};
