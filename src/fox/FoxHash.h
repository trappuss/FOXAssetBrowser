// FoxHash.h — Fox Engine name hashing (PathFileNameCode), the 13-bit extension
// codes, and hash→name resolution against the community dictionaries.
//
// The 64-bit code every archive keys files by is:
//     low 51 bits : CityHash64WithSeeds(path-without-extension) & 0x3FFFFFFFFFFFF
//     top 13 bits : extension code = (same hash of the bare extension text) & 0x1FFF
// plus a "meta" flag bit (0x4000000000000) for paths outside /Assets/ (and
// /Assets/tpptest). This is byte-for-byte the scheme in GzsTool's
// Hashing.cs, which round-trips against the shipped archives.
#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace fox {

constexpr quint64 kMetaFlag  = 0x4000000000000ULL;
constexpr quint64 kPathMask  = 0x3FFFFFFFFFFFFULL;   // low 51 bits

// Hash of a path with the extension stripped (everything from the FIRST '.').
// Handles the /Assets/ prefix-strip and meta-flag rules. `removeExtension=false`
// hashes the text as given (used for extension codes).
quint64 hashFileName(const QString& text, bool removeExtension = true);

// The full 64-bit code for a path INCLUDING its extension: (extCode<<51)|pathHash.
quint64 hashFileNameWithExtension(const QString& filePath);

// Latin-1 byte forms of the two hashes above. The QString versions are thin
// wrappers that convert and call these, so there is one implementation of each
// rule; call these directly when the text is already bytes and the conversion
// would be pure overhead (the dictionary loader hashes 410k lines).
quint64 hashFileNameLatin1(const char* text, int len, bool removeExtension = true);
quint64 hashFileNameLegacyLatin1(const char* text, int len,
                                 bool removeExtension = true);

// The pre-SQAR "legacy" hash (GZ-era FPK entry encryption keys):
// CityHash64WithSeeds(text+'\0', seed0, (text[0]<<16)+len) & 0xFFFFFFFFFFFF.
quint64 hashFileNameLegacy(const QString& text, bool removeExtension = true);

// 13-bit extension code for a bare extension string ("ftex", "1.ftexs", ...).
quint64 hashExtension(const QString& ext);

// ── Dictionary-backed resolution ─────────────────────────────────────────────
// One process-wide table: pathHash(51-bit) → path text, plus the fixed
// extension-code → extension-text map built from GzsTool's known-extension list.
class HashResolver {
public:
    static HashResolver& instance();

    // Read one dictionary file (one path per line) and QUEUE its lines. Every
    // line is indexed under BOTH hash schemes (TPP PathFileNameCode and the
    // GZ-era legacy hash). Returns lines read.
    //
    // NOTHING IS LOOKED UP UNTIL THE TABLES ARE BUILT. Building three tables
    // per file meant the shipped qar dictionary — 388,376 lines — paid for
    // three QHash inserts per line, measured at 347 ms of the 500 ms the whole
    // index build took. Queueing costs 31 ms and the build then runs once, on
    // three threads, in about 45 ms. finishDictionaries() does that; every
    // lookup also triggers it, so a caller that forgets gets a slow first
    // lookup rather than an empty answer.
    int loadDictionary(const QString& filePath);
    // Build the lookup tables from everything queued so far. Idempotent, and
    // cheap when nothing has been queued since the last call.
    void finishDictionaries() const { ensureBuilt(); }

    // Full-code resolution. Returns true only when BOTH the path and the
    // extension resolved; `nameOut` always receives something usable
    // ("<hex>.<ext>", "<path>.<ext>" or "<hex>._unknown").
    bool tryResolve(quint64 hash, QString* nameOut) const;
    // Every dictionary name currently loaded (dev/tooling: lets an external
    // extractor be handed name → path-hash pairs instead of re-implementing
    // CityHash).
    QList<QString> allNames() const;
    // Every extension the hasher knows, for tooling that has to name a pulled
    // archive entry from its key.
    QList<QString> allExtensions() const { return m_extensions.values(); }

