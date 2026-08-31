// GrxlaFile.cpp — see GrxlaFile.h.
#include "fox/GrxlaFile.h"

#include <QHash>
#include <QStringList>
#include <QtEndian>
#include <cstring>

namespace fox {
namespace {
constexpr int kHeaderSize = 16;
}  // namespace

bool GrxlaFile::isGrx(const QByteArray& data)
{
    return data.size() >= kHeaderSize
        && (std::memcmp(data.constData(), "FGxL", 4) == 0
            || std::memcmp(data.constData(), "FGxO", 4) == 0);
}

bool GrxlaFile::parse(const QByteArray& data)
{
    m_entries.clear();
    m_complete = false;
    m_headerOk = false;
    m_occluder = false;
    m_error.clear();

    if (!isGrx(data)) {
        m_error = QStringLiteral("not a FGxL/FGxO array");
        return false;
    }
    m_occluder = std::memcmp(data.constData(), "FGxO", 4) == 0;
    const char* p = data.constData();
    const qint64 size = data.size();

    // Documented as constants: header size 10 at 0x8, a 1 at 0xC. Read and
    // checked rather than skipped — if either moves, this reader should say so
    // instead of walking a chain that starts somewhere else.
    const quint32 hdr = qFromLittleEndian<quint32>(p + 0x8);
    const quint32 one = qFromLittleEndian<quint32>(p + 0xC);
    m_headerOk = (hdr == 10u && one == 1u);
    if (!m_headerOk) {
        // STOP. The walk starts at 0x10 because the header is 16 bytes; if the
        // header does not say what it is documented to say, the chain may not
        // start there, and walking anyway can land on a zero dword by accident
        // and report "complete" for a file whose layout never held. A reader
        // whose whole purpose is the assertion "N of N" must not be able to
        // produce a false positive.
        m_error = QStringLiteral("header fields are %1/%2, expected 10/1")
                      .arg(hdr).arg(one);
        return true;
    }

    qint64 at = kHeaderSize;
    // A bound on the COUNT as well as the offset: a corrupt size of 0 would
    // otherwise spin here forever on a file that is otherwise readable.
    for (int guard = 0; guard < 200000; ++guard) {
        // FOUR bytes for the tag, not eight. The terminator is documented as
        // four zero bytes; requiring its trailing size dword to be present too
        // means a file that ENDS at its terminator reads as "ran off the end",
        // and an install of them reads as "0 of N complete" — which is exactly
        // the wrong conclusion about a layout that held.
        if (at + 4 > size) {
            m_error = QStringLiteral("ran off the end at 0x%1")
                          .arg(at, 0, 16);
            return true;   // parsed as far as it went; complete() says no
        }
        const quint32 tag = qFromLittleEndian<quint32>(p + at);
        if (tag == 0) {           // the terminator
            m_complete = true;
            return true;
        }
        if (at + 8 > size) {
            m_error = QStringLiteral("entry at 0x%1 has no size").arg(at, 0, 16);
            return true;
        }
        const quint32 esz = qFromLittleEndian<quint32>(p + at + 4);
        if (esz < 8 || at + qint64(esz) > size) {
            m_error = QStringLiteral("entry at 0x%1 declares size %2")
                          .arg(at, 0, 16).arg(esz);
            return true;
        }
        GrxEntry e;
        // Sanitised: a tag is four raw bytes, and a stray 0x09 or 0x0A would
        // otherwise inject a field or a row break straight into the TSV this
        // feeds.
        e.type.clear();
        for (int b = 0; b < 4; ++b) {
            const uchar c = uchar(p[at + b]);
            if (c >= 0x20 && c < 0x7F) e.type += QLatin1Char(char(c));
            else e.type += QStringLiteral("\\x%1")
                               .arg(c, 2, 16, QLatin1Char('0'));
        }
        e.size = esz;
        e.offset = at;
        m_entries.append(e);
        at += esz;
    }
    m_error = QStringLiteral("more than 200000 entries — refusing to walk on");
    return true;
}

QString GrxlaFile::describe() const
{
    QHash<QString, int> byType;
    for (const GrxEntry& e : m_entries) ++byType[e.type];
    QStringList keys = byType.keys();
    keys.sort();
    QStringList bits;
    for (const QString& k : keys)
        bits << QStringLiteral("%1x%2").arg(k).arg(byType.value(k));
    return bits.join(QLatin1Char(' '));
}

}  // namespace fox
