// ViewportBar.cpp — see ViewportBar.h.
#include "view/ViewportBar.h"

#include "view/ViewportGizmo.h"

#include <QActionGroup>
#include <QEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QToolButton>
#include <QWheelEvent>

#include "gl/GLModelWidget.h"
#include "gl/ViewEnvironment.h"
#include "view/ViewGlyphs.h"
#include "view/ViewportPanel.h"

namespace fox {
namespace {

constexpr int kMargin = 8;
constexpr int kBall = 22;

// The card the whole strip sits in. Dark whatever the system theme is, for the
// same reason the popovers are: it floats over a 3D scene whose background the
// user chooses, and a strip that follows the system palette is invisible on
// half of them.
const char* const kBarQss =
    "QWidget#fabViewBar{background:rgba(24,26,30,215);border:1px solid "
    "rgba(255,255,255,26);border-radius:5px;}"
    "QToolButton{border:1px solid transparent;border-radius:3px;}"
    "QToolButton:checked{background:rgba(255,255,255,34);"
    "border-color:rgba(255,255,255,55);}"
    "QToolButton:hover{background:rgba(255,255,255,18);}";

// A shading ball: a sphere lit the way that mode lights the model. Drawn
// rather than shipped as four PNGs, because the whole point of the row is that
// the pictures ARE the modes — a wireframe ball is a wire sphere, a flat one
// is a disc of flat colour, a shaded one has a terminator, a rendered one has
// a specular highlight — and four icons in a resource file would drift from
// what the viewport actually does the first time either changed.
QIcon shadingBall(ShadingMode mode, int px)
{
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(1.5, 1.5, px - 3.0, px - 3.0);
    const QColor ink(0xd2, 0xd2, 0xd8);
    switch (mode) {
        case ShadingMode::Wireframe: {
            p.setPen(QPen(ink, 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(r);
            // Two meridians and an equator: a sphere as a wire cage, which is
            // what a wireframe of a ball looks like.
            p.drawEllipse(r.adjusted(r.width() * 0.30, 0, -r.width() * 0.30, 0));
            p.drawLine(QPointF(r.left(), r.center().y()),
                       QPointF(r.right(), r.center().y()));
            break;
        }
        case ShadingMode::Flat: {
            p.setPen(Qt::NoPen);
            p.setBrush(ink);
            p.drawEllipse(r);
            break;
        }
        case ShadingMode::Shaded: {
            QLinearGradient g(r.topLeft(), r.bottomRight());
            g.setColorAt(0.0, QColor(0xf0, 0xf0, 0xf4));
            g.setColorAt(1.0, QColor(0x4a, 0x4a, 0x52));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawEllipse(r);
            break;
        }
        case ShadingMode::Rendered: {
            QRadialGradient g(r.center() + QPointF(-r.width() * 0.22,
                                                   -r.height() * 0.24),
                              r.width() * 0.95);
            g.setColorAt(0.0, QColor(0xff, 0xff, 0xff));
            g.setColorAt(0.45, QColor(0xc8, 0xc0, 0xb4));
            g.setColorAt(1.0, QColor(0x2c, 0x2e, 0x34));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawEllipse(r);
            // The highlight is what tells Rendered from Shaded at 22 pixels.
            p.setBrush(QColor(255, 255, 255, 205));
            p.drawEllipse(r.center() + QPointF(-r.width() * 0.20,
                                               -r.height() * 0.24),
                          r.width() * 0.10, r.height() * 0.10);
            break;
        }
    }
    p.end();
    return QIcon(pm);
}

QToolButton* barButton(QWidget* parent, const QIcon& icon, const QString& tip,
                       bool checkable)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon);
    b->setIconSize(QSize(kBall - 4, kBall - 4));
    b->setCheckable(checkable);
    b->setAutoRaise(true);
    b->setToolTip(tip);
    b->setFixedSize(kBall + 4, kBall + 2);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

}  // namespace

ViewportBar::ViewportBar(GLModelWidget* view, ViewportPanel* panel)
    : QWidget(view), m_view(view), m_panel(panel)
{
    setObjectName(QStringLiteral("fabViewBar"));
    setStyleSheet(QLatin1String(kBarQss));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::NoFocus);

    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(5, 3, 5, 3);
    h->setSpacing(2);

