// FcnpFile.cpp — see FcnpFile.h.
#include "fox/FcnpFile.h"

#include <QtEndian>

namespace fox {
namespace {

quint32 rdU32(const QByteArray& d, qsizetype at)
{
    return at >= 0 && at + 4 <= d.size()
        ? qFromLittleEndian<quint32>(d.constData() + at)
        : 0u;
}
qint32 rdS32(const QByteArray& d, qsizetype at)
{
    return static_cast<qint32>(rdU32(d, at));
}
float rdF32(const QByteArray& d, qsizetype at)
{
    const quint32 v = rdU32(d, at);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
QString cstrAt(const QByteArray& d, qsizetype at)
{
    if (at <= 0 || at >= d.size()) return {};
    qsizetype end = at;
    while (end < d.size() && d.at(end) != '\0' && end - at < 128) ++end;
    return QString::fromLatin1(d.constData() + at, static_cast<int>(end - at));
}

// "Parent" property key (observed hash in every fcnp).
constexpr quint32 kParentKey = 0x92142324u;

}  // namespace

const ConnectPoint* FcnpFile::find(const QString& name) const
{
    for (const ConnectPoint& c : m_points)
        if (c.name == name) return &c;
    return nullptr;
}

bool FcnpFile::parse(const QByteArray& d)
{
    m_points.clear();
    if (d.size() < 0x50) return false;
    const qsizetype nodes = rdU32(d, 4);
    if (nodes < 0x10 || nodes + 48 > d.size()) return false;

    // FoxDataNode (48B): name(0) nameStr(4) flags(8) dataOff(12) dataSize(16)
    // parent(20) child(24) prev(28) next(32) params(36). Self-relative.
    qsizetype p = nodes;
    for (int guard = 0; guard < 4096; ++guard) {
        if (p < 0 || p + 48 > d.size()) break;
        ConnectPoint cp;
        cp.name = cstrAt(d, p + rdS32(d, p + 4));
        const qsizetype dataOff = rdS32(d, p + 12);
        const qint32 dataSize = rdS32(d, p + 16);
        if (cp.name.isEmpty() || dataOff <= 0 || dataSize < 48
            || p + dataOff + 48 > d.size())
            break;
        const qsizetype at = p + dataOff;
        for (int k = 0; k < 3; ++k) cp.pos[k] = rdF32(d, at + k * 4);
        for (int k = 0; k < 4; ++k) cp.quat[k] = rdF32(d, at + 16 + k * 4);
        for (int k = 0; k < 3; ++k) cp.scale[k] = rdF32(d, at + 32 + k * 4);

        // Param record: {u32 count; count × {u32 keyHash, s32 keyStrOff,
        // u32 valHash, s32 valStrOff}} — each string offset is relative to
        // ITS PAIR's hash field (key: entry+0; value: entry+8).
        // Parent = the bone this point hangs off.
        const qsizetype paramsOff = rdS32(d, p + 36);
        if (paramsOff > 0 && p + paramsOff + 4 <= d.size()) {
            const qsizetype rec = p + paramsOff;
            const quint32 count = rdU32(d, rec);
            qsizetype e = rec + 4;
            for (quint32 i = 0; i < count && count <= 16; ++i, e += 16) {
                if (e + 16 > d.size()) break;
                if (rdU32(d, e) == kParentKey) {
                    cp.parentBone = cstrAt(d, e + 8 + rdS32(d, e + 12));
                    break;
                }
            }
        }
        m_points.append(cp);

        const qint32 next = rdS32(d, p + 32);
        if (next == 0) break;
        p += next;
    }
    return !m_points.isEmpty();
}

}  // namespace fox
