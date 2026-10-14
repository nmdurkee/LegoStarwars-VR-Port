#pragma once

#include <cmath>
#include <cstring>

// 4x4 float matrices in the engine's row-vector convention: rows 0-2 are the
// axes, row 3 the translation, and a * b applies a first, then b.

// out = a * b (out may alias a or b).
inline void Mat4Mul(float *out, const float *a, const float *b) {
    float r[16];
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            r[row * 4 + col] = a[row * 4 + 0] * b[0 * 4 + col] + a[row * 4 + 1] * b[1 * 4 + col] +
                               a[row * 4 + 2] * b[2 * 4 + col] + a[row * 4 + 3] * b[3 * 4 + col];
        }
    }
    memcpy(out, r, sizeof(r));
}

// Inverse of an affine matrix (the 3x3 part may include scale). Returns false
// if it is singular.
inline bool Mat4InverseAffine(float *out, const float *m) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-12f) {
        return false;
    }
    const float k = 1.0f / det;
    float r[16] = {(e * i - f * h) * k, (c * h - b * i) * k, (b * f - c * e) * k, 0.0f,
                   (f * g - d * i) * k, (a * i - c * g) * k, (c * d - a * f) * k, 0.0f,
                   (d * h - e * g) * k, (b * g - a * h) * k, (a * e - b * d) * k, 0.0f,
                   0.0f,               0.0f,               0.0f,               1.0f};
    for (int col = 0; col < 3; ++col) {
        r[12 + col] = -(m[12] * r[0 * 4 + col] + m[13] * r[1 * 4 + col] + m[14] * r[2 * 4 + col]);
    }
    memcpy(out, r, sizeof(r));
    return true;
}
