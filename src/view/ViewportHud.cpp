// ViewportHud.cpp — see ViewportHud.h.
#include "view/ViewportHud.h"

#include "view/ViewportBar.h"

#include <QEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>

#include "gl/GLModelWidget.h"

namespace fox {
namespace {

// How many labels are worth drawing. A character skeleton is 490 bones and
// every one of them projects to a point somewhere near the others; past a few
// hundred the overlay is a grey smear that hides the model it is annotating
// and costs a full text layout per frame. The cap is stated in the corner
// rather than applied silently — "that is all of them" and "that is the first
// two hundred" are different facts.
constexpr int kMaxLabels = 200;

void drawLabel(QPainter& p, const QPointF& at, const QString& text,
               const QColor& ink)
{
    const QFontMetrics fm = p.fontMetrics();
    const QRectF r = QRectF(fm.boundingRect(text)).translated(at + QPointF(6, -2));
    p.fillRect(r.adjusted(-2, -1, 2, 1), QColor(0, 0, 0, 130));
    p.setPen(ink);
    p.drawText(r.bottomLeft(), text);
}

}  // namespace

ViewportHud::ViewportHud(GLModelWidget* view) : QWidget(view), m_view(view)
{
    // The camera owns the viewport. Every pixel of this widget has to fall
    // through to it, or the overlay would make a third of the model
    // un-draggable the moment statistics were switched on.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);
    if (!view) return;
    view->installEventFilter(this);
    setGeometry(view->rect());
    // Repaint on anything that can move a label or change the numbers. The
    // camera one matters most: a label is a projection, so it is stale the
    // instant the camera moves and there is nothing else to notice that.
    connect(view, &GLModelWidget::cameraChanged, this,
            qOverload<>(&QWidget::update));
    connect(view, &GLModelWidget::sceneChanged, this,
            qOverload<>(&QWidget::update));
    connect(view, &GLModelWidget::displayChanged, this,
            qOverload<>(&QWidget::update));

    // ── The exit-fullscreen button (§5) ──────────────────────────────────
    // A REAL button, because the section is explicit: "never trap the user
    // behind a hotkey they didn't read a tooltip for". It is the one child of
    // this otherwise mouse-transparent widget that takes clicks, so it gets a
    // parent that does — itself, with the transparent attribute cleared on the
    // button rather than on the HUD.
    // PARENTED TO THE VIEWPORT, not to this HUD. Measured:
    // WA_TransparentForMouseEvents on a parent makes QWidget::childAt() skip
    // the whole subtree, so the button was unclickable and the press fell
    // through and started a camera orbit — clearing the attribute on the child
    // is a no-op, because it was already false. Which made the one control §5
    // insists on ("never trap the user behind a hotkey") a decoration.
    m_exit = new QPushButton(QStringLiteral("\u2715  Exit fullscreen"), view);
    m_exit->setCursor(Qt::PointingHandCursor);
    m_exit->setFocusPolicy(Qt::NoFocus);
    m_exit->setStyleSheet(QStringLiteral(
        "QPushButton { background: rgba(20,21,24,210); color: #e8e8ee;"
        " border: 1px solid #55596244; border-radius: 4px; padding: 5px 10px; }"
        "QPushButton:hover { background: rgba(40,42,48,235); }"));
    m_exit->hide();
    connect(m_exit, &QPushButton::clicked, this, [this] {
        if (m_view) m_view->setViewportFullscreen(false);
    });
    connect(view, &GLModelWidget::fullscreenChanged, this, [this](bool on) {
        if (!m_exit) return;
        m_exit->setVisible(on);
        if (on) m_exit->raise();
        layoutExitButton();
    });
    raise();
}

bool ViewportHud::eventFilter(QObject* o, QEvent* e)
{
    if (o == m_view && e->type() == QEvent::Resize) {
        setGeometry(m_view->rect());
        raise();
        layoutExitButton();
    }
    return QWidget::eventFilter(o, e);
}

void ViewportHud::paintEvent(QPaintEvent*)
{
    if (!m_view) return;
    const bool wantStats = m_view->showStats();
    const bool wantHelp = m_view->showHelp();
    const auto bones = m_view->boneLabelsOnScreen();
    const auto sockets = m_view->connectLabelsOnScreen();
    if (!wantStats && !wantHelp && bones.isEmpty() && sockets.isEmpty()) return;

    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    QFont f = font();
    f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1.0));
    p.setFont(f);

    if (wantStats) {
        const QString text = m_view->statsText();
        const QFontMetrics fm(f);
        QRectF box = fm.boundingRect(QRect(0, 0, width() - 24, height()),
                                     Qt::TextWordWrap, text);
        // UNDER THE BAR, which is top-left now. Asked rather than assumed:
        // the bar's height depends on the icon size and the platform's
        // padding, and a hard-coded offset would either overlap it or leave a
        // gap. barTop() answers 8 when there is no bar at all — the Files
        // preview builds one, but a future viewport might not.
        box.translate(10, barTop());
        p.fillRect(box.adjusted(-6, -4, 6, 4), QColor(0, 0, 0, 140));
        p.setPen(QColor(0xe4, 0xe4, 0xea));
        p.drawText(box, Qt::TextWordWrap, text);
    }

    // Sockets first and in their own colour: there are a handful of them and
    // they are the ones anyone is actually looking for. Bones are the haystack.
    for (const auto& s : sockets)
        drawLabel(p, s.second, s.first, QColor(0x66, 0xf0, 0xd8));

    if (wantHelp) drawHelp(p);

    int drawn = 0;
    for (const auto& b : bones) {
        if (drawn >= kMaxLabels) break;
        drawLabel(p, b.second, b.first, QColor(0xff, 0xc2, 0x6a));
        ++drawn;
    }
    if (drawn < bones.size()) {
        p.setPen(QColor(0xff, 0xc2, 0x6a));
        p.drawText(QPointF(10, height() - 10),
                   QStringLiteral("bone names: showing %1 of %2 on screen")
                       .arg(drawn)
                       .arg(bones.size()));
    }
}

