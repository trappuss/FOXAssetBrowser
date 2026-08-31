// ChannelStrip.h — the six channel thumbnails under a texture preview (§7).
//
// Channel isolation already existed as five buttons, and buttons are the wrong
// control for it: choosing a channel is a LOOKING task — "which of these holds
// the mask" — and a row of labels makes you try all five to find out. The strip
// answers it at a glance, and clicking one is the same switch the buttons were.
//
// Six tiles, because LUMA is worth having beside the four: a Fox SRM packs
// three unrelated maps into RGB, and the luminance tile is what shows whether a
// channel is flat before you isolate it.
//
// The tiles are built from the image the preview ALREADY decoded — no second
// decode, no disk access — and are rebuilt only when that image changes.
#pragma once
#include <QImage>
#include <QVector>
#include <QWidget>

#include "preview/ImageView.h"

namespace fox {

class ChannelStrip : public QWidget {
    Q_OBJECT
public:
    explicit ChannelStrip(QWidget* parent = nullptr);

    // Rebuild from a decoded RGBA image. A null image empties the strip.
    void setImage(const QImage& rgba);
    void setCurrent(ImageView::Channel c);
    ImageView::Channel current() const { return m_current; }
    int tileSize() const { return m_tile; }

Q_SIGNALS:
    void channelPicked(ImageView::Channel c);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    QSize sizeHint() const override;

private:
    int tileAt(const QPoint& p) const;

    struct Tile { QImage img; QString label; ImageView::Channel channel; };
    QVector<Tile> m_tiles;
    ImageView::Channel m_current = ImageView::RGB;
    int m_tile = 64;
};

}  // namespace fox
