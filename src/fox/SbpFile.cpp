// SbpFile.cpp — see SbpFile.h.
#include "fox/SbpFile.h"

#include <QtEndian>

namespace fox {

bool SbpFile::parse(const QByteArray& data)
{
    m_entries.clear();
    if (!isSbp(data)) return false;
    const quint8 count = static_cast<quint8>(data.at(4));
    // header: magic u32, count u8, headerSize u16, pad u8, then count × 12B.
    if (8 + count * 12 > data.size()) return false;
    for (int i = 0; i < count; ++i) {
        const qsizetype at = 8 + i * 12;
        SbpSub s;
        s.magic = data.mid(at, 4);
        while (s.magic.endsWith('\0')) s.magic.chop(1);
        s.offset = qFromLittleEndian<quint32>(data.constData() + at + 4);
        s.size = qFromLittleEndian<quint32>(data.constData() + at + 8);
        if (static_cast<qint64>(s.offset) + s.size > data.size()) continue;
        m_entries.append(s);
    }
    return !m_entries.isEmpty();
}

QVector<SbpWem> SbpFile::listWems(const QByteArray& sbp)
{
    QVector<SbpWem> out;
    SbpFile f;
    if (!f.parse(sbp)) return out;
    for (const SbpSub& sub : f.entries()) {
        // Stream packages: STPL magic, u32 count, then {u32 id, u32 offset}
        // pairs (offsets relative to the package start). Entry sizes are the
        // gaps between successive offsets (validated: each lands on "RIFF").
        if (sub.size < 8) continue;
        const char* p = sbp.constData() + sub.offset;
        if (qstrncmp(p, "STPL", 4) != 0) continue;
        const quint32 count = qFromLittleEndian<quint32>(p + 4);
        if (count == 0 || 8 + static_cast<qint64>(count) * 8 > sub.size) continue;
        struct Row {
            quint32 id, off;
        };
        QVector<Row> rows;
        rows.reserve(count);
        bool ok = true;
        for (quint32 i = 0; i < count; ++i) {
            Row r;
            r.id = qFromLittleEndian<quint32>(p + 8 + i * 8);
            r.off = qFromLittleEndian<quint32>(p + 8 + i * 8 + 4);
            if (r.off >= sub.size) { ok = false; break; }
            rows.append(r);
        }
        if (!ok) continue;
        for (int i = 0; i < rows.size(); ++i) {
            const quint32 end = i + 1 < rows.size() ? rows[i + 1].off : sub.size;
            if (end <= rows[i].off) continue;
            SbpWem w;
            w.id = rows[i].id;
            w.absOffset = sub.offset + rows[i].off;
            w.size = end - rows[i].off;
            out.append(w);
        }
    }
    return out;
}

}  // namespace fox
