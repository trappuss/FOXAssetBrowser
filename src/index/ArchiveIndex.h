// ArchiveIndex.h — the in-memory catalogue every tab works from.
//
// Scan: walk the configured game folder(s), open every SQAR archive found
// (magic-sniffed, not extension-matched — TPP/GZ/Survive name their .dat/.qar
// files differently), parse entry tables, resolve names against the loaded
// dictionaries. That much is cheap (headers only) and runs on a background
// thread at startup or when the folder changes.
//
// Deep scan (optional, cached): FPK/FPKD/PFTXS entries are CONTAINERS — a
// top-level walk sees only their hash, while the real fmdl/ftex/lua population
// lives inside ("the container trap"). The deep pass decompresses each
// container once, records its child listing (paths only, data discarded), and
// caches the result on disk keyed by every archive's (path, size, mtime), so
// the second launch is instant.
//
// Threading contract: rebuild() runs on a detached worker; the built state is
// swapped in on the GUI thread via a queued call, and readyChanged(bool) tells
// views to repopulate. A generation counter discards an in-flight build when
// the game folder changes mid-scan (the D4 background-index shape).
#pragma once
#include <QHash>
#include <QMultiHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <cstdint>
#include <memory>

#include "fox/GzsFile.h"
#include "index/GameId.h"
#include "fox/QarFile.h"

namespace fox {

// One extractable file, top-level or inside a container.
// Delete index caches written by SUPERSEDED versions of this build. Template
// §1: "superseded cache versions are pruned automatically at startup". Called
// once from main(), before anything reads one. Returns how many it removed.
int pruneOldCaches();

struct IndexedFile {
    int archiveId = -1;      // index into ArchiveIndex::archives()
    int entryIdx = -1;       // index into that archive's entry table
    qint32 childIdx = -1;    // -1 = the archive entry itself; ≥0 = file inside the container
    quint64 hash = 0;        // PathFileNameCode — or the GZ legacy scheme when gz
    quint32 size = 0;        // uncompressed size (children: size inside the container)
    bool named = false;      // resolved via dictionary / stored path
    bool gz = false;         // true = entry of a .g0s archive (GZ hash scheme)
    bool shadowed = false;   // another archive with higher mount priority also
                             // carries this exact hash — this copy is the one
                             // the GAME would not load (mod installs do this)
    QString path;            // display path, always forward-slashed
};

// Loose: a plain directory of extracted assets, mounted as if it were an
// archive. Development only — it is how a partial install can be topped up with
// the handful of files a feature needs, so the browser can be exercised against
// real data without a full copy of the game.
enum class ArchiveKind : quint8 { Sqar, Gzs, Loose };

struct IndexedArchive {
    QString filePath;        // absolute path on disk
    QString shortName;       // file name only, for the tree root
    // Mount priority: higher wins when two archives carry the same hash.
    // Fox mounts the numbered "master\<N>\00.dat/01.dat" slots as well as the
    // chunk/texture archives (all four names appear in mgsvtpp.exe), and mod
    // managers put replacement assets in those numbered slots. Archives are
    // sorted by (priority, path) so indexing is deterministic instead of
    // depending on directory-iteration order.
    int priority = 0;
    ArchiveKind kind = ArchiveKind::Sqar;
    // Which game this archive belongs to. Decided by a MAJORITY VOTE over the
    // asset paths its named entries resolve to, not by its file name: one
    // install can hold four games and "chunk0.dat" is a name three of them use.
    // A .g0s is Ground Zeroes by construction (nothing else ships that format).
    GameId game = GameId::Unknown;
    std::shared_ptr<QarFile> qar;   // when kind == Sqar
    std::shared_ptr<GzsFile> gzs;   // when kind == Gzs
    QStringList loosePaths;         // when kind == Loose: file per entry index
    // …and its size, captured during the SAME directory walk. QDirIterator
    // already has the stat; asking QFileInfo for it again later is a second
    // syscall per file, and a loose install is tens of thousands of files.
    QVector<quint32> looseSizes;
};

enum class ContainerKind : quint8 { None, Fpk, Fpkd, Pftxs, Sbp };

class ArchiveIndex : public QObject {
    Q_OBJECT
public:
    static ArchiveIndex& instance();

