// FrdvFile.cpp — see FrdvFile.h. Operator math mirrors the reference exactly
// (System.Numerics conventions via animmath).
#include "anim/FrdvFile.h"

#include <QtEndian>
#include <cmath>

namespace frdv {

using namespace animmath;

qint16 FrdvFile::Op::s16(int o) const
{
    return o + 2 <= d.size() ? qFromLittleEndian<qint16>(d.constData() + o) : -1;
}
qint32 FrdvFile::Op::i32(int o) const
{
    return o + 4 <= d.size() ? qFromLittleEndian<qint32>(d.constData() + o) : 0;
}
float FrdvFile::Op::f32(int o) const
{
    if (o + 4 > d.size()) return 0.0f;
    const quint32 v = qFromLittleEndian<quint32>(d.constData() + o);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
Vec3 FrdvFile::Op::v3(int o) const
{
    return Vec3(f32(o), f32(o + 4), f32(o + 8));
}

bool FrdvFile::parse(const QByteArray& data)
{
    m_ops.clear();
    if (data.size() < 16 || !data.startsWith("FRDV")) return false;
    const quint32 count = qFromLittleEndian<quint32>(data.constData() + 8);
    if (count == 0 || count > 8192) return false;
    const int tbl = 16;   // 12-byte header, aligned to 16
    for (quint32 e = 0; e < count; ++e) {
        if (tbl + static_cast<qsizetype>(e) * 4 + 4 > data.size()) break;
        const qint32 off =
            qFromLittleEndian<qint32>(data.constData() + tbl + e * 4);
        if (off < 0 || off + 0x60 > data.size()) continue;
        Op op;
        op.d = data.mid(off, qMin<qsizetype>(0x80, data.size() - off));
        m_ops.append(op);
    }
    return !m_ops.isEmpty();
}

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg2Rad = kPi / 180.0f;

Vec3 normOrZero(const Vec3& v)
{
    const float l = v.length();
    return l > 1e-8f ? v / l : Vec3(0, 0, 0);
}

// q^t == Slerp(Identity, q, t); t<0 reverses, shortest path.
Quat quatPow(Quat q, float t)
{
    q = normalizeQ(q);
    if (q.w < 0.0f) q = Quat{-q.x, -q.y, -q.z, -q.w};
    const float w = qBound(-1.0f, q.w, 1.0f);
    const float s = std::sqrt(qMax(0.0f, 1.0f - w * w));
    if (s < 1e-6f) return Quat{};
    const float angle = std::acos(w) * t;
    const float ns = std::sin(angle) / s;
    return Quat{q.x * ns, q.y * ns, q.z * ns, std::cos(angle)};
}

// swing = rotation taking `axis` to where the local rotation moves it.
Quat swingOf(const Quat& lr, Vec3 axis)
{
    axis = normOrZero(axis);
    return fromTo(axis, rotate(axis, lr));
}

// twist = Conj(Swing) ⊗ lr (fixed-axis decomposition, engine-verified order).
Quat twistOf(const Quat& lr, const Vec3& axis)
{
    return quatMul(conjugate(swingOf(lr, axis)), lr);
}

// signed pitch angle (radians): mirrors fox::animx::PitchAOperator.
float pitchAngle(const Quat& lr, Vec3 a1, Vec3 a2)
{
    a1 = normOrZero(a1);
    a2 = normOrZero(a2);
    const Vec3 ra = rotate(a1, lr);
    const float proj = dot(ra, a2);
    const float perp = -dot(ra, cross(a1, a2));
    float frac =
        std::atan2(std::fabs(proj), std::fabs(perp) + 1e-10f) / (kPi * 0.5f);
    if (proj < 0.0f) frac = -frac;
    const float swing = std::acos(qBound(-1.0f, dot(ra, a1), 1.0f));
    return frac * swing;
}

// signed yaw angle — like pitch but measured 90° around.
float yawAngle(const Quat& lr, Vec3 a1, Vec3 a2)
{
    a1 = normOrZero(a1);
    a2 = normOrZero(a2);
    const Vec3 ra = rotate(a1, lr);
    const float proj = dot(ra, a2);
    const float perp = -dot(ra, cross(a1, a2));
    const float atanV =
        std::atan2(std::fabs(proj), std::fabs(perp) + 1e-10f) / (kPi * 0.5f);
    const float frac = perp < 0.0f ? atanV - 1.0f : 1.0f - atanV;
    const float swing = std::acos(qBound(-1.0f, dot(ra, a1), 1.0f));
    return frac * swing;
}

Quat axisAngleQ(int axis, float rad)
{
    const float s = std::sin(rad * 0.5f), c = std::cos(rad * 0.5f);
    if (axis == 0) return Quat{s, 0, 0, c};
    if (axis == 1) return Quat{0, s, 0, c};
    return Quat{0, 0, s, c};
}

void setComp(Vec3& v, int i, float val)
{
    if (i == 0) v.x = val;
    else if (i == 1) v.y = val;
    else v.z = val;
}

}  // namespace

int FrdvFile::apply(const fox::FmdlFile& model, QVector<Mat4>& animWorld) const
{
    const int count = model.bones().size();
    if (m_ops.isEmpty() || animWorld.size() < count) return 0;

    QVector<Quat> orient(count);
    QVector<Vec3> pos(count);
    for (int i = 0; i < count; ++i) {
        orient[i] = quatFromMatrix(animWorld[i]);
        pos[i] = animWorld[i].translationRow();
    }
    const auto localRot = [&](int s, int sp) {
        return sp < 0 ? orient[s] : quatMul(conjugate(orient[sp]), orient[s]);
    };
    const auto mapB = [&](int skel) { return skel >= 0 && skel < count ? skel : -1; };

    int applied = 0;
    for (const Op& op : m_ops) {
        const int type = op.s16(0);
        const int tgt = mapB(op.s16(2)), src = mapB(op.s16(4)),
                  src2 = mapB(op.s16(6));
        const int tgtP = mapB(op.s16(8)), srcP = mapB(op.s16(0xa)),
                  srcP2 = mapB(op.s16(0xc));
        if (tgt < 0 || tgtP < 0) continue;

        const auto& tb = model.bones()[tgt];
        Vec3 trans(tb.localPos[0], tb.localPos[1], tb.localPos[2]);
        Quat local;
        bool ok = true;

        switch (type) {
        case 2:   // Rot
            if (src < 0) { ok = false; break; }
            local = quatPow(localRot(src, srcP), op.f32(0x10));
            break;
        case 5:   // Bend (swing)
            if (src < 0) { ok = false; break; }
            local = quatPow(swingOf(localRot(src, srcP), op.v3(0x40)), op.f32(0x10));
            break;
        case 7:   // Roll (twist)
            if (src < 0) { ok = false; break; }
            local = quatPow(twistOf(localRot(src, srcP), op.v3(0x40)), op.f32(0x10));
            break;
        case 8: {   // BendRoll
            if (src < 0) { ok = false; break; }
            const Quat lr = localRot(src, srcP);
            const Vec3 ax = op.v3(0x40);
            local = quatMul(quatPow(swingOf(lr, ax), op.f32(0x10)),
                            quatPow(twistOf(lr, ax), op.f32(0x24)));
            break;
        }
        case 9: {   // RotRoll
            if (src < 0) { ok = false; break; }
            const Quat rot = quatPow(localRot(src, srcP), op.f32(0x10));
            const Quat roll = src2 >= 0
                ? quatPow(twistOf(localRot(src2, srcP2), op.v3(0x40)), op.f32(0x24))
                : Quat{};
            local = quatMul(rot, roll);
            break;
        }
        case 22: {   // Mirror
            if (src < 0) { ok = false; break; }
            const Quat l = localRot(src, srcP);
            local = Quat{-l.x, l.y, l.z, -l.w};
            break;
        }
        case 10: {   // PitchL (dot → translation)
            if (src < 0) { ok = false; break; }
            const Vec3 ra = rotate(op.v3(0x40), localRot(src, srcP));
            const float p = qBound(op.f32(0x18),
                                   dot(ra, op.v3(0x50)) * op.f32(0x10),
                                   op.f32(0x1c));
            setComp(trans, op.i32(0x20), p * 0.1f);
            break;
        }
        case 1: {   // RotATrn (rotation magnitude → translation)
            if (src < 0) { ok = false; break; }
            const float ang = std::acos(
                qBound(0.0f, std::fabs(localRot(src, srcP).w), 1.0f));
            const float a = qBound(op.f32(0x18),
                                   ang * (op.f32(0x10) * 360.0f / kPi),
                                   op.f32(0x1c));
            setComp(trans, op.i32(0x20), a * 0.1f);
            break;
        }
        case 3: {   // RotATurnRot
            if (src < 0) { ok = false; break; }
            const Quat lr = localRot(src, srcP);
            local = quatPow(lr, op.f32(0x24));
            const float ang = std::acos(qMin(std::fabs(lr.w), 1.0f));
            const float a = qBound(op.f32(0x18),
                                   ang * (op.f32(0x10) * 360.0f / kPi),
                                   op.f32(0x1c));
            setComp(trans, op.i32(0x20), (a + op.f32(0x14)) * 0.1f);
            break;
        }
        case 4: {   // BendATrn
            if (src < 0) { ok = false; break; }
            const Quat sw = swingOf(localRot(src, srcP), op.v3(0x40));
            const float ang = std::acos(qBound(0.0f, std::fabs(sw.w), 1.0f));
            const float a = qBound(op.f32(0x18),
                                   ang * (op.f32(0x10) * 360.0f / kPi),
                                   op.f32(0x1c));
            setComp(trans, op.i32(0x20), a * 0.1f);
            break;
        }
        case 6: {   // BendATrnBend
            if (src < 0) { ok = false; break; }
            const Quat sw = swingOf(localRot(src, srcP), op.v3(0x40));
            const float ang = std::acos(qBound(0.0f, std::fabs(sw.w), 1.0f));
            const float a = qBound(op.f32(0x18),
                                   ang * (op.f32(0x10) * 360.0f / kPi),
                                   op.f32(0x1c));
            setComp(trans, op.i32(0x20), (a + op.f32(0x14)) * 0.1f);
            local = quatPow(sw, op.f32(0x24));
            break;
        }
        case 11: {   // PitchA (pitch angle → axis rotation)
            if (src < 0) { ok = false; break; }
            float pitch =
                pitchAngle(localRot(src, srcP), op.v3(0x40), op.v3(0x50))
                * op.f32(0x10);
            pitch = qBound(op.f32(0x18) * kDeg2Rad, pitch,
                           op.f32(0x1c) * kDeg2Rad);
            local = axisAngleQ(op.i32(0x20), pitch);
            break;
        }
        case 12: {   // RollPitchL
            if (src < 0) { ok = false; break; }
            const Quat lr = localRot(src, srcP);
            const Vec3 a1 = op.v3(0x40);
            const Vec3 ra = rotate(a1, lr);
            const float p = qBound(op.f32(0x2c),
                                   dot(ra, op.v3(0x50)) * op.f32(0x24),
                                   op.f32(0x30));
            setComp(trans, op.i32(0x34), p * 0.1f);
            local = quatPow(twistOf(lr, a1), op.f32(0x10));
            break;
        }
        case 13: {   // YawAPitchL
            if (src < 0) { ok = false; break; }
            const Quat lr = localRot(src, srcP);
            const Vec3 a1 = op.v3(0x40), a2 = op.v3(0x50);
            const Vec3 ra = rotate(a1, lr);
            const float p = qBound(op.f32(0x2c), dot(ra, a2) * op.f32(0x24),
                                   op.f32(0x30));
            setComp(trans, op.i32(0x34), p * 0.1f);
            float yaw = yawAngle(lr, a1, a2) * op.f32(0x10);
            yaw = qBound(op.f32(0x18) * kDeg2Rad, yaw, op.f32(0x1c) * kDeg2Rad);
            local = axisAngleQ(op.i32(0x20), yaw);
            break;
        }
        case 14: {   // YawAPitchA (blended target direction)
            if (src < 0) { ok = false; break; }
            const Quat lr = localRot(src, srcP);
            const Vec3 a1 = op.v3(0x40), a2 = op.v3(0x50);
            float yaw = yawAngle(lr, a1, a2) * op.f32(0x10);
            yaw = qBound(op.f32(0x18) * kDeg2Rad, yaw, op.f32(0x1c) * kDeg2Rad);
            float pitch = pitchAngle(lr, a1, a2) * op.f32(0x24);
            pitch = qBound(op.f32(0x2c) * kDeg2Rad, pitch,
                           op.f32(0x30) * kDeg2Rad);
            if (op.i32(0x38) == 1) {
                local = quatMul(axisAngleQ(op.i32(0x20), yaw),
                                axisAngleQ(op.i32(0x34), pitch));
                break;
            }
            const float n = std::fabs(yaw) + std::fabs(pitch);
            float sp = 0.0f, sy = 0.0f;
            if (n >= 1e-10f) {
                const float s = std::sin(n);
                sp = std::sin(pitch / n * (kPi * 0.5f)) * s;
                sy = std::sin(yaw / n * (-kPi * 0.5f)) * s;
            }
            float w = std::sqrt(qMax(0.0f, 1.0f - sp * sp - sy * sy));
            if (n >= kPi * 0.5f) w = -w;
            const Vec3 v = a1 * w + a2 * sp + cross(a1, a2) * sy;
            local = fromTo(a1, v);
            break;
        }
        case 15: {   // Dircns (two-point aim constraint, custom write)
            if (src < 0 || src2 < 0) { ok = false; break; }
            pos[tgt] = pos[tgtP] + rotate(trans, orient[tgtP]);
            const Vec3 a = op.v3(0x40), bv = op.v3(0x50);
            const Vec3 p1 = pos[src] + rotate(op.v3(0x60) * 0.1f, orient[src])
                - pos[tgt];
            const Vec3 p2 = pos[src2] + rotate(op.v3(0x70) * 0.1f, orient[src2])
                - pos[tgt];
            if (p1.lengthSq() >= 1e-6f && p2.lengthSq() >= 1e-6f) {
                const Vec3 u = cross(a, bv);
                const Vec3 x = normOrZero(p1);
                const Vec3 wAx = cross(normOrZero(p2), x);
                if (u.lengthSq() >= 1e-8f && wAx.lengthSq() >= 1e-6f
                    && cross(wAx, x).lengthSq() >= 1e-6f) {
                    const Quat q1 = fromTo(normOrZero(a), x);
                    const Vec3 u1 = rotate(normOrZero(u), q1);
                    const Quat q2 = fromTo(u1, normOrZero(wAx));
                    orient[tgt] = normalizeQ(quatMul(q1, q2));
                }
            }
            animWorld[tgt] = mul(Mat4::fromQuat(orient[tgt]),
                                 Mat4::translation(pos[tgt]));
            ++applied;
            continue;   // custom write — skip the tail
        }
        case 17:
        case 18: {   // SwellRot (rotation part; scale side-output skipped)
            if (src < 0) { ok = false; break; }
            const Quat lr = localRot(src, srcP);
            local = quatPow(twistOf(lr, op.v3(0x40)), op.f32(0x10));
            break;
        }
        default:   // 16 (scale-only) + 19-21/23 (material params) + unknown
            ok = false;
            break;
        }
        if (!ok) continue;

        const Quat pOri = orient[tgtP];
        orient[tgt] = normalizeQ(quatMul(pOri, local));
        pos[tgt] = pos[tgtP] + rotate(trans, pOri);
        animWorld[tgt] =
            mul(Mat4::fromQuat(orient[tgt]), Mat4::translation(pos[tgt]));
        ++applied;
    }
    return applied;
}

}  // namespace frdv
