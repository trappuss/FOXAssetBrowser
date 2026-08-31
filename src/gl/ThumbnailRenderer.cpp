// ThumbnailRenderer.cpp — see ThumbnailRenderer.h.
#include "gl/ThumbnailRenderer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QThread>
#include <QVector3D>
#include <cfloat>

#include "app/SehGuard.h"
#include "index/ArchiveIndex.h"
#include "preview/ModelLoader.h"

namespace fox {
namespace {

constexpr int kMaxSize = 512;

const char* const kVert = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUv;
uniform mat4 uMvp;
uniform mat4 uModel;
out vec2 vUv;
out vec3 vN;
void main() {
    vUv = aUv;
    vN = mat3(uModel) * aNormal;
    gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

// Base colour only. The two-term wrap is not lighting in any physical sense —
// it exists so a silhouette is legible at 96 pixels. Alpha is written opaque
// for the model and left at zero everywhere else, which is what makes the
// background transparent.
const char* const kFrag = R"(#version 330 core
in vec2 vUv;
in vec3 vN;
uniform sampler2D uTex;
uniform int uHasTex;
out vec4 fragColor;
void main() {
    vec3 base = uHasTex != 0 ? texture(uTex, vUv).rgb : vec3(0.72);
    vec3 n = normalize(vN);
    float key = clamp(dot(n, normalize(vec3(0.4, 0.7, 0.6))) * 0.5 + 0.5, 0.0, 1.0);
    float fill = clamp(dot(n, normalize(vec3(-0.5, 0.2, -0.4))) * 0.5 + 0.5, 0.0, 1.0);
    fragColor = vec4(base * (0.55 + 0.45 * key) + base * 0.18 * fill, 1.0);
}
)";

inline qint64 pixmapBytes(const QPixmap& pm)
{
    // Plus a fixed slab for the QPixmap/QHash bookkeeping, so a cache of tiny
    // icons is not accounted as free.
    return qint64(pm.width()) * pm.height() * 4 + 128;
}

inline quint64 cacheKey(int fileIdx, int size)
{
    return (quint64(fileIdx) << 16) | quint64(size & 0xFFFF);
}

}  // namespace

// ---------------------------------------------------------------- worker ---

ThumbnailWorker::ThumbnailWorker(QOpenGLContext* ctx, QOffscreenSurface* surface)
    : m_ctx(ctx), m_surface(surface)
{
}

// CONTRACT: this MUST emit done() exactly once on every path, whatever
// happens. ThumbnailRenderer's m_busy flag is cleared by that signal and by
// nothing else, so a path that returns without emitting stops the entire grid
// permanently and silently. Do not add an early return here.
void ThumbnailWorker::renderOne(int fileIdx, int size, int generation)
{
    QImage img;
    if (!m_dead) {
        // The worker parses archive data that a partial or modified install can
        // make arbitrarily malformed, and drives a GPU driver. Both fault as
        // access violations, and the translator is per-thread — the GUI
        // thread's does not cover this one.
        seh::installSehTranslator();
        seh::HardwareFault fault;
        if (!seh::runGuarded("thumbnail", [&] { img = draw(fileIdx, size); },
                             &fault)) {
            qWarning("thumbnails: hardware fault rendering file %d — %s",
                     fileIdx, qUtf8Printable(fault.what));
            img = QImage();
            // The context may be left current and the framebuffer bound. Drop
            // the GL objects so the next thumbnail rebuilds them cleanly rather
            // than inheriting whatever state the fault left behind.
            if (m_ctx && m_ctx->isValid()) {
                delete m_fbo;
                m_fbo = nullptr;
                m_fboSize = 0;
                m_ctx->doneCurrent();
            }
        }
    }
    emit done(fileIdx, size, img, generation);
}

void ThumbnailWorker::teardown()
{
    if (m_dead) return;
    m_dead = true;
    if (m_ctx && m_ctx->makeCurrent(m_surface)) {
        QOpenGLFunctions_3_3_Core gl;
        if (gl.initializeOpenGLFunctions() && m_vao) {
            gl.glDeleteVertexArrays(1, &m_vao);
            gl.glDeleteBuffers(1, &m_vbo);
            gl.glDeleteBuffers(1, &m_ibo);
            gl.glDeleteTextures(1, &m_tex);
        }
        delete m_fbo;
        m_fbo = nullptr;
        delete m_prog;
        m_prog = nullptr;
        m_ctx->doneCurrent();
    }
    delete m_ctx;
    m_ctx = nullptr;
}

QImage ThumbnailWorker::draw(int fileIdx, int size)
{
    size = qBound(32, size, kMaxSize);
    const bool timing = qEnvironmentVariableIsSet("FOXAB_THUMB_TIMING");
    QElapsedTimer t;
    t.start();
    modelload::LoadedModel lm = modelload::loadForThumbnail(fileIdx);
    const qint64 tLoad = t.elapsed();
    if (!lm.ok || lm.uploads.isEmpty()) return {};

    if (!m_ctx || !m_ctx->makeCurrent(m_surface)) return {};
    QOpenGLFunctions_3_3_Core gl;
    if (!gl.initializeOpenGLFunctions()) { m_ctx->doneCurrent(); return {}; }

    // Render at double size and scale down: a 96-pixel model has very thin
    // features, and one free supersample is cheaper than multisampling.
    const int render = qMin(size * 2, kMaxSize);
    if (!m_fbo || m_fboSize != render) {
        delete m_fbo;
        QOpenGLFramebufferObjectFormat ffmt;
        ffmt.setAttachment(QOpenGLFramebufferObject::Depth);
        ffmt.setInternalTextureFormat(GL_RGBA8);
        m_fbo = new QOpenGLFramebufferObject(render, render, ffmt);
        m_fboSize = render;
    }
    if (!m_fbo->isValid()) { m_ctx->doneCurrent(); return {}; }
    QOpenGLFramebufferObject& fbo = *m_fbo;
    fbo.bind();

    if (!m_prog) {
        m_prog = new QOpenGLShaderProgram;
        if (!m_prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kVert)
            || !m_prog->addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag)
            || !m_prog->link()) {
            qWarning("thumbnails: shader failed — %s",
                     qUtf8Printable(m_prog->log()));
            delete m_prog;
            m_prog = nullptr;
            fbo.release();
            m_ctx->doneCurrent();
            return {};
        }
    }
    QOpenGLShaderProgram& prog = *m_prog;

    // Bounds, so every model is framed the same way whatever its scale.
    QVector3D lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const GLMeshUpload& m : lm.uploads)
        for (int i = 0; i + 2 < m.interleaved.size(); i += kVertexFloats) {
            lo.setX(qMin(lo.x(), m.interleaved[i]));
            lo.setY(qMin(lo.y(), m.interleaved[i + 1]));
            lo.setZ(qMin(lo.z(), m.interleaved[i + 2]));
            hi.setX(qMax(hi.x(), m.interleaved[i]));
            hi.setY(qMax(hi.y(), m.interleaved[i + 1]));
            hi.setZ(qMax(hi.z(), m.interleaved[i + 2]));
        }
    if (lo.x() > hi.x()) { fbo.release(); m_ctx->doneCurrent(); return {}; }
    const QVector3D centre = (lo + hi) * 0.5f;
    const float radius = qMax(0.001f, (hi - lo).length() * 0.5f);

    QMatrix4x4 proj;
    proj.perspective(35.0f, 1.0f, radius * 0.05f, radius * 12.0f);
    QMatrix4x4 view;
    // A three-quarter view from slightly above: a straight-on shot makes a
    // helmet and a hat look the same.
    const QVector3D eye = centre
        + QVector3D(0.62f, 0.42f, 1.0f).normalized() * (radius * 3.0f);
    view.lookAt(eye, centre, QVector3D(0, 1, 0));
    QMatrix4x4 model;

    gl.glViewport(0, 0, render, render);
    gl.glClearColor(0.0f, 0.0f, 0.0f, 0.0f);   // transparent
    gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl.glEnable(GL_DEPTH_TEST);
    gl.glDisable(GL_CULL_FACE);   // Fox meshes are not consistently wound

    prog.bind();
    prog.setUniformValue("uMvp", proj * view * model);
    prog.setUniformValue("uModel", model);
    prog.setUniformValue("uTex", 0);

    // Buffers and one scratch texture, made once and reused. Creating and
    // destroying a QOpenGLTexture per mesh per thumbnail was a driver call
    // storm for no benefit.
    if (!m_vao) {
        gl.glGenVertexArrays(1, &m_vao);
        gl.glGenBuffers(1, &m_vbo);
        gl.glGenBuffers(1, &m_ibo);
        gl.glGenTextures(1, &m_tex);
    }
    gl.glBindVertexArray(m_vao);
    const GLuint vbo = m_vbo, ibo = m_ibo;

    for (const GLMeshUpload& m : lm.uploads) {
        if (m.interleaved.isEmpty() || m.indices.isEmpty()) continue;
        gl.glBindBuffer(GL_ARRAY_BUFFER, vbo);
        gl.glBufferData(GL_ARRAY_BUFFER,
                        m.interleaved.size() * qsizetype(sizeof(float)),
                        m.interleaved.constData(), GL_STREAM_DRAW);
        gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                        m.indices.size() * qsizetype(sizeof(quint32)),
                        m.indices.constData(), GL_STREAM_DRAW);
        const int stride = kVertexFloats * sizeof(float);
        gl.glEnableVertexAttribArray(0);
        gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
        gl.glEnableVertexAttribArray(1);
        gl.glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                                 reinterpret_cast<void*>(3 * sizeof(float)));
        gl.glEnableVertexAttribArray(2);
        gl.glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                                 reinterpret_cast<void*>(6 * sizeof(float)));

        const bool hasTex = m.materialSlot >= 0
            && m.materialSlot < lm.textures.size()
            && !lm.textures[m.materialSlot].isNull();
        if (hasTex) {
            // The low-res assembly already gives a few hundred pixels; cap it
            // anyway so one oddly-authored map cannot dominate the upload.
            QImage src = lm.textures[m.materialSlot];
            if (src.width() > 256 || src.height() > 256)
                src = src.scaled(256, 256, Qt::KeepAspectRatio,
                                 Qt::FastTransformation);
            // NO vertical flip — the same convention the viewport uses and for
            // the same reason: FMDL UVs are top-origin, and row 0 of the QImage
            // is v=0. Mirroring here turned every map upside down, which reads
            // as "the wood grain is on the wrong end of the grip" rather than
            // as an obviously flipped image.
            src = src.convertToFormat(QImage::Format_RGBA8888);
            gl.glActiveTexture(GL_TEXTURE0);
            gl.glBindTexture(GL_TEXTURE_2D, m_tex);
            gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl.glPixelStorei(GL_UNPACK_ROW_LENGTH,
                             int(src.bytesPerLine() / 4));
            gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, src.width(), src.height(),
                            0, GL_RGBA, GL_UNSIGNED_BYTE, src.constBits());
            gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        }
        prog.setUniformValue("uHasTex", hasTex ? 1 : 0);
        gl.glDrawElements(GL_TRIANGLES, m.indices.size(), GL_UNSIGNED_INT, nullptr);
    }

    gl.glBindVertexArray(0);
    prog.release();

    QImage out = fbo.toImage();
    fbo.release();
    m_ctx->doneCurrent();
    if (timing)
        qInfo("thumb[%s]: load %lld ms, gl %lld ms, %d mesh(es), %d texture(s)",
              qUtf8Printable(QThread::currentThread()->objectName()),
              tLoad, t.elapsed() - tLoad, int(lm.uploads.size()),
              lm.texturesFound);
    if (out.isNull()) return {};
    if (out.format() != QImage::Format_ARGB32)
        out = out.convertToFormat(QImage::Format_ARGB32);
    return out.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

