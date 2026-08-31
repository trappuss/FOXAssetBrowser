// FpkFile.h — reader for Fox Engine FPK / FPKD packages ("foxfpk"/"foxfpkd
// win" magic). These are the nested containers most .dat entries actually are:
// a top-level archive walk that stops at .fpk/.fpkd sees only container hashes
// — the real fmdl/ftex/mtar/lua population lives one level down, in here.
//
// Byte-exact port of GzsTool.Core/Fpk. Entries carry a real path string
// (sometimes still encrypted with the legacy-hash XOR — handled), so FPK
// contents need no dictionary at all.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <cstdint>

namespace fox {

struct FpkEntry {
    QString filePath;         // as stored (may begin with a drive prefix "Z:")
    quint32 dataOffset = 0;   // relative to the FPK blob start
    qint32  dataSize = 0;
    quint8  md5[16] = {};
};

class FpkFile {
public:
    static bool isFpk(const QByteArray& data);

    // Parse the header + entry table from an in-memory FPK blob (they arrive
    // out of QAR entries, so there is no file handle to stream from).
    bool parse(const QByteArray& data);

    bool isFpkd() const { return m_fpkd; }
    const QVector<FpkEntry>& entries() const { return m_entries; }
    const QVector<QString>& references() const { return m_references; }
    QString errorString() const { return m_error; }

    // Extract one entry's bytes (applies the legacy-hash payload decryption
    // when the 0x1B/0x1C marker byte is present). `data` must be the same blob
    // given to parse().
    static QByteArray readEntry(const QByteArray& data, const FpkEntry& entry);

    // "Z:/foo/bar.fmdl" → "/foo/bar.fmdl" (drive prefix dropped, slashes forward).
    static QString normalizedPath(const QString& storedPath);

private:
    bool m_fpkd = false;
    QVector<FpkEntry> m_entries;
    QVector<QString> m_references;
    QString m_error;
};

}  // namespace fox
