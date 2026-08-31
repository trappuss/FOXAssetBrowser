// TextureUsers.cpp — see TextureUsers.h.
#include "index/TextureUsers.h"

#include <QDataStream>
#include <QDateTime>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QThread>
#include <thread>

#include "app/AppPaths.h"
#include "fox/FmdlFile.h"
#include "index/ArchiveIndex.h"

namespace fox {

namespace {

constexpr quint32 kMagic = 0x55584554;   // "TEXU"

QString cachePath()
{
    return AppPaths::cacheFile(
        QStringLiteral("fox_texusers_v%1.bin").arg(kTexUsersCacheVersion));
}

// What this map DEPENDS ON, not just what the archives are.
//
// The archive list alone was wrong in a way that could not be recovered from
// without deleting the file by hand. Every .fmdl in a TPP install is a
// container CHILD, so whether the index holds any models at all is decided by
// the DEEP SCAN setting, not by the archives: sweep with deep scan off, cache
// an empty map, turn deep scan on — same archives, same fingerprint, cache hit,
// and every texture in the game is reported an orphan for ever.
//
// The resolved model PATHS are cached too, and those come from the
// dictionaries, so a dictionary that arrives later has to invalidate this or
// ASSOCIATED MODELS shows hex names with "Open in Models" permanently dead.
//
// And the file COUNT stands in for the content of a loose mount, whose folder
// stat says nothing about the files under it.
QString fingerprint()
{
    const ArchiveIndex& ix = ArchiveIndex::instance();
    QString fp;
    for (const IndexedArchive& a : ix.archives()) {
        const QFileInfo fi(a.filePath);
        fp += QStringLiteral("%1|%2|%3;")
                  .arg(a.filePath)
                  .arg(fi.size())
                  .arg(fi.lastModified().toSecsSinceEpoch());
    }
    fp += QStringLiteral("deep=%1;files=%2;named=%3;dict=%4;")
              .arg(ix.deepScanned() ? 1 : 0)
              .arg(ix.files().size())
              .arg(ix.namedCount())
              .arg(ix.dictionaryEntries());
    return fp;
}

QMutex g_mx;

}  // namespace

QMutex* TextureUsers::mutexPtr() const { return &g_mx; }

TextureUsers& TextureUsers::instance()
{
    static TextureUsers t;
    return t;
}

QVector<TextureUse> TextureUsers::usesOf(quint64 textureHash) const
{
    QMutexLocker lock(mutexPtr());
    return m_byTexture.value(textureHash);
}

int TextureUsers::userCount(quint64 textureHash) const
{
    QMutexLocker lock(mutexPtr());
    const auto it = m_byTexture.constFind(textureHash);
    return it == m_byTexture.constEnd() ? 0 : int(it->size());
}

QSet<quint64> TextureUsers::texturesWhere(
    const std::function<bool(quint64)>& modelOk) const
{
    QSet<quint64> out;
    if (!modelOk) return out;
    // The model verdicts are memoised across the whole pass: one model carries
    // dozens of textures, and re-resolving its tags for each of them is where
    // the time would go.
    // The pairs are taken under the lock; the PREDICATE runs outside it. It is
    // caller-supplied code — today's reaches into ArchiveIndex and ModelTags —
    // and calling arbitrary code under a non-recursive mutex that the same
    // caller's other queries also take is a deadlock waiting for its second
    // caller.
    QVector<QPair<quint64, QVector<quint64>>> pairs;
    {
        QMutexLocker lock(mutexPtr());
        pairs.reserve(m_byTexture.size());
        for (auto it = m_byTexture.constBegin(); it != m_byTexture.constEnd(); ++it) {
            QVector<quint64> models;
            for (const TextureUse& u : *it)
                if (!models.contains(u.modelHash)) models.append(u.modelHash);
            pairs.append({it.key(), models});
        }
    }
    QHash<quint64, bool> verdict;
    for (const auto& p : pairs) {
        for (const quint64 mh : p.second) {
            auto v = verdict.constFind(mh);
            const bool ok = v != verdict.constEnd()
                                ? *v
                                : verdict.insert(mh, modelOk(mh)).value();
            if (ok) { out.insert(p.first); break; }
        }
    }
    return out;
}

int TextureUsers::textureCount() const
{
    QMutexLocker lock(mutexPtr());
    return int(m_byTexture.size());
}

int TextureUsers::modelCount() const
{
    QMutexLocker lock(mutexPtr());
    return m_models;
}

int TextureUsers::opaqueModelCount() const
{
    QMutexLocker lock(mutexPtr());
    return m_opaqueModels;
}

void TextureUsers::shutdown()
{
    m_stopping.store(true);
    ++m_generation;
    // Wait for the sweep to notice. It checks both flags every model, so this
    // is bounded by one model's parse, not by the rest of the install.
    while (m_running.load()) QThread::msleep(5);
}

void TextureUsers::reset()
{
    // Bump the generation FIRST: a sweep still running for the previous index
    // checks it and throws its own result away rather than publishing rows that
    // name file indices nobody holds any more.
    ++m_generation;
    // The whole visible state moves together, under the lock the sweep's
    // publish also takes. Bumping the generation, clearing the map and setting
    // the state used to be three unlocked steps, and any interleaving that put
    // them either side of the sweep's publish left a STALE map marked Ready.
    {
        QMutexLocker lock(mutexPtr());
        m_byTexture.clear();
        m_models = 0;
        m_opaqueModels = 0;
        m_state.store(State::Idle);
    }
    m_done.store(0);
    m_total.store(0);
}

void TextureUsers::build()
{
    if (m_stopping.load()) return;
    if (m_state.load() == State::Building || m_state.load() == State::Ready)
        return;
    // A sweep can still be RUNNING while the state says Idle — reset() puts it
    // back to Idle so nothing trusts the old map, but the thread is still
    // walking every model in the install. Starting a second one would double
    // the work and race two writers to the same cache file.
    if (m_running.load()) return;
    if (!ArchiveIndex::instance().ready()) return;
    m_state.store(State::Building);
    m_running.store(true);
    // Detached, like the other background catalogues in this application. The
    // generation check inside sweep() is what makes that safe across a rescan;
    // shutdown() is what makes it safe across process exit.
    std::thread([this] { sweep(); }).detach();
}

bool TextureUsers::loadCache(const QString& fp)
{
    QFile f(cachePath());
    if (!f.open(QIODevice::ReadOnly)) return false;
    QDataStream in(&f);
    quint32 magic = 0, version = 0;
    QString storedFp;
    in >> magic >> version >> storedFp;
    if (magic != kMagic || version != kTexUsersCacheVersion || storedFp != fp)
        return false;
    qint32 models = 0, textures = 0, opaque = 0;
    in >> models >> opaque >> textures;
    // VALIDATED AGAINST THE FILE, not just for sign. A count is the one field
    // that turns a corrupt byte into an allocation: 1.9e9 reserved on the sweep
    // thread is a bad_alloc with no handler, which is std::terminate. The
    // cheapest true bound is the bytes that remain — no record is smaller than
    // its key.
    const qint64 remaining = f.size() - f.pos();
    const qint64 kMinRecord = 12;   // key + count, at the absolute minimum
    if (models < 0 || opaque < 0 || textures < 0
        || qint64(textures) * kMinRecord > remaining
        || in.status() != QDataStream::Ok)
        return false;
    QHash<quint64, QVector<TextureUse>> map;
    map.reserve(textures);
    for (qint32 i = 0; i < textures && in.status() == QDataStream::Ok; ++i) {
        quint64 key = 0;
        qint32 n = 0;
        in >> key >> n;
        if (n < 0 || qint64(n) * kMinRecord > f.size() - f.pos()) return false;
        QVector<TextureUse> uses;
        uses.reserve(n);
        for (qint32 j = 0; j < n && in.status() == QDataStream::Ok; ++j) {
            TextureUse u;
            in >> u.modelHash >> u.modelPath >> u.material >> u.shader >> u.role;
            uses.append(u);
        }
        map.insert(key, uses);
    }
    if (in.status() != QDataStream::Ok) return false;
    {
        QMutexLocker lock(mutexPtr());
        m_byTexture = map;
        m_models = models;
        m_opaqueModels = opaque;
    }
    // PUBLISH the counts on the cache path too. done()/total() were only ever
    // set by the sweep, so after a cache hit the map was complete and the
    // counters still read 0 — and File ▸ Index rendered that as
    // "Texture → model — done — 0 model(s)", which is a state that cannot
    // exist. Anything that reports progress has to report it on every path
    // that reaches the same end.
    m_total.store(models);
    m_done.store(models);
    qInfo("texusers: cache hit — %d model(s), %d texture(s)", models, textures);
    return true;
}

void TextureUsers::saveCache(const QString& fp) const
{
    QSaveFile out(cachePath());
    if (!out.open(QIODevice::WriteOnly)) {
        qWarning("texusers: cannot write %s", qUtf8Printable(cachePath()));
        return;
    }
    // SNAPSHOT under the lock, write outside it. Serialising a map of tens of
    // thousands of entries to disk with the mutex held blocks every GUI call
    // that touches it — and userCount() is called once per candidate row, on
    // every keystroke.
    QHash<quint64, QVector<TextureUse>> snapshot;
    qint32 models = 0, opaque = 0;
    {
        QMutexLocker lock(mutexPtr());
        snapshot = m_byTexture;
        models = qint32(m_models);
        opaque = qint32(m_opaqueModels);
    }
    // KEYS IN SORTED ORDER, not QHash order. QHash iterates in whatever order
    // its buckets happen to be in, which depends on insertion order — so the
    // same map built by a different number of sweep threads wrote a different
    // FILE with identical contents. Sorting makes the cache a function of what
    // was found rather than of how it was found, which is what lets "did this
    // change?" be answered with a checksum.
    QVector<quint64> keys(snapshot.keyBegin(), snapshot.keyEnd());
    std::sort(keys.begin(), keys.end());
    QDataStream os(&out);
    os << kMagic << kTexUsersCacheVersion << fp;
    os << models << opaque << qint32(keys.size());
    for (const quint64 k : keys) {
        const QVector<TextureUse>& v = snapshot[k];
        os << k << qint32(v.size());
        for (const TextureUse& u : v)
            os << u.modelHash << u.modelPath << u.material << u.shader << u.role;
    }
    if (!out.commit()) qWarning("texusers: cache write failed");
}

void TextureUsers::sweep()
{
    const quint64 myGeneration = m_generation.load();
    ArchiveIndex& index = ArchiveIndex::instance();
    const QString fp = fingerprint();

    if (loadCache(fp)) {
        // CHECKED AFTER the load, under the same lock the publish takes.
        // loadCache does multi-megabyte file I/O, and a rescan during it used
        // to clear the map and then have this store Ready over the top.
        QMutexLocker lock(mutexPtr());
        if (m_generation.load() != myGeneration || m_stopping.load()) {
            m_byTexture.clear();
            m_models = 0;
            m_opaqueModels = 0;
            m_state.store(State::Failed);
            m_running.store(false);
            return;
        }
        m_state.store(State::Ready);
        lock.unlock();
        m_running.store(false);
        Q_EMIT finished(true);
        return;
    }

    // The models to read, collected up front so `total` is a real number rather
    // than one that grows while the bar is moving.
    QVector<int> models;
    {
        const auto& files = index.files();
        for (int i = 0; i < files.size(); ++i)
            if (ArchiveIndex::extensionOf(files[i]) == QLatin1String("fmdl"))
                models.append(i);
    }
    m_total.store(int(models.size()));
    m_done.store(0);
    qInfo("texusers: sweeping %d model(s)…", int(models.size()));
    const qint64 startedAt = QDateTime::currentMSecsSinceEpoch();

    // ── The sweep, across every core ────────────────────────────────────
    // Reading a model is decrypt + inflate + parse: independent per file, and
    // ArchiveIndex::readFile is documented thread-safe. Single-threaded this
    // was ~120 us a model, which is a tenth of a second over the test tree and
    // the better part of a minute over a stock install's tens of thousands.
    //
    // CHUNKED, AND MERGED IN CHUNK ORDER, so the result is bit-identical to
    // the sequential sweep: each texture's user list keeps the order the
    // models were listed in. A free-for-all over one shared map would have
    // needed a lock per texture reference and produced a different — and
    // run-to-run unstable — ordering, which the cache file would then bake in.
    struct Chunk {
        int first = 0, last = 0;   // [first, last)
        QHash<quint64, QVector<TextureUse>> map;
        int parsed = 0, failed = 0, opaque = 0;
    };
    const int hw = int(std::thread::hardware_concurrency());
    // One thread below a few hundred models: the split costs more than it saves,
    // and it keeps the common "loose folder with a dozen models" case simple.
    int workers = models.size() < 256 ? 1 : qBound(1, hw > 0 ? hw : 4, 16);
    // Env-gated override, permanent (§13). Two reasons it stays in: proving
    // the parallel and sequential sweeps produce a BIT-IDENTICAL cache is a
    // one-command test with it and a rebuild without, and a machine where the
    // parallel sweep misbehaves can be put back to one thread without a build.
    if (const QByteArray e = qgetenv("FOXAB_TEXUSERS_THREADS"); !e.isEmpty()) {
        bool ok = false;
        const int want = e.toInt(&ok);
        if (ok && want > 0) workers = qBound(1, want, 64);
    }
    QVector<Chunk> chunks(workers);
    {
        const int per = int((models.size() + workers - 1) / workers);
        for (int w = 0; w < workers; ++w) {
            chunks[w].first = qMin(int(models.size()), w * per);
            chunks[w].last = qMin(int(models.size()), (w + 1) * per);
        }
    }
    std::atomic<int> doneCount{0};
    std::atomic<bool> abandoned{false};
    const auto runChunk = [&](Chunk& c) {
        for (int n = c.first; n < c.last; ++n) {
            // A rescan supersedes this sweep: stop rather than spend minutes
            // building a map for an index that no longer exists. Checked in
            // every worker, and once one gives up they all do.
            if (m_generation.load() != myGeneration || m_stopping.load()) {
                abandoned.store(true);
                return;
            }
            if (abandoned.load()) return;
            const int fi = models[n];
            if (fi < 0 || fi >= index.files().size()) continue;
            const IndexedFile& f = index.files()[fi];
            FmdlFile m;
            if (!m.parse(index.readFile(f))) { ++c.failed; continue; }
            ++c.parsed;
            // A model whose textures carry NO path hash tells this map nothing.
            // Ground Zeroes string-table models are the population: FmdlFile
            // stores their textures by name and leaves pathHash at 0, so they
            // parse fine and contribute nothing. Counted, so "no model uses it"
            // can be stated with the caveat it deserves rather than as a
            // measured fact.
            bool anyHash = false;
            for (const FmdlMaterialInstance& mat : m.materials()) {
                for (const FmdlTextureRef& t : mat.textures)
                    if (t.pathHash) { anyHash = true; break; }
                if (anyHash) break;
            }
            if (!m.materials().isEmpty() && !anyHash) ++c.opaque;
            for (const FmdlMaterialInstance& mat : m.materials()) {
                for (const FmdlTextureRef& t : mat.textures) {
                    if (!t.pathHash) continue;
                    TextureUse u;
                    u.modelHash = f.hash;
                    u.modelPath = f.named ? f.path : QString();
                    u.material = mat.name;
                    u.shader = mat.shader;
                    u.role = t.role;
                    c.map[t.pathHash].append(u);
                }
            }
            const int d = doneCount.fetch_add(1) + 1;
            m_done.store(d);
            // Bounded signalling: one every 256 models, not one per model. A
            // signal per model over 60,000 models is 60,000 queued cross-thread
            // events, which costs more than the parsing does.
            if ((d & 255) == 0) Q_EMIT progress(d, int(models.size()));
        }
    };
    if (workers == 1) {
        runChunk(chunks[0]);
    } else {
        // std::vector, not QVector: std::thread is move-only and Qt's
        // container wants to copy on reallocation.
        std::vector<std::thread> pool;
        pool.reserve(size_t(workers - 1));
        for (int w = 1; w < workers; ++w)
            pool.emplace_back([&runChunk, &chunks, w] { runChunk(chunks[w]); });
        runChunk(chunks[0]);
        for (std::thread& t : pool) t.join();
    }
    if (abandoned.load()) {
        qInfo("texusers: sweep abandoned — %s",
              m_stopping.load() ? "shutting down"
                                : "the index was rebuilt under it");
        // FAILED, not left at Building. Anything waiting on ready() waited for
        // ever otherwise, including the harness's own wait loop.
        m_state.store(State::Failed);
        m_running.store(false);
        return;
    }

    QHash<quint64, QVector<TextureUse>> map;
    int parsed = 0, failed = 0, opaque = 0;
    for (Chunk& c : chunks) {
        parsed += c.parsed;
        failed += c.failed;
        opaque += c.opaque;
        for (auto it = c.map.begin(); it != c.map.end(); ++it) {
            QVector<TextureUse>& dst = map[it.key()];
            dst.append(it.value());
        }
    }

    {
        // The check and the publish under ONE lock, so a reset() cannot land
        // between them and leave a superseded map marked Ready.
        QMutexLocker lock(mutexPtr());
        if (m_generation.load() != myGeneration || m_stopping.load()) {
            m_state.store(State::Failed);
            m_running.store(false);
            return;
        }
        m_byTexture = map;
        m_models = parsed;
        m_opaqueModels = opaque;
    }
    m_done.store(int(models.size()));
    const qint64 ms = QDateTime::currentMSecsSinceEpoch() - startedAt;
    qInfo("texusers: %d model(s) read, %d unparsed, %d with no usable texture "
          "reference, %d texture(s) referenced, %lld ms on %d thread(s)",
          parsed, failed, opaque, int(map.size()), ms, workers);
    saveCache(fp);
    m_state.store(State::Ready);
    m_running.store(false);
    Q_EMIT progress(int(models.size()), int(models.size()));
    Q_EMIT finished(true);
}

}  // namespace fox