// -------------------------------------------------------------- renderer ---

ThumbnailRenderer& ThumbnailRenderer::instance()
{
    static ThumbnailRenderer r;
    return r;
}

bool ThumbnailRenderer::has(int fileIdx, int size) const
{
    return m_cache.contains(cacheKey(fileIdx, size));
}

QPixmap ThumbnailRenderer::cached(int fileIdx, int size) const
{
    return m_cache.value(cacheKey(fileIdx, size));
}

int ThumbnailRenderer::outstanding() const
{
    return m_queue.size() + (m_busy ? 1 : 0);
}

void ThumbnailRenderer::request(int fileIdx, int size)
{
    if (fileIdx < 0) return;
    const quint64 key = cacheKey(fileIdx, size);
    if (m_cache.contains(key) || m_queued.contains(key)) return;
    // pump() takes the key OUT of m_queued when it submits, so without this the
    // cell that is being rendered right now gets queued all over again on the
    // next scroll settle — a whole redundant parse and decode per page.
    if (m_busy && key == m_inFlight) return;
    m_queued.insert(key);
    m_queue.append(key);
    pump();
}

// Build the context and start the thread NOW, rather than on the first cache
// miss — which, since paint() drives the queue, would otherwise happen inside a
// paint event with the viewport's painter active.
void ThumbnailRenderer::prewarm()
{
    ensureWorker();
}

