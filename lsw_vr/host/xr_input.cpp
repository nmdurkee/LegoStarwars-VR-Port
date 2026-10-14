#include "xr_input.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/log.h"

namespace {
    XrPath Path(XrInstance instance, const char *text) {
        XrPath path = XR_NULL_PATH;
        xrStringToPath(instance, text, &path);
        return path;
    }

    VrPose ToVrPose(const XrPosef &pose) {
        return {{pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
                {pose.position.x, pose.position.y, pose.position.z}};
    }

    bool Locate(XrSpace action_space, XrSpace base, XrTime time, VrPose *out) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(action_space, base, time, &location))) {
            return false;
        }
        const XrSpaceLocationFlags needed =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((location.locationFlags & needed) != needed) {
            return false;
        }
        *out = ToVrPose(location.pose);
        return true;
    }
} // namespace

XrInput::~XrInput() {
    Shutdown();
}

void XrInput::Shutdown() {
    for (int hand = 0; hand < 2; ++hand) {
        if (grip_spaces_[hand] != XR_NULL_HANDLE) xrDestroySpace(grip_spaces_[hand]);
        if (aim_spaces_[hand] != XR_NULL_HANDLE) xrDestroySpace(aim_spaces_[hand]);
        grip_spaces_[hand] = aim_spaces_[hand] = XR_NULL_HANDLE;
    }
    if (action_set_ != XR_NULL_HANDLE) xrDestroyActionSet(action_set_);  // also destroys its actions
    action_set_ = XR_NULL_HANDLE;
}

bool XrInput::Init(XrInstance instance, XrSession session) {
    instance_ = instance;
    hand_paths_[0] = Path(instance, "/user/hand/left");
    hand_paths_[1] = Path(instance, "/user/hand/right");

    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(set_info.actionSetName, "gameplay");
    strcpy_s(set_info.localizedActionSetName, "Gameplay");
    if (XR_FAILED(xrCreateActionSet(instance, &set_info, &action_set_))) {
        Log("vr: xrCreateActionSet failed");
        return false;
    }

    auto create = [&](XrAction *action, const char *name, XrActionType type, bool per_hand) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = type;
        strcpy_s(info.actionName, name);
        strcpy_s(info.localizedActionName, name);
        if (per_hand) {
            info.countSubactionPaths = 2;
            info.subactionPaths = hand_paths_;
        }
        return XR_SUCCEEDED(xrCreateAction(action_set_, &info, action));
    };
    const bool created = create(&grip_pose_, "grip_pose", XR_ACTION_TYPE_POSE_INPUT, true) &&
                         create(&aim_pose_, "aim_pose", XR_ACTION_TYPE_POSE_INPUT, true) &&
                         create(&trigger_, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, true) &&
                         create(&squeeze_, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT, true) &&
                         create(&stick_, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true) &&
                         create(&stick_click_, "thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true) &&
                         create(&button_a_, "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
                         create(&button_b_, "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
                         create(&button_x_, "button_x", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
                         create(&button_y_, "button_y", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
                         create(&menu_, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    if (!created) {
        Log("vr: creating input actions failed");
        return false;
    }

    std::vector<XrActionSuggestedBinding> bindings;
    const char *sides[2] = {"left", "right"};
    char path[128];
    for (int hand = 0; hand < 2; ++hand) {
        const struct {
            XrAction action;
            const char *input;
        } per_hand[] = {{grip_pose_, "grip/pose"},     {aim_pose_, "aim/pose"},
                        {trigger_, "trigger/value"},   {squeeze_, "squeeze/value"},
                        {stick_, "thumbstick"},        {stick_click_, "thumbstick/click"}};
        for (const auto &binding : per_hand) {
            sprintf_s(path, "/user/hand/%s/input/%s", sides[hand], binding.input);
            bindings.push_back({binding.action, Path(instance, path)});
        }
    }
    bindings.push_back({button_a_, Path(instance, "/user/hand/right/input/a/click")});
    bindings.push_back({button_b_, Path(instance, "/user/hand/right/input/b/click")});
    bindings.push_back({button_x_, Path(instance, "/user/hand/left/input/x/click")});
    bindings.push_back({button_y_, Path(instance, "/user/hand/left/input/y/click")});
    bindings.push_back({menu_, Path(instance, "/user/hand/left/input/menu/click")});

    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = Path(instance, "/interaction_profiles/oculus/touch_controller");
    suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    suggested.suggestedBindings = bindings.data();
    if (XR_FAILED(xrSuggestInteractionProfileBindings(instance, &suggested))) {
        Log("vr: Touch controller bindings rejected");
        return false;
    }

    for (int hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space_info.subactionPath = hand_paths_[hand];
        space_info.poseInActionSpace.orientation.w = 1.0f;
        space_info.action = grip_pose_;
        xrCreateActionSpace(session, &space_info, &grip_spaces_[hand]);
        space_info.action = aim_pose_;
        xrCreateActionSpace(session, &space_info, &aim_spaces_[hand]);
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &action_set_;
    if (XR_FAILED(xrAttachSessionActionSets(session, &attach))) {
        Log("vr: attaching input actions failed");
        return false;
    }
    return true;
}

void XrInput::Read(XrSession session, XrSpace space, XrTime display_time, bool focused, VrHand hands[2]) {
    memset(hands, 0, 2 * sizeof(VrHand));
    if (!focused || action_set_ == XR_NULL_HANDLE) {
        return;
    }
    XrActiveActionSet active{action_set_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session, &sync))) {
        return;
    }

    auto get_float = [&](XrAction action, XrPath subaction) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = subaction;
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        return XR_SUCCEEDED(xrGetActionStateFloat(session, &info, &state)) && state.isActive ? state.currentState
                                                                                             : 0.0f;
    };
    auto get_bool = [&](XrAction action, XrPath subaction) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = subaction;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(session, &info, &state)) && state.isActive &&
               state.currentState;
    };

    for (int hand = 0; hand < 2; ++hand) {
        VrHand &out = hands[hand];
        out.tracked = Locate(grip_spaces_[hand], space, display_time, &out.grip) &&
                      Locate(aim_spaces_[hand], space, display_time, &out.aim);
        out.trigger = get_float(trigger_, hand_paths_[hand]);
        out.squeeze = get_float(squeeze_, hand_paths_[hand]);
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = stick_;
        info.subactionPath = hand_paths_[hand];
        XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &info, &stick)) && stick.isActive) {
            out.stick_x = stick.currentState.x;
            out.stick_y = stick.currentState.y;
        }
        if (get_bool(stick_click_, hand_paths_[hand])) out.buttons |= kVrButtonStick;
    }
    if (get_bool(button_a_, XR_NULL_PATH)) hands[1].buttons |= kVrButtonA;
    if (get_bool(button_b_, XR_NULL_PATH)) hands[1].buttons |= kVrButtonB;
    if (get_bool(button_x_, XR_NULL_PATH)) hands[0].buttons |= kVrButtonX;
    if (get_bool(button_y_, XR_NULL_PATH)) hands[0].buttons |= kVrButtonY;
    if (get_bool(menu_, XR_NULL_PATH)) hands[0].buttons |= kVrButtonMenu;
}
