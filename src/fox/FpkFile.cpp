// FpkFile.cpp — see FpkFile.h. Mirrors GzsTool.Core/Fpk (FpkFile.cs,
// FpkEntry.cs, FpkString.cs).
#include "fox/FpkFile.h"

#include <QtEndian>
#include <cstring>

#include "fox/FoxHash.h"

namespace fox {
namespace {

// Header layout (48 bytes):
//   0  "foxf"      4  "pk"        6  type byte (' ' = fpk, 'd' = fpkd)
//   7  'w'         8  "in"       10  u32 fileSize   [14..31] zeros
//  32  u32 2      36  u32 fileCount   40  u32 referenceCount   44  zeros
constexpr quint32 kMagicFoxf = 0x66786F66;   // "foxf" little-endian

struct Cursor {
    const char* p;
    qsizetype size;
    qsizetype pos = 0;
    bool ok = true;

    quint32 u32()
    {
        if (pos + 4 > size) { ok = false; return 0; }
        const quint32 v = qFromLittleEndian<quint32>(p + pos);
        pos += 4;
        return v;
    }
    void skip(qsizetype n)
    {
        if (pos + n > size) ok = false;
        else pos += n;
    }
    QByteArray bytes(qsizetype n)
    {
        if (pos + n > size) { ok = false; return {}; }
        QByteArray b(p + pos, n);
        pos += n;
        return b;
    }
};

// FpkString: {u32 stringOffset, pad4, u32 stringLength, pad4}; the text lives
// at stringOffset in the blob.
QString readFpkString(Cursor& c, const QByteArray& blob)
{
    const quint32 offset = c.u32();
    c.skip(4);
    const quint32 length = c.u32();
    c.skip(4);
    if (!c.ok) return {};
    if (offset + length > static_cast<quint32>(blob.size())) { c.ok = false; return {}; }
    return QString::fromLatin1(blob.constData() + offset, static_cast<int>(length));
}

}  // namespace

bool FpkFile::isFpk(const QByteArray& data)
{
    return data.size() >= 8 && qFromLittleEndian<quint32>(data.constData()) == kMagicFoxf
        && data.at(4) == 'p' && data.at(5) == 'k';
}

bool FpkFile::parse(const QByteArray& data)
{
    m_entries.clear();
    m_references.clear();
    m_error.clear();

    if (!isFpk(data)) {
        m_error = QStringLiteral("not an FPK ('foxfpk' magic missing)");
        return false;
    }
    m_fpkd = data.at(6) == 'd';

    Cursor c{data.constData(), data.size()};
    c.skip(10);                 // "foxfpk" + type + "win"
    c.u32();                    // fileSize
    c.skip(18);
    c.u32();                    // constant 2
    const quint32 fileCount = c.u32();
    const quint32 referenceCount = c.u32();
    c.skip(4);
    if (!c.ok || fileCount > 1000000u || referenceCount > 100000u) {
        m_error = QStringLiteral("corrupt FPK header");
        return false;
    }

    m_entries.reserve(static_cast<int>(fileCount));
    for (quint32 i = 0; i < fileCount && c.ok; ++i) {
        FpkEntry e;
        e.dataOffset = c.u32();
        c.skip(4);
        e.dataSize = static_cast<qint32>(c.u32());
        c.skip(4);
        e.filePath = readFpkString(c, data);
        const QByteArray md5 = c.bytes(16);
        if (md5.size() == 16) std::memcpy(e.md5, md5.constData(), 16);
        if (c.ok) m_entries.append(e);
    }
    for (quint32 i = 0; i < referenceCount && c.ok; ++i)
        m_references.append(readFpkString(c, data));

    if (!c.ok) {
        m_error = QStringLiteral("truncated FPK entry table");
        m_entries.clear();
        m_references.clear();
        return false;
    }
    return true;
}

QString FpkFile::normalizedPath(const QString& storedPath)
{
    QString p = storedPath;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const int colon = p.indexOf(QLatin1Char(':'));
    if (colon != -1) p = p.mid(colon + 1);
    return p;
}

QByteArray FpkFile::readEntry(const QByteArray& data, const FpkEntry& entry)
{
    if (entry.dataSize < 0
        || static_cast<qint64>(entry.dataOffset) + entry.dataSize > data.size())
        return {};
    QByteArray result(data.constData() + entry.dataOffset, entry.dataSize);

    // GZ-era encrypted payload: marker byte 0x1B/0x1C, then bytes XOR-chained
    // against the ~legacy-hash of the lower-cased file NAME (extension kept).
    if (entry.dataSize > 0) {
        const quint8 marker = static_cast<quint8>(result.at(0));
        if (marker == 0x1B || marker == 0x1C) {
            const QString norm = normalizedPath(entry.filePath);
            const QString fileName = norm.section(QLatin1Char('/'), -1).toLower();
            const quint64 hash = hashFileNameLegacy(fileName, /*removeExtension=*/false);
            quint64 keyVal = ~hash;
            quint8 key[8];
            qToLittleEndian(keyVal, key);

            QByteArray plain(result.size() - 1, Qt::Uninitialized);
            for (qsizetype i = 0; i < result.size() - 1; ++i) {
                key[i % 8] = static_cast<quint8>(
                    key[i % 8] ^ static_cast<quint8>(result.at(i + 1)));
                plain[i] = static_cast<char>(key[i % 8]);
            }
            // A valid decryption ends in a trailing NUL that is then dropped.
            if (!plain.isEmpty() && plain.at(plain.size() - 1) == '\0') {
                plain.chop(1);
                result = plain;
            }
        }
    }
    return result;
}

}  // namespace fox