void ThumbnailRenderer::cancelQueued()
{
    m_queue.clear();
    m_queued.clear();
}

void ThumbnailRenderer::pump()
{
    if (m_busy || m_queue.isEmpty()) return;
    // Never hand the render thread a file index while the archive index is
    // being rebuilt. loadForThumbnail() holds bare references into the index's
    // vectors, and install() replaces those vectors wholesale on the GUI
    // thread — a render straddling that swap reads freed memory. Requests made
    // during a rebuild simply wait; reset() clears them when it lands.
    if (ArchiveIndex::instance().building()) return;
    if (!ensureWorker()) { cancelQueued(); return; }
    const quint64 key = m_queue.takeFirst();
    m_queued.remove(key);
    m_busy = true;
    m_inFlight = key;
    emit submit(int(key >> 16), int(key & 0xFFFF), m_generation);
}

void ThumbnailRenderer::onDone(int fileIdx, int size, const QImage& img,
                               int generation)
{
    // Torn down already: the cache has been dropped on purpose and must not be
    // repopulated, because this object outlives QApplication.
    if (!m_thread) return;
    const quint64 done = cacheKey(fileIdx, size);
    // Clear the flag only for the submission that is actually outstanding. A
    // late result from before a reset() must not un-busy a render that has
    // since been started, or pump() runs a second one alongside it and
    // outstanding() stops meaning anything.
    if (m_busy && done == m_inFlight) { m_busy = false; m_inFlight = 0; }
    // A render that started before the index was rebuilt describes a file index
    // that no longer means the same thing. Drop it rather than cache it.
    if (generation == m_generation) {
        const quint64 key = done;
        // A failure caches a null pixmap on purpose: without that, a model that
        // cannot be loaded is re-parsed every time it scrolls back into view.
        const QPixmap pm = img.isNull() ? QPixmap() : QPixmap::fromImage(img);
        if (!m_cache.contains(key)) m_order.append(key);
        else m_bytes -= pixmapBytes(m_cache.value(key));
        m_cache.insert(key, pm);
        m_bytes += pixmapBytes(pm);
        // Bounded on purpose. Thumbnails are a browsing aid; a cache that grows
        // without limit while someone scrolls a forty-thousand-model list is
        // bloat, and re-rendering the oldest few costs a fraction of a second.
        while (m_bytes > kMaxBytes && m_order.size() > 1) {
            const quint64 old = m_order.takeFirst();
            // take(), not value() + remove(): a missing key would otherwise
            // subtract a phantom 128 bytes, and a negative m_bytes turns
            // eviction off entirely instead of failing loudly.
            m_bytes -= pixmapBytes(m_cache.take(old));
        }
        emit ready(fileIdx, size);
    }
    pump();
}

