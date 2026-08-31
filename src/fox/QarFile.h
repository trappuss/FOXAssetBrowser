// QarFile.h — reader for Fox Engine SQAR archives (.dat / .qar / .g0s).
//
// Byte-exact port of GzsTool.Core's QarFile/QarEntry + Decrypt1Stream/
// Decrypt2Stream (the community reference that round-trips the shipped
// archives). Two container versions exist: 1 (MGSV TPP/GZ) and 2 (Survive);
// they differ in the section-list cipher, the size-field order and the
// Decrypt1 keystream.
//
// Reading is two-phase by design:
//   • open() parses ONLY the header + section table + per-entry headers —
//     cheap enough to index a whole install's archives up front.
//   • readEntry() seeks, decrypts (Decrypt1, then Decrypt2 when the payload
//     carries an encryption magic) and inflates one entry on demand.
#pragma once
#include <QFile>
#include <QString>
#include <QVector>
#include <cstdint>

namespace fox {

struct QarEntry {
    quint64 hash = 0;              // full 64-bit PathFileNameCode (incl. meta bit)
    quint32 uncompressedSize = 0;
    quint32 compressedSize = 0;
    qint64  dataOffset = 0;        // absolute offset of the payload in the archive
    quint8  md5[16] = {};          // per-entry data hash — also the Decrypt1 seed (v2)
    quint32 encryption = 0;        // 0, Magic1 (0xA0F8EFE6) or Magic2 (0xE3F8EFE6)
    quint32 key = 0;               // Decrypt2 key when encryption != 0
    bool    compressed = false;
};

class QarFile {
public:
    // True when the stream starts with 'SQAR'.
    static bool isQar(const QString& filePath);

    // Parse header + entry table. On failure returns false and errorString() says why.
    bool open(const QString& filePath);

    // Install a table that was parsed on an earlier run instead of reading the
    // archive again. The entry scan is the whole cost of indexing an install
    // (193k scattered header reads over a 17-archive set, ~6 s), and it
    // produces the same answer every launch until the file itself changes —
    // so ArchiveIndex persists it and hands it back here. The caller owns
    // deciding the table is still valid; see ArchiveIndex's archive cache.
    void adopt(const QString& filePath, quint32 version, quint32 flags,
               int unreadableEntries, QVector<QarEntry>&& entries);

    const QString& filePath() const { return m_filePath; }
    quint32 version() const { return m_version; }
    quint32 flags() const { return m_flags; }
    const QVector<QarEntry>& entries() const { return m_entries; }
    // Entry headers that were all zeros — a hole in the file rather than an
    // entry (truncated download, partial copy, damaged archive). Non-zero here
    // means the archive is incomplete and some assets simply are not present.
    int unreadableEntries() const { return m_unreadableEntries; }
    QString errorString() const { return m_error; }

    // Decrypt + decompress one entry. Thread-safe: opens its own file handle,
    // so extraction can fan out across archives AND within one archive.
    QByteArray readEntry(const QarEntry& entry) const;

    // Decrypt WITHOUT the final zlib inflate (diagnostic / raw dumps).
    QByteArray readEntryRaw(const QarEntry& entry) const;

private:
    QByteArray readDecrypted(const QarEntry& entry) const;

    QString m_filePath;
    QString m_error;
    int m_unreadableEntries = 0;
    quint32 m_flags = 0;
    quint32 m_version = 0;
    QVector<QarEntry> m_entries;
};

// The two per-entry payload ciphers, exposed for reuse (FPKD-in-dat payloads
// arrive through the same path). Both operate in place.
namespace qarcrypto {
constexpr quint32 kMagic1 = 0xA0F8EFE6;   // 8-byte header: magic, key
constexpr quint32 kMagic2 = 0xE3F8EFE6;   // 16-byte header: magic, key, size, size

// Decrypt1: XOR keystream indexed by (hashLow, absolute offset/11); version 2
// additionally mixes a 64-bit seed taken from the entry's MD5.
void decrypt1(QByteArray& data, quint32 version, quint32 hashLow,
              const quint8 md5[16], qint64 startOffset = 0);

// Decrypt2: multiplicative-congruential XOR stream over 32-bit words
// (the final 0–3 bytes stay clear).
void decrypt2(QByteArray& data, quint32 key);
}  // namespace qarcrypto

}  // namespace fox
