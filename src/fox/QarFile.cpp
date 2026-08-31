// QarFile.cpp — see QarFile.h. Constants and control flow mirror
// GzsTool.Core (QarFile.cs / QarEntry.cs / Decrypt1Stream.cs / Decrypt2Stream.cs)
// exactly; deviations are marked.
#include "fox/QarFile.h"

#include <QDataStream>
#include <QtEndian>
#include <cstring>
#include <utility>

#include "fox/FoxZlib.h"

namespace fox {
namespace {

constexpr quint32 kQarMagic = 0x52415153;   // 'SQAR'
constexpr quint32 kXor1 = 0x41441043;
constexpr quint32 kXor2 = 0x11C22050;
constexpr quint32 kXor3 = 0xD05608C3;
constexpr quint32 kXor4 = 0x532C7319;
const quint32 kXorTable[4] = {kXor1, kXor2, kXor3, kXor4};

quint32 readU32(QFile& f, bool* ok)
{
    quint8 b[4];
    if (f.read(reinterpret_cast<char*>(b), 4) != 4) { *ok = false; return 0; }
    return qFromLittleEndian<quint32>(b);
}

// Section-list decryption — QarFile.DecryptSectionList. Version 2 chains a
// rotating XOR through the list; version 1 is a fixed 4-entry XOR table.
QVector<quint64> decryptSectionList(quint32 fileCount, const QByteArray& sections,
                                    quint32 version)
{
    QVector<quint64> result(static_cast<int>(fileCount));
    const char* p = sections.constData();

    if (version != 2) {
        for (int i = 0; i < result.size(); ++i) {
            const int offset1 = i * 8;
            const int offset2 = i * 8 + 4;
            quint32 i1 = qFromLittleEndian<quint32>(p + offset1);
            quint32 i2 = qFromLittleEndian<quint32>(p + offset2);
            i1 ^= kXorTable[(i + offset1 / 5) % 4];
            i2 ^= kXorTable[(i + offset2 / 5) % 4];
            result[i] = (static_cast<quint64>(i2) << 32) | i1;
        }
    } else {
        quint32 xorKey = 0xA2C18EC3;
        for (int i = 0; i < result.size(); ++i) {
            const int offset1 = i * 8;
            const int offset2 = i * 8 + 4;
            const quint32 section1 = qFromLittleEndian<quint32>(p + offset1);
            const quint32 section2 = qFromLittleEndian<quint32>(p + offset2);
            const quint32 i1 = section1 ^ kXorTable[(xorKey + offset1 / 5) % 4];
            const quint32 i2 = section2 ^ kXorTable[(xorKey + offset2 / 5) % 4];
            result[i] = (static_cast<quint64>(i2) << 32) | i1;

            const int rotation = static_cast<int>(i2 / 256) % 19;
            const quint32 rotated =
                rotation == 0 ? i1 : ((i1 >> rotation) | (i1 << (32 - rotation)));
            xorKey ^= rotated;
        }
    }
    return result;
}

}  // namespace

namespace qarcrypto {

void decrypt1(QByteArray& data, quint32 version, quint32 hashLow,
              const quint8 md5[16], qint64 startOffset)
{
    static const quint32 kTable[8] = {
        0xBB8ADEDB, 0x65229958, 0x08453206, 0x88121302,
        0x4C344955, 0x2C02F10C, 0x4887F823, 0xF3818583,
    };

    // v2 seeds the stream from one half of the entry MD5, picked by hash parity.
    quint64 seed = 0;
    std::memcpy(&seed, md5 + (hashLow % 2) * 8, 8);
    const quint32 seedLow = static_cast<quint32>(seed);
    const quint32 seedHigh = static_cast<quint32>(seed >> 32);

    char* raw = data.data();
    const qsizetype len = data.size();
    const qsizetype blocks = len / 8;

    if (version != 2) {
        for (qsizetype i = 0; i < blocks; ++i) {
            const qsizetype offset1 = i * 8;
            const qint64 abs1 = offset1 + startOffset;
            const int index = static_cast<int>(2 * ((hashLow + abs1 / 11) % 4));
            quint32 u1 = qFromLittleEndian<quint32>(raw + offset1) ^ kTable[index];
            quint32 u2 = qFromLittleEndian<quint32>(raw + offset1 + 4) ^ kTable[index + 1];
            qToLittleEndian(u1, raw + offset1);
            qToLittleEndian(u2, raw + offset1 + 4);
        }
        const qsizetype remaining = len % 8;
        for (qsizetype i = 0; i < remaining; ++i) {
            const qsizetype offset = blocks * 8 + i;
            const qint64 offsetAbs = offset + startOffset;
            const int index = static_cast<int>(
                2 * ((hashLow + (offsetAbs - (offsetAbs % 8)) / 11) % 4));
            const int decryptionIndex = static_cast<int>(offset % 8);
            const quint32 xorMask =
                decryptionIndex < 4 ? kTable[index] : kTable[index + 1];
            // NOTE: mirrors the C# byte path bit-for-bit, including its shift by
            // (8*decryptionIndex) — for indices 4-7 that shift exceeds 31 and
            // C#'s '<<' masks the count mod 32, which is what the game does too.
            const int shift = (8 * decryptionIndex) & 31;
            const quint8 xorMaskByte = static_cast<quint8>((xorMask >> shift) & 0xff);
            raw[offset] = static_cast<char>(static_cast<quint8>(raw[offset]) ^ xorMaskByte);
        }
    } else {
        for (qsizetype i = 0; i < blocks; ++i) {
            const qsizetype offset1 = i * 8;
            const qint64 abs1 = offset1 + startOffset;
            const int index = static_cast<int>(
                2 * ((hashLow + seed + static_cast<quint64>(abs1 / 11)) % 4));
            quint32 u1 = qFromLittleEndian<quint32>(raw + offset1) ^ kTable[index] ^ seedLow;
            quint32 u2 =
                qFromLittleEndian<quint32>(raw + offset1 + 4) ^ kTable[index + 1] ^ seedHigh;
            qToLittleEndian(u1, raw + offset1);
            qToLittleEndian(u2, raw + offset1 + 4);
        }
        const qsizetype remaining = len % 8;
        for (qsizetype i = 0; i < remaining; ++i) {
            const qsizetype offset = blocks * 8 + i;
            const qsizetype offsetBlock = offset - (offset % 8);
            const qint64 offsetBlockAbs = offsetBlock + startOffset;
            const int index = static_cast<int>(
                2 * ((hashLow + seed + static_cast<quint64>(offsetBlockAbs / 11)) % 4));
            const int decryptionIndex = static_cast<int>(offset % 8);
            const quint32 xorMask =
                decryptionIndex < 4 ? kTable[index] : kTable[index + 1];
            const quint8 xorMaskByte =
                static_cast<quint8>((xorMask >> (8 * (decryptionIndex % 4))) & 0xff);
            const quint32 seedMask = decryptionIndex < 4 ? seedLow : seedHigh;
            const quint8 seedByte =
                static_cast<quint8>((seedMask >> (8 * (decryptionIndex % 4))) & 0xff);
            raw[offset] = static_cast<char>(
                static_cast<quint8>(raw[offset]) ^ (xorMaskByte ^ seedByte));
        }
    }
}

void decrypt2(QByteArray& data, quint32 key)
{
    const quint32 mulKey = 278u * key;
    quint32 blockKey = key | ((key ^ 25974u) << 16);

    char* raw = data.data();
    qsizetype size = data.size();
    qsizetype pos = 0;
    while (size >= 4) {
        quint32 v = qFromLittleEndian<quint32>(raw + pos);
        v ^= blockKey;
        qToLittleEndian(v, raw + pos);
        blockKey = mulKey + 48828125u * blockKey;
        pos += 4;
        size -= 4;
    }
    // The final 0-3 bytes are not encrypted.
}

}  // namespace qarcrypto

bool QarFile::isQar(const QString& filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    quint8 b[4];
    if (f.read(reinterpret_cast<char*>(b), 4) != 4) return false;
    return qFromLittleEndian<quint32>(b) == kQarMagic;
}

bool QarFile::open(const QString& filePath)
{
    m_unreadableEntries = 0;
    m_filePath = filePath;
    m_entries.clear();
    m_error.clear();

    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        m_error = QStringLiteral("cannot open: %1").arg(f.errorString());
        return false;
    }

