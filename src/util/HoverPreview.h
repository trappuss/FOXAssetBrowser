// HoverPreview.h — the popup that appears when the pointer rests on a row or a
// grid cell, showing the art big enough to actually look at.
//
// Three things make it useful rather than annoying:
//
//   * it waits. A pointer crossing a list is not a request to see anything, so
//     nothing happens until the pointer has been still for kDelayMs.
//   * it resizes on the wheel. The right size for a colour swatch and for a
//     character model are not the same, and the size is remembered.
//   * it stays on screen. The popup is placed beside the pointer and then
//     pushed back inside the screen it is on, so a row near the bottom right
//     does not open a preview half off the desktop.
//
// The content is an image plus up to three lines: a NAME, the asset PATH as a
// small dim subheader, and any number of extra facts the caller knows (a
// texture's size and format, a model's triangle and bone counts).
//
// One instance is shared by every caller — two previews on screen at once is
// never what anyone wanted.
#pragma once
#include <QImage>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <functional>

namespace hover {

// How long the pointer must rest before anything appears.
constexpr int kDelayMs = 500;
// The image box, in pixels. The wheel walks between these.
constexpr int kMinImage = 128;
constexpr int kMaxImage = 768;
constexpr int kDefaultImage = 320;
constexpr int kStepImage = 48;

// What to show. Everything but `image` is optional; a request with a null image
// and no text does nothing.
struct Content {
    QPixmap image;
    QString name;      // "Face 19" / "avf0_type1_def"
    QString path;      // "/Assets/ssd/chara/avm/Scenes/…" — the dim subheader
    QStringList info;  // "1024x1024 BC7" / "12 981 triangles · 706 bones"
};

class Preview : public QWidget {
    Q_OBJECT
public:
    static Preview& instance();

    // Ask for a preview at this global position. The popup appears kDelayMs
    // later unless cancel() or another request for a different key arrives
    // first. `key` identifies what is being hovered so that re-entering the
    // same row does not restart the wait.
    void request(const QString& key, const QPoint& globalPos, const Content& c);
    // Same, but the content is built only if the wait completes — for callers
    // whose content costs something (decoding a texture, rendering a model).
    void requestLazy(const QString& key, const QPoint& globalPos,
                     std::function<Content()> build);
    void cancel();
    // Re-run the lazy builder and repaint, for content that arrives after the
    // popup opened — a model thumbnail still being rendered, say. Does nothing
    // unless the popup is up and was built lazily.
    void refresh();

    // The current image box size, so a caller can decode at the right scale.
    int imageSize() const { return m_size; }

protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    explicit Preview(QWidget* parent = nullptr);
    void popUp();
    void relayout();
    void place();

    QTimer m_timer;
    QString m_key;
    QPoint m_pos;
    Content m_content;
    std::function<Content()> m_build;
    int m_size = kDefaultImage;
    QSize m_wanted;
};

}  // namespace hover
