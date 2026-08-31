// ImageView.h — 2D texture/image viewer: wheel zoom around the cursor, drag
// pan, RGB/R/G/B/A channel isolation, checkerboard behind alpha, 1:1 / fit
// modes, pixel readout in a status line.
#pragma once
#include <QImage>
#include <QWidget>

class QLabel;

class ImageView : public QWidget {
    Q_OBJECT
public:
    explicit ImageView(QWidget* parent = nullptr);

    void setImage(const QImage& image, const QString& caption = QString());
    void clear();
    const QImage& image() const { return m_image; }

    // Luma is Rec. 709 luminance of RGB. It is here rather than in a caller
    // because a Fox SRM packs three unrelated maps into RGB, and "is this
    // channel flat" is the question the strip under the viewer answers.
    enum Channel { RGB, R, G, B, A, Luma };
    void setChannel(Channel c);
    Channel channel() const { return m_channel; }
    // The checkerboard behind the image. On by default — alpha you cannot see
    // is alpha you get wrong — but a mask read against a flat ground is easier,
    // and §7 asks for the toggle rather than the permanent choice.
    void setAlphaBackground(bool on);
    bool alphaBackground() const { return m_alphaBg; }
    // Middle-click fits; middle-drag still pans. See mousePressEvent.
    QPoint m_midPress;
    bool m_midDown = false;
    void fitToView();
    void oneToOne();

signals:
    void statusText(const QString& text);   // zoom/pixel info for a host status line
    // A new image landed, or the channel changed. Both exist so a control that
    // MIRRORS this view — the channel strip, the toolbar's checked button —
    // cannot drift out of step with it, which is what a set of plain buttons
    // driving it one way already did.
    void imageChanged();
    void channelChanged(ImageView::Channel c);

protected:
    void paintEvent(QPaintEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    // Double-click resets the view (§7). It was scroll-to-zoom with no way
    // back but resizing the window.
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void rebuildDisplay();
    void emitStatus(const QPoint& imagePos = QPoint(-1, -1));

    QImage m_image;         // original RGBA
    QImage m_display;       // channel-filtered version actually drawn
    QString m_caption;
    Channel m_channel = RGB;
    bool m_alphaBg = true;
    double m_zoom = 1.0;
    QPointF m_offset;       // top-left of the image in widget coords
    QPoint m_lastMouse;
    bool m_fitted = true;   // auto-fit until the user zooms
};