    bool ok = true;
    const quint32 magic = readU32(f, &ok);
    if (!ok || magic != kQarMagic) {
        m_error = QStringLiteral("not an SQAR archive");
        return false;
    }
    m_flags = readU32(f, &ok) ^ kXor1;
    const quint32 fileCount = readU32(f, &ok) ^ kXor2;
    const quint32 unknownCount = readU32(f, &ok) ^ kXor3;
    readU32(f, &ok);                       // blockFileEnd ^ kXor4 (unused on read)
    readU32(f, &ok);                       // offsetFirstFile ^ kXor1 (unused on read)
    m_version = readU32(f, &ok) ^ kXor1;   // 1 = MGSV, 2 = Survive
    readU32(f, &ok);                       // unknown2 ^ kXor2
    if (!ok) {
        m_error = QStringLiteral("truncated header");
        return false;
    }
    // Sanity bound: fileCount drives an 8-byte-per-entry table read. A corrupt
    // count would otherwise ask for gigabytes. (Deviation from GzsTool, which
    // trusts the field; the largest shipped archive holds ~127k entries.)
    if (fileCount > 4000000u) {
        m_error = QStringLiteral("implausible entry count %1").arg(fileCount);
        return false;
    }

    const int blockShiftBits = (m_flags & 0x800) ? 12 : 10;

