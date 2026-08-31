// GzsFile.h — reader for Ground Zeroes' .g0s archives (the format GzsTool was
// named after; support was removed from that tool in 2015 — this port comes
// from its git history, commit c889fa8, GzsTool/Gzs/* + Utility/Encryption.cs).
//
// Layout (nothing at the file start — detection is by FOOTER):
//   • last 20 bytes: {i32 entryCount, i32 0x71610000, i32 entryBlockOffset,
//     i32 0, i32 footerSize == 20}
//   • entry table at 16*entryBlockOffset: {u64 hash, u32 offset16, u32 size}
//     — data lives at 16*offset16.
//   • per-entry: an outer XOR stream keyed by the entry's 16-byte-block offset
//     ("DeEncryptQar"), then optionally magic 0xA0F8EFE6 + u32 key and a
//     second multiplicative XOR stream ("DeEncrypt"). No zlib layer.
//   • hash = legacyPathHash(48 bits) | extensionId << 52, where the id indexes
//     a fixed 0–106 table (NOT a hash — this is why GZ "extension codes" look
//     sequential) and the path hash is the pre-SQAR CityHash variant
//     (hashFileNameLegacy).
#pragma once
#include <QString>
#include <QVector>
#include <cstdint>

namespace fox {

struct GzsEntry {
    quint64 hash = 0;       // legacy path hash | extId << 52
    quint32 offset16 = 0;   // data offset in 16-byte blocks
    quint32 size = 0;
};

class GzsFile {
public:
    // Footer check (0x71610000 magic + footerSize 20).
    static bool isGzs(const QString& filePath);

    bool open(const QString& filePath);

    // Install a table parsed on an earlier run — see QarFile::adopt.
    void adopt(const QString& filePath, QVector<GzsEntry>&& entries);

    const QString& filePath() const { return m_filePath; }
    const QVector<GzsEntry>& entries() const { return m_entries; }
    QString errorString() const { return m_error; }

    // Decrypt one entry (outer stream + optional inner key stream).
    QByteArray readEntry(const GzsEntry& entry) const;

    // Extension text for a GZ extension id ("" for id 0, "?" when out of table).
    static QString extensionForId(int extId);
    static int extensionCount();

private:
    QString m_filePath;
    QString m_error;
    QVector<GzsEntry> m_entries;
};

}  // namespace fox