    // ── The four shading balls ──────────────────────────────────────────
    static const ShadingMode kModes[] = {ShadingMode::Wireframe,
                                         ShadingMode::Flat, ShadingMode::Shaded,
                                         ShadingMode::Rendered};
    for (const ShadingMode m : kModes) {
        auto* b = barButton(this, shadingBall(m, kBall),
                            QStringLiteral("%1 \u2014 %2")
                                .arg(QString::fromLatin1(shadingModeName(m)),
                                     QString::fromLatin1(shadingModeNote(m))),
                            true);
        connect(b, &QToolButton::clicked, this, [this, m] {
            if (m_view) m_view->setShadingMode(m);
        });
        h->addWidget(b);
        m_balls.append(b);
    }

    // ── The channel caret ───────────────────────────────────────────────
    m_channel = new QToolButton(this);
    m_channel->setText(QStringLiteral("⌄"));
    m_channel->setAutoRaise(true);
    m_channel->setCheckable(true);
    m_channel->setFixedSize(16, kBall + 2);
    m_channel->setFocusPolicy(Qt::NoFocus);
    m_channel->setStyleSheet(QStringLiteral("QToolButton{color:#d2d2d8;}"));
    m_channel->setToolTip(QStringLiteral(
        "Look at ONE material channel instead of the shaded result — base "
        "colour, normals, roughness, the reflection mask, ambient occlusion, "
        "translucency, metalness, UVs, or the lighting alone.\n\n"
        "Fox packs ambient occlusion, roughness and the reflection mask into "
        "the three channels of one SRM texture, and has no metalness map at "
        "all — its F0 comes from the material's FMTT preset. The channels are "
        "named for what this engine has.\n\n"
        "Scroll here to step through them; click for the list."));
    buildChannelMenu();
    connect(m_channel, &QToolButton::clicked, this, [this] {
        if (m_channelMenu)
            m_channelMenu->exec(mapToGlobal(
                QPoint(m_channel->x(), m_channel->y() + m_channel->height())));
        m_channel->setChecked(m_view && m_view->debugView() != DebugView::Off);
    });
    m_channel->installEventFilter(this);   // the wheel steps the channel
    h->addWidget(m_channel);

    if (!panel) {
        setLayout(h);
        if (m_view) m_view->installEventFilter(this);
        syncFromView();
        reposition();
        return;
    }

    // ── The popovers ────────────────────────────────────────────────────
    // Anchored to the button that opens them. Template §5 calls for Graphics,
    // Camera and Lighting; the overlay switches live under Graphics, because
    // "what is drawn on top of the model" is a graphics question and giving
    // them a fourth button would put the master gate one level further from
    // the thing it gates.
    struct Pop { const char* key; int glyph; const char* tip; };
    static const Pop kPops[] = {
        // Glyph 10, the half-split ball — NOT 0, which is the wire sphere and
        // is already the first shading ball two buttons to the left. Two
        // controls with the same picture on one strip is a strip you have to
        // read twice.
        {"graphics", 10,
         "Graphics — the overlays drawn over the model, and what the channel "
         "viewer is showing."},
        {"lighting", 12,
         "Lighting — the key light, the gains, the exposure and the "
         "environment the scene is lit in."},
        {"camera", 9,
         "Camera — projection, framing, turntable, and capture."},
        {"overlays", 4,
         "Overlays — everything drawn OVER the model, behind one master "
         "switch: statistics, the ground grid, the origin axes, the skeleton, "
         "bone names, connect points, the selection outline and the corner "
         "gizmo."},
    };
    for (const Pop& def : kPops) {
        auto* b = barButton(this, foxglyph::toolIcon(def.glyph,
                                                     QColor(0xd2, 0xd2, 0xd8)),
                            QString::fromLatin1(def.tip), false);
        const QString key = QString::fromLatin1(def.key);
        connect(b, &QToolButton::clicked, this, [this, key, b] {
            if (m_panel) m_panel->showPopover(key, b);
        });
        h->addWidget(b);
        m_popoverBtns.append(b);
    }

