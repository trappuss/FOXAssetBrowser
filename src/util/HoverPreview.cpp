// HoverPreview.cpp — see HoverPreview.h.
#include "util/HoverPreview.h"

#include <QApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QWheelEvent>
#include <QtGlobal>

namespace hover {
namespace {
constexpr int kPad = 10;       // frame inset
constexpr int kGap = 6;        // image to text
constexpr int kCursorGap = 18; // pointer to popup
constexpr int kMaxTextWidth = 520;
}  // namespace

Preview& Preview::instance()
{
    static Preview* p = new Preview();
    return *p;
}

Preview::Preview(QWidget* parent)
    : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint
                          | Qt::NoDropShadowWindowHint)
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    m_timer.setSingleShot(true);
    m_timer.setInterval(kDelayMs);
    connect(&m_timer, &QTimer::timeout, this, &Preview::popUp);
}

void Preview::request(const QString& key, const QPoint& globalPos,
                      const Content& c)
{
    m_build = nullptr;
    if (key == m_key && isVisible()) {   // same row: just follow the pointer
        m_pos = globalPos;
        place();
        return;
    }
    m_key = key;
    m_pos = globalPos;
    m_content = c;
    hide();
    qApp->removeEventFilter(this);
    m_timer.start();
}

void Preview::requestLazy(const QString& key, const QPoint& globalPos,
                          std::function<Content()> build)
{
    if (key == m_key && isVisible()) {
        m_pos = globalPos;
        place();
        return;
    }
    m_key = key;
    m_pos = globalPos;
    m_content = Content();
    m_build = std::move(build);
    hide();
    qApp->removeEventFilter(this);
    m_timer.start();
}

void Preview::refresh()
{
    if (!isVisible() || !m_build) return;
    const bool wasNull = m_content.image.isNull();
    m_content = m_build();
    if (wasNull && m_content.image.isNull()) return;   // nothing new arrived
    relayout();
    place();
    update();
}

void Preview::cancel()
{
    m_timer.stop();
    m_key.clear();
    m_build = nullptr;
    hide();
    // Unconditionally: the filter can outlive a hide() that came from a new
    // request rather than from here, and an application-wide filter left
    // installed sees every event in the process.
    qApp->removeEventFilter(this);
}

void Preview::popUp()
{
    // The content is built HERE, not at request time: the pointer crosses
    // dozens of rows on the way to the one it stops at, and decoding a texture
    // for each of them would make scrolling the thing it is meant to help.
    // The builder is KEPT, not consumed: content can arrive late (a thumbnail
    // still rendering) and refresh() re-runs it in place.
    if (m_build) m_content = m_build();
    if (qEnvironmentVariableIsSet("FOXAB_HOVER_DEBUG"))
        qInfo("hover: popUp key=%s img=%s name=%s", qUtf8Printable(m_key),
              m_content.image.isNull() ? "null" : "ok",
              qUtf8Printable(m_content.name));
    if (m_content.image.isNull() && m_content.name.isEmpty()
        && m_content.info.isEmpty())
        return;
    relayout();
    place();
    show();
    raise();
    // While the popup is up, the wheel resizes it instead of scrolling whatever
    // is underneath. The filter is application-wide because the popup itself is
    // transparent to the mouse — the events arrive at the list, not here.
    qApp->installEventFilter(this);
}

bool Preview::eventFilter(QObject* o, QEvent* e)
{
    if (!isVisible()) return QWidget::eventFilter(o, e);
    if (e->type() == QEvent::Wheel) {
        auto* w = static_cast<QWheelEvent*>(e);
        const int dy = w->angleDelta().y();
        // Ctrl+wheel belongs to whoever is underneath — the models grid uses it
        // to resize its icons, and an application-wide filter runs BEFORE the
        // receiver's own, so swallowing it here would make that impossible for
        // as long as a preview happened to be up.
        if (dy != 0 && !(w->modifiers() & Qt::ControlModifier)) {
            const int was = m_size;
            m_size = qBound(kMinImage,
                            m_size + (dy > 0 ? kStepImage : -kStepImage),
                            kMaxImage);
            if (m_size != was) {
                // A lazily built preview can be rebuilt at the new size; a
                // ready-made pixmap is simply drawn larger.
                relayout();
                place();
                update();
            }
            return true;   // swallow: do not scroll the list underneath
        }
    }
    // Any click, key or window change takes it away.
    if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::KeyPress
        || e->type() == QEvent::FocusOut || e->type() == QEvent::WindowDeactivate)
        cancel();
    return QWidget::eventFilter(o, e);
}

