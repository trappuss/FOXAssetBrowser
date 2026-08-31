// ImageView.cpp — see ImageView.h.
#include "preview/ImageView.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <cmath>

ImageView::ImageView(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(200, 200);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
}

void ImageView::setImage(const QImage& image, const QString& caption)
{
    m_image = image.convertToFormat(QImage::Format_RGBA8888);
    m_caption = caption;
    m_fitted = true;
    rebuildDisplay();
    fitToView();
    emit imageChanged();
}

void ImageView::clear()
{
    m_image = QImage();
    m_display = QImage();
    update();
    // ANNOUNCED, like setImage. Without this the channel strip below kept
    // showing the previous texture's six tiles over a viewer that had moved on
    // to "no image" or to a decode failure — and clicking one of those tiles
    // set a channel on nothing.
    emit imageChanged();
}

void ImageView::setChannel(Channel c)
{
    if (m_channel == c) return;
    m_channel = c;
    rebuildDisplay();
    update();
    emitStatus();
    emit channelChanged(c);
}

void ImageView::rebuildDisplay()
{
    if (m_image.isNull()) {
        m_display = QImage();
        return;
    }
    if (m_channel == RGB) {
        m_display = m_image;
        return;
    }
    m_display = QImage(m_image.size(), QImage::Format_RGBA8888);
    const int shift = m_channel == R ? 0 : m_channel == G ? 1 : m_channel == B ? 2 : 3;
    for (int y = 0; y < m_image.height(); ++y) {
        const quint8* src = m_image.constScanLine(y);
        quint8* dst = m_display.scanLine(y);
        for (int x = 0; x < m_image.width(); ++x) {
            const quint8 v =
                m_channel == Luma
                    ? quint8((src[x * 4 + 0] * 2126 + src[x * 4 + 1] * 7152
                              + src[x * 4 + 2] * 722) / 10000)
                    : src[x * 4 + shift];
            dst[x * 4 + 0] = v;
            dst[x * 4 + 1] = v;
            dst[x * 4 + 2] = v;
            dst[x * 4 + 3] = 255;
        }
    }
}

void ImageView::fitToView()
{
    if (m_image.isNull() || width() <= 0 || height() <= 0) return;
    const double zx = double(width()) / m_image.width();
    const double zy = double(height()) / m_image.height();
    m_zoom = qMin(1.0, qMin(zx, zy));   // never upscale on fit
    m_offset = QPointF((width() - m_image.width() * m_zoom) / 2.0,
                       (height() - m_image.height() * m_zoom) / 2.0);
    m_fitted = true;
    update();
    emitStatus();
}

void ImageView::oneToOne()
{
    if (m_image.isNull()) return;
    m_zoom = 1.0;
    m_offset = QPointF((width() - m_image.width()) / 2.0,
                       (height() - m_image.height()) / 2.0);
    m_fitted = false;
    update();
    emitStatus();
}

void ImageView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(34, 36, 40));
    if (m_display.isNull()) {
        p.setPen(QColor(120, 120, 126));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("no image"));
        return;
    }
    const QRectF target(m_offset,
                        QSizeF(m_image.width() * m_zoom, m_image.height() * m_zoom));

    // Checkerboard behind the image so alpha is visible. Off, the ground is
    // flat, which is easier to read a mask against.
    p.save();
    p.setClipRect(target);
    if (m_alphaBg) {
        const int cell = 12;
        for (int y = int(target.top()) / cell * cell; y < target.bottom(); y += cell)
            for (int x = int(target.left()) / cell * cell; x < target.right(); x += cell)
                p.fillRect(QRect(x, y, cell, cell),
                           ((x / cell + y / cell) & 1) ? QColor(52, 54, 58)
                                                       : QColor(64, 66, 70));
    } else {
        p.fillRect(target, QColor(24, 25, 28));
    }
    p.restore();

    p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 1.0);
    p.drawImage(target, m_display);
}

void ImageView::wheelEvent(QWheelEvent* e)
{
    if (m_image.isNull()) return;
    const double factor = std::pow(1.18, e->angleDelta().y() / 120.0);
    const QPointF pos = e->position();
    const QPointF imgPt = (pos - m_offset) / m_zoom;
    m_zoom = qBound(0.02, m_zoom * factor, 64.0);
    m_offset = pos - imgPt * m_zoom;
    m_fitted = false;
    update();
    emitStatus();
}

void ImageView::mousePressEvent(QMouseEvent* e)
{
    m_lastMouse = e->pos();
    // A middle-click FITS the image. Same gesture the 3D viewport uses to reset
    // its camera, so the two ways of looking at an asset reset the same way —
    // and it replaces the Fit / 1:1 buttons that were taking a row of height
    // above every texture.
    //
    // Press and release are tracked separately because middle-DRAG still pans:
    // a click is a press and release in the same place, and a drag is not. The
    // threshold is the same few pixels the viewport's right-drag uses, because
    // a mouse moves a little under a real click.
    if (e->button() == Qt::MiddleButton) {
        m_midPress = e->pos();
        m_midDown = true;
    }
}

void ImageView::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::MiddleButton && m_midDown) {
        m_midDown = false;
        if ((e->pos() - m_midPress).manhattanLength() <= 4) fitToView();
    }
}

void ImageView::mouseMoveEvent(QMouseEvent* e)
{
    if (e->buttons() & (Qt::LeftButton | Qt::MiddleButton)) {
        m_offset += e->pos() - m_lastMouse;
        m_fitted = false;
        update();
    }
    m_lastMouse = e->pos();
    if (!m_image.isNull()) {
        const QPointF imgPt = (QPointF(e->pos()) - m_offset) / m_zoom;
        const QPoint ip(int(imgPt.x()), int(imgPt.y()));
        if (m_image.rect().contains(ip)) {
            emitStatus(ip);
            return;
        }
    }
    emitStatus();
}

void ImageView::setAlphaBackground(bool on)
{
    if (m_alphaBg == on) return;
    m_alphaBg = on;
    update();
}

void ImageView::mouseDoubleClickEvent(QMouseEvent*)
{
    fitToView();
}

void ImageView::resizeEvent(QResizeEvent*)
{
    if (m_fitted) fitToView();
}

void ImageView::emitStatus(const QPoint& imagePos)
{
    if (m_image.isNull()) {
        emit statusText(QString());
        return;
    }
    QString s = QStringLiteral("%1  %2x%3  %4%")
                    .arg(m_caption)
                    .arg(m_image.width())
                    .arg(m_image.height())
                    .arg(int(m_zoom * 100));
    if (imagePos.x() >= 0) {
        const QRgb px = m_image.pixel(imagePos);
        s += QStringLiteral("  (%1,%2) rgba %3,%4,%5,%6")
                 .arg(imagePos.x())
                 .arg(imagePos.y())
                 .arg(qRed(px))
                 .arg(qGreen(px))
                 .arg(qBlue(px))
                 .arg(qAlpha(px));
    }
    emit statusText(s);
}
