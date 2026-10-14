#include "arm_ik.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "mat4.h"

namespace {
    Vec3f Add(Vec3f a, Vec3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    Vec3f Sub(Vec3f a, Vec3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    Vec3f Scale(Vec3f a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    float Dot(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3f Cross(Vec3f a, Vec3f b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    float Length(Vec3f a) { return std::sqrt(Dot(a, a)); }

    // Any unit vector perpendicular to unit vector v.
    Vec3f AnyPerpendicular(Vec3f v) {
        const Vec3f axis = std::fabs(v.x) < 0.9f ? Vec3f{1, 0, 0} : Vec3f{0, 1, 0};
        return Normalize(Cross(v, axis));
    }

    void SetRows(float out[16], Vec3f r0, Vec3f r1, Vec3f r2) {
        const float m[16] = {r0.x, r0.y, r0.z, 0, r1.x, r1.y, r1.z, 0, r2.x, r2.y, r2.z, 0, 0, 0, 0, 1};
        memcpy(out, m, sizeof(m));
    }
} // namespace

Vec3f Normalize(Vec3f v) {
    const float length = Length(v);
    return length > 1e-8f ? Scale(v, 1.0f / length) : Vec3f{0, 0, 0};
}

Vec3f TransformPoint(Vec3f v, const float m[16]) {
    return {v.x * m[0] + v.y * m[4] + v.z * m[8] + m[12], v.x * m[1] + v.y * m[5] + v.z * m[9] + m[13],
            v.x * m[2] + v.y * m[6] + v.z * m[10] + m[14]};
}

Vec3f TransformDirection(Vec3f v, const float m[16]) {
    return {v.x * m[0] + v.y * m[4] + v.z * m[8], v.x * m[1] + v.y * m[5] + v.z * m[9],
            v.x * m[2] + v.y * m[6] + v.z * m[10]};
}

Vec3f SolveTwoBone(Vec3f s, Vec3f e, Vec3f h, Vec3f target, Vec3f pole, Vec3f *reached) {
    const float upper = Length(Sub(e, s));
    const float lower = Length(Sub(h, e));
    Vec3f to_target = Sub(target, s);
    float distance = Length(to_target);
    const Vec3f dir = distance > 1e-6f ? Scale(to_target, 1.0f / distance) : Normalize(Sub(h, s));
    // Keep inside the reachable shell (never fully straight: the bend plane
    // would become undefined and the elbow would snap).
    const float max_reach = (upper + lower) * 0.999f;
    const float min_reach = std::fabs(upper - lower) * 1.001f + 1e-5f;
    distance = std::clamp(distance, min_reach, max_reach);

    // Distance from the shoulder, along dir, to the foot of the elbow.
    const float along = (upper * upper + distance * distance - lower * lower) / (2.0f * distance);
    const float out = std::sqrt(std::max(upper * upper - along * along, 0.0f));
    Vec3f bend = Sub(pole, Scale(dir, Dot(pole, dir)));
    bend = Length(bend) > 1e-6f ? Normalize(bend) : AnyPerpendicular(dir);

    *reached = Add(s, Scale(dir, distance));
    return Add(Add(s, Scale(dir, along)), Scale(bend, out));
}

void RotationBetween(float out[16], Vec3f a, Vec3f b) {
    a = Normalize(a);
    b = Normalize(b);
    const float c = Dot(a, b);
    // Identity for a zero-length input (coincident joints, target on a joint)
    // rather than collapsing everything below to a point.
    if (c > 0.99999f || Length(a) == 0.0f || Length(b) == 0.0f) {
        SetRows(out, {1, 0, 0}, {0, 1, 0}, {0, 0, 1});
        return;
    }
    // Rodrigues, axis k = a x b / |a x b|. For opposite vectors use any
    // perpendicular axis (a half turn).
    const Vec3f k = c < -0.99999f ? AnyPerpendicular(a) : Normalize(Cross(a, b));
    const float angle = std::acos(std::clamp(c, -1.0f, 1.0f));
    const float sn = std::sin(angle), cs = std::cos(angle), t = 1.0f - cs;
    // Column-vector rotation matrix R; the row-vector form is its transpose.
    const float r[3][3] = {{t * k.x * k.x + cs, t * k.x * k.y - sn * k.z, t * k.x * k.z + sn * k.y},
                           {t * k.x * k.y + sn * k.z, t * k.y * k.y + cs, t * k.y * k.z - sn * k.x},
                           {t * k.x * k.z - sn * k.y, t * k.y * k.z + sn * k.x, t * k.z * k.z + cs}};
    SetRows(out, {r[0][0], r[1][0], r[2][0]}, {r[0][1], r[1][1], r[2][1]}, {r[0][2], r[1][2], r[2][2]});
}

void RotationAbout(float out[16], const float r[16], Vec3f p) {
    const float to_origin[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -p.x, -p.y, -p.z, 1};
    const float back[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, p.x, p.y, p.z, 1};
    float tmp[16];
    Mat4Mul(tmp, to_origin, r);
    Mat4Mul(out, tmp, back);
}

void AlignFrames(float out[16], Vec3f local_forward, Vec3f local_up, Vec3f target_forward, Vec3f target_up) {
    // Orthonormal frames (forward, up, side) in both spaces.
    const Vec3f lf = Normalize(local_forward);
    Vec3f lu = Sub(local_up, Scale(lf, Dot(local_up, lf)));
    lu = Length(lu) > 1e-6f ? Normalize(lu) : AnyPerpendicular(lf);
    const Vec3f ls = Cross(lf, lu);
    const Vec3f tf = Normalize(target_forward);
    Vec3f tu = Sub(target_up, Scale(tf, Dot(target_up, tf)));
    tu = Length(tu) > 1e-6f ? Normalize(tu) : AnyPerpendicular(tf);
    const Vec3f ts = Cross(tf, tu);
    // Row-vector: v_target = v_local * Q with Q = L^T * T (L, T: rows are the
    // frame axes; L is orthonormal so its inverse is its transpose).
    float l_t[16], t[16];
    SetRows(l_t, {lf.x, lu.x, ls.x}, {lf.y, lu.y, ls.y}, {lf.z, lu.z, ls.z});
    SetRows(t, tf, tu, ts);
    Mat4Mul(out, l_t, t);
}
