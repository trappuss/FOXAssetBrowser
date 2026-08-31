#include "util/Extract.h"

#include <QHash>
#include <QMutex>
#include <limits>
#include <QStringList>

#include "fox/FoxHash.h"
#include "fox/GzsFile.h"
#include "fox/FmdlFile.h"
#include "fox/BcDecode.h"
#include "fox/FtexFile.h"
#include "fox/PftxsFile.h"
#include "index/ArchiveIndex.h"

namespace extract {

using fox::ArchiveIndex;
using fox::HashResolver;
using fox::IndexedFile;

QString relativePathFor(const IndexedFile& f)
{
    if (!f.named) {
        QString name;
        HashResolver::instance().tryResolve(f.hash, &name);   // "<hex>.<ext>"
        return QStringLiteral("unresolved/") + name;
    }
    QString p = f.path;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    // Defensive: strip leading slashes and any ".." segments so a hostile path
    // string inside an FPK cannot climb out of the chosen output folder.
    QStringList parts = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    parts.removeAll(QStringLiteral(".."));
    parts.removeAll(QStringLiteral("."));
    if (parts.isEmpty()) return QStringLiteral("unresolved/empty-path");
    return parts.join(QLatin1Char('/'));
}

bool isFtex(const IndexedFile& f)
{
    return ArchiveIndex::extensionOf(f) == QLatin1String("ftex");
}

bool isFtexs(const IndexedFile& f)
{
    return ArchiveIndex::extensionOf(f).endsWith(QLatin1String(".ftexs"));
}

QByteArray assembleFtexToDds(const IndexedFile& f, QString* error, int* missingMips,
                             bool lowRes)
{
    ArchiveIndex& index = ArchiveIndex::instance();
    const QByteArray ftexData = index.readFile(f);
    if (ftexData.isEmpty()) {
        if (error) *error = QStringLiteral("could not read .ftex entry");
        return {};
    }
    fox::FtexFile ftex;
    if (!ftex.parse(ftexData)) {
        if (error) *error = ftex.errorString();
        return {};
    }

    // Sibling providers. A texture inside a PFTXS pack finds its streams in
    // the pack FIRST (small mips ship there), then falls back to the global
    // hash lookup — TPP keeps the high mips in the separate texture archives.
    std::function<QByteArray(int)> packProvider;
    std::function<QByteArray(int)> provider;

    if (f.childIdx >= 0) {
        // Texture inside a PFTXS pack: siblings live in the same group.
        // Through the run-scoped cache. This is the read the cache exists
        // for: assembling every texture in one pack re-inflates the whole pack
        // once per texture, and a pack holds hundreds. Outside a run the call
        // reads straight through and behaves exactly as it did.
        const QByteArray pack = blobcache::readEntry(f.archiveId, f.entryIdx);
        fox::PftxsFile pftxs;
        if (!pack.isEmpty() && pftxs.parse(pack)) {
            // Locate the group whose flattened entry range covers childIdx.
            int flat = 0, groupIdx = -1;
            for (int gi = 0; gi < pftxs.groups().size(); ++gi) {
                const int n = pftxs.groups()[gi].entries.size();
                if (f.childIdx < flat + n) { groupIdx = gi; break; }
                flat += n;
            }
            if (groupIdx >= 0) {
                // Capture the pack + a COPY of the group (not a pointer into a
                // stack local): the lambda outlives this scope.
                const fox::PftxsGroup home = pftxs.groups()[groupIdx];
                const bool gz = f.gz;
                packProvider = [pack, home, gz](int fileNo) -> QByteArray {
                    const QString want = QStringLiteral("%1.ftexs").arg(fileNo);
                    for (const fox::PftxsSubEntry& s : home.entries) {
                        const QString ext = gz
                            ? fox::GzsFile::extensionForId(
                                  static_cast<int>((s.hash >> 52) & 0xFFFF)).mid(1)
                            : HashResolver::instance().extensionFor(s.hash >> 51);
                        if (ext == want) return fox::PftxsFile::readEntry(pack, s);
                    }
                    return {};
                };
            }
        }
    }
    if (f.gz) {
        // GZ scheme: .N.ftexs ids are 95+N in the fixed table (96 = .1.ftexs).
        const quint64 pathHash = f.hash & 0xFFFFFFFFFFFFULL;
        provider = [&index, pathHash](int fileNo) -> QByteArray {
            if (fileNo < 1 || fileNo > 5) return {};
            const quint64 sibling =
                (static_cast<quint64>(95 + fileNo) << 52) | pathHash;
            const IndexedFile* sib = index.findByHash(sibling);
            return sib && sib->gz ? index.readFile(*sib) : QByteArray();
        };
    }
    else {
        const quint64 pathAndMeta = f.hash & ((1ULL << 51) - 1);
        provider = [&index, pathAndMeta](int fileNo) -> QByteArray {
            const quint64 extCode =
                fox::hashExtension(QStringLiteral("%1.ftexs").arg(fileNo));
            const quint64 sibling = (extCode << 51) | pathAndMeta;
            const IndexedFile* sib = index.findByHash(sibling);
            return sib ? index.readFile(*sib) : QByteArray();
        };
    }

    // A thumbnail wants the small end of the mip chain, and the .ftex already
    // holds it. Handing assembleDds a provider that returns nothing makes it
    // degrade to exactly those mips — and skips reading and inflating every
    // stream file, which is nearly the whole cost.
    if (lowRes) {
        const QByteArray small = ftex.assembleDds(
            ftexData, [](int) { return QByteArray(); }, missingMips);
        if (!small.isEmpty()) return small;
        // Some textures keep nothing inline; fall through to the full path
        // rather than showing an empty cell.
    }

    std::function<QByteArray(int)> chained = provider;
    if (packProvider) {
        const auto global = provider;
        chained = [packProvider, global](int fileNo) -> QByteArray {
            QByteArray b = packProvider(fileNo);
            if (b.isEmpty() && global) b = global(fileNo);
            return b;
        };
    }
    const QByteArray dds = ftex.assembleDds(ftexData, chained, missingMips);
    if (dds.isEmpty() && error)
        *error = QStringLiteral("no mip data could be assembled");
    return dds;
}

// ── Decoded-texture cache ────────────────────────────────────────────────────
// One .ftex is asked for many times over: materials on a model share maps, a
// character shares maps between parts, and re-tinting or re-posing reloads the
// lot. Measured with a counter in this function, on a release build:
//
//   Survivor (5 parts, PBR)         125 decodes for  30 distinct textures
//   DD soldier + camo (PBR)         100 decodes for  22 distinct textures
//   Venom, one model, PBR            50 decodes for  13 distinct textures
//
// Three quarters of the work was a texture the program had just finished
// decoding. Each of those is an archive read, a decrypt, an inflate, a mip
// assembly across up to five .ftexs stream files, and a BC decode.
//
// Keyed on the file's POSITION in the index rather than its hash: hashes come
// from two different schemes (TPP PathCode64 and the GZ legacy hash) that share
// a value space, and the position is what findByHash already resolved to.
// Bounded by decoded pixel bytes and evicted least-recently-used, so a long
// session browsing hundreds of models cannot grow without limit.
namespace {

constexpr qint64 kTexCacheMaxBytes = 256LL << 20;
// A second cap on the COUNT, because eviction picks the least-recent by
// scanning. Full-size maps are megabytes each, so the byte cap alone would
// hold a few dozen; thumbnails are tens of kilobytes, and browsing a large
// grid would otherwise pack the cache with thousands of them and make each
// eviction walk the lot.
constexpr int kTexCacheMaxEntries = 2048;

struct TexCacheEntry {
    QImage img;
    quint64 tick = 0;
    qint64 bytes = 0;
};

QMutex g_texMutex;
QHash<quint64, TexCacheEntry> g_texCache[2];   // [0] full res, [1] low res
quint64 g_texTick = 0;
qint64 g_texBytes = 0;
quint64 g_texGeneration = 0;

// Caller holds g_texMutex.
void texCacheTrimLocked()
{
    while (g_texBytes > kTexCacheMaxBytes
           || g_texCache[0].size() + g_texCache[1].size() > kTexCacheMaxEntries) {
        int oldestTable = -1;
        quint64 oldestTick = std::numeric_limits<quint64>::max();
        quint64 oldestKey = 0;
        for (int t = 0; t < 2; ++t)
            for (auto it = g_texCache[t].constBegin();
                 it != g_texCache[t].constEnd(); ++it)
                if (it->tick < oldestTick) {
                    oldestTick = it->tick;
                    oldestKey = it.key();
                    oldestTable = t;
                }
        if (oldestTable < 0) break;
        // constFind, not operator[]: the non-const subscript would DEFAULT-
        // INSERT if the scan and the removal ever disagreed, quietly adding a
        // phantom zero-byte entry instead of failing where it could be seen.
        const auto victim = g_texCache[oldestTable].constFind(oldestKey);
        if (victim == g_texCache[oldestTable].constEnd()) break;
        g_texBytes -= victim->bytes;
        g_texCache[oldestTable].remove(oldestKey);
    }
}

}  // namespace

void clearTextureCache()
{
    QMutexLocker lock(&g_texMutex);
    g_texCache[0].clear();
    g_texCache[1].clear();
    g_texBytes = 0;
}

QImage textureImageFor(const fox::FmdlTextureRef& ref, bool gzModel, bool lowRes)
{
    ArchiveIndex& index = ArchiveIndex::instance();
    const IndexedFile* tex = nullptr;

    if (!gzModel && ref.pathHash != 0) {
        // TPP: the stored PathCode64 IS the archive key of the .ftex.
        tex = index.findByHash(ref.pathHash);
    } else if (!ref.path.isEmpty()) {
        // GZ: legacy path hash + the fixed ftex extension id (52).
        const quint64 code = (52ULL << 52) | fox::hashFileNameLegacy(ref.path);
        tex = index.findByHash(code);
        if (tex && !tex->gz) tex = nullptr;
    }
    if (!tex) return {};

    // Kill switch, in the same spirit as the other env-gated diagnostics: it
    // makes a cold measurement possible and gives a way to rule the cache out
    // if a texture ever looks stale.
    static const bool kNoCache = qEnvironmentVariableIsSet("FOXAB_NO_TEXCACHE");
    if (kNoCache)
        return fox::bc::decodeDds(assembleFtexToDds(*tex, nullptr, nullptr, lowRes));

    const QVector<IndexedFile>& files = index.files();
    const int slot = lowRes ? 1 : 0;
    const quint64 key = quint64(tex - files.constData());
    const quint64 generation = index.installGeneration();

    {
        QMutexLocker lock(&g_texMutex);
        // A rescan re-numbers every file, so keys from the previous index mean
        // nothing. Checked here rather than wired to a signal, which would put
        // a back-dependency from the index onto this file.
        if (generation != g_texGeneration) {
            g_texCache[0].clear();
            g_texCache[1].clear();
            g_texBytes = 0;
            g_texGeneration = generation;
        }
        const auto it = g_texCache[slot].find(key);
        if (it != g_texCache[slot].end()) {
            it->tick = ++g_texTick;
            // SHARED with the cache. QImage is copy-on-write, so every caller
            // that writes through QPainter, scanLine() or bits() detaches
            // first and is safe — but a caller that reaches for an IN-PLACE
            // mutator (QImage::convertTo, unlike convertToFormat) would edit
            // every other holder's copy, including one already handed to the
            // exporter. Treat what comes back from here as read-only.
            return it->img;
        }
    }

    // Decoded OUTSIDE the lock: this is the expensive part, and holding the
    // mutex across it would serialise the extractor's worker threads. Two
    // threads racing on the same texture both decode it and the second insert
    // simply replaces an identical image.
    const QByteArray dds = assembleFtexToDds(*tex, nullptr, nullptr, lowRes);
    const QImage img = fox::bc::decodeDds(dds);

    QMutexLocker lock(&g_texMutex);
    if (generation != g_texGeneration) return img;   // rescanned mid-decode
    TexCacheEntry e;
    e.img = img;
    e.tick = ++g_texTick;
    e.bytes = qint64(img.sizeInBytes());
    const auto old = g_texCache[slot].constFind(key);
    if (old != g_texCache[slot].constEnd()) g_texBytes -= old->bytes;
    g_texBytes += e.bytes;
    g_texCache[slot].insert(key, e);
    texCacheTrimLocked();
    return img;
}


// ── The run-scoped decode cache ────────────────────────────────────────────
namespace blobcache {
namespace {

struct Entry { QByteArray data; qint64 tick = 0; };

QMutex g_mx;
QHash<quint64, Entry> g_map;
qint64 g_bytes = 0;
qint64 g_budget = 0;
qint64 g_tick = 0;
qint64 g_hits = 0;
qint64 g_misses = 0;
int g_depth = 0;   // nested scopes: the OUTERMOST one owns the lifetime
// THE INDEX GENERATION the cached blobs belong to. A rescan renumbers every
// archive and every entry, so (archiveId, entryIdx) from the previous index
// names a different container — and returning one container's bytes for
// another is the worst failure this cache could have. The texture cache above
// keys on the same value for the same reason; this one did not, and its only
// protection was a rescan guard that a still-open Scope can outlive.
quint64 g_generation = 0;

// Read straight from the archive, no cache. The two archive kinds keep their
// entries in different objects and neither is reachable through a common base,
// so the dispatch lives here once rather than at each call site.
QByteArray readRaw(int archiveId, int entryIdx)
{
    ArchiveIndex& index = ArchiveIndex::instance();
    if (archiveId < 0 || archiveId >= index.archives().size()) return {};
    const fox::IndexedArchive& arch = index.archives()[archiveId];
    if (arch.kind == fox::ArchiveKind::Sqar) {
        if (!arch.qar || entryIdx < 0 || entryIdx >= arch.qar->entries().size())
            return {};
        return arch.qar->readEntry(arch.qar->entries()[entryIdx]);
    }
    if (arch.kind == fox::ArchiveKind::Gzs) {
        if (!arch.gzs || entryIdx < 0 || entryIdx >= arch.gzs->entries().size())
            return {};
        return arch.gzs->readEntry(arch.gzs->entries()[entryIdx]);
    }
    // Loose. There is no container to inflate — the entry IS a file on disk —
    // so there is nothing here worth caching and nothing this function can
    // read: (archiveId, entryIdx) does not identify a loose file's bytes.
    // Explicit rather than falling through to the Gzs branch, which is what an
    // if/else pair did and which dereferenced a null gzs on a loose mount.
    return {};
}

// Evict least-recently-used until the budget is met. Called with g_mx held.
void trimLocked()
{
    while (g_bytes > g_budget && !g_map.isEmpty()) {
        auto oldest = g_map.begin();
        for (auto it = g_map.begin(); it != g_map.end(); ++it)
            if (it->tick < oldest->tick) oldest = it;
        g_bytes -= oldest->data.size();
        g_map.erase(oldest);
    }
}

}  // namespace

QByteArray readEntry(int archiveId, int entryIdx)
{
    const quint64 key = (quint64(quint32(archiveId)) << 32) | quint32(entryIdx);
    const quint64 generation = ArchiveIndex::instance().installGeneration();
    {
        QMutexLocker lock(&g_mx);
        if (generation != g_generation) {
            g_map.clear();
            g_bytes = 0;
            g_generation = generation;
        }
        if (g_depth == 0) {
            // No run in progress. Read straight through — measured behaviour
            // identical to what every call site did before this existed.
        } else {
            const auto it = g_map.find(key);
            if (it != g_map.end()) {
                it->tick = ++g_tick;
                ++g_hits;
                return it->data;   // implicitly shared; the copy is a refcount
            }
            ++g_misses;
        }
    }
    // OUTSIDE the lock. Inflating a 200 MB container while holding the mutex
    // would serialise every worker behind whichever one happened to miss, which
    // is slower than having no cache at all.
    QByteArray data = readRaw(archiveId, entryIdx);
    if (data.isEmpty()) return data;
    QMutexLocker lock(&g_mx);
    if (g_depth == 0) return data;
    // The index may have been rebuilt while this read was in flight, in which
    // case the bytes belong to the OLD numbering and must not be filed under
    // the new one. Return them (the caller asked under the old numbering too)
    // but do not keep them.
    if (ArchiveIndex::instance().installGeneration() != g_generation) return data;
    // Another worker may have inserted the same key while this one read it.
    // Keeping the existing copy means the two workers share one allocation.
    const auto it = g_map.find(key);
    if (it != g_map.end()) {
        it->tick = ++g_tick;
        return it->data;
    }
    g_map.insert(key, Entry{data, ++g_tick});
    g_bytes += data.size();
    trimLocked();
    return data;
}

Stats stats()
{
    QMutexLocker lock(&g_mx);
    return Stats{g_bytes, g_hits, g_misses, int(g_map.size())};
}

Scope::Scope(qint64 budgetBytes)
{
    QMutexLocker lock(&g_mx);
    if (g_depth++ == 0) {
        g_map.clear();
        g_bytes = 0;
        g_hits = 0;
        g_misses = 0;
        g_tick = 0;
        g_generation = ArchiveIndex::instance().installGeneration();
        g_budget = qMax(qint64(16LL * 1024 * 1024), budgetBytes);
    }
}

Scope::~Scope()
{
    QMutexLocker lock(&g_mx);
    if (--g_depth == 0) {
        g_map.clear();
        g_bytes = 0;
    }
    if (g_depth < 0) g_depth = 0;
}

}  // namespace blobcache

}  // namespace extract
