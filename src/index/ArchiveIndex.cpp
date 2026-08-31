// ArchiveIndex.cpp — see ArchiveIndex.h.
#include "index/ArchiveIndex.h"
#include "index/TextureUsers.h"

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>

#include "app/AppPaths.h"
#include "app/Config.h"
#include "fox/FoxHash.h"
#include "fox/FpkFile.h"
#include "fox/PftxsFile.h"
#include "fox/SbpFile.h"

namespace fox {
namespace {

constexpr quint32 kCacheMagic = 0x58444946;   // "FIDX"
// Bump whenever the SET or ORDER of indexed entries can change, not just when
// the on-disk layout does — a stale cache silently reinstates the old index
// after a rebuild, which reads as "my fix did nothing".
//   v2: sbp/stp wem children
//   v3: empty (all-zero) entry headers skipped; archives sorted by mount
//       priority; per-file `shadowed` flag
//   v4: the .lng2 extensions joined kKnownExtensions, so
//       hashFileNameWithExtension now returns a real type id where it used to
//       return 0 — every cached FPK/SBP child hash for a language table is a
//       stale key. Nothing reads those keys today, but the cache is the only
//       thing that decides whether a container child is re-hashed, so the
//       version has to move with the extension table.
//   v5: "pcsp" joined kKnownExtensions (precomputed sky) — same reason as v4,
//       the extension table and this version move together.
constexpr quint32 kCacheVersion = 5;

QString cachePath()
{
    return AppPaths::cacheFile(QStringLiteral("fox_index_v%1.bin").arg(kCacheVersion));
}

// ── Archive entry-table cache ────────────────────────────────────────────────
// Parsing the per-entry headers is the whole cost of starting up: the section
// table scatters 32-byte headers across a multi-gigabyte archive, and a
// 17-archive install reads 193k of them for ~6 s of wall clock — every launch,
// for an answer that cannot change until the archive file itself does.
//
// So the tables are persisted and reinstalled. This is separate from the
// container cache below and keyed PER ARCHIVE rather than over the whole set,
// so adding one .dat (or a mod dropping a 00.dat in) re-reads that file alone
// instead of the install.
constexpr quint32 kArchCacheMagic = 0x43524146;   // "FARC"
// Bump on ANY change to the record layout below or to what counts as a valid
// table — a stale cache silently reinstates the old entry offsets, which reads
// as an archive full of garbage rather than as a cache problem.
//   v1: initial
//   v2: explicit little-endian packed records (was a raw struct block, which
//       depended on compiler padding and could not detect a field REORDER at
//       constant sizeof); content-tagged validity
constexpr quint32 kArchCacheVersion = 2;

// Each entry is written as a fixed, explicitly little-endian record rather than
// a memcpy of the struct. Field-at-a-time through QDataStream would cost over a
// million reads on a full install; a raw struct block is fast but its layout is
// the compiler's, its tail padding writes uninitialised stack bytes to a file
// the user carries around, and a field reorder that keeps sizeof the same slips
// past every check. Packing by hand keeps the single-memcpy speed and has none
// of that.
constexpr int kQarRec = 48;   // hash, sizes, offset, md5, encryption, key
constexpr int kGzsRec = 16;   // hash, offset16, size

QString archCachePath()
{
    return AppPaths::cacheFile(
        QStringLiteral("fox_archives_v%1.bin").arg(kArchCacheVersion));
}

}  // namespace

// ── Superseded caches are pruned at startup (template §1) ───────────────────
// The version is in the FILE NAME, deliberately: an old file can then never be
// opened at all, rather than being read by a build that would give it a
// different meaning. The cost of that discipline is that every bump leaves the
// previous file on disk for ever — a portable folder that "keeps working when
// you move it" should not also accumulate a dead 40 MB index per release.
//
// Numbered rather than globbed on "fox_index_v*": a glob would also delete a
// NEWER build's cache if both are run out of one folder, and the older build
// would then re-scan every launch while quietly wiping the newer one's work.
// Counting down from the current version deletes only what is genuinely
// behind us.
int pruneOldCaches()
{
    // Caches moved from data\ into data\cache\ (AppPaths). Do this FIRST, or
    // the prune below looks in the new folder, finds nothing, and leaves every
    // superseded cache sitting in the old one for ever.
    if (const int moved = AppPaths::migrateCaches(); moved > 0)
        qInfo("index: moved %d cache file(s) into data/cache/ — caches and "
              "settings no longer share a folder", moved);

    // BOTH caches. The first version of this pruned only fox_index_v*, and
    // fox_archives_v1.bin — the big one, the entry tables that are the whole
    // cost of starting up — was already superseded by v2 and would have sat in
    // every existing data\ folder for ever.
    struct Cache { const char* fmt; quint32 current; };
    const Cache kCaches[] = {
        {"fox_index_v%1.bin", kCacheVersion},
        {"fox_archives_v%1.bin", kArchCacheVersion},
        // The texture→model map (index/TextureUsers.h). Listed HERE rather than
        // pruned by its own owner because this is the one place that knows what
        // this application leaves in data\, and a cache whose pruning lives
        // beside its writer is a cache that stops being pruned the moment the
        // writer is only constructed on demand — which this one is.
        // The constant comes from TextureUsers.h, not from a copy here: a
        // hand-maintained duplicate stops being maintained the first time
        // someone bumps the other one, and the prune loop then silently becomes
        // a no-op — the exact failure this block exists to prevent.
        {"fox_texusers_v%1.bin", fox::kTexUsersCacheVersion},
    };
    int removed = 0;
    for (const Cache& c : kCaches) {
        for (quint32 v = 1; v < c.current; ++v) {
            const QString old = AppPaths::cacheFile(
                QString::fromLatin1(c.fmt).arg(v));
            if (QFile::exists(old) && QFile::remove(old)) {
                qInfo("index: pruned superseded cache %s", qUtf8Printable(old));
                ++removed;
            }
        }
    }
    return removed;
}

namespace {

struct CachedArchiveTable {
    qint64 size = 0;
    qint64 mtime = 0;      // milliseconds — a second is coarse enough to miss
    quint64 tag = 0;       // content tag over the head and tail of the file
    quint8 kind = 0;       // 0 = SQAR, 1 = GZS
    quint32 version = 0;   // QAR container version
    quint32 flags = 0;
    qint32 unreadable = 0;
    QVector<QarEntry> qarEntries;
    QVector<GzsEntry> gzsEntries;
};

// Identity of an archive file as it is on disk right now. Size and timestamp
// alone are not enough: restoring a backup with timestamps preserved (cp -p,
// rsync --times, most mod managers' "keep file timestamps"), or patching entry
// payloads in place without repacking, changes the contents while leaving both
// untouched — and the cache would then hand out offsets into a file that no
// longer matches. Hashing 64 KB from each end covers the QAR header, the
// section table and the tail for a couple of milliseconds per archive, against
// the seconds the cache saves.
quint64 archiveTagFor(const QString& path, qint64 size)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    quint64 h = 1469598103934665603ULL;      // FNV-1a, 64-bit
    const auto mix = [&h](const QByteArray& b) {
        for (const char c : b) {
            h ^= static_cast<quint8>(c);
            h *= 1099511628211ULL;
        }
    };
    constexpr qint64 kWindow = 64 * 1024;
    mix(f.read(kWindow));
    if (size > kWindow * 2 && f.seek(size - kWindow)) mix(f.read(kWindow));
    h ^= static_cast<quint64>(size);
    return h;
}

QHash<QString, CachedArchiveTable> loadArchiveCache()
{
    QHash<QString, CachedArchiveTable> out;
    QFile f(archCachePath());
    if (!f.open(QIODevice::ReadOnly)) return out;
    QDataStream in(&f);
    // Pinned: the default stream version travels with the Qt release, so a
    // toolchain upgrade could otherwise desync the QString reads while every
    // header check still passed.
    in.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0, version = 0;
    in >> magic >> version;
    if (magic != kArchCacheMagic || version != kArchCacheVersion) return out;
    qint32 count = 0;
    in >> count;
    if (count < 0 || count > 100000) return out;
    for (qint32 i = 0; i < count && in.status() == QDataStream::Ok; ++i) {
        QString path;
        CachedArchiveTable t;
        qint32 n = 0;
        in >> path >> t.size >> t.mtime >> t.tag >> t.kind >> t.version
           >> t.flags >> t.unreadable >> n;
        // Bounded to the same ceiling the parsers themselves enforce. Without
        // an upper bound a corrupt length turns a cache miss into a multi-
        // gigabyte allocation and a crash.
        if (n < 0 || n > 4000000 || in.status() != QDataStream::Ok) return {};
        const int rec = t.kind == 0 ? kQarRec : kGzsRec;
        QByteArray block(n ? in.device()->read(qint64(n) * rec) : QByteArray());
        if (block.size() != qint64(n) * rec) return {};
        const uchar* p = reinterpret_cast<const uchar*>(block.constData());
        if (t.kind == 0) {
            t.qarEntries.resize(n);
            for (qint32 e = 0; e < n; ++e, p += kQarRec) {
                QarEntry& q = t.qarEntries[e];
                q.hash = qFromLittleEndian<quint64>(p);
                q.uncompressedSize = qFromLittleEndian<quint32>(p + 8);
                q.compressedSize = qFromLittleEndian<quint32>(p + 12);
                q.dataOffset = qint64(qFromLittleEndian<quint64>(p + 16));
                std::memcpy(q.md5, p + 24, 16);
                q.encryption = qFromLittleEndian<quint32>(p + 40);
                q.key = qFromLittleEndian<quint32>(p + 44);
                // Derived exactly as the parser derives it, so it cannot drift.
                q.compressed = q.uncompressedSize != q.compressedSize;
            }
        } else {
            t.gzsEntries.resize(n);
            for (qint32 e = 0; e < n; ++e, p += kGzsRec) {
                GzsEntry& g = t.gzsEntries[e];
                g.hash = qFromLittleEndian<quint64>(p);
                g.offset16 = qFromLittleEndian<quint32>(p + 8);
                g.size = qFromLittleEndian<quint32>(p + 12);
            }
        }
        out.insert(path, std::move(t));
    }
    return out;
}

void saveArchiveCache(const QHash<QString, CachedArchiveTable>& tables)
{
    QSaveFile f(archCachePath());
    if (!f.open(QIODevice::WriteOnly)) return;
    QDataStream os(&f);
    os.setVersion(QDataStream::Qt_6_0);
    os << kArchCacheMagic << kArchCacheVersion;
    os << qint32(tables.size());
    QByteArray block;
    for (auto it = tables.constBegin(); it != tables.constEnd(); ++it) {
        const CachedArchiveTable& t = it.value();
        const qint32 n = t.kind == 0 ? qint32(t.qarEntries.size())
                                     : qint32(t.gzsEntries.size());
        os << it.key() << t.size << t.mtime << t.tag << t.kind << t.version
           << t.flags << t.unreadable << n;
        if (n == 0) continue;
        const int rec = t.kind == 0 ? kQarRec : kGzsRec;
        block.fill('\0', qsizetype(n) * rec);
        uchar* p = reinterpret_cast<uchar*>(block.data());
        if (t.kind == 0) {
            for (qint32 e = 0; e < n; ++e, p += kQarRec) {
                const QarEntry& q = t.qarEntries[e];
                qToLittleEndian<quint64>(q.hash, p);
                qToLittleEndian<quint32>(q.uncompressedSize, p + 8);
                qToLittleEndian<quint32>(q.compressedSize, p + 12);
                qToLittleEndian<quint64>(quint64(q.dataOffset), p + 16);
                std::memcpy(p + 24, q.md5, 16);
                qToLittleEndian<quint32>(q.encryption, p + 40);
                qToLittleEndian<quint32>(q.key, p + 44);
            }
        } else {
            for (qint32 e = 0; e < n; ++e, p += kGzsRec) {
                const GzsEntry& g = t.gzsEntries[e];
                qToLittleEndian<quint64>(g.hash, p);
                qToLittleEndian<quint32>(g.offset16, p + 8);
                qToLittleEndian<quint32>(g.size, p + 12);
            }
        }
        os.writeRawData(block.constData(), int(block.size()));
    }
    // A failed commit means the next launch pays full price, which is worth a
    // line in the log rather than a silent mystery.
    if (!f.commit())
        qWarning("index: could not write the archive cache (%s) — the next "
                 "launch will re-read every archive",
                 qUtf8Printable(f.errorString()));
}

// One cached container listing: which top-level entry it was, and its children.
struct CachedChild {
    QString path;
    quint64 hash = 0;
    quint32 size = 0;
    bool named = false;
    bool gz = false;   // hash uses the GZ scheme (PFTXS child of a .g0s entry)
};
struct CachedContainer {
    int archiveId = -1;
    int entryIdx = -1;
    QVector<CachedChild> children;
};

QString fingerprintFor(const QVector<IndexedArchive>& archives)
{
    QString fp;
    for (const IndexedArchive& a : archives) {
        const QFileInfo fi(a.filePath);
        fp += QStringLiteral("%1|%2|%3;")
                  .arg(a.filePath)
                  .arg(fi.size())
                  .arg(fi.lastModified().toSecsSinceEpoch());
    }
    return fp;
}

}  // namespace