    // Ground Zeroes .g0s resolution: hash = legacy 48-bit path hash with the
    // extension ID (a table index, not a hash) at bit 52.
    bool tryResolveGzs(quint64 hash, QString* nameOut) const;

    // StrCode64 lookup (FMDL bone/material names use the legacy hash). Empty
    // string when unknown.
    QString legacyNameFor(quint64 hash) const;

    // StrCode32 lookup (gani track / motion-point names = low 32 bits of the
    // legacy hash). Empty string when unknown.
    QString strCode32NameFor(quint32 hash) const;

    // Extension text for a 13-bit code ("" when unknown).
    QString extensionFor(quint64 extCode) const;

    int size() const;
    // How many dictionary FILES have been read. The "have the dictionaries
    // been loaded yet" guard, and it has to be this rather than size(): the
    // resolver ships with a built-in vocabulary of engine strings, so the
    // table is never empty and a size()-based guard silently skipped every
    // dictionary the moment those strings moved into the same table.
    int dictionaryFileCount() const { return m_dictFiles; }

private:
    HashResolver();

    // ── Storage: one blob of bytes, three flat open-addressed tables ─────
    // The names are NOT stored as QStrings. 400k paths in three
    // QHash<quint64, QString> tables is 1.2M node allocations at startup and
    // ~60 MB resident; the text is already in memory as the file's own bytes,
    // so a table entry is a key and a (offset, length) into that. A lookup
    // builds the QString it returns, which costs one allocation on a path
    // nobody walks in a loop.
    //
    // Linear probing over a power-of-two array, so a probe is a cache line
    // rather than a pointer chase. len == 0 marks an empty slot, which is why
    // the blob starts with one pad byte: offset 0 must never be a real entry.
    struct Slot {
        quint64 key = 0;
        quint32 off = 0;
        quint32 len = 0;
    };
    struct Table {
        // NOT named `slots`: Qt #defines that as a keyword and the member
        // vanished, with the compiler only saying "declaration does not declare
        // anything".
        QVector<Slot> cells;
        quint64 mask = 0;
        int count = 0;
        void reset(qsizetype wantEntries);
        void insert(quint64 key, quint32 off, quint32 len);
        const Slot* find(quint64 key) const;
    };
    // One parsed line, awaiting the build. Both hashes are computed once here;
    // the three tables key off them differently.
    struct Pending {
        quint64 path = 0;
        quint64 legacy = 0;
        quint32 off = 0;
        quint32 len = 0;
        // The built-in engine vocabulary is a StrCode64/StrCode32 answer only.
        // Those strings are material roles, not asset paths, so indexing them
        // under the TPP path hash as well would let one of them win the name
        // of a real archive entry that happened to collide — and because they
        // are queued first, it would win. They were never in that table
        // before; they stay out of it.
        bool pathIndexed = true;
    };

    void queue(const char* text, int len, bool pathIndexed = true);
    void build() const;
    void ensureBuilt() const
    {
        if (!m_built.load(std::memory_order_acquire)) build();
    }
    QString textAt(const Slot* s) const
    {
        return s ? QString::fromLatin1(m_blob.constData() + s->off, int(s->len))
                 : QString();
    }

    QByteArray m_blob;              // every dictionary's bytes, back to back
    QVector<Pending> m_pending;     // parsed lines, in first-wins order
    mutable Table m_names;          // masked path hash → path (TPP scheme)
    mutable Table m_legacyNames;    // 48-bit legacy hash → path (GZ scheme)
    mutable Table m_strCode32;      // low 32 bits of the legacy hash → name
    mutable std::atomic<bool> m_built{false};
    mutable std::mutex m_buildMutex;
    int m_dictFiles = 0;
    QHash<quint64, QString> m_extensions;   // 13-bit code → extension text
};

}  // namespace fox
