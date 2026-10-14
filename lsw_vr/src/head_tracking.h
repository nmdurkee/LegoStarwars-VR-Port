#pragma once

#include "shared/vr_types.h"

// Maps the headset's tracking space onto the game world. The game camera
// keeps only its yaw and position; the head supplies the full rotation, so the
// horizon stays level whatever the game camera's pitch is. Head movement
// offsets the camera position, and controllers map through the same transform,
// so head, hands and saber line up.
class HeadTracker {
public:
    // Game units per real meter. A minifig's eyes are about 0.45 units up, so
    // being minifig-sized means roughly 0.3.
    static constexpr float kDefaultScale = 0.3f;

    // Latches the pose for this game frame (every camera set in the frame uses
    // the same pose). nullptr = no pose available. The first pose recenters.
    void Latch(const VrHeadState *head);

    // Makes the current head direction and position "straight ahead".
    void Recenter();

    bool HasPose() const {
        return has_pose_;
    }

    void SetScale(float game_units_per_meter) {
        scale_ = game_units_per_meter;
    }
    float Scale() const {
        return scale_;
    }

    // Rewrites a game camera matrix (row-vector: right, up, forward, position)
    // and remembers it as the base the tracking space maps onto. The camera
    // goes where `view` is (the head, or one eye for stereo); without it, the
    // head.
    void Apply(float *mtx);
    void Apply(float *mtx, const VrPose &view);

    // Maps a tracked pose (host LOCAL space) into the game world as of the last
    // Apply: position in game units, axes as rows right/up/forward. False until
    // Apply has run with a pose.
    bool ToWorld(const VrPose &pose, float position[3], float axes[9]) const;

    // Where the latched head is relative to the recentered spot, in the game
    // world for a camera facing base_yaw: game units, before any base position
    // (what Apply would add to the camera position). False without a pose.
    bool HeadOffset(float base_yaw, float offset[3]) const;

    // The game camera field of view matching the headset: vertical fov in
    // radians and aspect (= tan(v/2) / tan(h/2), the game's convention), plus
    // the symmetric tangents that describe it for the host.
    // False (outputs untouched) if the headset's field of view is unusable.
    bool Fov(float *fov_radians, float *aspect, VrFovTangents *tangents) const;

    // The latched pose in the host's LOCAL space, for the frame metadata.
    const VrPose &RawPose() const {
        return head_.pose;
    }

private:
    VrHeadState head_ = {};
    bool has_pose_ = false;
    bool centered_ = false;
    float center_yaw_ = 0.0f;
    float center_position_[3] = {};
    float scale_ = kDefaultScale;
    // Game camera yaw and position the tracking origin maps to (last Apply).
    bool has_base_ = false;
    float base_yaw_ = 0.0f;
    float base_position_[3] = {};
};
