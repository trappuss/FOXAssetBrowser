// TexThumbCache.cpp — see TexThumbCache.h.
#include "index/TexThumbCache.h"

#include <QCoreApplication>

#include "app/SehGuard.h"
#include "fox/BcDecode.h"
#include "index/ArchiveIndex.h"
#include "util/Extract.h"

namespace fox {
namespace {
// 64 MB of decoded thumbnails, the same budget the model thumbnails get. Big
// enough that scrolling back through a page costs nothing, small enough that a
// forty-thousand-texture browse does not become a memory leak with a view.
constexpr qint64 kMaxBytes = 64LL * 1024 * 1024;

qint64 pixmapBytes(const QPixmap& p)
{
    return qint64(p.width()) * p.height() * 4 + 128;
}
}  // namespace

// ── Worker ───────────────────────────────────────────────────────────────────

void TexThumbWorker::decodeOne(int fileIdx, int size, int generation)
{
    // MUST emit done() on every path, including failure: the cache's `m_busy`
    // flag is cleared there, and a decode that returns without emitting stops
    // the queue forever.
    QImage out;
    seh::runGuarded("texthumb", [&] {
        const ArchiveIndex& ix = ArchiveIndex::instance();
        if (fileIdx < 0 || fileIdx >= ix.files().size()) return;
        const QByteArray dds = extract::assembleFtexToDds(ix.files()[fileIdx]);
        if (dds.isEmpty()) return;
        const QImage img = bc::decodeDds(dds);
        if (img.isNull()) return;
        out = img.scaled(size, size, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
    });
    Q_EMIT done(fileIdx, size, out, generation);
}

// ── Cache ────────────────────────────────────────────────────────────────────

TexThumbCache& TexThumbCache::instance()
{
    static TexThumbCache c;
    return c;
}

TexThumbCache::TexThumbCache() = default;

TexThumbCache::~TexThumbCache()
{
    shutdown();
}

void TexThumbCache::shutdown()
{
    // Called from QCoreApplication::aboutToQuit as well as from the destructor.
    // This object is a function-local static, so without the early call its
    // QPixmaps would be destroyed at process exit — after the platform plugin
    // has gone — which is a crash on the way out rather than a leak.
    m_queue.clear();
    m_cache.clear();
    m_order.clear();
    m_bytes = 0;
    if (m_thread) {
        m_thread->quit();
        if (!m_thread->wait(3000)) m_thread->terminate();
        m_thread->wait(500);
        delete m_thread;
        m_thread = nullptr;
        m_worker = nullptr;   // deleteLater'd by the finished() connection
    }
}

QString TexThumbCache::key(int fileIdx, int size)
{
    return QString::number(fileIdx) + QLatin1Char('@') + QString::number(size);
}

QPixmap TexThumbCache::cached(int fileIdx, int size) const
{
    return m_cache.value(key(fileIdx, size));
}

bool TexThumbCache::has(int fileIdx, int size) const
{
    return m_cache.contains(key(fileIdx, size));
}

int TexThumbCache::outstanding() const
{
    return m_queue.size() + (m_busy ? 1 : 0);
}

void TexThumbCache::ensureWorker()
{
    if (m_worker) return;
    m_thread = new QThread;
    m_thread->setObjectName(QStringLiteral("texthumbs"));
    m_worker = new TexThumbWorker;
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(qApp, &QCoreApplication::aboutToQuit, this,
            &TexThumbCache::shutdown, Qt::DirectConnection);
    connect(this, &TexThumbCache::wantOne, m_worker, &TexThumbWorker::decodeOne,
            Qt::QueuedConnection);
    connect(m_worker, &TexThumbWorker::done, this, &TexThumbCache::onDone,
            Qt::QueuedConnection);
    m_thread->start(QThread::LowPriority);
}

void TexThumbCache::request(int fileIdx, int size)
{
    if (fileIdx < 0 || size <= 0) return;
    const QString k = key(fileIdx, size);
    if (m_cache.contains(k)) return;
    // Already being decoded. Without this a repaint — and one is coalesced
    // after EVERY completed decode — re-queues the cell currently in flight,
    // and a page of sixty costs a hundred and twenty decodes.
    if (m_busy && k == m_inFlight) return;
    for (const auto& q : m_queue)
        if (q.first == fileIdx && q.second == size) return;
    m_queue.append({fileIdx, size});
    pump();
}

void TexThumbCache::cancelQueued()
{
    m_queue.clear();
}

void TexThumbCache::pump()
{
    // Nothing is decoded while the index is still being built: the file table
    // the worker reads is being rewritten under it.
    if (m_busy || m_queue.isEmpty() || ArchiveIndex::instance().building()) return;
    ensureWorker();
    const auto next = m_queue.takeFirst();
    m_busy = true;
    m_inFlight = key(next.first, next.second);
    Q_EMIT wantOne(next.first, next.second, m_generation);
}

void TexThumbCache::onDone(int fileIdx, int size, const QImage& img,
                           int generation)
{
    m_busy = false;
    m_inFlight.clear();
    if (generation == m_generation) {
        // A null result is cached too, so an undecodable texture is attempted
        // once rather than on every repaint for the rest of the session.
        insert(key(fileIdx, size),
               img.isNull() ? QPixmap() : QPixmap::fromImage(img));
        Q_EMIT ready(fileIdx, size);
    }
    pump();
}

void TexThumbCache::insert(const QString& k, const QPixmap& pm)
{
    // Re-inserting the same key must not double-count: leaving the old bytes
    // on the total makes the cache evict at half its stated budget, dropping
    // thumbnails that are still on screen.
    if (!m_cache.contains(k)) m_order.append(k);
    else m_bytes -= pixmapBytes(m_cache.value(k));
    m_bytes += pixmapBytes(pm);
    m_cache.insert(k, pm);
    while (m_bytes > kMaxBytes && !m_order.isEmpty()) {
        const QString old = m_order.takeFirst();
        const auto it = m_cache.constFind(old);
        if (it == m_cache.constEnd()) continue;
        m_bytes -= pixmapBytes(it.value());
        m_cache.erase(it);
    }
}

void TexThumbCache::reset()
{
    ++m_generation;
    m_queue.clear();
    if (m_worker && m_thread && m_thread->isRunning()) {
        // Wait for whatever is under way. It holds a reference INTO the archive
        // index that is about to be replaced — the file table, the archive
        // list and the QarFile it is reading from — so returning before it
        // finishes is a use-after-free, not a stale thumbnail. This is the one
        // place the GUI thread legitimately blocks on the decode thread, and it
        // happens once per re-index.
        QMetaObject::invokeMethod(m_worker, [] {}, Qt::BlockingQueuedConnection);
    }
    m_cache.clear();
    m_order.clear();
    m_bytes = 0;
}

}  // namespace fox