    const QByteArray sectionsData = f.read(static_cast<qint64>(8) * fileCount);
    if (sectionsData.size() != static_cast<qsizetype>(8 * fileCount)) {
        m_error = QStringLiteral("truncated section table");
        return false;
    }
    f.seek(f.pos() + static_cast<qint64>(16) * unknownCount);

    const QVector<quint64> sections = decryptSectionList(fileCount, sectionsData, m_version);

    // Read each 32-byte header where its section points. This looks like it
    // wants optimising — 193k scattered seek+read pairs over a 17-archive
    // install — and TWO attempts measured WORSE, both for the same reason:
    // shipped archives are SPARSE. tpp_texture0.dat is 3.5 GB apparent and
    // ~400 MB allocated; the median gap between consecutive headers is 10 KB
    // and the 90th percentile is 285 KB. Reading a 32-byte header out of a
    // hole costs no I/O at all.
    //
    //   sorted 1 MB sliding window   14.2 s  (turns hole-skips into real reads)
    //   whole-file mmap              44.4 s  (~128 KB of fault-ahead per 32 B)
    //   this                          6.8 s
    //
    // So the scan stays as it is and the COST IS REMOVED INSTEAD by caching
    // the parsed table — see ArchiveIndex, which persists these entries and
    // skips this function entirely on an unchanged install.
    m_entries.reserve(sections.size());
    for (const quint64 section : sections) {
        const quint64 sectionBlock = section >> 40;
        const qint64 sectionOffset = static_cast<qint64>(sectionBlock) << blockShiftBits;
        if (!f.seek(sectionOffset)) continue;

        // QarEntry.Read: a 32-byte XOR-masked header, then the payload. The
        // 8-byte peek below follows immediately, so both come in one call —
        // the second read always hit QFile's buffer anyway, but this drops a
        // QByteArray allocation per entry. The peek stays OPTIONAL: it could
        // come up short at end of file when it was its own read, and a header
        // with no payload behind it must still register as an entry.
        quint8 hdr[40];
        const qint64 got = f.read(reinterpret_cast<char*>(hdr), 40);
        if (got < 32) continue;
        const bool havePeek = got == 40;

        // A header of all zeros is not an entry — it is a hole: a truncated
        // download, a sparse/partial copy, or a damaged archive. Decrypting it
        // yields the XOR key itself, so EVERY such entry decodes to the same
        // plausible-looking hash and the index fills with thousands of
        // identical files. Detect it on the raw bytes, before the XOR.
        bool allZero = true;
        for (int i = 0; i < 32; ++i)
            if (hdr[i] != 0) { allZero = false; break; }
        if (allZero) { ++m_unreadableEntries; continue; }
        const quint32 hashLow = qFromLittleEndian<quint32>(hdr + 0) ^ kXor1;
        const quint32 hashHigh = qFromLittleEndian<quint32>(hdr + 4) ^ kXor1;
        const quint32 size1 = qFromLittleEndian<quint32>(hdr + 8) ^ kXor2;
        const quint32 size2 = qFromLittleEndian<quint32>(hdr + 12) ^ kXor3;
        const quint32 md51 = qFromLittleEndian<quint32>(hdr + 16) ^ kXor4;
        const quint32 md52 = qFromLittleEndian<quint32>(hdr + 20) ^ kXor1;
        const quint32 md53 = qFromLittleEndian<quint32>(hdr + 24) ^ kXor1;
        const quint32 md54 = qFromLittleEndian<quint32>(hdr + 28) ^ kXor2;

        QarEntry e;
        e.hash = (static_cast<quint64>(hashHigh) << 32) | hashLow;
        e.uncompressedSize = m_version != 2 ? size1 : size2;
        e.compressedSize = m_version != 2 ? size2 : size1;
        e.compressed = e.uncompressedSize != e.compressedSize;
        qToLittleEndian(md51, e.md5 + 0);
        qToLittleEndian(md52, e.md5 + 4);
        qToLittleEndian(md53, e.md5 + 8);
        qToLittleEndian(md54, e.md5 + 12);
        e.dataOffset = sectionOffset + 32;

        // Peek the first 8 payload bytes through Decrypt1 to detect the inner
        // (Decrypt2) encryption header.
        if (havePeek) {
            QByteArray head(reinterpret_cast<const char*>(hdr) + 32, 8);
            qarcrypto::decrypt1(head, m_version, hashLow, e.md5, 0);
            const quint32 maybeMagic = qFromLittleEndian<quint32>(head.constData());
            if (maybeMagic == qarcrypto::kMagic1 || maybeMagic == qarcrypto::kMagic2) {
                e.encryption = maybeMagic;
                e.key = qFromLittleEndian<quint32>(head.constData() + 4);
            }
        }
        m_entries.append(e);
    }
    return true;
}