    // ── The master overlay gate ─────────────────────────────────────────
    // Beside the Overlays button, and only the SWITCH: the list it is the
    // master of is that button's page. It used to carry a caret with the
    // whole list under it, which made three copies of eight booleans — this
    // dropdown, the Graphics page, and the gate itself.
    m_overlayBtn = barButton(
        this, foxglyph::toolIcon(11, QColor(0xd2, 0xd2, 0xd8)),
        QStringLiteral(
            "Overlays on or off — one switch over all of them.\n\nTurning it "
            "off hides them and REMEMBERS which were on, so turning it back on "
            "restores exactly what you had. WHICH ones are on is the button to "
            "the left."),
        true);
    m_overlayBtn->setChecked(m_overlays.master);
    connect(m_overlayBtn, &QToolButton::toggled, this,
            [this](bool on) { setOverlaysMaster(on); });
    h->addWidget(m_overlayBtn);

    if (m_view) {
        m_view->installEventFilter(this);
        connect(m_view, &GLModelWidget::displayChanged, this,
                &ViewportBar::syncFromView);
        connect(m_view, &GLModelWidget::sceneChanged, this,
                &ViewportBar::syncFromView);
    }
    syncFromView();
    reapplyOverlays();
    reposition();
}

// Every overlay, from ONE table — the same one the Graphics popover reads, so
// the two lists cannot drift apart the way two hand-written copies would.
const QVector<ViewportBar::OverlayDef>& ViewportBar::overlayDefs()
{
    static const QVector<OverlayDef> kDefs = {
        {QStringLiteral("stats"), QStringLiteral("Statistics")},
        {QStringLiteral("grid"), QStringLiteral("Ground grid")},
        {QStringLiteral("axes"), QStringLiteral("Origin axes")},
        {QStringLiteral("skeleton"), QStringLiteral("Skeleton")},
        {QStringLiteral("bonenames"), QStringLiteral("Bone names")},
        {QStringLiteral("connectpoints"), QStringLiteral("Connect points")},
        {QStringLiteral("selection"), QStringLiteral("Selection outline")},
        {QStringLiteral("gizmo"), QStringLiteral("Axis gizmo (corner)")},
    };
    return kDefs;
}

bool ViewportBar::overlayState(const QString& key) const
{
    if (key == QLatin1String("stats")) return m_overlays.stats;
    if (key == QLatin1String("grid")) return m_overlays.grid;
    if (key == QLatin1String("axes")) return m_overlays.axes;
    if (key == QLatin1String("skeleton")) return m_overlays.skeleton;
    if (key == QLatin1String("bonenames")) return m_overlays.boneNames;
    if (key == QLatin1String("connectpoints")) return m_overlays.connectPoints;
    if (key == QLatin1String("selection")) return m_overlays.selection;
    if (key == QLatin1String("gizmo")) return m_overlays.gizmo;
    return false;
}

QString ViewportBar::openOverlayMenuForShot()
{
    // THE PAGE now, not a dropdown. The dropdown is gone; the harness flag
    // keeps its name because it is in recorded commands, and what it opens is
    // the thing that replaced what it used to open.
    if (!m_panel) return QStringLiteral("no panel");
    for (int i = 0; i < m_popoverBtns.size(); ++i) {
        if (i != 3) continue;   // the Overlays button, in kPops order
        m_panel->showPopover(QStringLiteral("overlays"), m_popoverBtns[i]);
        break;
    }
    QStringList out;
    out << QStringLiteral("Show overlays=%1").arg(m_overlays.master ? 1 : 0);
    for (const OverlayDef& d : overlayDefs())
        out << QStringLiteral("%1=%2").arg(d.label).arg(overlayState(d.key) ? 1 : 0);
    return out.join(QStringLiteral(" | "));
}

QString ViewportBar::scrollChannelForShot(int steps)
{
    if (!m_channel || !m_view) return QStringLiteral("no caret");
    const int dir = steps >= 0 ? 1 : -1;
    for (int i = 0; i < qAbs(steps); ++i) {
        // A real wheel event through the real filter — the clamp lives there,
        // and a harness that re-implemented the stepping would be testing
        // itself.
        QWheelEvent w(QPointF(0, 0), m_channel->mapToGlobal(QPoint(0, 0)),
                      QPoint(0, 0), QPoint(0, dir > 0 ? -120 : 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(m_channel, &w);
    }
    return QString::fromLatin1(debugViewName(m_view->debugView()));
}

void ViewportBar::buildChannelMenu()
{
    m_channelMenu = new QMenu(this);
    auto* group = new QActionGroup(m_channelMenu);
    group->setExclusive(true);
    for (const DebugView d : debugViews()) {
        QAction* a = m_channelMenu->addAction(
            QString::fromLatin1(debugViewName(d)));
        a->setCheckable(true);
        a->setData(int(d));
        a->setToolTip(QString::fromLatin1(debugViewNote(d)));
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, d] {
            if (m_view) m_view->setDebugView(d);
        });
    }
    m_channelMenu->setToolTipsVisible(true);
}

void ViewportBar::setOverlay(const QString& key, bool on)
{
    if (key == QLatin1String("stats")) m_overlays.stats = on;
    else if (key == QLatin1String("grid")) m_overlays.grid = on;
    else if (key == QLatin1String("axes")) m_overlays.axes = on;
    else if (key == QLatin1String("skeleton")) m_overlays.skeleton = on;
    else if (key == QLatin1String("bonenames")) m_overlays.boneNames = on;
    else if (key == QLatin1String("connectpoints")) m_overlays.connectPoints = on;
    else if (key == QLatin1String("gizmo")) m_overlays.gizmo = on;
    else if (key == QLatin1String("selection")) m_overlays.selection = on;
    else return;
    reapplyOverlays();
}

void ViewportBar::setOverlaysMaster(bool on)
{
    m_overlays.master = on;
    if (m_overlayBtn && m_overlayBtn->isChecked() != on) {
        QSignalBlocker block(m_overlayBtn);
        m_overlayBtn->setChecked(on);
    }
    reapplyOverlays();
}

void ViewportBar::reapplyOverlays()
{
    if (!m_view) return;
    const bool g = m_overlays.master;
    // The AND is the whole gate. Nothing else in the application may call
    // these six setters; everything writes into m_overlays and comes here.
    m_view->setShowStats(g && m_overlays.stats);
    m_view->setShowGrid(g && m_overlays.grid);
    m_view->setShowAxes(g && m_overlays.axes);
    m_view->setShowSkeleton(g && m_overlays.skeleton);
    m_view->setShowBoneNames(g && m_overlays.boneNames);
    m_view->setShowConnectPoints(g && m_overlays.connectPoints);
    m_view->setShowSelection(g && m_overlays.selection);
    // The gizmo is a WIDGET, not something the renderer draws, so the gate
    // reaches it by visibility rather than through a setShow* on the view.
    // Same rule though: nothing else may show or hide it.
    if (auto* giz = m_view->findChild<ViewportGizmo*>(
            QString(), Qt::FindDirectChildrenOnly))
        giz->setVisible(g && m_overlays.gizmo);
    emit overlaysChanged(m_overlays);
}

bool ViewportBar::openPopover(const QString& name)
{
    if (!m_panel) return false;
    static const char* const kKeys[] = {"graphics", "lighting", "camera"};
    for (int i = 0; i < 3 && i < m_popoverBtns.size(); ++i)
        if (name.compare(QLatin1String(kKeys[i]), Qt::CaseInsensitive) == 0)
            return m_panel->showPopover(QString::fromLatin1(kKeys[i]),
                                        m_popoverBtns[i]);
    return false;
}

bool ViewportBar::openChannelMenu()
{
    if (!m_channelMenu) return false;
    m_channelMenu->popup(mapToGlobal(QPoint(m_channel->x(),
                                            m_channel->y() + m_channel->height())));
    return true;
}

void ViewportBar::syncFromView()
{
    if (!m_view || m_syncing) return;
    m_syncing = true;
    const ShadingMode cur = m_view->shadingMode();
    for (int i = 0; i < m_balls.size(); ++i) {
        QSignalBlocker block(m_balls[i]);
        m_balls[i]->setChecked(int(cur) == i);
    }
    // Rendered is only honest when the scene was loaded with its maps. It stays
    // clickable — choosing it is how the owning tab is told to fetch them —
    // but the tooltip says what the viewport can currently do.
    if (m_balls.size() > int(ShadingMode::Rendered)) {
        const bool maps = m_view->hasPbrMaps();
        m_balls[int(ShadingMode::Rendered)]->setToolTip(
            QStringLiteral("%1 — %2%3")
                .arg(QString::fromLatin1(shadingModeName(ShadingMode::Rendered)),
                     QString::fromLatin1(shadingModeNote(ShadingMode::Rendered)),
                     maps ? QString()
                          : QStringLiteral(
                                "\n\nThis scene was loaded without its PBR "
                                "maps. Choosing this loads them once.")));
    }
    if (m_channel) {
        const DebugView d = m_view->debugView();
        QSignalBlocker block(m_channel);
        m_channel->setChecked(d != DebugView::Off);
        m_channel->setToolTip(
            d == DebugView::Off
                ? m_channel->toolTip()
                : QStringLiteral("Channel: %1 \u2014 %2")
                      .arg(QString::fromLatin1(debugViewName(d)),
                           QString::fromLatin1(debugViewNote(d))));
        if (m_channelMenu)
            for (QAction* a : m_channelMenu->actions())
                a->setChecked(a->data().toInt() == int(d));
    }
    m_syncing = false;
}

void ViewportBar::reposition()
{
    if (!m_view) return;
    adjustSize();
    // TOP-LEFT. It was top-right, mirroring Blender — but Blender's viewport
    // header is a full-width strip and this is a floating island, and on the
    // right it sat where the axis gizmo has to go and where a widening N-panel
    // column kept pushing it. Left, and the two things that were already there
    // move: the statistics overlay drops below it (ViewportHud) and the
    // exit-fullscreen button goes to the top centre.
    move(kMargin, kMargin);
    raise();
}

bool ViewportBar::eventFilter(QObject* o, QEvent* e)
{
    if (o == m_view && e->type() == QEvent::Resize) reposition();
    // Scrolling the caret steps through the channels, which is what the
    // template asks for and what the tooltip promises. Wrapping rather than
    // stopping at the ends: the list is a ring of ten and a person spinning a
    // wheel to compare two of them should not have to notice where the ends
    // are.
    if (o == m_channel && e->type() == QEvent::Wheel && m_view) {
        auto* w = static_cast<QWheelEvent*>(e);
        const QVector<DebugView> all = debugViews();
        if (all.isEmpty()) return true;
        int at = int(all.indexOf(m_view->debugView()));
        if (at < 0) at = 0;
        // CLAMPED, not wrapped. It wrapped, which made the list feel
        // infinite: scrolling past the last channel silently landed back on
        // "Off" at the top, so a slow scroll through nine channels could pass
        // the one you wanted twice without you seeing it stop anywhere.
        const int step = w->angleDelta().y() > 0 ? -1 : 1;
        const int want = qBound(0, at + step, int(all.size()) - 1);
        if (want == at) return true;   // already at an end: eat the wheel, do
                                       // nothing, and never zoom the camera
        at = want;
        m_view->setDebugView(all[at]);
        syncFromView();
        return true;   // never let it reach the viewport's zoom
    }
    return QWidget::eventFilter(o, e);
}

}  // namespace fox
