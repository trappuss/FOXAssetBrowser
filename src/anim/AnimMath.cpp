#include "anim/AnimMath.h"

namespace animmath {

Quat quatFromMatrix(const Mat4& M)
{
    // System.Numerics Quaternion.CreateFromRotationMatrix, adapted to our
    // row-major storage (same element roles).
    const float trace = M.m[0][0] + M.m[1][1] + M.m[2][2];
    Quat q;
    if (trace > 0.0f) {
        float s = std::sqrt(trace + 1.0f);
        q.w = s * 0.5f;
        s = 0.5f / s;
        q.x = (M.m[1][2] - M.m[2][1]) * s;
        q.y = (M.m[2][0] - M.m[0][2]) * s;
        q.z = (M.m[0][1] - M.m[1][0]) * s;
    } else if (M.m[0][0] >= M.m[1][1] && M.m[0][0] >= M.m[2][2]) {
        float s = std::sqrt(1.0f + M.m[0][0] - M.m[1][1] - M.m[2][2]);
        const float invS = 0.5f / s;
        q.x = 0.5f * s;
        q.y = (M.m[0][1] + M.m[1][0]) * invS;
        q.z = (M.m[0][2] + M.m[2][0]) * invS;
        q.w = (M.m[1][2] - M.m[2][1]) * invS;
    } else if (M.m[1][1] > M.m[2][2]) {
        float s = std::sqrt(1.0f + M.m[1][1] - M.m[0][0] - M.m[2][2]);
        const float invS = 0.5f / s;
        q.x = (M.m[1][0] + M.m[0][1]) * invS;
        q.y = 0.5f * s;
        q.z = (M.m[2][1] + M.m[1][2]) * invS;
        q.w = (M.m[2][0] - M.m[0][2]) * invS;
    } else {
        float s = std::sqrt(1.0f + M.m[2][2] - M.m[0][0] - M.m[1][1]);
        const float invS = 0.5f / s;
        q.x = (M.m[2][0] + M.m[0][2]) * invS;
        q.y = (M.m[2][1] + M.m[1][2]) * invS;
        q.z = 0.5f * s;
        q.w = (M.m[0][1] - M.m[1][0]) * invS;
    }
    return normalizeQ(q);
}

Vec3 perpendicularTo(const Vec3& v)
{
    Vec3 c = cross(v, Vec3(1, 0, 0));
    if (c.lengthSq() < 1e-6f) c = cross(v, Vec3(0, 1, 0));
    return norm(c);
}

Quat fromTo(Vec3 a, Vec3 b)
{
    // Degenerate inputs → identity (matches the reference's zero-vector path).
    if (a.lengthSq() <= 1e-16f || b.lengthSq() <= 1e-16f) return Quat{};
    a = norm(a);
    b = norm(b);
    const float d = dot(a, b);
    if (d >= 1.0f - 1e-6f) return Quat{};
    if (d <= -1.0f + 1e-6f) {
        const Vec3 axis = perpendicularTo(a);
        // 180° about axis.
        return Quat{axis.x, axis.y, axis.z, 0.0f};
    }
    const Vec3 c = cross(a, b);
    return normalizeQ(Quat{c.x, c.y, c.z, 1.0f + d});
}

}  // namespace animmath
