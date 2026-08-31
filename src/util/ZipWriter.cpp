#include "util/ZipWriter.h"

#include <QDateTime>
#include <QFile>
#include <QtEndian>
#include <zlib.h>

#include "fox/FoxZlib.h"

namespace zipwriter {
namespace {

void putU16(QByteArray& b, quint16 v)
{
    char raw[2];
    qToLittleEndian(v, raw);
    b.append(raw, 2);
}
void putU32(QByteArray& b, quint32 v)
{
    char raw[4];
    qToLittleEndian(v, raw);
    b.append(raw, 4);
}

// MS-DOS date and time, which is what ZIP records. Seconds have one-bit
// precision (the field holds seconds/2) and the epoch is 1980 — a file dated
// before that cannot be represented, so it is clamped rather than wrapped into
// a date in the far future.
void dosStamp(const QDateTime& when, quint16* dosTime, quint16* dosDate)
{
    // An invalid QDateTime is the ZIP epoch rather than today: see Entry.
    const QDate d = when.isValid() ? when.date() : QDate(1980, 1, 1);
    const QTime t = when.isValid() ? when.time() : QTime(0, 0, 0);
    const int year = qMax(1980, d.year());
    *dosDate = quint16(((year - 1980) << 9) | (d.month() << 5) | d.day());
    *dosTime = quint16((t.hour() << 11) | (t.minute() << 5) | (t.second() / 2));
}

}  // namespace

QString write(const QString& outPath, const QVector<Entry>& entries)
{
    // ── The limits of the 1989 format, refused rather than wrapped ──────────
    // The entry counts in the end-of-central-directory record are 16 bits and
    // every size and offset in the format is 32. ZIP64 exists to lift both,
    // and this writer does not implement it — so the sizes that would need it
    // are refused HERE, with a sentence, instead of being truncated into a
    // header that reads as a small valid archive. A silently wrong archive is
    // worse than no archive: it opens, it lists, and the members are garbage.
    constexpr int kMaxEntries = 65535;
    constexpr qint64 kMax32 = 0xFFFFFFFFLL;
    if (entries.size() > kMaxEntries)
        return QStringLiteral(
            "%1 files is more than a plain ZIP can hold (%2). This writer does "
            "not implement ZIP64.").arg(entries.size()).arg(kMaxEntries);
    for (const Entry& e : entries)
        if (qint64(e.data.size()) > kMax32)
            return QStringLiteral(
                "'%1' is larger than 4 GB, which a plain ZIP cannot record. "
                "This writer does not implement ZIP64.").arg(e.name);

    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly))
        return QStringLiteral("Could not write %1: %2")
            .arg(outPath, out.errorString());

    QByteArray central;
    quint32 offset = 0;
    int written = 0;
    for (const Entry& e : entries) {
        const QByteArray name = e.name.toUtf8();
        quint16 dosTime = 0, dosDate = 0;
        dosStamp(e.modified, &dosTime, &dosDate);
        const quint32 crc =
            quint32(::crc32(0, reinterpret_cast<const Bytef*>(e.data.constData()),
                            uInt(e.data.size())));
        QByteArray body = fox::zlibDeflateRaw(e.data);
        // STORED when deflate did not pay, which for an already-compressed
        // member is most of the time. Method 0 and method 8 are both ordinary
        // ZIP; picking the smaller is the whole rule.
        const bool deflated = !body.isEmpty() && body.size() < e.data.size();
        if (!deflated) body = e.data;

        QByteArray local;
        putU32(local, 0x04034b50);
        putU16(local, 20);                       // version needed
        putU16(local, 0);                        // flags
        putU16(local, deflated ? 8 : 0);         // method
        putU16(local, dosTime);
        putU16(local, dosDate);
        putU32(local, crc);
        putU32(local, quint32(body.size()));
        putU32(local, quint32(e.data.size()));
        putU16(local, quint16(name.size()));
        putU16(local, 0);                        // extra length
        local.append(name);
        if (out.write(local) != local.size() || out.write(body) != body.size())
            return QStringLiteral("Short write to %1").arg(outPath);

        putU32(central, 0x02014b50);
        putU16(central, 20);                     // version made by
        putU16(central, 20);                     // version needed
        putU16(central, 0);
        putU16(central, deflated ? 8 : 0);
        putU16(central, dosTime);
        putU16(central, dosDate);
        putU32(central, crc);
        putU32(central, quint32(body.size()));
        putU32(central, quint32(e.data.size()));
        putU16(central, quint16(name.size()));
        putU16(central, 0);                      // extra
        putU16(central, 0);                      // comment
        putU16(central, 0);                      // disk number start
        putU16(central, 0);                      // internal attributes
        putU32(central, 0);                      // external attributes
        putU32(central, offset);
        central.append(name);

        // Checked as it grows rather than totalled up front, because what
        // overflows is the OFFSET of a member — the compressed sizes are not
        // known until each one is deflated.
        const qint64 next = qint64(offset) + local.size() + body.size();
        if (next > kMax32) {
            out.close();
            QFile::remove(outPath);
            return QStringLiteral(
                "The archive passed 4 GB while writing '%1', which a plain ZIP "
                "cannot address. This writer does not implement ZIP64.")
                .arg(e.name);
        }
        offset = quint32(next);
        ++written;
    }

    const quint32 cdOffset = offset;
    if (out.write(central) != central.size())
        return QStringLiteral("Short write to %1").arg(outPath);

    QByteArray end;
    putU32(end, 0x06054b50);
    putU16(end, 0);                              // this disk
    putU16(end, 0);                              // disk with the directory
    putU16(end, quint16(written));
    putU16(end, quint16(written));
    putU32(end, quint32(central.size()));
    putU32(end, cdOffset);
    putU16(end, 0);                              // comment length
    if (out.write(end) != end.size())
        return QStringLiteral("Short write to %1").arg(outPath);
    out.close();
    return {};
}

}  // namespace zipwriter
