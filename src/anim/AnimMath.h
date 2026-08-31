// AnimMath.h — minimal row-vector / row-major float math matching
// System.Numerics semantics EXACTLY, because the pose solver (AnimPose) is a
// port of C# code written against those conventions:
//   • Mat4 is row-major; transform(v, M) computes v·M (translation in row 3).
//   • mul(A, B) applies A then B (System.Numerics Matrix4x4 operator*).
//   • quatMul(a, b) is System.Numerics' operator* formula.
//   • rotate(v, q) is Vector3.Transform(v, Quaternion).
// Do NOT swap in QMatrix4x4/QQuaternion — their column-vector conventions
// invert every composition in the ported code.
#pragma once
#include <cmath>

#include "fox/GaniAnim.h"   // fox::Quat

namespace animmath {

using fox::Quat;

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
    float lengthSq() const { return x * x + y * y + z * z; }
};

inline float dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline Vec3 norm(const Vec3& v)
{
    const float l = v.length();
    return l > 1e-8f ? v / l : Vec3(0, 1, 0);
}

// Row-major 4x4; row 3 = translation. Identity by default.
struct Mat4 {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    static Mat4 translation(const Vec3& t)
    {
        Mat4 r;
        r.m[3][0] = t.x;
        r.m[3][1] = t.y;
        r.m[3][2] = t.z;
        return r;
    }

    // System.Numerics Matrix4x4.CreateFromQuaternion (row-vector form).
    static Mat4 fromQuat(const Quat& q)
    {
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const float xy = q.x * q.y, wz = q.z * q.w, xz = q.z * q.x;
        const float wy = q.y * q.w, yz = q.y * q.z, wx = q.x * q.w;
        Mat4 r;
        r.m[0][0] = 1.0f - 2.0f * (yy + zz);
        r.m[0][1] = 2.0f * (xy + wz);
        r.m[0][2] = 2.0f * (xz - wy);
        r.m[1][0] = 2.0f * (xy - wz);
        r.m[1][1] = 1.0f - 2.0f * (zz + xx);
        r.m[1][2] = 2.0f * (yz + wx);
        r.m[2][0] = 2.0f * (xz + wy);
        r.m[2][1] = 2.0f * (yz - wx);
        r.m[2][2] = 1.0f - 2.0f * (yy + xx);
        return r;
    }

    Vec3 translationRow() const { return {m[3][0], m[3][1], m[3][2]}; }
};

// v · M (point transform, w = 1).
inline Vec3 transform(const Vec3& v, const Mat4& M)
{
    return {
        v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0] + M.m[3][0],
        v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1] + M.m[3][1],
        v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2] + M.m[3][2],
    };
}

// direction transform (w = 0).
inline Vec3 transformDir(const Vec3& v, const Mat4& M)
{
    return {
        v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0],
        v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1],
        v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2],
    };
}

// A then B (System.Numerics operator*: result = value1 * value2 applies value1 first
// under row-vector transforms).
inline Mat4 mul(const Mat4& A, const Mat4& B)
{
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = A.m[i][0] * B.m[0][j] + A.m[i][1] * B.m[1][j]
                + A.m[i][2] * B.m[2][j] + A.m[i][3] * B.m[3][j];
    return r;
}

// Inverse of a RIGID matrix (rotation + translation, no scale or shear) — the
// only kind the pose solver produces. Transposing the 3x3 and re-projecting the
// translation is exact where a general inverse would spend a determinant and
// lose precision on a matrix that never needed one.
inline Mat4 invertRigid(const Mat4& M)
{
    Mat4 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = M.m[j][i];
    const Vec3 t = M.translationRow();
    r.m[3][0] = -(t.x * r.m[0][0] + t.y * r.m[1][0] + t.z * r.m[2][0]);
    r.m[3][1] = -(t.x * r.m[0][1] + t.y * r.m[1][1] + t.z * r.m[2][1]);
    r.m[3][2] = -(t.x * r.m[0][2] + t.y * r.m[1][2] + t.z * r.m[2][2]);
    return r;
}

// System.Numerics Quaternion operator* (q1 * q2).
inline Quat quatMul(const Quat& q1, const Quat& q2)
{
    const float cx = q1.y * q2.z - q1.z * q2.y;
    const float cy = q1.z * q2.x - q1.x * q2.z;
    const float cz = q1.x * q2.y - q1.y * q2.x;
    const float d = q1.x * q2.x + q1.y * q2.y + q1.z * q2.z;
    Quat r;
    r.x = q1.x * q2.w + q2.x * q1.w + cx;
    r.y = q1.y * q2.w + q2.y * q1.w + cy;
    r.z = q1.z * q2.w + q2.z * q1.w + cz;
    r.w = q1.w * q2.w - d;
    return r;
}

inline Quat conjugate(const Quat& q)
{
    return {-q.x, -q.y, -q.z, q.w};
}

inline Quat normalizeQ(const Quat& q)
{
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-12f) return Quat{};
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}

// Vector3.Transform(v, q): v' = v + 2·cross(q.xyz, cross(q.xyz, v) + w·v).
inline Vec3 rotate(const Vec3& v, const Quat& q)
{
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 c1 = cross(u, v) + v * q.w;
    const Vec3 c2 = cross(u, c1);
    return v + c2 * 2.0f;
}

// Quaternion.CreateFromRotationMatrix (row-vector matrix), normalized.
Quat quatFromMatrix(const Mat4& M);

// shortest-arc a→b (both unit).
Quat fromTo(Vec3 a, Vec3 b);

Vec3 perpendicularTo(const Vec3& v);

}  // namespace animmath