bool ThumbnailRenderer::ensureWorker()
{
    if (m_worker) return true;
    if (m_workerFailed) return false;
    auto* ctx = new QOpenGLContext;
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setAlphaBufferSize(8);
    ctx->setFormat(fmt);
    if (!ctx->create()) {
        delete ctx;
        m_workerFailed = true;
        qWarning("thumbnails: no OpenGL context — the grid will show names only");
        return false;
    }
    // A QOffscreenSurface has to be CREATED on the GUI thread; it may then be
    // made current from another one. So it stays owned here and only the
    // context moves.
    m_surface = new QOffscreenSurface;
    m_surface->setFormat(ctx->format());
    m_surface->create();
    if (!m_surface->isValid()) {
        delete m_surface;
        m_surface = nullptr;
        delete ctx;
        m_workerFailed = true;
        qWarning("thumbnails: no offscreen surface — the grid will show names only");
        return false;
    }

    m_thread = new QThread;
    m_thread->setObjectName(QStringLiteral("thumbnails"));
    ctx->moveToThread(m_thread);
    m_worker = new ThumbnailWorker(ctx, m_surface);
    m_worker->moveToThread(m_thread);
    connect(this, &ThumbnailRenderer::submit, m_worker, &ThumbnailWorker::renderOne);
    connect(m_worker, &ThumbnailWorker::done, this, &ThumbnailRenderer::onDone);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    // Below the interface on purpose: a thumbnail is never worth a dropped
    // frame in the viewport.
    m_thread->start(QThread::LowPriority);
    if (auto* app = QCoreApplication::instance())
        connect(app, &QCoreApplication::aboutToQuit, this,
                &ThumbnailRenderer::shutdown);
    return true;
}