// ── The F1 list ─────────────────────────────────────────────────────────────
// Every viewport shortcut this tool has, in one place, over the thing they act
// on. §5 asks for the fullscreen exit button by name; this is the same argument
// one step earlier — a key nobody was told about is a key nobody has.
void ViewportHud::drawHelp(QPainter& p)
{
    static const struct { const char* key; const char* what; } kRows[] = {
        {"Double-click", "select the part under the pointer"},
        {"Shift-click", "add a part to the selection"},
        {"Ctrl-click", "add or remove one part"},
        {"Right-click", "menu for what you clicked; on the selection, deselect"},
        {"H", "hide the selected parts"},
        {"Alt+H", "show every part again"},
        {"Shift+H", "hide everything EXCEPT the selected parts"},
        {".", "frame the selected part (whole scene when none)"},
        {"F", "fullscreen viewport"},
        {"Esc", "leave fullscreen, or close this list"},
        // ASCII ONLY in this table. These are const char* and are read with
        // QLatin1String, so a UTF-8 middot here arrives as two Latin-1
        // characters — the same mojibake this project already hit once in a
        // shading-mode note. A separator that has to be typographic belongs in
        // a QStringLiteral, not in a char array.
        {"Drag", "orbit  -  middle-drag pans  -  wheel zooms"},
        {"Middle-click", "reset the camera"},
        {"F1", "this list"},
    };
    QFont f = p.font();
    f.setPointSizeF(qMax(8.0, f.pointSizeF() + 1.0));
    p.setFont(f);
    const QFontMetrics fm(f);
    int keyW = 0, textW = 0;
    for (const auto& r : kRows) {
        keyW = qMax(keyW, fm.horizontalAdvance(QLatin1String(r.key)));
        textW = qMax(textW, fm.horizontalAdvance(QLatin1String(r.what)));
    }
    const int pad = 14, gap = 16, line = fm.height() + 4;
    const int w = pad * 2 + keyW + gap + textW;
    const int h = pad * 2 + line * int(sizeof(kRows) / sizeof(kRows[0])) + line;
    const QRect box((width() - w) / 2, qMax(8, (height() - h) / 2), w, h);
    p.fillRect(box, QColor(0, 0, 0, 205));
    p.setPen(QColor(0x55, 0x59, 0x62));
    p.drawRect(box.adjusted(0, 0, -1, -1));
    int y = box.top() + pad;
    p.setPen(QColor(0xf0, 0xf0, 0xf4));
    p.drawText(QRect(box.left() + pad, y, w - pad * 2, line),
               Qt::AlignLeft, QStringLiteral("Viewport"));
    y += line;
    for (const auto& r : kRows) {
        p.setPen(QColor(0x9c, 0xd8, 0xff));
        p.drawText(QRect(box.left() + pad, y, keyW, line),
                   Qt::AlignRight | Qt::AlignVCenter, QLatin1String(r.key));
        p.setPen(QColor(0xdc, 0xdc, 0xe2));
        p.drawText(QRect(box.left() + pad + keyW + gap, y, textW, line),
                   Qt::AlignLeft | Qt::AlignVCenter, QLatin1String(r.what));
        y += line;
    }
}

// The height at which overlay text may start: below the viewport bar when
// there is one, otherwise the plain margin.
int ViewportHud::barTop() const
{
    if (!m_view) return 8;
    if (ViewportBar* bar = m_view->findChild<ViewportBar*>(
            QString(), Qt::FindDirectChildrenOnly))
        if (bar->isVisible()) return bar->y() + bar->height() + 6;
    return 8;
}

void ViewportHud::layoutExitButton()
{
    if (!m_exit || !m_view) return;
    m_exit->adjustSize();
    // TOP CENTRE. It was top-left, which is where the viewport bar lives now;
    // top-right is the axis gizmo. The centre is the one place on the top edge
    // that nothing else claims — and it is where a browser puts the same
    // message, so it is the first place anyone looks.
    m_exit->move(qMax(0, (m_view->width() - m_exit->width()) / 2), 12);
    m_exit->raise();
}

}  // namespace fox
