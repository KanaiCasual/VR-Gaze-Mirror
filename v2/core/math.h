// VR Gaze Mirror 2.0 - core: the little geometry the mirror needs, without DirectXMath or OpenXR types.
#pragma once

#include <algorithm>
#include <cmath>

namespace gaze_mirror {

    struct Vec3 {
        float x, y, z;
    };
    struct Quat {
        float x, y, z, w; // Same layout as XrQuaternionf and DirectX's XMFLOAT4.
    };

    // Half-angles of a view, as tangents: what OpenXR's XrFovf and OpenVR's GetProjectionRaw both describe.
    struct FovTan {
        float left, right, up, down; // left/down negative in the usual case.
    };

    // One eye's image: its size in pixels and the head orientation (world <- view) it was rendered with.
    struct EyeView {
        float width = 0, height = 0;
        FovTan fov{};
        Quat orientation{0, 0, 0, 1};
        bool valid = false;
    };

    inline Vec3 Rotate(const Quat& q, const Vec3& v) {
        const Vec3 u{q.x, q.y, q.z};
        const Vec3 c1{u.y * v.z - u.z * v.y + q.w * v.x, u.z * v.x - u.x * v.z + q.w * v.y, u.x * v.y - u.y * v.x + q.w * v.z};
        const Vec3 c2{u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z, u.x * c1.y - u.y * c1.x};
        return {v.x + 2 * c2.x, v.y + 2 * c2.y, v.z + 2 * c2.z};
    }
    inline Quat Conjugate(const Quat& q) {
        return {-q.x, -q.y, -q.z, q.w};
    }
    inline Vec3 InverseRotate(const Quat& q, const Vec3& v) {
        return Rotate(Conjugate(q), v);
    }
    inline Quat Normalize(Quat q) {
        const float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (n < 1e-8f) return {0, 0, 0, 1};
        return {q.x / n, q.y / n, q.z / n, q.w / n};
    }
    // The orientation whose axes are these unit vectors (x right, y up, z back), from the rotation matrix they form.
    inline Quat QuatFromAxes(const Vec3& x, const Vec3& y, const Vec3& z) {
        const float m00 = x.x, m01 = y.x, m02 = z.x, m10 = x.y, m11 = y.y, m12 = z.y, m20 = x.z, m21 = y.z, m22 = z.z;
        const float trace = m00 + m11 + m22;
        if (trace > 0.f) {
            const float s = sqrtf(trace + 1.f) * 2.f;
            return Normalize({(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s});
        }
        if (m00 > m11 && m00 > m22) {
            const float s = sqrtf(1.f + m00 - m11 - m22) * 2.f;
            return Normalize({0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s});
        }
        if (m11 > m22) {
            const float s = sqrtf(1.f + m11 - m00 - m22) * 2.f;
            return Normalize({(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s});
        }
        const float s = sqrtf(1.f + m22 - m00 - m11) * 2.f;
        return Normalize({(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s});
    }
    // The same forward direction with the world's up (+y) as up: the orientation without its sideways tilt. Looking almost
    // straight up or down there is no level, and the orientation stays as it is.
    inline Quat Level(const Quat& q) {
        const Vec3 f = Rotate(q, {0.f, 0.f, -1.f});
        if (fabsf(f.y) > 0.995f) return q;
        const float rightLength = sqrtf(f.z * f.z + f.x * f.x);
        const Vec3 right{-f.z / rightLength, 0.f, f.x / rightLength}; // forward x up
        const Vec3 up{right.y * f.z - right.z * f.y, right.z * f.x - right.x * f.z, right.x * f.y - right.y * f.x}; // right x forward
        return QuatFromAxes(right, up, {-f.x, -f.y, -f.z});
    }
    inline float Dot(const Quat& a, const Quat& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }
    // The angle between two orientations, radians.
    inline float AngleBetween(const Quat& a, const Quat& b) {
        return 2.f * acosf(std::min(fabsf(Dot(a, b)), 1.f));
    }
    inline Quat Slerp(const Quat& a, Quat b, float t) {
        float d = Dot(a, b);
        if (d < 0) {
            b = {-b.x, -b.y, -b.z, -b.w};
            d = -d;
        }
        if (d > 0.9995f) return Normalize({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
        const float theta = acosf(d), s = sinf(theta), wa = sinf((1 - t) * theta) / s, wb = sinf(t * theta) / s;
        return Normalize({a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb});
    }

    // A direction in view space -> where it lands in the eye image (pixels). False when it points away from the image.
    inline bool ProjectDirection(const EyeView& view, const Vec3& inEye, float& x, float& y) {
        const float depth = -inEye.z;
        const float w = view.fov.right - view.fov.left, h = view.fov.up - view.fov.down;
        if (depth < 0.0001f || fabsf(w) < 0.0001f || fabsf(h) < 0.0001f) return false;
        const float u = (inEye.x / depth - view.fov.left) / w;
        const float v = (view.fov.up - inEye.y / depth) / h;
        if (!std::isfinite(u) || !std::isfinite(v)) return false;
        x = u * view.width;
        y = v * view.height;
        return true;
    }

    // Where a point of the image (pixels) seen under head orientation `from` shows up under `to`. Rotation only: the
    // point is treated as infinitely far away, which is what makes motion relative to the world instead of the head.
    inline bool Reproject(const EyeView& view, const Quat& from, const Quat& to, float& x, float& y) {
        const float w = view.fov.right - view.fov.left, h = view.fov.up - view.fov.down;
        if (view.width < 1.f || view.height < 1.f || fabsf(w) < 0.0001f || fabsf(h) < 0.0001f) return false;
        const float u = x / view.width, v = y / view.height;
        const Vec3 previous{view.fov.left + w * u, view.fov.up - h * v, -1.f};
        const Vec3 current = InverseRotate(to, Rotate(from, previous));
        if (-current.z < 0.05f) return false;
        return ProjectDirection(view, current, x, y);
    }

} // namespace gaze_mirror
