// GaniAnim.h — Fox Engine GANI animation decode + sampling.
//
// Ported from Fox_Parser's MgsvModBldr.Tools.Anim (GaniStructs / AnimBitReader /
// FoxVectormath / GaniAnimation / GaniFile), itself a verified 1:1 port of the
// decode paths in Tpp_main_win64. Covers the TPP v2 ("GANI2") stream:
//   • layout tracks (shared per-archive): TrackHeader + TrackUnit + TrackData —
//     bone-name StrCode32 + per-segment type/bit size.
//   • per-clip data: TrackMiniHeader + Gani2TrackData (bit size + 24-bit
//     self-relative blob offset per segment).
//   • quat streams: bit-packed smallest-three (theta,x,y + 3 sign bits per
//     key, 8-bit frame deltas between keys).
//   • vector streams: byte-aligned AnimHalf (16-bit) or float32 components,
//     linear or Hermite (tangents after each non-first key).
// Sampling reproduces the game's segment-walk + slerp/lerp/hermite evaluation
// in float32 (std sin/cos in place of the UCRT's — visually identical).
#pragma once
#include <QHash>
#include <QString>
#include <QVector>
#include <cstdint>

namespace fox {

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

enum class GaniSegType : quint8 {
    Quat = 0, Float = 1, Vector2 = 2, Vector3 = 3, Vector4 = 4,
    QuatDiff = 5, VectorDiff = 6,
};

// ── Layout (shared track structure) ─────────────────────────────────────────
struct GaniTrackData {
    qint32 dataOffset = 0;    // layout: relative to its record (v2 unused; v1 = blob)
    qint32 recordAt = -1;     // absolute offset of this 8-byte record in the buffer
    qint16 msId = 0;
    GaniSegType type = GaniSegType::Quat;
    quint8 componentBitSize = 0;
};

struct GaniLayoutUnit {
    quint32 nameHash = 0;     // StrCode32
    quint8 flags = 0;         // LOOP=1, HERMITE=2, IS_STATIC=4
    QVector<GaniTrackData> segments;
};

struct GaniLayout {
    int unitCount = 0;
    int segmentCount = 0;
    int frameCount = 0;       // of the layout header (clips carry their own)
    int frameScale = 1;       // signed byte at header+0x10
    QVector<GaniLayoutUnit> units;
    bool valid() const { return unitCount > 0 && units.size() == unitCount; }
};

// Parse a layout-track payload (node 0x4fbdaaef data, or a v1 gani's UNIT node).
bool parseGaniLayout(const QByteArray& buf, int start, GaniLayout* out);

// ── Decoded animation ───────────────────────────────────────────────────────
struct GaniChannel {
    bool isRot = false;
    int segIndex = -1;            // FLAT segment index across the clip (frig binding)
    GaniSegType type = GaniSegType::Quat;
    bool isStatic = false;
    bool hermite = false;
    float frameScale = 1.0f;
    QVector<int> deltas;          // deltas[0] == 0
    QVector<Quat> quats;          // rot channels
    QVector<float> vecs;          // xyz per key (vector channels)
    QVector<float> tans;          // hermite tangents, xyz per key ([0] unused)
    QVector<float> durations;     // n-1
    QVector<float> invDur;        // n-1

    Quat sampleRot(float frame) const;
    void sampleVec(float frame, float out[3]) const;

private:
    void walk(float time, int* seg, float* t, bool* ended) const;
};

struct GaniTrack {
    quint32 nameHash = 0;         // StrCode32
    QString name;                 // resolved bone name ("" when unknown)
    bool isStatic = false;
    QVector<GaniChannel> channels;

    bool hasRot() const;
    bool hasPos() const;
    // Last channel of each kind wins (TrackControl::GetTransformData semantics).
    Quat sampleRot(float frame) const;
    bool samplePos(float frame, float out[3]) const;
};

struct GaniAnim {
    int frameCount = 0;
    QVector<GaniTrack> tracks;
    bool valid() const { return !tracks.isEmpty(); }

    // Channel by FLAT segment index (frig seg shorts address these). Null when
    // the clip does not animate that segment.
    const GaniChannel* channelBySeg(int segIndex) const;
};

// Decode one v2 per-clip blob using the archive's shared layout.
GaniAnim decodeGani2(const QByteArray& clip, const GaniLayout& layout);

// Decode a v1 (Ground Zeroes era) gani: a FoxData node tree (ROOT → MOTION →
// UNIT) carrying its own inline track layout — the same TrackHeader bytes a
// v2 archive shares as its .trk — with each segment's keyframe blob addressed
// by the layout record's dataOffset (self-relative). Blob encoding is
// IDENTICAL to v2 (verified byte-for-byte on matched GZ/TPP clip pairs by the
// reference transcoder), so the same stream decoder runs underneath.
GaniAnim decodeGaniV1(const QByteArray& clip);

}  // namespace fox