struct ArchiveIndex::BuildResult {
    QVector<IndexedArchive> archives;
    QVector<IndexedFile> files;
    int shadowedCount = 0;
    int unreadableCount = 0;
    QString shadowWinner, shadowLoser;   // first pair seen, for the log line
    QHash<quint64, int> byHash;
    QMultiHash<quint64, int> byPathHash;
    int namedCount = 0;
    int dictEntries = 0;
    bool deepScanned = false;
};

ArchiveIndex& ArchiveIndex::instance()
{
    static ArchiveIndex idx;
    return idx;
}

ArchiveIndex::ArchiveIndex(QObject* parent) : QObject(parent) {}

QString ArchiveIndex::extensionOf(const IndexedFile& f)
{
    if (f.gz) {
        QString ext = GzsFile::extensionForId(static_cast<int>((f.hash >> 52) & 0xFFFF));
        if (ext.startsWith(QLatin1Char('.'))) ext.remove(0, 1);
        return ext == QLatin1String("?") ? QString() : ext;
    }
    return HashResolver::instance().extensionFor(f.hash >> 51);
}

ContainerKind ArchiveIndex::containerKindFor(const IndexedFile& f)
{
    const QString ext = extensionOf(f);
    if (ext == QLatin1String("fpk")) return ContainerKind::Fpk;
    if (ext == QLatin1String("fpkd")) return ContainerKind::Fpkd;
    if (ext == QLatin1String("pftxs")) return ContainerKind::Pftxs;
    if (ext == QLatin1String("sbp")) return ContainerKind::Sbp;
    return ContainerKind::None;
}

