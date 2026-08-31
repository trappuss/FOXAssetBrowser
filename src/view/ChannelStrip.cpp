// ChannelStrip.cpp — see ChannelStrip.h.
#include "view/ChannelStrip.h"

#include <QMouseEvent>
#include <QPainter>

namespace fox {

namespace {

// Downsample to thumbnail size WITHOUT going through Qt's scaler.
//
// QImage::scaled() round-trips through ARGB32_Premultiplied, which multiplies
// RGB by alpha and cannot get it back. Measured on Qt 6: a pixel of
// rgba(10,200,30, a=0) comes out of scaled() as (0,0,0,0), and a=64 comes out
// as (12,199,32,64). So every packed channel under a cutout alpha reads as
// BLACK in the tiles while the viewer, which never scales, shows it correctly —
// and the case this strip exists for (an SRM packing three unrelated maps into
// RGB behind an alpha) is exactly the case it would have got wrong.
//
// A box average, one pass, on the unpremultiplied bytes. Alpha is averaged as
// its own channel and never touches the others.
QImage boxDownsample(const QImage& src, int tile)
{
    const int sw = src.width(), sh = src.height();
    if (sw <= 0 || sh <= 0) return {};
    const double scale = qMin(double(tile) / sw, double(tile) / sh);
    const int dw = qMax(1, int(sw * scale));
    const int dh = qMax(1, int(sh * scale));
    QImage out(dw, dh, QImage::Format_RGBA8888);
    for (int dy = 0; dy < dh; ++dy) {
        const int y0 = dy * sh / dh;
        const int y1 = qMax(y0 + 1, (dy + 1) * sh / dh);
        quint8* dst = out.scanLine(dy);
        for (int dx = 0; dx < dw; ++dx) {
            const int x0 = dx * sw / dw;
            const int x1 = qMax(x0 + 1, (dx + 1) * sw / dw);
            quint32 acc[4] = {0, 0, 0, 0};
            quint32 n = 0;
            for (int y = y0; y < y1; ++y) {
                const quint8* s = src.constScanLine(y);
                for (int x = x0; x < x1; ++x) {
                    acc[0] += s[x * 4 + 0];
                    acc[1] += s[x * 4 + 1];
                    acc[2] += s[x * 4 + 2];
                    acc[3] += s[x * 4 + 3];
                    ++n;
                }
            }
            if (!n) n = 1;
            for (int c = 0; c < 4; ++c) dst[dx * 4 + c] = quint8(acc[c] / n);
        }
    }
    return out;
}

// One channel as a grey image, at thumbnail size.
QImage channelTile(const QImage& small, int shift)
{
    QImage out(small.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < small.height(); ++y) {
        const quint8* src = small.constScanLine(y);
        quint8* dst = out.scanLine(y);
        for (int x = 0; x < small.width(); ++x) {
            const quint8 v = src[x * 4 + shift];
            dst[x * 4 + 0] = v;
            dst[x * 4 + 1] = v;
            dst[x * 4 + 2] = v;
            dst[x * 4 + 3] = 255;
        }
    }
    return out;
}

QImage lumaTile(const QImage& small)
{
    QImage out(small.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < small.height(); ++y) {
        const quint8* src = small.constScanLine(y);
        quint8* dst = out.scanLine(y);
        for (int x = 0; x < small.width(); ++x) {
            // Rec. 709, integer — the same weights the rest of this
            // application's luminance uses, so two views of one texture agree.
            const int v = (src[x * 4 + 0] * 2126 + src[x * 4 + 1] * 7152
                           + src[x * 4 + 2] * 722) / 10000;
            dst[x * 4 + 0] = quint8(v);
            dst[x * 4 + 1] = quint8(v);
            dst[x * 4 + 2] = quint8(v);
            dst[x * 4 + 3] = 255;
        }
    }
    return out;
}

}  // namespace

ChannelStrip::ChannelStrip(QWidget* parent) : QWidget(parent)
{
    setToolTip(QStringLiteral(
        "The same texture as RGB, then each channel on its own, then its "
        "luminance. Click one to isolate it in the viewer. A Fox SRM packs "
        "three unrelated maps into RGB, so which channel holds what is a "
        "question you answer by looking."));
    setCursor(Qt::PointingHandCursor);
    hide();   // nothing decoded yet
}

QSize ChannelStrip::sizeHint() const
{
    // Six tiles plus their labels. Fixed height so the preview above it does
    // not resize every time a texture is selected.
    return QSize(6 * (m_tile + 6) + 6, m_tile + 20);
}

void ChannelStrip::setImage(const QImage& rgba)
{
    m_tiles.clear();
    if (!rgba.isNull()) {
        const QImage small =
            boxDownsample(rgba.convertToFormat(QImage::Format_RGBA8888), m_tile);
        if (small.isNull()) { setVisible(false); update(); return; }
        m_tiles.append({small, QStringLiteral("RGBA"), ImageView::RGB});
        m_tiles.append({channelTile(small, 0), QStringLiteral("RED"), ImageView::R});
        m_tiles.append({channelTile(small, 1), QStringLiteral("GREEN"), ImageView::G});
        m_tiles.append({channelTile(small, 2), QStringLiteral("BLUE"), ImageView::B});
        m_tiles.append({channelTile(small, 3), QStringLiteral("ALPHA"), ImageView::A});
        m_tiles.append({lumaTile(small), QStringLiteral("LUMA"), ImageView::Luma});
    }
    // Hidden when there is nothing to show, rather than an empty band of
    // reserved height under a viewer that says "select a file".
    setVisible(!m_tiles.isEmpty());
    update();
}

void ChannelStrip::setCurrent(ImageView::Channel c)
{
    if (m_current == c) return;
    m_current = c;
    update();
}

int ChannelStrip::tileAt(const QPoint& p) const
{
    const int pitch = m_tile + 6;
    const int i = (p.x() - 3) / pitch;
    return (i >= 0 && i < m_tiles.size() && p.y() < m_tile + 4) ? i : -1;
}

void ChannelStrip::mousePressEvent(QMouseEvent* e)
{
    const int i = tileAt(e->pos());
    if (i < 0) return;
    setCurrent(m_tiles[i].channel);
    Q_EMIT channelPicked(m_tiles[i].channel);
}

void ChannelStrip::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const int pitch = m_tile + 6;
    for (int i = 0; i < m_tiles.size(); ++i) {
        const QRect box(3 + i * pitch, 2, m_tile, m_tile);
        p.fillRect(box, QColor(28, 30, 34));
        if (!m_tiles[i].img.isNull()) {
            const QSize s = m_tiles[i].img.size();
            const QRect at(box.left() + (box.width() - s.width()) / 2,
                           box.top() + (box.height() - s.height()) / 2,
                           s.width(), s.height());
            p.drawImage(at, m_tiles[i].img);
        }
        const bool cur = m_tiles[i].channel == m_current;
        p.setPen(QPen(cur ? palette().highlight().color() : QColor(70, 72, 78),
                      cur ? 2.0 : 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(box.adjusted(0, 0, -1, -1));
        p.setPen(cur ? palette().highlight().color() : palette().text().color());
        QFont f = p.font();
        f.setPointSizeF(qMax(6.5, f.pointSizeF() - 2.0));
        p.setFont(f);
        p.drawText(QRect(box.left(), box.bottom() + 2, m_tile, 14),
                   Qt::AlignHCenter | Qt::AlignTop, m_tiles[i].label);
    }
}

}  // namespace fox
