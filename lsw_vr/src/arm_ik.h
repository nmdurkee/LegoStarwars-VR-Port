#pragma once

// Arm IK math on the engine's row-vector matrices (rows: axes, translation;
// v' = v * M). Pure functions, unit-tested without the game.

struct Vec3f {
    float x, y, z;
};

// Two-bone IK: shoulder s, current elbow e and hand h fix the bone lengths;
// returns the elbow position that puts the hand on `target` (clamped to the
// arm's reach), bending toward `pole` (a direction from the shoulder).
// *reached gets the hand position actually reached.
Vec3f SolveTwoBone(Vec3f s, Vec3f e, Vec3f h, Vec3f target, Vec3f pole, Vec3f *reached);

// Rotation (4x4, row-vector) turning direction a onto direction b by the
// shortest arc.
void RotationBetween(float out[16], Vec3f a, Vec3f b);

// The rigid transform that rotates by r (4x4 rotation) about point p:
// out = T(-p) * r * T(p).
void RotationAbout(float out[16], const float r[16], Vec3f p);

// Orientation (3x3 in a 4x4, translation zero) mapping a local frame onto a
// target frame: local_forward -> target_forward exactly, and local_up as
// close to target_up as the first constraint allows.
void AlignFrames(float out[16], Vec3f local_forward, Vec3f local_up, Vec3f target_forward, Vec3f target_up);

// Vector helpers.
Vec3f TransformPoint(Vec3f v, const float m[16]);      // v * m (with translation)
Vec3f TransformDirection(Vec3f v, const float m[16]);  // v * m (no translation)
Vec3f Normalize(Vec3f v);