namespace {

// Mount priority for one archive file. Fox loads the numbered slots under
// "master\<N>\" (00.dat / 01.dat — both names, and the "%s%s\1\" template,
// appear verbatim in mgsvtpp.exe) alongside master's chunk/texture archives,
// and mod managers install replacement assets into those numbered slots. When
// the same asset exists in both, the numbered slot is the copy the game uses,
// so it must be the copy the browser shows.
int mountPriority(const QString& path)
{
    const QString p = QDir::fromNativeSeparators(path);
    // …/master/<digits>/xx.dat  → a numbered mount slot.
    static const QRegularExpression slot(
        QStringLiteral("/master/(\\d+)/[^/]+$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = slot.match(p);
    if (m.hasMatch()) return 1000 + m.captured(1).toInt();
    return 0;
}

}  // namespace

// Insert one just-appended file into the hash lookup, resolving the case where
// another archive already carries the same asset.
//
// A mod install puts a replacement asset in a higher-priority mount while the
// stock copy stays in its chunk, so the SAME hash legitimately appears twice.
// The browser must show the copy the game loads, and must say so about the
// other one. Every insertion site goes through this — top-level SQAR entries,
// GZ entries, and (crucially) container children, which is where nearly all
// real models and textures live: resolving only the top level would leave the
// children picking a copy by accident.
void ArchiveIndex::insertResolved(BuildResult* r, quint64 hash, int idx)
{
    if (hash == 0) return;
    const auto prev = r->byHash.constFind(hash);
    if (prev == r->byHash.constEnd()) { r->byHash.insert(hash, idx); return; }

    const int prevIdx = prev.value();
    const int prevArchId = r->files[prevIdx].archiveId;
    const int archId = r->files[idx].archiveId;
    if (prevArchId < 0 || archId < 0) return;
    // An asset cannot override itself; two copies inside ONE archive are not a
    // mod override. Leave byHash pointing at the first and say nothing.
    if (prevArchId == archId) return;

    const IndexedArchive& prevArch = r->archives[prevArchId];
    const IndexedArchive& arch = r->archives[archId];
    // Only a DIFFERENT mount priority is a real override. Two archives at the
    // same priority (e.g. an asset that lives in both chunk0 and texture0 —
    // 98 of them in a stock TPP install) give us no evidence about which copy
    // the engine prefers, so keep the first and claim nothing: labelling those
    // "overridden" would flag a clean install as modded.
    if (arch.priority == prevArch.priority) return;
    const bool newWins = arch.priority > prevArch.priority;
    r->files[newWins ? prevIdx : idx].shadowed = true;
    if (newWins) r->byHash.insert(hash, idx);
    if (r->shadowWinner.isEmpty()) {
        r->shadowWinner = newWins ? arch.shortName : prevArch.shortName;
        r->shadowLoser = newWins ? prevArch.shortName : arch.shortName;
    }
    ++r->shadowedCount;
}

void ArchiveIndex::rebuild(const QStringList& gameDirs, const QString& dictDir,
                           bool deepScan)
{
    const quint64 generation = ++m_generation;
    m_building.store(true);
    m_ready.store(false);
    emit readyChanged(false);

    std::thread([this, gameDirs, dictDir, deepScan, generation] {
        auto result = std::make_shared<BuildResult>();
        QElapsedTimer timer;
        timer.start();

        // ── Dictionaries ─────────────────────────────────────────────────────
        HashResolver& resolver = HashResolver::instance();
        if (resolver.dictionaryFileCount() == 0 && !dictDir.isEmpty()) {
            QElapsedTimer dictTimer;
            dictTimer.start();
            int lines = 0, files = 0;
            QDirIterator it(dictDir, {QStringLiteral("*.txt")}, QDir::Files);
            while (it.hasNext()) {
                const QString f = it.next();
                lines += resolver.loadDictionary(f);
                ++files;
            }
            const qint64 readMs = dictTimer.elapsed();
            // ONE build for all of them, rather than three table inserts per
            // line per file. The two halves are reported separately because
            // they behave differently: reading is I/O and scales with bytes,
            // the build is cache misses and scales with lines.
            resolver.finishDictionaries();
            qInfo("index: dictionaries — %d file(s), %d line(s) read in %lld ms,"
                  " %d name(s) indexed in %lld ms",
                  files, lines, static_cast<long long>(readMs), resolver.size(),
                  static_cast<long long>(dictTimer.elapsed() - readMs));
        }
        result->dictEntries = resolver.size();

        // ── Find archives (magic sniff, not extension match) ─────────────────
        // Entry tables come from the cache whenever the archive file's size and
        // timestamp still match what was recorded, which is the difference
        // between a ~6 s startup and a near-instant one. `fresh` counts the
        // ones that had to be read, so an unchanged install never rewrites the
        // cache file.
        // Three phases, timed apart. They were one number — "archives" — and
        // that number was 11489 ms on the user's install with EVERY entry
        // table served from cache and nothing read from disk, which no single
        // total can explain. The cache being perfect and the phase still
        // costing eleven seconds is the whole reason these are split.
        qint64 msCacheLoad = 0, msTableRead = 0;
        QElapsedTimer phaseTimer;
        phaseTimer.start();
        QHash<QString, CachedArchiveTable> archCache = loadArchiveCache();
        msCacheLoad = phaseTimer.elapsed();
        const qint64 msWalkStart = timer.elapsed();
        QHash<QString, CachedArchiveTable> archNext;
        int cachedTables = 0, freshTables = 0;

        for (const QString& dir : gameDirs) {
            QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString path = it.next();
                if (generation != m_generation.load()) return;   // superseded
                QFile f(path);
                if (f.size() < 32 || !f.open(QIODevice::ReadOnly)) continue;
                char magic[4];
                if (f.read(magic, 4) != 4) continue;
                const bool sqar = qFromLittleEndian<quint32>(magic) == 0x52415153u;
                f.close();

                // Identity of the file as it is RIGHT NOW. A cached table is
                // only reinstalled when both still match — a game patch or a
                // mod install changes at least one of them.
                const QFileInfo fi(path);
                const qint64 nowSize = fi.size();
                const qint64 nowMtime = fi.lastModified().toMSecsSinceEpoch();
                const auto cached = archCache.constFind(path);
                // Size and timestamp are checked first because they are free;
                // the content tag costs two 64 KB reads and is only worth
                // taking when the cheap tests have already agreed.
                const bool maybeCached = cached != archCache.constEnd()
                    && cached->size == nowSize && cached->mtime == nowMtime
                    && cached->kind == (sqar ? 0 : 1);
                const quint64 nowTag =
                    maybeCached ? archiveTagFor(path, nowSize) : 0;
                const bool haveCached = maybeCached && cached->tag == nowTag;

                IndexedArchive a;
                a.filePath = path;
                a.shortName = QFileInfo(path).fileName();
                a.priority = mountPriority(path);
                if (sqar) {
                    a.qar = std::make_shared<QarFile>();
                    if (haveCached) {
                        QVector<QarEntry> e = cached->qarEntries;
                        a.qar->adopt(path, cached->version, cached->flags,
                                     cached->unreadable, std::move(e));
                        archNext.insert(path, *cached);
                        ++cachedTables;
                    } else {
                        emit progress(
                            QStringLiteral("Reading %1…").arg(a.shortName));
                        phaseTimer.restart();
                        const bool openedQAR = a.qar->open(path);
                        msTableRead += phaseTimer.elapsed();
                        if (!openedQAR) {
                            qWarning("index: %s: %s", qUtf8Printable(path),
                                     qUtf8Printable(a.qar->errorString()));
                            continue;
                        }
                        CachedArchiveTable t;
                        t.size = nowSize;
                        t.mtime = nowMtime;
                        t.tag = archiveTagFor(path, nowSize);
                        t.kind = 0;
                        t.version = a.qar->version();
                        t.flags = a.qar->flags();
                        t.unreadable = a.qar->unreadableEntries();
                        t.qarEntries = a.qar->entries();
                        archNext.insert(path, std::move(t));
                        ++freshTables;
                    }
                } else if (GzsFile::isGzs(path)) {
                    // Ground Zeroes .g0s: no leading magic — detected by footer.
                    a.kind = ArchiveKind::Gzs;
                    a.gzs = std::make_shared<GzsFile>();
                    if (haveCached) {
                        QVector<GzsEntry> e = cached->gzsEntries;
                        a.gzs->adopt(path, std::move(e));
                        archNext.insert(path, *cached);
                        ++cachedTables;
                    } else {
                        emit progress(
                            QStringLiteral("Reading %1…").arg(a.shortName));
                        phaseTimer.restart();
                        const bool openedGZS = a.gzs->open(path);
                        msTableRead += phaseTimer.elapsed();
                        if (!openedGZS) {
                            qWarning("index: %s: %s", qUtf8Printable(path),
                                     qUtf8Printable(a.gzs->errorString()));
                            continue;
                        }
                        CachedArchiveTable t;
                        t.size = nowSize;
                        t.mtime = nowMtime;
                        t.tag = archiveTagFor(path, nowSize);
                        t.kind = 1;
                        t.gzsEntries = a.gzs->entries();
                        archNext.insert(path, std::move(t));
                        ++freshTables;
                    }
                } else {
                    continue;
                }
                result->archives.append(a);
            }
        }

        // Rewrite only when something actually changed — including an archive
        // DISAPPEARING, which shows up as a smaller set than the cache held.
        if (freshTables > 0 || archNext.size() != archCache.size())
            saveArchiveCache(archNext);
        if (cachedTables > 0 || freshTables > 0)
            qInfo("index: entry tables — %d from cache, %d read from disk",
                  cachedTables, freshTables);

        // ── Loose mounts: the dev tree, and the MOD FOLDER ───────────────
        // A plain directory of extracted assets, mounted above the archives so
        // its copies win over whatever they hold. Two of them, one mechanic:
        //
        //   loose (1000)  --loose / Config::sessionLooseDir. Development. It
        //                 is what makes a partial install testable: pull the
        //                 files a feature needs out of a real install, drop
        //                 them here, and every path resolves them exactly as
        //                 if they had come out of a .dat.
        //   mod   (1100)  Config::modDir. The user's replacement assets. It
        //                 outranks the dev mount deliberately — a person's own
        //                 mod folder is not something a harness flag should be
        //                 able to shadow out from under them.
        //
        // ONE walk, called twice. They differ by a folder and a number, and two
        // copies of a directory walk is two places for the asset-path rule to
        // drift — which on this mount is the rule that decides whether a hash
        // resolves at all.
        const auto mountLoose = [&](const QString& dir, int priority,
                                    const char* tag) {
            if (dir.isEmpty() || !QDir(dir).exists()) return;
            {
                const QString looseDir = dir;
                IndexedArchive a;
                a.filePath = looseDir;
                a.shortName = QLatin1String(tag) + QDir(looseDir).dirName();
                a.priority = priority;
                a.kind = ArchiveKind::Loose;
                QDirIterator it(looseDir, QDir::Files, QDirIterator::Subdirectories);
                // Path AND size in one pass, paired, then sorted together —
                // QDirIterator's fileInfo() is free (it already stat'd to know
                // the entry is a file) and re-asking per file later doubled
                // the syscalls for the whole tree.
                QVector<QPair<QString, quint32>> found;
                while (it.hasNext()) {
                    const QString fp = it.next();
                    found.append({fp, quint32(it.fileInfo().size())});
                }
                std::sort(found.begin(), found.end(),
                          [](const QPair<QString, quint32>& x,
                             const QPair<QString, quint32>& y) {
                              return x.first < y.first;
                          });
                a.loosePaths.reserve(found.size());
                a.looseSizes.reserve(found.size());
                for (const auto& pr : found) {
                    a.loosePaths.append(pr.first);
                    a.looseSizes.append(pr.second);
                }
                if (!a.loosePaths.isEmpty()) result->archives.append(a);
            }
        };
        mountLoose(Config::sessionLooseDir(), 1000, "loose:");
        mountLoose(Config::modDir(), 1100, "mod:");

        // Deterministic mount order: highest priority last, so the simple
        // first-wins hash map below ends up holding the LOWEST-priority copy —
        // hence the explicit overwrite when a higher-priority archive repeats a
        // hash. Sorting also removes the old dependency on the order
        // QDirIterator happened to walk the directory in.
        std::sort(result->archives.begin(), result->archives.end(),
                  [](const IndexedArchive& x, const IndexedArchive& y) {
                      if (x.priority != y.priority) return x.priority < y.priority;
                      return x.filePath < y.filePath;
                  });

        const qint64 msArchives = timer.elapsed();
        // ── Top-level entries ────────────────────────────────────────────────
        // COUNTED FIRST, so the three containers below are grown once each.
        // A stock install is ~300k entries: without this, `files` reallocates
        // about twenty times — copying a vector of structs that each own a
        // QString — and the two hashes rehash on the way up through every
        // power of two. The count is free; the archives already know it.
        {
            qsizetype total = 0;
            for (const IndexedArchive& arch : result->archives) {
                switch (arch.kind) {
                case ArchiveKind::Loose: total += arch.loosePaths.size(); break;
                case ArchiveKind::Sqar:
                    if (arch.qar) total += arch.qar->entries().size();
                    break;
                case ArchiveKind::Gzs:
                    if (arch.gzs) total += arch.gzs->entries().size();
                    break;
                }
            }
            result->files.reserve(total);
            result->byHash.reserve(total);
            result->byPathHash.reserve(total);
        }
        for (int ai = 0; ai < result->archives.size(); ++ai) {
            const IndexedArchive& arch = result->archives[ai];
            if (arch.kind == ArchiveKind::Loose) {
                // The prefix to strip, once. QDir::relativeFilePath cleans and
                // re-splits both sides on every call, and constructing a QDir
                // per file to ask it was the single most expensive thing in
                // the loose walk — every one of these paths came out of an
                // iterator rooted here, so the relative part is a substring.
                QString root = QDir(arch.filePath).absolutePath();
                if (!root.endsWith(QLatin1Char('/'))) root += QLatin1Char('/');
                const qsizetype rootLen = root.size();
                for (int ei = 0; ei < arch.loosePaths.size(); ++ei) {
                    // The asset path is the file's position under the folder:
                    // <loose>/Assets/ssd/… is /Assets/ssd/…, which is exactly
                    // the layout the extractor writes.
                    const QString& abs = arch.loosePaths[ei];
                    QString rel = abs.startsWith(root)
                        ? abs.mid(rootLen)
                        : QDir(root).relativeFilePath(abs);
                    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
                    IndexedFile f;
                    f.archiveId = ai;
                    f.entryIdx = ei;
                    f.path = QLatin1Char('/') + rel;
                    f.named = true;
                    f.size = ei < arch.looseSizes.size() ? arch.looseSizes[ei]
                                                         : 0;
                    f.hash = hashFileNameWithExtension(f.path);
                    result->files.append(f);
                    insertResolved(result.get(), f.hash, result->files.size() - 1);
                }
                continue;
            }
            if (arch.kind == ArchiveKind::Sqar) {
                result->unreadableCount += arch.qar->unreadableEntries();
                const auto& entries = arch.qar->entries();
                for (int ei = 0; ei < entries.size(); ++ei) {
                    const QarEntry& e = entries[ei];
                    IndexedFile f;
                    f.archiveId = ai;
                    f.entryIdx = ei;
                    f.hash = e.hash;
                    f.size = e.uncompressedSize;
                    f.named = resolver.tryResolve(e.hash, &f.path);
                    if (f.named) ++result->namedCount;
                    const int idx = result->files.size();
                    result->files.append(f);
                    insertResolved(result.get(), e.hash, idx);
                    result->byPathHash.insert(e.hash & kPathMask, idx);
                }
            } else {
                const auto& entries = arch.gzs->entries();
                for (int ei = 0; ei < entries.size(); ++ei) {
                    const GzsEntry& e = entries[ei];
                    IndexedFile f;
                    f.archiveId = ai;
                    f.entryIdx = ei;
                    f.hash = e.hash;
                    f.size = e.size;
                    f.gz = true;
                    f.named = resolver.tryResolveGzs(e.hash, &f.path);
                    if (f.named) ++result->namedCount;
                    const int idx = result->files.size();
                    result->files.append(f);
                    insertResolved(result.get(), e.hash, idx);
                }
            }
        }
        // ── Which game is each archive? ──────────────────────────────────────
        // Majority vote over the paths that actually resolved. A .g0s is Ground
        // Zeroes outright, and an archive with nothing named keeps Unknown
        // rather than being guessed at from its name.
        {
            QVector<QVector<int>> votes(result->archives.size(),
                                        QVector<int>(kGameCount, 0));
            for (const IndexedFile& f : result->files) {
                if (!f.named || f.archiveId < 0) continue;
                const GameId g = gameForAssetPath(f.path);
                if (g != GameId::Unknown) ++votes[f.archiveId][int(g)];
            }
            for (int ai = 0; ai < result->archives.size(); ++ai) {
                IndexedArchive& a = result->archives[ai];
                if (a.kind == ArchiveKind::Gzs) { a.game = GameId::GroundZeroes; continue; }
                int best = 0, bestN = 0, total = 0;
                for (int g = 1; g < kGameCount; ++g) {
                    total += votes[ai][g];
                    if (votes[ai][g] > bestN) { bestN = votes[ai][g]; best = g; }
                }
                if (bestN > 0) a.game = GameId(best);
                if (total > 0)
                    qInfo("index: %s -> %s (%d of %d named entries)",
                          qUtf8Printable(a.shortName), gameShortName(a.game), bestN,
                          total);
            }
        }

        // PER PHASE, not one number. "the index took 6 seconds" is not a
        // fact anybody can act on; "the entry tables took 200 ms and naming
        // 300k of them took 4 s" is. The phases are cumulative subtractions of
        // one timer, so they always add up to the total.
        qInfo("index: %d archives, %lld top-level entries (%d named) in %lld ms"
              " — dictionaries %lld ms, archive cache %lld ms, discovery walk"
              " %lld ms, entry tables %lld ms, entries %lld ms",
              static_cast<int>(result->archives.size()),
              static_cast<long long>(result->files.size()), result->namedCount,
              static_cast<long long>(timer.elapsed()),
              static_cast<long long>(msWalkStart - msCacheLoad),
              static_cast<long long>(msCacheLoad),
              static_cast<long long>(msArchives - msWalkStart - msTableRead),
              static_cast<long long>(msTableRead),
              static_cast<long long>(timer.elapsed() - msArchives));
        const qint64 msBeforeDeep = timer.elapsed();

        // ── Deep scan (containers), disk-cached ──────────────────────────────
        // Counted across the whole scan and reported once at the end.
        int pftxsFailures = 0;
        QString pftxsFirstError;
        // A pack whose TEXL header declares more textures than the walk could
        // read is not a broken pack — it is a pack sitting on a HOLE. Tallied
        // so a partially-downloaded install says so once, instead of every
        // feature that needed those textures quietly finding nothing.
        int pftxsShortPacks = 0;
        qint64 pftxsDeclared = 0, pftxsRead = 0;
        bool cacheHit = false;
        if (deepScan) {
            const QString fingerprint = fingerprintFor(result->archives);
        QVector<CachedContainer> containers;

            QFile cf(cachePath());
            if (cf.open(QIODevice::ReadOnly)) {
                QDataStream in(&cf);
                quint32 magic = 0, version = 0;
                QString storedFp;
                in >> magic >> version >> storedFp;
                if (magic == kCacheMagic && version == kCacheVersion
                    && storedFp == fingerprint) {
                    qint32 count = 0;
                    in >> count;
                    containers.reserve(count);
                    for (qint32 i = 0; i < count && in.status() == QDataStream::Ok; ++i) {
                        CachedContainer c;
                        qint32 childCount = 0;
                        in >> c.archiveId >> c.entryIdx >> childCount;
                        c.children.reserve(childCount);
                        for (qint32 j = 0; j < childCount; ++j) {
                            CachedChild ch;
                            in >> ch.path >> ch.hash >> ch.size >> ch.named >> ch.gz;
                            c.children.append(ch);
                        }
                        containers.append(c);
                    }
                    cacheHit = in.status() == QDataStream::Ok;
                }
            }

            // Bounds-checked on every kind. This used to dereference arch.gzs
            // for anything that was not Sqar, which is NULL for a loose folder
            // — so an .fpk sitting in one took the indexer down with it. Loose
            // archives are now skipped by the caller (see the loop below) and
            // the null checks here keep the crash from coming back by another
            // route.
            const auto readTopLevel = [&result](const IndexedFile& f) -> QByteArray {
                if (f.archiveId < 0 || f.archiveId >= result->archives.size())
                    return {};
                const IndexedArchive& arch = result->archives[f.archiveId];
                if (arch.kind == ArchiveKind::Loose) return {};
                if (arch.kind == ArchiveKind::Sqar) {
                    if (!arch.qar || f.entryIdx < 0
                        || f.entryIdx >= arch.qar->entries().size())
                        return {};
                    return arch.qar->readEntry(arch.qar->entries()[f.entryIdx]);
                }
                if (!arch.gzs || f.entryIdx < 0
                    || f.entryIdx >= arch.gzs->entries().size())
                    return {};
                return arch.gzs->readEntry(arch.gzs->entries()[f.entryIdx]);
            };

            if (!cacheHit) {
                containers.clear();

                // ── The work list ───────────────────────────────────────────
                // One pass that both counts and collects, replacing the count
                // pass and the work pass. It is also the unit the chunking
                // divides, so the filters below are applied ONCE and every
                // worker gets a list it can simply walk.
                QVector<int> jobs;
                for (int fi = 0; fi < result->files.size(); ++fi) {
                    const IndexedFile& f = result->files[fi];
                    if (f.childIdx >= 0) continue;
                    // NOT a loose folder, and this is a correctness rule rather
                    // than an optimisation. readFile() has no childIdx step for
                    // a Loose entry: it returns the whole file. So enumerating
                    // an .fpk's children in a loose mount would index entries
                    // that all read back as the CONTAINER's bytes — and because
                    // a loose mount outranks every archive, those bogus children
                    // would win the hash lookup against the real assets. It also
                    // sidesteps a stale-cache trap: the container cache
                    // fingerprints an archive by its own size and mtime, and a
                    // FOLDER's mtime does not move when a file inside it is
                    // edited, nor when the sorted file list shifts every
                    // entryIdx along.
                    //
                    // Nothing is lost. A loose folder is extracted assets, so
                    // whatever is inside an .fpk there is already sitting beside
                    // it as a real file.
                    if (f.archiveId >= 0 && f.archiveId < result->archives.size()
                        && result->archives[f.archiveId].kind == ArchiveKind::Loose)
                        continue;
                    if (containerKindFor(f) == ContainerKind::None) continue;
                    jobs.append(fi);
                }
                const int total = int(jobs.size());

                // ── Chunked, merged in chunk order ──────────────────────────
                // Measured on the user's install before this existed: the deep
                // scan was 88643 ms of a 119125 ms cold index — 74% of it, and
                // the largest single cost in the tool. It parallelises because
                // every dependency is already concurrent-safe, which was
                // checked rather than assumed: QarFile::readEntry and
                // GzsFile::readEntry each open their OWN QFile per call and
                // share no handle; HashResolver guards its build with an
                // atomic flag and a mutex and is pure reads afterwards (and
                // finishDictionaries() has already run by here); FpkFile,
                // PftxsFile and SbpFile parse a local blob into a local object.
                //
                // Chunks are contiguous ranges of `jobs` and are merged in
                // CHUNK ORDER, so `containers` comes out in exactly the order
                // the sequential scan produced it — which is what keeps the
                // cache file byte-identical however many threads wrote it.
                struct Chunk {
                    int first = 0, last = 0;   // [first, last)
                    QVector<CachedContainer> out;
                    int pftxsFailures = 0;
                    QString pftxsFirstError;
                    int pftxsShortPacks = 0;
                    qint64 pftxsDeclared = 0, pftxsRead = 0;
                };
                const int hw = int(std::thread::hardware_concurrency());
                // Below a few hundred containers the split costs more than it
                // saves, and it keeps the "loose folder with a dozen models"
                // case on one thread where it belongs.
                int workers = total < 256 ? 1 : qBound(1, hw > 0 ? hw : 4, 16);
                // Env-gated override, permanent (§13), and the ACCEPTANCE TEST
                // for this change: FOXAB_DEEPSCAN_THREADS=1 and =8 must write a
                // byte-identical fox_index_v5.bin. It is also the way back to
                // one thread on a machine where the parallel scan misbehaves,
                // without a rebuild.
                if (const QByteArray e = qgetenv("FOXAB_DEEPSCAN_THREADS");
                    !e.isEmpty()) {
                    bool ok = false;
                    const int want = e.toInt(&ok);
                    if (ok && want > 0) workers = qBound(1, want, 64);
                }
                QVector<Chunk> chunks(workers);
                {
                    const int per = (total + workers - 1) / workers;
                    for (int w = 0; w < workers; ++w) {
                        chunks[w].first = qMin(total, w * per);
                        chunks[w].last = qMin(total, (w + 1) * per);
                    }
                }

                std::atomic<int> doneCount{0};
                std::atomic<bool> superseded{false};
                const auto runChunk = [&](Chunk& ck) {
                    QVector<CachedContainer>& out = ck.out;
                    for (int n = ck.first; n < ck.last; ++n) {
                        // A rescan supersedes this build: stop rather than
                        // spend a minute and a half indexing for an index that
                        // no longer exists. Checked in every worker, and once
                        // one gives up they all do.
                        if (generation != m_generation.load()
                            || superseded.load()) {
                            superseded.store(true);
                            return;
                        }
                        const IndexedFile& f = result->files[jobs[n]];
                        const ContainerKind kind = containerKindFor(f);

                        // Bounded signalling: one every 64 containers, from
                        // whichever worker crosses the boundary. The text is
                        // the same either way — it is a count, not an order.
                        const int d = doneCount.fetch_add(1) + 1;
                        if ((d & 63) == 0)
                            emit progress(
                                QStringLiteral("Indexing containers… %1/%2")
                                    .arg(d)
                                    .arg(total));

                        const QByteArray blob = readTopLevel(f);
                        if (blob.isEmpty()) continue;

                        CachedContainer c;
                        c.archiveId = f.archiveId;
                        c.entryIdx = f.entryIdx;
                        if (kind == ContainerKind::Sbp) {
                            // Sound bundle: expose the STPL-embedded wems as
                            // children named "<bundle path>/<wemId>.wem".
                            const auto wems = SbpFile::listWems(blob);
                            // Unnamed bundles need a distinct prefix, or every
                            // unnamed bundle's wems would collide on "/<id>.wem".
                            const QString base = f.path.isEmpty()
                                ? QStringLiteral("unresolved/sbp_%1")
                                      .arg(f.hash, 0, 16)
                                : f.path;
                            for (const SbpWem& w : wems) {
                                CachedChild ch;
                                ch.path = QStringLiteral("%1/%2.wem")
                                              .arg(base)
                                              .arg(w.id);
                                ch.hash = hashFileNameWithExtension(ch.path);
                                ch.size = w.size;
                                ch.named = true;
                                c.children.append(ch);
                            }
                        } else if (kind == ContainerKind::Pftxs) {
                            PftxsFile p;
                            if (!p.parse(blob)) {
                                // A container that will not parse is data the
                                // browser silently does not have. Counted and
                                // reported once at the end rather than a line per
                                // pack: Ground Zeroes carries 62 files with this
                                // extension that are not packs at all, and
                                // sixty-two warnings say no more than one does.
                                ++ck.pftxsFailures;
                                if (ck.pftxsFirstError.isEmpty())
                                    ck.pftxsFirstError =
                                        QStringLiteral("%1: %2")
                                            .arg(f.path.section(QLatin1Char('/'), -1),
                                                 p.errorString());
                                continue;
                            }
                            if (p.readTextures() < p.declaredTextures()) {
                                ++ck.pftxsShortPacks;
                                ck.pftxsDeclared += p.declaredTextures();
                                ck.pftxsRead += p.readTextures();
                            }
                            for (const PftxsGroup& g : p.groups()) {
                                for (const PftxsSubEntry& s : g.entries) {
                                    CachedChild ch;
                                    ch.hash = s.hash;
                                    ch.size = s.size;
                                    ch.gz = f.gz;   // GZ packs carry GZ-scheme hashes
                                    ch.named = f.gz
                                        ? HashResolver::instance().tryResolveGzs(s.hash, &ch.path)
                                        : HashResolver::instance().tryResolve(s.hash, &ch.path);
                                    c.children.append(ch);
                                }
                            }
                        } else {
                            FpkFile p;
                            if (!p.parse(blob)) continue;
                            for (const FpkEntry& fe : p.entries()) {
                                CachedChild ch;
                                ch.path = FpkFile::normalizedPath(fe.filePath);
                                ch.hash = hashFileNameWithExtension(ch.path);
                                ch.size = static_cast<quint32>(fe.dataSize);
                                ch.named = true;   // FPKs store real path strings
                                c.children.append(ch);
                            }
                        }
                        out.append(c);
                    }
                };
                if (workers == 1) {
                    runChunk(chunks[0]);
                } else {
                    // std::vector, not QVector: std::thread is move-only and
                    // Qt's container wants to copy on reallocation.
                    std::vector<std::thread> pool;
                    pool.reserve(size_t(workers - 1));
                    for (int w = 1; w < workers; ++w)
                        pool.emplace_back(
                            [&runChunk, &chunks, w] { runChunk(chunks[w]); });
                    runChunk(chunks[0]);
                    for (std::thread& t : pool) t.join();
                }
                if (superseded.load()) return;

                // Merge IN CHUNK ORDER. This is the line the determinism rests
                // on: chunk 0's containers, then chunk 1's, and within a chunk
                // the order the sequential scan would have produced.
                for (Chunk& ck : chunks) {
                    containers.append(ck.out);
                    pftxsFailures += ck.pftxsFailures;
                    if (pftxsFirstError.isEmpty())
                        pftxsFirstError = ck.pftxsFirstError;
                    pftxsShortPacks += ck.pftxsShortPacks;
                    pftxsDeclared += ck.pftxsDeclared;
                    pftxsRead += ck.pftxsRead;
                }

                // Persist for next launch (atomic; a torn cache must never load).
                QSaveFile out(cachePath());
                if (out.open(QIODevice::WriteOnly)) {
                    QDataStream os(&out);
                    os << kCacheMagic << kCacheVersion << fingerprint;
                    os << static_cast<qint32>(containers.size());
                    for (const CachedContainer& c : containers) {
                        os << c.archiveId << c.entryIdx
                           << static_cast<qint32>(c.children.size());
                        for (const CachedChild& ch : c.children)
                            os << ch.path << ch.hash << ch.size << ch.named << ch.gz;
                    }
                    out.commit();
                }
            }

            // Splice container children into the flat file list. They also go
            // into the hash lookups (top-level entries win on collision) so
            // pack-internal textures are findable by PathCode — models whose
            // .ftex lives only inside a PFTXS resolve through the same path
            // as archive-level textures.
            for (const CachedContainer& c : containers) {
                if (c.archiveId < 0 || c.archiveId >= result->archives.size()) continue;
                for (int j = 0; j < c.children.size(); ++j) {
                    const CachedChild& ch = c.children[j];
                    IndexedFile f;
                    f.archiveId = c.archiveId;
                    f.entryIdx = c.entryIdx;
                    f.childIdx = j;
                    f.hash = ch.hash;
                    f.size = ch.size;
                    f.named = ch.named;
                    f.gz = ch.gz;
                    f.path = ch.path;
                    if (f.named) ++result->namedCount;
                    const int idx = result->files.size();
                    result->files.append(f);
                    insertResolved(result.get(), ch.hash, idx);
                    if (!ch.gz)
                        result->byPathHash.insert(ch.hash & kPathMask, idx);
                }
            }
            result->deepScanned = true;
        }

        // Summary AFTER the deep scan: container children go through the same
        // override resolution, and nearly every real model/texture is a child,
        // so the count is only complete once they are spliced in.
        if (result->unreadableCount > 0)
            qWarning("index: %d entry header(s) were empty and skipped — an "
                     "archive is incomplete (truncated download or partial "
                     "copy), so some assets are simply not present",
                     result->unreadableCount);
        if (result->shadowedCount > 0)
            qInfo("index: %d asset(s) exist in more than one archive — using the "
                  "higher-priority mount (e.g. %s over %s), which is the copy "
                  "the game loads; mod installs do this",
                  result->shadowedCount, qUtf8Printable(result->shadowWinner),
                  qUtf8Printable(result->shadowLoser));
        if (deepScan) {
            if (pftxsFailures > 0)
                qWarning("index: %d texture pack(s) could not be read (first: "
                         "%s) — their textures are not listed",
                         pftxsFailures, qUtf8Printable(pftxsFirstError));
            // The loudest thing this program can tell you about a partial
            // install. The archives themselves cannot: a hole decrypts to
            // plausible-looking bytes rather than to zeros, so nothing upstream
            // notices. The packs CAN, because each one states its own count.
            if (pftxsShortPacks > 0)
                qWarning("index: %d texture pack(s) hold fewer textures than "
                         "they declare (%lld of %lld) — this install is "
                         "INCOMPLETE, and the art in the missing part of those "
                         "packs (faces, eyebrows, beards, hair colours) will "
                         "simply not appear. Re-download or verify the game "
                         "files to fix it.",
                         pftxsShortPacks, static_cast<long long>(pftxsRead),
                         static_cast<long long>(pftxsDeclared));
            qInfo("index: deep scan %s — %lld files total, %lld ms for the "
                  "scan, %lld ms all in",
                  cacheHit ? "(cached)" : "(built)",
                  static_cast<long long>(result->files.size()),
                  static_cast<long long>(timer.elapsed() - msBeforeDeep),
                  static_cast<long long>(timer.elapsed()));
        }

        QMetaObject::invokeMethod(
            this, [this, result, generation] { install(result, generation); },
            Qt::QueuedConnection);
    }).detach();
}

void ArchiveIndex::install(std::shared_ptr<BuildResult> result, quint64 generation)
{
    if (generation != m_generation.load()) return;   // a newer rebuild superseded us
    m_archives = std::move(result->archives);
    m_files = std::move(result->files);
    m_byHash = std::move(result->byHash);
    m_byPathHash = std::move(result->byPathHash);
    m_namedCount = result->namedCount;
    m_dictEntries = result->dictEntries;
    m_deepScanned = result->deepScanned;
    // AFTER the swap: this is what index-derived caches key their validity on,
    // so it must not change until the list they would be keyed against has.
    ++m_installGeneration;
    m_building.store(false);
    m_ready.store(true);
    emit readyChanged(true);
}

QByteArray ArchiveIndex::readFile(const IndexedFile& f) const
{
    if (f.archiveId < 0 || f.archiveId >= m_archives.size()) return {};
    const IndexedArchive& arch = m_archives[f.archiveId];
    QByteArray blob;
    if (arch.kind == ArchiveKind::Loose) {
        if (f.entryIdx < 0 || f.entryIdx >= arch.loosePaths.size()) return {};
        QFile lf(arch.loosePaths[f.entryIdx]);
        if (!lf.open(QIODevice::ReadOnly)) return {};
        return lf.readAll();
    }
    if (arch.kind == ArchiveKind::Sqar) {
        if (f.entryIdx < 0 || f.entryIdx >= arch.qar->entries().size()) return {};
        blob = arch.qar->readEntry(arch.qar->entries()[f.entryIdx]);
    } else {
        if (f.entryIdx < 0 || f.entryIdx >= arch.gzs->entries().size()) return {};
        blob = arch.gzs->readEntry(arch.gzs->entries()[f.entryIdx]);
    }
    if (f.childIdx < 0 || blob.isEmpty()) return blob;

    // Child inside a container: parse and pull the one file.
    if (SbpFile::isSbp(blob)) {
        const auto wems = SbpFile::listWems(blob);   // same order as the scan
        if (f.childIdx >= wems.size()) return {};
        return SbpFile::readWem(blob, wems[f.childIdx]);
    }
    if (PftxsFile::isPftxs(blob)) {
        PftxsFile p;
        if (!p.parse(blob)) return {};
        int flat = 0;
        for (const PftxsGroup& g : p.groups())
            for (const PftxsSubEntry& s : g.entries)
                if (flat++ == f.childIdx) return PftxsFile::readEntry(blob, s);
        return {};
    }
    FpkFile p;
    if (!p.parse(blob)) return {};
    if (f.childIdx >= p.entries().size()) return {};
    return FpkFile::readEntry(blob, p.entries()[f.childIdx]);
}

const IndexedFile* ArchiveIndex::findByHash(quint64 hash) const
{
    const auto it = m_byHash.constFind(hash);
    return it == m_byHash.constEnd() ? nullptr : &m_files[it.value()];
}

int ArchiveIndex::fileIndexForPath(const QString& assetPath) const
{
    if (assetPath.isEmpty()) return -1;
    return m_byHash.value(hashFileNameWithExtension(assetPath), -1);
}

QList<const IndexedFile*> ArchiveIndex::findByPathHash(quint64 pathHash51) const
{
    QList<const IndexedFile*> out;
    for (auto it = m_byPathHash.constFind(pathHash51);
         it != m_byPathHash.constEnd() && it.key() == pathHash51; ++it)
        out.append(&m_files[it.value()]);
    return out;
}

GameId ArchiveIndex::gameOf(const IndexedFile& f) const
{
    // Ground Zeroes assets are all under /Assets/tpp, so the archive has the
    // only true answer for them; ask the path first everywhere else.
    if (f.gz) return GameId::GroundZeroes;
    if (f.named) {
        const GameId g = gameForAssetPath(f.path);
        if (g != GameId::Unknown) return g;
    }
    if (f.archiveId >= 0 && f.archiveId < m_archives.size())
        return m_archives[f.archiveId].game;
    return GameId::Unknown;
}

}  // namespace fox