int ThumbnailRenderer::refresh(int fileIdx)
{
    if (fileIdx < 0) return 0;
    // Which sizes this model is cached at, collected BEFORE anything is
    // dropped: the loop that follows re-queues them, and re-queueing while
    // walking the container it is about to write into is how a rehash
    // invalidates the iterator under you.
    QVector<int> sizes;
    for (auto it = m_cache.constBegin(); it != m_cache.constEnd(); ++it)
        if (int(it.key() >> 16) == fileIdx)
            sizes.append(int(it.key() & 0xFFFF));
    for (int sz : sizes) m_cache.remove(cacheKey(fileIdx, sz));
    for (int sz : sizes) request(fileIdx, sz);
    return sizes.size();
}

void ThumbnailRenderer::reset()
{
    ++m_generation;
    cancelQueued();
    if (m_worker && m_thread && m_thread->isRunning()) {
        // Wait for whatever is under way. It is reading the archive index that
        // is about to be replaced, and this is the one place the GUI thread
        // legitimately blocks on the render thread — it happens once per
        // re-index, never while browsing.
        QMetaObject::invokeMethod(m_worker, [] {}, Qt::BlockingQueuedConnection);
    }
    // m_busy is deliberately NOT cleared. The drain above guarantees that any
    // render under way has finished and already POSTED its done() to this
    // thread; that result will arrive, clear the flag itself and be thrown away
    // on the generation check. Clearing it here would let pump() start a second
    // render while the first result was still in the queue.
    m_cache.clear();
    m_order.clear();
    m_bytes = 0;
}

void ThumbnailRenderer::shutdown()
{
    if (!m_thread) return;
    cancelQueued();
    if (m_worker && m_thread->isRunning())
        QMetaObject::invokeMethod(m_worker, "teardown", Qt::BlockingQueuedConnection);
    m_thread->quit();
    if (m_thread->wait(3000)) {
        delete m_thread;
    } else {
        // Deleting a running QThread aborts. The process is on its way out, so
        // leak it deliberately and say so.
        qWarning("thumbnails: render thread did not stop — leaking it");
    }
    m_thread = nullptr;
    m_worker = nullptr;
    delete m_surface;
    m_surface = nullptr;
    m_busy = false;
    m_inFlight = 0;
    // Drop the pixmaps HERE. This object has static storage, so its destructor
    // runs after ~QApplication, and destroying a QPixmap at that point is not
    // safe on every platform.
    m_cache.clear();
    m_order.clear();
    m_bytes = 0;
    // Do not build another one during teardown.
    m_workerFailed = true;
}

}  // namespace fox
