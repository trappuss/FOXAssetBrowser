// DfrmFile.cpp — see DfrmFile.h.
#include "fox/DfrmFile.h"

#include <QtEndian>

namespace fox {
namespace {
constexpr quint32 kMagic = 0x4D524644;   // "DFRM"

quint32 u32At(const QByteArray& d, qsizetype p)
{
    return qFromLittleEndian<quint32>(d.constData() + p);
}
quint16 u16At(const QByteArray& d, qsizetype p)
{
    return qFromLittleEndian<quint16>(d.constData() + p);
}
}  // namespace

bool DfrmFile::isDfrm(const QByteArray& data)
{
    return data.size() >= 64 && u32At(data, 0) == kMagic;
}

bool DfrmFile::parse(const QByteArray& d)
{
    m_error.clear();
    m_meshes.clear();
    m_weld.clear();
    m_fanAt.clear();
    m_fans.clear();
    m_groupAt.clear();
    m_members.clear();
    if (!isDfrm(d)) {
        m_error = QStringLiteral("not a DFRM file");
        return false;
    }

    // 0x08 is the HEADER SIZE (64), not a count — reading six counts from
    // there shifts every pair by one and silently produces a file whose meshes
    // and fans still decode (they are one apart in the table) while the seam
    // groups come out empty. Counts start at 0x0C, offsets at 0x24, and the
    // two run in the same order:
    //
    //   meshes  welded  groups  meshVerts  fans  groupMembers
    quint32 cnt[6], off[6];
    for (int i = 0; i < 6; ++i) cnt[i] = u32At(d, 12 + i * 4);
    for (int i = 0; i < 6; ++i) off[i] = u32At(d, 36 + i * 4);

    const quint32 nMesh = cnt[0], nWelded = cnt[1], nGroups = cnt[2];
    const quint32 nMeshVerts = cnt[3], nFans = cnt[4], nMembers = cnt[5];
    const quint32 oMesh = off[0], oWelded = off[1], oGroup = off[2];
    const quint32 oWeld = off[3], oFans = off[4], oMembers = off[5];

    const auto fits = [&](quint64 o, quint64 n, quint64 stride) {
        return o + n * stride <= quint64(d.size());
    };
    if (!fits(oMesh, nMesh, 8) || !fits(oWelded, nWelded, 8)
        || !fits(oWeld, nMeshVerts, 4) || !fits(oFans, nFans, 4)
        || !fits(oGroup, nGroups, 8) || !fits(oMembers, nMembers, 4)) {
        m_error = QStringLiteral("a section runs past the end of the file");
        return false;
    }

    m_weld.resize(int(nMeshVerts));
    for (quint32 i = 0; i < nMeshVerts; ++i)
        m_weld[int(i)] = u32At(d, oWeld + i * 4);

    m_meshes.reserve(int(nMesh));
    for (quint32 i = 0; i < nMesh; ++i) {
        DfrmMesh m;
        m.vertexCount = u32At(d, oMesh + i * 8);
        // Stored as a byte offset into the weld table; kept as an index, which
        // is what every caller actually wants.
        const quint32 byteOff = u32At(d, oMesh + i * 8 + 4);
        m.firstWeld = byteOff >= oWeld ? (byteOff - oWeld) / 4 : 0;
        m_meshes.append(m);
    }

    m_fans.resize(int(nFans));
    for (quint32 i = 0; i < nFans; ++i) {
        m_fans[int(i)].mesh = u16At(d, oFans + i * 4);
        m_fans[int(i)].triangle = u16At(d, oFans + i * 4 + 2);
    }
    m_fanAt.reserve(int(nWelded));
    for (quint32 i = 0; i < nWelded; ++i) {
        const quint32 c = u32At(d, oWelded + i * 8);
        const quint32 b = u32At(d, oWelded + i * 8 + 4);
        m_fanAt.append({c, b >= oFans ? (b - oFans) / 4 : 0});
    }

    m_members.resize(int(nMembers));
    for (quint32 i = 0; i < nMembers; ++i)
        m_members[int(i)] = u32At(d, oMembers + i * 4);
    m_groupAt.reserve(int(nGroups));
    for (quint32 i = 0; i < nGroups; ++i) {
        const quint32 c = u32At(d, oGroup + i * 8);
        const quint32 b = u32At(d, oGroup + i * 8 + 4);
        m_groupAt.append({c, b >= oMembers ? (b - oMembers) / 4 : 0});
    }
    return true;
}

QVector<DfrmFan> DfrmFile::fansFor(int weldedIndex) const
{
    QVector<DfrmFan> out;
    if (weldedIndex < 0 || weldedIndex >= m_fanAt.size()) return out;
    const auto [c, first] = m_fanAt[weldedIndex];
    for (quint32 k = 0; k < c && int(first + k) < m_fans.size(); ++k)
        out.append(m_fans[int(first + k)]);
    return out;
}

QVector<quint32> DfrmFile::group(int i) const
{
    QVector<quint32> out;
    if (i < 0 || i >= m_groupAt.size()) return out;
    const auto [c, first] = m_groupAt[i];
    for (quint32 k = 0; k < c && int(first + k) < m_members.size(); ++k)
        out.append(m_members[int(first + k)]);
    return out;
}

QString DfrmFile::summary() const
{
    if (!m_error.isEmpty()) return m_error;
    return QStringLiteral(
               "%1 mesh(es), %2 mesh vertex/vertices welded to %3 position(s), "
               "%4 face-fan record(s), %5 seam group(s) over %6 member(s)")
        .arg(m_meshes.size())
        .arg(m_weld.size())
        .arg(m_fanAt.size())
        .arg(m_fans.size())
        .arg(m_groupAt.size())
        .arg(m_members.size());
}

}  // namespace fox