    // Configure + kick a rebuild (returns immediately; work is on a thread).
    void rebuild(const QStringList& gameDirs, const QString& dictDir, bool deepScan);
    bool ready() const { return m_ready.load(); }
    bool building() const { return m_building.load(); }

    const QVector<IndexedArchive>& archives() const { return m_archives; }
    // The game one file belongs to: its own asset path when it has one, the
    // archive's majority answer when it does not.
    GameId gameOf(const IndexedFile& f) const;
    const QVector<IndexedFile>& files() const { return m_files; }
    int namedCount() const { return m_namedCount; }
    int dictionaryEntries() const { return m_dictEntries; }
    bool deepScanned() const { return m_deepScanned; }

    // The file's extension text ("ftex", "fpk", …, "" when unknown) — dispatches
    // between the TPP extension-code scheme and the GZ id table.
    static QString extensionOf(const IndexedFile& f);
    // What container (if any) an entry is, judged by its extension.
    static ContainerKind containerKindFor(const IndexedFile& f);

    // Read one indexed file's bytes (decrypt + inflate; for children, also
    // open the container). Thread-safe.
    QByteArray readFile(const IndexedFile& f) const;

    // Find a top-level entry by full 64-bit hash (ftexs sibling lookup etc.).
    const IndexedFile* findByHash(quint64 hash) const;
    // Index into files() for an asset path, or -1. O(1): the path is hashed
    // and looked up in the same map findByHash uses.
    //
    // THIS EXISTS BECAUSE EVERY CALLER WAS WRITING A LINEAR SCAN. Six of them
    // did — the rig binder twice per model load, the animation binder, the
    // Customize composer — each walking all 300k entries comparing QStrings
    // to find one file, on a path the user hits by clicking a row. The map is
    // already built and already knows the answer, and it knows a better one:
    // a scan returns the FIRST entry with that path, which for a modded
    // install can be the shadowed copy the game would not load, while this
    // returns the copy that wins.
    int fileIndexForPath(const QString& assetPath) const;

    // Bumped when a new file list is INSTALLED, not when a rebuild is
    // requested. Anything that caches something derived from the index — a
    // decoded texture, a parsed table — can hold the value it last saw and
    // throw its cache away when this no longer matches, without needing a
    // signal connection or a back-dependency on this class.
    //
    // The distinction matters and cost a real bug: a rebuild REQUEST bumps its
    // counter and emits readyChanged(false) immediately, but m_files is not
    // replaced until install() runs, much later. A cache keyed on a file's
    // position that invalidated on the request counter would clear, then be
    // refilled during the gap with positions into the OLD list, then sail past
    // its own check after the swap and serve one file's data for another's.
    quint64 installGeneration() const { return m_installGeneration.load(); }
    // All top-level entries sharing a 51-bit path hash (any extension).
    QList<const IndexedFile*> findByPathHash(quint64 pathHash51) const;

signals:
    void progress(const QString& message);
    void readyChanged(bool ready);

private:
    explicit ArchiveIndex(QObject* parent = nullptr);

    struct BuildResult;
    void install(std::shared_ptr<BuildResult> result, quint64 generation);
    // Hash-map insertion with mod-override resolution — see the definition.
    // A private static rather than a free helper only because BuildResult is
    // a private nested type.
    static void insertResolved(BuildResult* r, quint64 hash, int idx);

    QVector<IndexedArchive> m_archives;
    QVector<IndexedFile> m_files;
    QHash<quint64, int> m_byHash;            // full hash → index into m_files (top-level)
    QMultiHash<quint64, int> m_byPathHash;   // 51-bit path hash → indices (top-level)
    int m_namedCount = 0;
    int m_dictEntries = 0;
    bool m_deepScanned = false;

    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_building{false};
    std::atomic<quint64> m_generation{0};
    std::atomic<quint64> m_installGeneration{0};
};

}  // namespace fox
