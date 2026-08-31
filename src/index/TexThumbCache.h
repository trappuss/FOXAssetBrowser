// TexThumbCache.h — decoded texture thumbnails, off the GUI thread.
//
// The models grid renders its cells with the GPU (ThumbnailRenderer); a texture
// grid only has to DECODE, but decoding is not cheap either — a 2048x2048 BC7
// map is tens of milliseconds, and a screenful is a second of frozen UI. So it
// works the same way: ask, get nothing, get told later.
//
// Same shape as ThumbnailRenderer deliberately, so the two tabs read alike:
//
//   cached(fileIdx, size)   what is in hand right now; never blocks
//   request(fileIdx, size)  queue it; `ready` fires on the GUI thread
//   cancelQueued()          drop what has not started — a fling must not leave
//                           the worker grinding through rows that scrolled away
//
// The cache is bounded by BYTES rather than by entry count, because a thumbnail
// of a 64x64 UI chip and one of a 4K terrain map are not the same thing to keep.
// A failed decode is cached as a null pixmap, so an undecodable texture is
// attempted once and never again.
#pragma once
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPixmap>
#include <QThread>
#include <QVector>

namespace fox {

class TexThumbWorker : public QObject {
    Q_OBJECT
public Q_SLOTS:
    void decodeOne(int fileIdx, int size, int generation);
Q_SIGNALS:
    void done(int fileIdx, int size, const QImage& img, int generation);
};

class TexThumbCache : public QObject {
    Q_OBJECT
public:
    static TexThumbCache& instance();

    QPixmap cached(int fileIdx, int size) const;
    bool has(int fileIdx, int size) const;
    void request(int fileIdx, int size);
    void cancelQueued();
    int outstanding() const;
    // Drop the pixmaps and stop the thread while a QApplication still exists.
    void shutdown();
    // The archives changed: every file index now means something else.
    void reset();

Q_SIGNALS:
    void ready(int fileIdx, int size);
    void wantOne(int fileIdx, int size, int generation);

private:
    TexThumbCache();
    ~TexThumbCache() override;
    void ensureWorker();
    void pump();
    void onDone(int fileIdx, int size, const QImage& img, int generation);
    static QString key(int fileIdx, int size);
    void insert(const QString& k, const QPixmap& pm);

    QThread* m_thread = nullptr;
    TexThumbWorker* m_worker = nullptr;
    QHash<QString, QPixmap> m_cache;
    QVector<QString> m_order;          // insertion order, for eviction
    qint64 m_bytes = 0;
    QVector<QPair<int, int>> m_queue;  // (fileIdx, size)
    QString m_inFlight;                // the one key the worker is decoding
    bool m_busy = false;
    int m_generation = 0;
};

}  // namespace fox