void QarFile::adopt(const QString& filePath, quint32 version, quint32 flags,
                    int unreadableEntries, QVector<QarEntry>&& entries)
{
    m_filePath = filePath;
    m_version = version;
    m_flags = flags;
    m_unreadableEntries = unreadableEntries;
    m_entries = std::move(entries);
    m_error.clear();
}

QByteArray QarFile::readDecrypted(const QarEntry& entry) const
{
    QFile f(m_filePath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    if (!f.seek(entry.dataOffset)) return {};

    QByteArray data = f.read(entry.compressedSize);
    if (data.size() != static_cast<qsizetype>(entry.compressedSize)) return {};

    qarcrypto::decrypt1(data, m_version, static_cast<quint32>(entry.hash & 0xFFFFFFFF),
                        entry.md5, 0);

    if (entry.encryption == qarcrypto::kMagic1 || entry.encryption == qarcrypto::kMagic2) {
        const int headerSize = entry.encryption == qarcrypto::kMagic1 ? 8 : 16;
        data.remove(0, headerSize);
        qarcrypto::decrypt2(data, entry.key);
    }
    return data;
}

QByteArray QarFile::readEntryRaw(const QarEntry& entry) const
{
    return readDecrypted(entry);
}

QByteArray QarFile::readEntry(const QarEntry& entry) const
{
    QByteArray data = readDecrypted(entry);
    if (data.isEmpty()) return data;
    if (entry.compressed)
        data = zlibInflate(data, static_cast<int>(entry.uncompressedSize));
    return data;
}

}  // namespace fox
