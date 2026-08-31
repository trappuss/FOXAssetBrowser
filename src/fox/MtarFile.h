// MtarFile.h — Fox Engine motion archive (.mtar): a set of GANI clips plus a
// shared per-archive track layout. Port of Fox_Parser's MtarFile2 (verified
// 30/30 byte-identical repacks of the stock player archives) with read-only
// scope.
//
//   header 0x20: version, fileCount, unitCount/segmentCount (mirror the shared
//   TrackHeader), motionPointUnitCount, flags (NEW=0x1000 → 32-byte v2 table,
//   HAS_SKEL_LIST=0x4000), commonInfoOffset (chain of 16-byte MtarMiniDataNode).
//   Node 0x4fbdaaef carries the shared track layout every clip depends on.
//
// v1 archives (GZ, version 201304220, flags 0) use 16-byte entries and each
// gani body carries its OWN layout — listed, and decoded per-clip when the
// body starts with a parsable FoxData layout (best-effort).
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

#include "fox/GaniAnim.h"

namespace fox {

struct MtarClip {
    quint64 hash = 0;
    QString name;              // resolved gani path ("" when unknown)
    quint32 offset = 0;        // absolute offset of the clip body
    int size = 0;              // bytes
    int motionPointsSize = 0;  // bytes (v2)
};

class MtarFile {
public:
    static bool looksLikeMtar(const QByteArray& data);

    // Parses header, entry table and (v2) the shared layout. Keeps a copy of
    // the archive bytes for clip access.
    bool parse(const QByteArray& data);

    quint32 version() const { return m_version; }
    quint16 flags() const { return m_flags; }
    bool isV2() const { return (m_flags & 0x1000) != 0; }
    const GaniLayout& layout() const { return m_layout; }
    const QVector<MtarClip>& clips() const { return m_clips; }
    QString errorString() const { return m_error; }

    // Raw bytes of one clip (v1: a standalone FoxData .gani; v2: the gani
    // body, which pairs with this archive's shared layout).
    QByteArray readClip(int clipIdx) const;

    // Decode one clip into a sampled animation (v2 only for now).
    GaniAnim decodeClip(int clipIdx) const;

private:
    QByteArray m_data;
    quint32 m_version = 0;
    quint16 m_flags = 0;
    GaniLayout m_layout;
    QVector<MtarClip> m_clips;
    QString m_error;
};

}  // namespace fox