void Preview::relayout()
{
    const QFontMetrics fmName(font());
    QFont small = font();
    small.setPointSizeF(qMax(6.5, font().pointSizeF() - 1.5));
    const QFontMetrics fmSmall(small);

    int imgW = 0, imgH = 0;
    if (!m_content.image.isNull()) {
        const QSize s = m_content.image.size().scaled(m_size, m_size,
                                                      Qt::KeepAspectRatio);
        imgW = s.width();
        imgH = s.height();
    }
    int textW = 0;
    if (!m_content.name.isEmpty())
        textW = qMax(textW, fmName.horizontalAdvance(m_content.name));
    if (!m_content.path.isEmpty())
        textW = qMax(textW, fmSmall.horizontalAdvance(m_content.path));
    for (const QString& s : m_content.info)
        textW = qMax(textW, fmSmall.horizontalAdvance(s));
    textW = qMin(textW, kMaxTextWidth);

    int textH = 0;
    if (!m_content.name.isEmpty()) textH += fmName.height();
    if (!m_content.path.isEmpty()) textH += fmSmall.height();
    textH += fmSmall.height() * m_content.info.size();

    const int w = kPad * 2 + qMax(imgW, textW);
    const int h = kPad * 2 + imgH + (textH > 0 && imgH > 0 ? kGap : 0) + textH;
    m_wanted = QSize(qMax(80, w), qMax(40, h));
    resize(m_wanted);
}

void Preview::place()
{
    QScreen* sc = QGuiApplication::screenAt(m_pos);
    if (!sc) sc = QGuiApplication::primaryScreen();
    const QRect avail = sc ? sc->availableGeometry() : QRect(0, 0, 1920, 1080);

    // Beside the pointer by preference, then pushed back inside the screen.
    // Flipping to the other side first keeps the popup off the pointer instead
    // of sliding it under one.
    int x = m_pos.x() + kCursorGap;
    int y = m_pos.y() + kCursorGap;
    if (x + width() > avail.right()) x = m_pos.x() - kCursorGap - width();
    if (y + height() > avail.bottom()) y = m_pos.y() - kCursorGap - height();
    x = qBound(avail.left(), x, avail.right() - width());
    y = qBound(avail.top(), y, avail.bottom() - height());
    move(x, y);
}

void Preview::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    const QColor bg = palette().window().color().darker(112);
    const QColor line = palette().mid().color();
    p.setPen(QPen(line, 1));
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1.0, height() - 1.0), 5, 5);

    int y = kPad;
    if (!m_content.image.isNull()) {
        const QPixmap pm = m_content.image.scaled(m_size, m_size,
                                                  Qt::KeepAspectRatio,
                                                  Qt::SmoothTransformation);
        p.drawPixmap((width() - pm.width()) / 2, y, pm);
        y += pm.height() + kGap;
    }
    QFont small = font();
    small.setPointSizeF(qMax(6.5, font().pointSizeF() - 1.5));

    if (!m_content.name.isEmpty()) {
        p.setFont(font());
        p.setPen(palette().text().color());
        const QFontMetrics fm(font());
        p.drawText(kPad, y + fm.ascent(),
                   fm.elidedText(m_content.name, Qt::ElideMiddle,
                                 width() - kPad * 2));
        y += fm.height();
    }
    p.setFont(small);
    const QFontMetrics fs(small);
    if (!m_content.path.isEmpty()) {
        QColor dim = palette().text().color();
        dim.setAlphaF(0.55f);
        p.setPen(dim);
        p.drawText(kPad, y + fs.ascent(),
                   fs.elidedText(m_content.path, Qt::ElideLeft,
                                 width() - kPad * 2));
        y += fs.height();
    }
    QColor mid = palette().text().color();
    mid.setAlphaF(0.80f);
    p.setPen(mid);
    for (const QString& s : m_content.info) {
        p.drawText(kPad, y + fs.ascent(),
                   fs.elidedText(s, Qt::ElideRight, width() - kPad * 2));
        y += fs.height();
    }
}

}  // namespace hover
