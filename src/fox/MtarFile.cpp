// MtarFile.cpp — see MtarFile.h.
#include "fox/MtarFile.h"

#include <QtEndian>

#include "fox/FoxHash.h"

namespace fox {
namespace {

constexpr quint32 kNodeLayoutTrack = 0x4FBDAAEF;
constexpr quint32 kVersionTpp = 201403250;
constexpr quint32 kVersionGz = 201304220;

}  // namespace

bool MtarFile::looksLikeMtar(const QByteArray& data)
{
    if (data.size() < 0x20) return false;
    const quint32 v = qFromLittleEndian<quint32>(data.constData());
    return v == kVersionTpp || v == kVersionGz;
}

bool MtarFile::parse(const QByteArray& data)
{
    m_data = data;
    m_clips.clear();
    m_error.clear();
    m_layout = GaniLayout();

    if (data.size() < 0x20) {
        m_error = QStringLiteral("too small for an MTAR header");
        return false;
    }
    const char* p = data.constData();
    m_version = qFromLittleEndian<quint32>(p);
    const quint32 fileCount = qFromLittleEndian<quint32>(p + 4);
    m_flags = qFromLittleEndian<quint16>(p + 0x12);
    const quint32 commonInfoOffset = qFromLittleEndian<quint32>(p + 0x14);
    if (m_version != kVersionTpp && m_version != kVersionGz) {
        m_error = QStringLiteral("unknown MTAR version %1").arg(m_version);
        return false;
    }
    if (fileCount > 100000u) {
        m_error = QStringLiteral("implausible clip count %1").arg(fileCount);
        return false;
    }

    const HashResolver& resolver = HashResolver::instance();
    const int entrySize = isV2() ? 32 : 16;
    qsizetype pos = 0x20;
    for (quint32 i = 0; i < fileCount; ++i, pos += entrySize) {
        if (pos + entrySize > data.size()) {
            m_error = QStringLiteral("truncated entry table");
            return false;
        }
        MtarClip c;
        c.hash = qFromLittleEndian<quint64>(p + pos);
        c.offset = qFromLittleEndian<quint32>(p + pos + 8);
        if (isV2()) {
            c.size = qFromLittleEndian<qint16>(p + pos + 12) * 0x10;
            c.motionPointsSize = qFromLittleEndian<qint16>(p + pos + 16) * 0x10;
        } else {
            c.size = qFromLittleEndian<qint32>(p + pos + 12);
        }
        QString name;
        // v2 tables carry TPP PathCode64; v1 (GZ) tables carry the g0s-style
        // 48-bit hash + typeId<<52 — resolve with the matching scheme, trying
        // the other as a fallback (TPP ships a few v1-container mtars).
        const bool named = isV2()
            ? (resolver.tryResolve(c.hash, &name)
               || resolver.tryResolveGzs(c.hash, &name))
            : (resolver.tryResolveGzs(c.hash, &name)
               || resolver.tryResolve(c.hash, &name));
        if (named)
            c.name = name.section(QLatin1Char('/'), -1);
        else
            c.name = QStringLiteral("%1.gani").arg(c.hash, 0, 16);
        m_clips.append(c);
    }

    // v2: shared layout from the CommonInfo node chain.
    if (isV2()) {
        qsizetype at = commonInfoOffset;
        int guard = 0;
        while (at + 16 <= data.size() && ++guard < 64) {
            const quint32 nodeName = qFromLittleEndian<quint32>(p + at);
            const qint32 dataSize = qFromLittleEndian<qint32>(p + at + 4);
            const qint32 next = qFromLittleEndian<qint32>(p + at + 8);
            if (dataSize < 0 || at + 16 + dataSize > data.size()) break;
            if (nodeName == kNodeLayoutTrack) {
                if (!parseGaniLayout(data, static_cast<int>(at + 16), &m_layout)) {
                    m_error = QStringLiteral("bad shared track layout");
                    return false;
                }
            }
            if (next == 0) break;
            at += next;
        }
        if (!m_layout.valid()) {
            m_error = QStringLiteral("no shared track layout (node 0x4fbdaaef)");
            return false;
        }
    }
    return true;
}

QByteArray MtarFile::readClip(int clipIdx) const
{
    if (clipIdx < 0 || clipIdx >= m_clips.size()) return {};
    const MtarClip& c = m_clips[clipIdx];
    if (c.offset + static_cast<qint64>(c.size) > m_data.size() || c.size <= 0)
        return {};
    return m_data.mid(c.offset, c.size);
}

GaniAnim MtarFile::decodeClip(int clipIdx) const
{
    GaniAnim anim;
    const QByteArray clip = readClip(clipIdx);
    if (clip.isEmpty()) return anim;
    if (isV2()) return decodeGani2(clip, m_layout);
    return decodeGaniV1(clip);   // v1: inline FoxData layout per clip
}

}  // namespace fox
