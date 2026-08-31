// PftxsFile.cpp — see PftxsFile.h.
#include "fox/PftxsFile.h"

#include <QtEndian>

namespace fox {
namespace {
constexpr quint32 kMagicPftx = 0x58544650;   // "PFTX"
constexpr quint32 kMagicTexl = 0x4C584554;   // "TEXL"
constexpr quint32 kMagicFtex = 0x58455446;   // "FTEX"

quint32 u32At(const QByteArray& d, qsizetype pos)
{
    return qFromLittleEndian<quint32>(d.constData() + pos);
}
quint64 u64At(const QByteArray& d, qsizetype pos)
{
    return qFromLittleEndian<quint64>(d.constData() + pos);
}
}  // namespace

bool PftxsFile::isPftxs(const QByteArray& data)
{
    return data.size() >= 32 && u32At(data, 0) == kMagicPftx;
}

int PftxsFile::readTextures() const
{
    int n = 0;
    for (const PftxsGroup& g : m_groups) n += int(g.entries.size());
    return n;
}

bool PftxsFile::parse(const QByteArray& data)
{
    m_groups.clear();
    m_error.clear();
    m_declared = 0;
    if (!isPftxs(data)) {
        m_error = QStringLiteral("not a PFTXS pack");
        return false;
    }
    // 16-byte PFTX header, then 16-byte TEXL header {magic, size, fileCount, pad}.
    if (u32At(data, 16) != kMagicTexl) {
        m_error = QStringLiteral("TEXL header missing");
        return false;
    }
    // The u32 at 24 is NOT a group count. Measured on the shipped packs: a
    // Survive body pack reports 17 there and contains exactly TWO FTEX groups —
    // a scan of the whole 1.9 MB finds four "FTEX" occurrences, two group
    // headers and the two .ftex payloads that begin with the same magic. Trying
    // to read seventeen groups walked straight into texture data and threw the
    // pack away, which is why 350 of Survive's 353 packs and 539 of the Phantom
    // Pain's carried no textures at all.
    //
    // So the group list ends where the data says it ends: walk while the next
    // header really is an FTEX group, and keep what was read.
    // Not a group count — a TEXTURE count. Measured: a Survive body pack
    // reports 17 here and holds two groups of eight and nine entries. Kept, so
    // a short read can be reported as short rather than as success.
    const quint32 headerField = u32At(data, 24);
    m_declared = headerField;
    constexpr quint32 kMaxGroups = 100000u;

    qsizetype pos = 32;
    for (quint32 g = 0; g < kMaxGroups; ++g) {
        if (pos + 32 > data.size()) break;   // ran out — keep what we have
        const qsizetype groupBase = pos;
        if (u32At(data, pos) != kMagicFtex) break;
        // A group header is magic, size, u64 hash, entry count, three zeros.
        // The three zeros are what tells a real header apart from a .ftex
        // payload, which starts with the same magic.
        if (u32At(data, pos + 20) != 0 || u32At(data, pos + 24) != 0
            || u32At(data, pos + 28) != 0)
            break;
        const quint32 count = u32At(data, pos + 16);
        if (count == 0 || count > 64u) break;
        PftxsGroup group;
        group.hash = u64At(data, pos + 8);
        pos += 32;
        bool truncated = false;
        for (quint32 i = 0; i < count; ++i) {
            if (pos + 16 > data.size()) { truncated = true; break; }
            PftxsSubEntry e;
            e.hash = u64At(data, pos);
            e.offset = u32At(data, pos + 8);
            e.size = u32At(data, pos + 12);
            e.absOffset = groupBase + e.offset;
            pos += 16;
            group.entries.append(e);
        }
        if (truncated || group.entries.isEmpty()) break;
        // An entry whose payload runs off the end is a pack we only half have.
        const PftxsSubEntry& last = group.entries.last();
        if (last.absOffset + qint64(last.size) > data.size()) break;
        m_groups.append(group);
        // The next group starts right after the last entry's data — which on
        // every shipped pack measured is also groupBase + the size field.
        pos = last.absOffset + qint64(last.size);
    }
    if (m_groups.isEmpty()) {
        m_error = QStringLiteral("no FTEX groups (header field %1)").arg(headerField);
        return false;
    }
    // Stopping early is normal — but not if a real group header is still ahead,
    // which would mean the walk lost the thread rather than reaching the end.
    for (qsizetype scan = pos; scan + 32 <= data.size(); scan += 4) {
        if (u32At(data, scan) != kMagicFtex) continue;
        if (u32At(data, scan + 20) || u32At(data, scan + 24) || u32At(data, scan + 28))
            continue;
        const quint32 c = u32At(data, scan + 16);
        if (c == 0 || c > 64u) continue;
        m_error = QStringLiteral("stopped after %1 group(s) with another at %2")
                      .arg(m_groups.size()).arg(scan);
        break;
    }
    return true;
}

QByteArray PftxsFile::readEntry(const QByteArray& data, const PftxsSubEntry& entry)
{
    if (entry.absOffset < 0
        || entry.absOffset + static_cast<qint64>(entry.size) > data.size())
        return {};
    return QByteArray(data.constData() + entry.absOffset, entry.size);
}

}  // namespace fox
