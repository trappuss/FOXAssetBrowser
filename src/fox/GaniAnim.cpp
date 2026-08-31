// GaniAnim.cpp — see GaniAnim.h. Control flow mirrors Fox_Parser's
// AnimBitReader / FoxVectormath / GaniFile (which mirror the game).
#include "fox/GaniAnim.h"

#include <QtEndian>
#include <cmath>
#include <cstring>

#include "fox/FoxHash.h"

namespace fox {
namespace {

constexpr float kPi = 3.14159274f;

// ── bit reader (LE bitstream, ≤ 32 bits) ────────────────────────────────────
quint32 readBits(const quint8* buf, qsizetype bufSize, qint64* bitPos, int bitSize)
{
    if (bitSize == 0) return 0;
    const qint64 bytePos = *bitPos >> 3;
    const int bitOffset = static_cast<int>(*bitPos & 7);
    const int totalBytes = (bitOffset + bitSize + 7) >> 3;
    if (bytePos + totalBytes > bufSize) {
        *bitPos += bitSize;
        return 0;
    }
    quint64 raw = 0;
    for (int i = 0; i < totalBytes; ++i)
        raw |= static_cast<quint64>(buf[bytePos + i]) << (8 * i);
    *bitPos += bitSize;
    const quint64 mask = (1ULL << bitSize) - 1;
    return static_cast<quint32>((raw >> bitOffset) & mask);
}

// ── quat dequantization (MT_GetQuatDataFromBuffer) ──────────────────────────
Quat dequantQuat(quint32 a, quint32 b, quint32 c, quint32 signs, int bitSize)
{
    const int maskI = (1 << bitSize) - 1;
    const float fmask = static_cast<float>(maskI);
    const float inv = 1.0f / fmask;
    const float ft = a * inv;
    const float x = b * inv;
    const float y = c * inv;
    const float halfTheta = ft * kPi * 0.5f;
    const float z = (1.0f - x) - y;
    const float lensq = (z * z + y * y) + x * x;
    const float invLen = 1.0f / std::sqrt(lensq);
    const float sx = (signs & 1) ? -1.0f : 1.0f;
    const float sy = (signs & 2) ? -1.0f : 1.0f;
    const float sz = (signs & 4) ? -1.0f : 1.0f;
    const float f = std::sin(halfTheta) * invLen;
    Quat q;
    q.x = x * sx * f;
    q.y = y * sy * f;
    q.z = z * sz * f;
    q.w = std::cos(halfTheta);
    return q;
}

Quat readQuat(const quint8* buf, qsizetype bufSize, qint64* bitPos, int bitSize)
{
    const quint32 a = readBits(buf, bufSize, bitPos, bitSize);
    const quint32 b = readBits(buf, bufSize, bitPos, bitSize);
    const quint32 c = readBits(buf, bufSize, bitPos, bitSize);
    const quint32 signs = readBits(buf, bufSize, bitPos, 3);
    return dequantQuat(a, b, c, signs, bitSize);
}

// AnimHalf → float32 (game's inline expansion).
float readAnimHalf(const quint8* buf, qsizetype* off)
{
    const quint16 value =
        static_cast<quint16>(buf[*off] | (buf[*off + 1] << 8));
    *off += 2;
    quint32 num = value & 0x7C00u;
    if (num > 0) num = (num + 0x1DC00u) << 13;
    num |= (static_cast<quint32>(value & 0x8000u) << 16)
        | (static_cast<quint32>(value & 0x3FFu) << 13);
    float f;
    std::memcpy(&f, &num, 4);
    return f;
}

float readF32(const quint8* buf, qsizetype* off)
{
    float f;
    std::memcpy(&f, buf + *off, 4);
    *off += 4;
    return f;
}

int compCount(GaniSegType t)
{
    switch (t) {
    case GaniSegType::Float: return 1;
    case GaniSegType::Vector2: return 2;
    case GaniSegType::Vector4: return 4;
    default: return 3;
    }
}

void readComps(const quint8* buf, qsizetype* off, int comps, int bits, float out[3])
{
    float v[4] = {0, 0, 0, 0};
    for (int i = 0; i < comps; ++i)
        v[i] = bits == 16 ? readAnimHalf(buf, off) : bits == 0 ? 0.0f : readF32(buf, off);
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
}

// ── slerp / lerp / hermite (game order, float32) ────────────────────────────
Quat slerpGame(float t, const Quat& a, const Quat& b)
{
    float d = ((b.w * a.w + b.z * a.z) + b.y * a.y) + b.x * a.x;
    const bool neg = d < 0.0f;
    const float dsel = neg ? -d : d;
    const float xabs = std::fabs(dsel);
    const bool interp = xabs < 0.999f;

    float scaleA, scaleB;
    if (interp) {
        const float angle = std::acos(qMin(xabs, 1.0f));
        const float s0 = std::sin(angle);
        scaleA = s0 > 1e-6f ? std::sin(angle * (1.0f - t)) / s0 : 1.0f - t;
        scaleB = s0 > 1e-6f ? std::sin(angle * t) / s0 : t;
    } else {
        scaleA = 1.0f - t;
        scaleB = t;
    }
    const float ax = neg ? -a.x : a.x;
    const float ay = neg ? -a.y : a.y;
    const float az = neg ? -a.z : a.z;
    const float aw = neg ? -a.w : a.w;
    Quat r;
    r.x = scaleA * ax + scaleB * b.x;
    r.y = scaleA * ay + scaleB * b.y;
    r.z = scaleA * az + scaleB * b.z;
    r.w = scaleA * aw + scaleB * b.w;
    const float lensq = ((r.w * r.w + r.z * r.z) + r.y * r.y) + r.x * r.x;
    if (lensq > 1e-12f) {
        const float inv = 1.0f / std::sqrt(lensq);
        r.x *= inv;
        r.y *= inv;
        r.z *= inv;
        r.w *= inv;
    }
    return r;
}

void hermiteEval(float t, const float p0[3], const float p1[3], const float m0[3],
                 const float m1[3], float out[3])
{
    const float t2 = t * t, t3 = t2 * t;
    for (int i = 0; i < 3; ++i) {
        const float a = (m1[i] + m0[i]) + (-2.0f * p1[i] + 2.0f * p0[i]);
        const float b = (-m1[i] + -2.0f * m0[i]) + (3.0f * p1[i] + -3.0f * p0[i]);
        out[i] = ((a * t3) + (b * t2)) + (m0[i] * t) + p0[i];
    }
}

}  // namespace

// ── layout parse ────────────────────────────────────────────────────────────
bool parseGaniLayout(const QByteArray& buf, int start, GaniLayout* out)
{
    const char* p = buf.constData();
    if (start + 20 > buf.size()) return false;
    GaniLayout layout;
    layout.unitCount = qFromLittleEndian<qint32>(p + start);
    layout.segmentCount = qFromLittleEndian<qint32>(p + start + 4);
    // +8 u16 trackId, +10 two unknown bytes
    layout.frameCount = qFromLittleEndian<qint32>(p + start + 12);
    layout.frameScale = static_cast<qint8>(p[start + 16]);
    if (layout.frameScale == 0) layout.frameScale = 1;
    if (layout.unitCount <= 0 || layout.unitCount > 4096) return false;
    if (start + 20 + layout.unitCount * 4 > buf.size()) return false;

    for (int i = 0; i < layout.unitCount; ++i) {
        const quint32 unitOff =
            qFromLittleEndian<quint32>(p + start + 20 + i * 4);
        qsizetype at = start + unitOff;
        if (at + 8 > buf.size()) return false;
        GaniLayoutUnit unit;
        unit.nameHash = qFromLittleEndian<quint32>(p + at);
        const int segCount = static_cast<quint8>(p[at + 4]);
        unit.flags = static_cast<quint8>(p[at + 5]);
        at += 8;
        for (int s = 0; s < segCount; ++s) {
            if (at + 8 > buf.size()) return false;
            GaniTrackData td;
            td.recordAt = static_cast<qint32>(at);
            td.dataOffset = qFromLittleEndian<qint32>(p + at);
            td.msId = qFromLittleEndian<qint16>(p + at + 4);
            const quint8 packed = static_cast<quint8>(p[at + 6]);
            td.type = static_cast<GaniSegType>(packed & 0x0F);
            td.componentBitSize = static_cast<quint8>(p[at + 7]);
            unit.segments.append(td);
            at += 8;
        }
        layout.units.append(unit);
    }
    *out = layout;
    return layout.valid();
}

// ── channel sampling ────────────────────────────────────────────────────────
void GaniChannel::walk(float time, int* seg, float* t, bool* ended) const
{
    *seg = 0;
    *t = 0;
    *ended = false;
    const int n = isRot ? quats.size() : vecs.size() / 3;
    if (isStatic || n <= 1 || durations.isEmpty()) return;
    if (time <= durations[0]) {
        *t = time * invDur[0];
        return;
    }
    int i = 0;
    while (true) {
        time -= durations[i];
        if (i + 1 >= durations.size()) {
            *seg = durations.size() - 1;
            *ended = true;
            return;
        }
        ++i;
        if (!(durations[i] < time)) break;
    }
    *seg = i;
    *t = time * invDur[i];
}

Quat GaniChannel::sampleRot(float frame) const
{
    const int n = quats.size();
    if (n == 0) return Quat{};
    if (isStatic || n == 1) return slerpGame(0.0f, quats[0], quats[0]);
    int seg;
    float t;
    bool ended;
    walk(frame * frameScale, &seg, &t, &ended);
    if (ended) return slerpGame(0.0f, quats[n - 1], quats[n - 1]);
    return slerpGame(t, quats[seg], quats[seg + 1]);
}

void GaniChannel::sampleVec(float frame, float out[3]) const
{
    const int n = vecs.size() / 3;
    out[0] = out[1] = out[2] = 0;
    if (n == 0) return;
    const auto key = [&](int i) { return vecs.constData() + i * 3; };
    if (isStatic || n == 1) {
        std::memcpy(out, key(0), 12);
        return;
    }
    int seg;
    float t;
    bool ended;
    walk(frame * frameScale, &seg, &t, &ended);
    if (ended) {
        std::memcpy(out, key(n - 1), 12);
        return;
    }
    if (hermite && tans.size() == vecs.size()) {
        const float* m1 = tans.constData() + (seg + 1) * 3;
        const float* m0 = seg == 0 ? m1 : tans.constData() + seg * 3;
        hermiteEval(t, key(seg), key(seg + 1), m0, m1, out);
        return;
    }
    const float omt = 1.0f - t;
    for (int i = 0; i < 3; ++i)
        out[i] = omt * key(seg)[i] + t * key(seg + 1)[i];
}

bool GaniTrack::hasRot() const
{
    for (const GaniChannel& c : channels)
        if (c.isRot) return true;
    return false;
}

bool GaniTrack::hasPos() const
{
    for (const GaniChannel& c : channels)
        if (!c.isRot) return true;
    return false;
}

Quat GaniTrack::sampleRot(float frame) const
{
    for (int i = channels.size() - 1; i >= 0; --i)
        if (channels[i].isRot) return channels[i].sampleRot(frame);
    return Quat{};
}

bool GaniTrack::samplePos(float frame, float out[3]) const
{
    for (int i = channels.size() - 1; i >= 0; --i)
        if (!channels[i].isRot) {
            channels[i].sampleVec(frame, out);
            return true;
        }
    return false;
}

// ── shared segment-stream decoder ───────────────────────────────────────────
namespace {

// Decode one segment's keyframe stream (identical encoding in v1 and v2).
// Returns false when the segment is an aux channel (Float/Vector2/Vector4) or
// its bit size is invalid — the channel is then not kept.
bool decodeSegmentChannel(const quint8* d, qsizetype size, qsizetype blobOff,
                          quint8 bits, GaniSegType type, bool isStatic,
                          bool herm, int frameCount, float scale, int absSeg,
                          GaniChannel* out)
{
    if (blobOff < 0 || blobOff >= size) return false;
    const bool isRot = type == GaniSegType::Quat || type == GaniSegType::QuatDiff;
    const bool isVec3 =
        type == GaniSegType::Vector3 || type == GaniSegType::VectorDiff;
    // Corrupt-clip hardening: the bit size feeds shift counts — reject values
    // outside the encodable range instead of producing NaN/garbled keys.
    if (isRot && (bits < 2 || bits > 30)) return false;
    if (!isRot && bits != 0 && bits != 16 && bits != 32) return false;

    GaniChannel ch;
    ch.isRot = isRot;
    ch.segIndex = absSeg;
    ch.type = type;
    ch.isStatic = isStatic;
    ch.hermite = herm;
    ch.frameScale = scale;
    ch.deltas.append(0);

    if (isRot) {
        qint64 bitPos = static_cast<qint64>(blobOff) * 8;
        ch.quats.append(readQuat(d, size, &bitPos, bits));
        if (!isStatic) {
            int acc = 0;
            while (acc < frameCount && (bitPos >> 3) < size) {
                const int delta = static_cast<int>(readBits(d, size, &bitPos, 8));
                if (delta == 0 && acc == 0) break;   // corrupt guard
                acc += delta;
                ch.quats.append(readQuat(d, size, &bitPos, bits));
                ch.deltas.append(delta);
            }
        }
    } else {
        const int comps = compCount(type);
        qsizetype off = blobOff;
        float v[3];
        if (off + comps * (bits == 16 ? 2 : 4) > size) return false;
        readComps(d, &off, comps, bits, v);
        ch.vecs << v[0] << v[1] << v[2];
        if (herm) ch.tans << 0.0f << 0.0f << 0.0f;
        if (!isStatic) {
            int acc = 0;
            while (acc < frameCount && off < size) {
                const int delta = d[off++];
                acc += delta;
                if (off + comps * (bits == 16 ? 2 : 4) > size) break;
                readComps(d, &off, comps, bits, v);
                ch.vecs << v[0] << v[1] << v[2];
                if (herm) {
                    if (off + comps * (bits == 16 ? 2 : 4) > size) break;
                    float tn[3];
                    readComps(d, &off, comps, bits, tn);
                    ch.tans << tn[0] << tn[1] << tn[2];
                }
                ch.deltas.append(delta);
            }
        }
        if (!isVec3) return false;   // FLOAT/VECTOR2/VECTOR4 are aux — dropped
    }

    // Durations + reciprocals (precomputed, like the game).
    const int n = ch.isRot ? ch.quats.size() : ch.vecs.size() / 3;
    if (n > 1) {
        ch.durations.resize(n - 1);
        ch.invDur.resize(n - 1);
        for (int i = 1; i < n; ++i) {
            const float dur = ch.deltas[i] * scale;
            ch.durations[i - 1] = dur;
            ch.invDur[i - 1] = dur > 0 ? 1.0f / dur : 0.0f;
        }
    }
    *out = std::move(ch);
    return true;
}

}  // namespace

// ── clip decode (GaniFile.DecodeV2Gani) ─────────────────────────────────────
GaniAnim decodeGani2(const QByteArray& clip, const GaniLayout& layout)
{
    GaniAnim anim;
    if (!layout.valid() || clip.size() < 8) return anim;
    const quint8* d = reinterpret_cast<const quint8*>(clip.constData());
    const qsizetype size = clip.size();

    // TrackMiniHeader
    const int frameCount = qFromLittleEndian<qint32>(clip.constData());
    const int paramCount = d[5];
    qsizetype pos = 8 + paramCount * 8;
    if (pos + layout.unitCount > size) return anim;
    QVector<quint8> unitFlags(layout.unitCount);
    for (int i = 0; i < layout.unitCount; ++i) unitFlags[i] = d[pos + i];
    pos += layout.unitCount;
    pos = (pos + 3) & ~qsizetype(3);
    const qsizetype segHeadersAt = pos;
    if (segHeadersAt + qsizetype(layout.segmentCount) * 4 > size) return anim;

    // gani2Base: align(8 + params*8 + unitCount, 4) — start of the seg-header table.
    const qsizetype gani2Base = segHeadersAt;

    anim.frameCount = frameCount;
    const float scale = static_cast<float>(layout.frameScale);
    const HashResolver& resolver = HashResolver::instance();

    int abs = 0;
    for (int ti = 0; ti < layout.unitCount; ++ti) {
        const GaniLayoutUnit& unit = layout.units[ti];
        const quint8 flags = ti < unitFlags.size() ? unitFlags[ti] : 0;
        const bool isStatic = (flags & 0x4) != 0;
        const bool herm = ((flags | unit.flags) & 0x2) != 0;
        GaniTrack track;
        track.nameHash = unit.nameHash;
        track.name = resolver.strCode32NameFor(unit.nameHash);
        track.isStatic = isStatic;

        for (int si = 0; si < unit.segments.size(); ++si, ++abs) {
            if (abs >= layout.segmentCount) break;
            const GaniTrackData& seg = unit.segments[si];
            const qsizetype entryAt = gani2Base + qsizetype(abs) * 4;
            const quint8 bits = d[entryAt];
            const quint32 rel = static_cast<quint32>(d[entryAt + 1])
                | (static_cast<quint32>(d[entryAt + 2]) << 8)
                | (static_cast<quint32>(d[entryAt + 3]) << 16);
            if (rel == 0) continue;   // clip does not animate this segment
            const qsizetype blobOff = entryAt + rel;

            GaniChannel ch;
            if (decodeSegmentChannel(d, size, blobOff, bits, seg.type, isStatic,
                                     herm, frameCount, scale, abs, &ch))
                track.channels.append(ch);
        }
        anim.tracks.append(track);
    }
    return anim;
}

// ── v1 clip decode (FoxData ROOT → MOTION → UNIT, inline layout) ───────────
namespace {

// FoxDataNode is 48 bytes: name(0) nameStr(4) flags(8) dataOff(12) dataSize(16)
// parent(20) child(24) prev(28) next(32) params(36) — offsets signed,
// self-relative. (Reference: Fox_Parser GaniV1.cs, verified on GZ archives.)
constexpr quint32 kFoxRoot = 3933341002u;
constexpr quint32 kFoxMotion = 143688520u;
constexpr quint32 kFoxUnit = 3337172921u;

struct FoxNode {
    quint32 name = 0;
    qint32 dataOff = 0;
    qint32 child = 0;
    qint32 next = 0;
};

FoxNode foxNodeAt(const quint8* d, qsizetype p)
{
    FoxNode n;
    n.name = qFromLittleEndian<quint32>(d + p);
    n.dataOff = qFromLittleEndian<qint32>(d + p + 12);
    n.child = qFromLittleEndian<qint32>(d + p + 24);
    n.next = qFromLittleEndian<qint32>(d + p + 32);
    return n;
}

qsizetype foxFindChild(const quint8* d, qsizetype size, qsizetype parent,
                       qint32 childOff, quint32 target)
{
    if (childOff == 0) return -1;
    qsizetype p = parent + childOff;
    for (int guard = 0; guard < 8192; ++guard) {
        if (p < 0 || p + 48 > size) return -1;
        const FoxNode n = foxNodeAt(d, p);
        if (n.name == target) return p;
        if (n.next == 0) return -1;
        p += n.next;
    }
    return -1;
}

}  // namespace

GaniAnim decodeGaniV1(const QByteArray& clip)
{
    GaniAnim anim;
    if (clip.size() < 48) return anim;
    const quint8* d = reinterpret_cast<const quint8*>(clip.constData());
    const qsizetype size = clip.size();

    const qsizetype nodes = qFromLittleEndian<quint32>(d + 4);
    if (nodes < 0 || nodes + 48 > size) return anim;
    const FoxNode root = foxNodeAt(d, nodes);
    if (root.name != kFoxRoot) return anim;
    const qsizetype motion = foxFindChild(d, size, nodes, root.child, kFoxMotion);
    if (motion < 0) return anim;
    const qsizetype unit =
        foxFindChild(d, size, motion, foxNodeAt(d, motion).child, kFoxUnit);
    if (unit < 0) return anim;

    const qsizetype pay = unit + foxNodeAt(d, unit).dataOff;
    GaniLayout layout;
    if (pay < 0 || !parseGaniLayout(clip, static_cast<int>(pay), &layout))
        return anim;

    anim.frameCount = layout.frameCount;
    const float scale = static_cast<float>(layout.frameScale);
    const HashResolver& resolver = HashResolver::instance();

    int abs = 0;
    for (const GaniLayoutUnit& u : layout.units) {
        const bool isStatic = (u.flags & 0x4) != 0;
        const bool herm = (u.flags & 0x2) != 0;
        GaniTrack track;
        track.nameHash = u.nameHash;
        track.name = resolver.strCode32NameFor(u.nameHash);
        track.isStatic = isStatic;

        for (const GaniTrackData& seg : u.segments) {
            const int segFlat = abs++;
            if (seg.dataOffset == 0 || seg.recordAt < 0) continue;
            const qsizetype blobOff =
                static_cast<qsizetype>(seg.recordAt) + seg.dataOffset;
            GaniChannel ch;
            if (decodeSegmentChannel(d, size, blobOff, seg.componentBitSize,
                                     seg.type, isStatic, herm,
                                     layout.frameCount, scale, segFlat, &ch))
                track.channels.append(ch);
        }
        anim.tracks.append(track);
    }
    return anim;
}

const GaniChannel* GaniAnim::channelBySeg(int segIndex) const
{
    if (segIndex < 0) return nullptr;
    for (const GaniTrack& t : tracks)
        for (const GaniChannel& c : t.channels)
            if (c.segIndex == segIndex) return &c;
    return nullptr;
}

}  // namespace fox
