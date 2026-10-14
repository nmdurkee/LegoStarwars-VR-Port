// Coordinate conventions:
//   OpenXR (host): right-handed, +Y up, -Z forward, meters.
//   Game: left-handed, +Y up, +Z forward; camera matrices are row-vector with
//   rows right/up/forward/position. Yaw a turns forward (0,0,1) into
//   (sin a, 0, cos a) (opensaga's NuVecRotateY).
// An OpenXR vector maps to the game by negating Z.
//
// Tracking space -> game world: take the point relative to the recentered
// origin, turn it from the recentered yaw to the game camera's yaw, scale it
// to game units, and add the game camera's position. Directions only turn.

#include "head_tracking.h"

#include <algorithm>
#include <cmath>

namespace {
    struct Vec3 {
        float x, y, z;
    };

    // Rotates v by quaternion q (x, y, z, w).
    Vec3 Rotate(const float *q, Vec3 v) {
        const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
        // t = 2 * cross(q.xyz, v); v' = v + w * t + cross(q.xyz, t)
        const float tx = 2.0f * (qy * v.z - qz * v.y);
        const float ty = 2.0f * (qz * v.x - qx * v.z);
        const float tz = 2.0f * (qx * v.y - qy * v.x);
        return {v.x + qw * tx + (qy * tz - qz * ty), v.y + qw * ty + (qz * tx - qx * tz),
                v.z + qw * tz + (qx * ty - qy * tx)};
    }

    Vec3 ToGame(Vec3 v) {
        return {v.x, v.y, -v.z};
    }

    // Rotation about the game's Y axis by yaw (radians).
    Vec3 RotateYaw(Vec3 v, float yaw) {
        const float c = std::cos(yaw);
        const float s = std::sin(yaw);
        return {v.x * c + v.z * s, v.y, v.z * c - v.x * s};
    }

    float YawOf(Vec3 forward) {
        return std::atan2(forward.x, forward.z);
    }

    // A pose's axes in game handedness (tracking space, not recentered).
    void PoseAxes(const VrPose &pose, Vec3 axes[3]) {
        axes[0] = ToGame(Rotate(pose.orientation, {1.0f, 0.0f, 0.0f}));   // right
        axes[1] = ToGame(Rotate(pose.orientation, {0.0f, 1.0f, 0.0f}));   // up
        axes[2] = ToGame(Rotate(pose.orientation, {0.0f, 0.0f, -1.0f}));  // forward
    }

    Vec3 PosePosition(const VrPose &pose) {
        return ToGame({pose.position[0], pose.position[1], pose.position[2]});
    }
} // namespace

void HeadTracker::Latch(const VrHeadState *head) {
    has_pose_ = head != nullptr;
    if (has_pose_) {
        head_ = *head;
        if (!centered_) {
            Recenter();
        }
    }
}

void HeadTracker::Recenter() {
    if (!has_pose_) {
        return;
    }
    Vec3 axes[3];
    PoseAxes(head_.pose, axes);
    center_yaw_ = YawOf(axes[2]);
    const Vec3 position = PosePosition(head_.pose);
    center_position_[0] = position.x;
    center_position_[1] = position.y;
    center_position_[2] = position.z;
    centered_ = true;
}

void HeadTracker::Apply(float *m) {
    Apply(m, head_.pose);
}

void HeadTracker::Apply(float *m, const VrPose &view) {
    if (!has_pose_) {
        return;
    }
    // The game camera contributes its yaw and position only.
    base_yaw_ = YawOf({m[8], m[9], m[10]});
    base_position_[0] = m[12];
    base_position_[1] = m[13];
    base_position_[2] = m[14];
    has_base_ = true;

    float position[3];
    float axes[9];
    ToWorld(view, position, axes);
    for (int row = 0; row < 3; ++row) {
        m[row * 4 + 0] = axes[row * 3 + 0];
        m[row * 4 + 1] = axes[row * 3 + 1];
        m[row * 4 + 2] = axes[row * 3 + 2];
        m[row * 4 + 3] = 0.0f;
    }
    m[12] = position[0];
    m[13] = position[1];
    m[14] = position[2];
    m[15] = 1.0f;
}

bool HeadTracker::ToWorld(const VrPose &pose, float position[3], float axes[9]) const {
    if (!has_base_) {
        return false;
    }
    const float turn = base_yaw_ - center_yaw_;
    const Vec3 p = PosePosition(pose);
    const Vec3 offset = RotateYaw({p.x - center_position_[0], p.y - center_position_[1], p.z - center_position_[2]},
                                  turn);
    position[0] = base_position_[0] + offset.x * scale_;
    position[1] = base_position_[1] + offset.y * scale_;
    position[2] = base_position_[2] + offset.z * scale_;
    Vec3 pose_axes[3];
    PoseAxes(pose, pose_axes);
    for (int row = 0; row < 3; ++row) {
        const Vec3 axis = RotateYaw(pose_axes[row], turn);
        axes[row * 3 + 0] = axis.x;
        axes[row * 3 + 1] = axis.y;
        axes[row * 3 + 2] = axis.z;
    }
    return true;
}

bool HeadTracker::HeadOffset(float base_yaw, float offset[3]) const {
    if (!has_pose_ || !centered_) {
        return false;
    }
    const Vec3 p = PosePosition(head_.pose);
    const Vec3 o = RotateYaw({p.x - center_position_[0], p.y - center_position_[1], p.z - center_position_[2]},
                             base_yaw - center_yaw_);
    offset[0] = o.x * scale_;
    offset[1] = o.y * scale_;
    offset[2] = o.z * scale_;
    return true;
}

bool HeadTracker::Fov(float *fov_radians, float *aspect, VrFovTangents *tangents) const {
    // The game's projection is symmetric: cover the larger side of each axis.
    const float tan_h = std::max(head_.fov.right, -head_.fov.left);
    const float tan_v = std::max(head_.fov.up, -head_.fov.down);
    // Real headsets are roughly 0.5..3 per side; anything else is bad data,
    // which must not reach the game camera as inf/NaN.
    if (!std::isfinite(tan_h) || !std::isfinite(tan_v) || tan_h < 0.1f || tan_v < 0.1f || tan_h > 10.0f ||
        tan_v > 10.0f) {
        return false;
    }
    *fov_radians = 2.0f * std::atan(tan_v);
    *aspect = tan_v / tan_h;
    *tangents = {-tan_h, tan_h, tan_v, -tan_v};
    return true;
}
