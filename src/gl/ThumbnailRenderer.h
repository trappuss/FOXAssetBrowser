// ThumbnailRenderer.h — small offscreen renders of indexed models, for the
// Models tab's grid view.
//
// The point is browsing: several hundred terse file names are far harder to
// search than several hundred pictures. So each row gets a real render of the
// model — not an icon sheet, not a guess from the name — drawn on a
// TRANSPARENT background so the grid reads as objects on the page rather than
// as a wall of dark tiles.
//
// Four things keep it cheap enough to sit behind a scrolling list:
//
//   • IT DOES NOT RUN ON THE GUI THREAD. Parsing an FMDL, pulling its textures
//     out of an archive and decoding them is tens to hundreds of milliseconds
//     of pure CPU work, and doing that between paint events is what made
//     scrolling stutter. A dedicated thread owns the OpenGL context and does
//     the whole load-decode-draw-readback; the GUI thread only ever hands over
//     a file index and receives a finished image.
//   • One OpenGL context and one framebuffer for the whole application, made
//     once and reused. Creating a context per thumbnail would cost more than
//     the render.
//   • BASE COLOUR ONLY. No normal maps, no specular, no shadow — one texture
//     fetch and a fixed two-light wrap so the silhouette still reads. Skipping
//     the normal maps alone removes most of the decode cost, because they are
//     the same size as the base maps and there are as many of them.
//   • Results are cached by (file, size) and never recomputed. The caller asks
//     only for what is on screen, so a list of forty thousand models renders
//     forty of them. The cache is bounded and lives in memory only — nothing
//     is ever written to disk.
//
// ThumbnailRenderer itself must be used from the GUI thread; every one of its
// entry points returns immediately.
#pragma once
#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QSize>

class QOpenGLContext;
class QOffscreenSurface;
class QOpenGLFramebufferObject;
class QOpenGLShaderProgram;
class QThread;

namespace fox {

// Lives on the render thread and owns every GL object. Nothing here touches a
// widget, a pixmap or the archive index's mutable state — the loaders it calls
// open their own file handles, so several passes never share one.
class ThumbnailWorker : public QObject {
    Q_OBJECT
public:
    ThumbnailWorker(QOpenGLContext* ctx, QOffscreenSurface* surface);

public Q_SLOTS:
    void renderOne(int fileIdx, int size, int generation);
    // Release the GL objects while the context can still be made current on
    // this thread. Called once, blocking, before the thread quits.
    void teardown();

Q_SIGNALS:
    void done(int fileIdx, int size, const QImage& img, int generation);

private:
    QImage draw(int fileIdx, int size);

    QOpenGLContext* m_ctx = nullptr;
    QOffscreenSurface* m_surface = nullptr;
    // Made once and kept: compiling the shader and building the framebuffer per
    // thumbnail cost more than drawing one.
    QOpenGLShaderProgram* m_prog = nullptr;
    QOpenGLFramebufferObject* m_fbo = nullptr;
    int m_fboSize = 0;
    unsigned int m_vao = 0, m_vbo = 0, m_ibo = 0, m_tex = 0;
    bool m_dead = false;
};

class ThumbnailRenderer : public QObject {
    Q_OBJECT
public:
    static ThumbnailRenderer& instance();

    // The cached thumbnail for this indexed model, or a null pixmap when it has
    // not been rendered yet. Never blocks.
    QPixmap cached(int fileIdx, int size) const;
    bool has(int fileIdx, int size) const;

    // Ask for a thumbnail. Returns at once; `ready` fires on the GUI thread
    // when the render lands. Asking twice for the same thing is free.
    void request(int fileIdx, int size);
    // Drop everything queued but not yet started. The caller does this before
    // re-queueing a new visible page, so a fling does not leave the thread
    // grinding through models that scrolled away minutes ago.
    void cancelQueued();
    // Build the GL context and start the render thread ahead of the first
    // request, so that work never lands inside a paint event.
    void prewarm();
    // Queued plus in flight — the dev harness waits on this.
    int outstanding() const;

    // The archives changed: every file index now means something else. Blocks
    // until any render already under way has finished, because that render is
    // reading the index that is about to be replaced.
    void reset();
    // Forget ONE model's thumbnails (every size) and re-render the sizes that
    // were cached. §15 asks for a re-render action, and the reason is specific:
    // a thumbnail is a picture of the model as it was when it was drawn, and
    // after a mod install or a re-extract it is a picture of the old one — with
    // no way to tell, because a stale thumbnail looks exactly like a fresh one.
    // Returns how many sizes were re-queued.
    int refresh(int fileIdx);
    int cachedCount() const { return m_cache.size(); }

Q_SIGNALS:
    // Internal: hands one request to the render thread.
    void submit(int fileIdx, int size, int generation);
    // A new thumbnail is in the cache.
    void ready(int fileIdx, int size);

private:
    ThumbnailRenderer() = default;
    bool ensureWorker();
    void pump();
    void onDone(int fileIdx, int size, const QImage& img, int generation);
    void shutdown();

    QThread* m_thread = nullptr;
    ThumbnailWorker* m_worker = nullptr;
    QOffscreenSurface* m_surface = nullptr;   // created here, used there
    bool m_workerFailed = false;
    bool m_busy = false;
    quint64 m_inFlight = 0;    // the key being rendered right now, when m_busy
    int m_generation = 0;

    QList<quint64> m_queue;            // waiting to be sent, in request order
    QSet<quint64> m_queued;            // membership test for the above
    QHash<quint64, QPixmap> m_cache;   // (fileIdx << 16 | size) → pixmap
    // Insertion order, so the cache can be held to a bound. This is a browsing
    // aid, not a database: it lives in memory, dies with the process, and never
    // grows past a few hundred entries.
    QList<quint64> m_order;
    qint64 m_bytes = 0;
    // Bounded by BYTES, not by entry count. A count bound is wrong in both
    // directions: 600 icons at 320 px would be a quarter of a gigabyte, and at
    // 48 px it evicts cells that are still on screen. A byte bound is the thing
    // actually being conserved, and no realistic viewport holds 64 MB of
    // thumbnails, so nothing on screen is ever evicted out from under itself.
    static constexpr qint64 kMaxBytes = 64ll * 1024 * 1024;
};

}  // namespace fox
