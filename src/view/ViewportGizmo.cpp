#include "view/ViewportGizmo.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <algorithm>
#include <cmath>

#include "gl/GLModelWidget.h"

namespace fox {

namespace {

// Blender's proportions, which is the whole point of the second pass at this:
// the first drew thin arms and small flat balls and read as a diagram of a
// gizmo rather than as one. In Blender the balls are LARGE relative to the
// arms (roughly a quarter of the radius), the arms are thick and stop at the
// ball rather than running under it, and the six of them are depth-sorted and
// depth-SCALED so the thing reads as a sphere you are looking into.
constexpr int kDialW = 76;         // square: the dial is all there is
constexpr qreal kArm = 25.0;       // centre → ball, at full size
constexpr qreal kBall = 9.0;

// Blender's own axis colours (its default dark theme), rather than the
// approximations the first pass used. These are the three every DCC tool has
// agreed on and getting them slightly wrong is exactly what made the gizmo
// look like a copy.
QColor axisColor(int axis)
{
    switch (axis % 3) {
        case 0: return QColor(0xFF, 0x33, 0x52);   // X
        case 1: return QColor(0x8B, 0xDC, 0x00);   // Y
        default: return QColor(0x28, 0x90, 0xFF);  // Z
    }
}

const char* axisLetter(int axis)
{
    switch (axis % 3) {
        case 0: return "X";
        case 1: return "Y";
        default: return "Z";
    }
}

}  // namespace

ViewportGizmo::ViewportGizmo(GLModelWidget* view) : QWidget(view), m_view(view)
{
    setFixedSize(kDialW, kDialW);
    setMouseTracking(true);
    setAttribute(Qt::WA_NoMousePropagation);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral(
        "Axis gizmo\n\n"
        "Hover a ball to see its axis, click it to look down that axis.\n\n"
        "Click the RING — anywhere inside the circle that is not a ball — to "
        "switch between perspective and orthographic. The ring fills in while "
        "orthographic is on."));
    if (view) {
        view->installEventFilter(this);
        // The balls swap places as the camera turns — the whole point of the
        // thing is that it shows you which way you are facing.
        connect(view, &GLModelWidget::cameraChanged, this,
                qOverload<>(&QWidget::update));
    }
    reposition();
}

void ViewportGizmo::setTopMargin(int px)
{
    m_top = px;
    reposition();
}

void ViewportGizmo::reposition()
{
    if (!m_view) return;
    move(m_view->width() - width() - 6, m_top);
    raise();
}

bool ViewportGizmo::eventFilter(QObject* o, QEvent* e)
{
    if (o == m_view && (e->type() == QEvent::Resize || e->type() == QEvent::Show))
        reposition();
    return QWidget::eventFilter(o, e);
}

// Where one axis points, in the camera's frame: x and y are screen directions
// (y already flipped, because screen y grows downward), z is DEPTH — positive
// toward the viewer.
//
// The camera's yaw and pitch ARE its rotation, so the world axes are projected
// by hand rather than pulled out of a view matrix: the same two angles the
// orbit handler writes, read back the same way, which keeps the gizmo honest
// when the camera is moved from anywhere else (the "." frame key, a reset, a
// snap from this very widget).
QVector3D ViewportGizmo::axisDir(int axis) const
{
    if (!m_view) return {};
    const float yaw = qDegreesToRadians(m_view->cameraYaw());
    const float pitch = qDegreesToRadians(m_view->cameraPitch());
    QVector3D v;
    switch (axis % 3) {
        case 0: v = QVector3D(1, 0, 0); break;
        case 1: v = QVector3D(0, 1, 0); break;
        default: v = QVector3D(0, 0, 1); break;
    }
    if (axis >= 3) v = -v;
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float x1 = v.x() * cy - v.z() * sy;
    const float z1 = v.x() * sy + v.z() * cy;
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float y2 = v.y() * cp - z1 * sp;
    const float z2 = v.y() * sp + z1 * cp;   // depth, +ve toward the viewer
    return QVector3D(x1, -y2, z2);
}

QPointF ViewportGizmo::dialCentre() const
{
    return QPointF(kDialW / 2.0, kDialW / 2.0);
}

QPointF ViewportGizmo::ballAt(int axis) const
{
    const QVector3D d = axisDir(axis);
    return dialCentre() + QPointF(d.x() * kArm, d.y() * kArm);
}

// How big and how solid a ball is, from its depth. Blender shrinks and fades
// the far half, and it is most of what makes the six of them read as a sphere
// rather than as a flat asterisk.
qreal ViewportGizmo::ballScale(int axis) const
{
    const qreal z = axisDir(axis).z();     // -1 (away) … +1 (toward)
    return 0.68 + 0.32 * ((z + 1.0) / 2.0);
}

qreal ViewportGizmo::dialRadius() { return kArm + kBall + 2; }

int ViewportGizmo::hitTest(const QPoint& at) const
{
    const QPointF p(at);
    // The BALLS first, then the centre. An axis pointing straight at the
    // camera projects ONTO the centre, and testing the centre first would
    // swallow its click — which is exactly what happened after snapping to an
    // axis: the centre was covered by that axis's own ball, so double-clicking
    // it snapped again instead of changing the projection. The button above is
    // the reliable path; this ordering keeps the balls clickable.
    int best = -2;
    qreal bestD = kBall + 2.0;
    for (int i = 0; i < 6; ++i) {
        const qreal d = QLineF(p, ballAt(i)).length();
        if (d <= bestD) { bestD = d; best = i; }
    }
    if (best >= 0) return best;
    // -1 is THE RING: anywhere inside the dial that is not a ball. That whole
    // area is the projection toggle, which is how D4 does it and what replaced
    // the small button that used to sit under the gizmo.
    //
    // It also fixes what made that button necessary: an axis pointing straight
    // at the camera projects onto the CENTRE, so a centre-only target was
    // covered by that axis's own ball the moment you snapped to a view. The
    // ring is never covered — the balls sit ON it, and everything between them
    // is still ring.
    return QLineF(p, dialCentre()).length() <= dialRadius() ? -1 : -2;
}

void ViewportGizmo::mouseMoveEvent(QMouseEvent* e)
{
    const int h = hitTest(e->pos());
    if (h == m_hover) return;
    m_hover = h;
    update();
}

void ViewportGizmo::leaveEvent(QEvent*)
{
    if (m_hover == -2) return;
    m_hover = -2;
    update();
}

void ViewportGizmo::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) { e->ignore(); return; }
    const int h = hitTest(e->pos());
    if (h >= 0) activate(h);
    else if (h == -1 && m_view) m_view->toggleOrthographic();
    e->accept();   // never let a click through to the orbit handler
}

bool ViewportGizmo::activate(int axis)
{
    if (!m_view || axis < 0 || axis > 5) return false;
    m_view->viewAlongAxis(axis % 3, axis >= 3);
    return true;
}

QPoint ViewportGizmo::ringPointForShot() const
{
    // Walk out from the centre until the hit test says RING rather than a
    // ball, in whichever direction is clear. Asking the real hit test rather
    // than assuming a spot keeps this honest when the camera moves.
    for (int deg = 0; deg < 360; deg += 15) {
        const qreal a = qDegreesToRadians(qreal(deg));
        const QPoint p = (dialCentre()
                          + QPointF(std::cos(a), std::sin(a)) * (dialRadius() - 3.0))
                             .toPoint();
        if (hitTest(p) == -1) return p;
    }
    return dialCentre().toPoint();
}

bool ViewportGizmo::toggleProjection()
{
    if (!m_view) return false;
    m_view->toggleOrthographic();
    return true;
}

// SWALLOWED, and deliberately does nothing. A single click on the ring already
// toggles the projection, so letting a double-click through would toggle it
// twice — back to where it started, which reads as the control being broken.
// Accepting the event also stops it reaching the viewport's own double-click,
// which would otherwise try to pick a submesh under the gizmo.
void ViewportGizmo::mouseDoubleClickEvent(QMouseEvent* e) { e->accept(); }

void ViewportGizmo::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF ctr = dialCentre();

    // THE RING. Only while the pointer is on the gizmo, which is Blender's
    // behaviour — at rest this is six coloured dots over the render and
    // nothing else, and a permanent disc is a hole punched in the picture.
    //
    // It is also the projection TOGGLE (see hitTest): a white circle that
    // appears when you hover, brightens when you point at it, and FILLS IN
    // while orthographic is on. The fill is the state indicator — there was a
    // word, "ORTHO", printed under the gizmo, and a word floating over the
    // render says it in a way nothing else in this viewport does.
    if (m_hover != -2) {
        const bool ortho = m_view && m_view->orthographic();
        const bool onRing = (m_hover == -1);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, onRing ? 105 : 70));
        p.drawEllipse(ctr, dialRadius(), dialRadius());
        if (ortho) {
            // Filled: a faint white wash inside the ring, so "orthographic" is
            // legible at a glance without reading anything.
            p.setBrush(QColor(255, 255, 255, onRing ? 46 : 30));
            p.drawEllipse(ctr, dialRadius() - 1.0, dialRadius() - 1.0);
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, onRing ? 235 : 120),
                      onRing ? 2.0 : 1.4));
        p.drawEllipse(ctr, dialRadius() - 0.8, dialRadius() - 0.8);
    }

    // BACK TO FRONT, by real depth. The first pass sorted by screen radius,
    // which cannot tell an axis pointing away from one pointing towards you —
    // they land at the same radius — so the gizmo read as inside out at half
    // the angles.
    int order[6] = {0, 1, 2, 3, 4, 5};
    std::sort(std::begin(order), std::end(order), [this](int a, int b) {
        return axisDir(a).z() < axisDir(b).z();
    });

    QFont f = font();
    f.setPixelSize(9);
    f.setBold(true);
    p.setFont(f);

    for (int k = 0; k < 6; ++k) {
        const int i = order[k];
        const QPointF at = ballAt(i);
        const bool positive = i < 3;
        const bool lit = (m_hover == i);
        const qreal s = ballScale(i);
        const qreal r = kBall * s + (lit ? 1.5 : 0.0);
        QColor c = axisColor(i);
        // The far half fades rather than changing hue. Alpha, not a darker
        // colour, because it has to sit over a render of any brightness.
        c.setAlphaF(qBound(0.45, 0.45 + 0.55 * s, 1.0));

        // THE ARM, positive axes only, and stopping at the ball rather than
        // running under it — a line drawn to the centre of a translucent ball
        // shows through it as a stripe. Six arms would be a star nobody can
        // read; three arms and six balls is the shape everyone recognises.
        if (positive) {
            const QPointF dir = at - ctr;
            const qreal len = std::hypot(dir.x(), dir.y());
            const QPointF stop =
                len > r ? ctr + dir * ((len - r + 1.0) / len) : ctr;
            QPen pen(c, lit ? 3.0 : 2.2);
            pen.setCapStyle(Qt::RoundCap);
            p.setPen(pen);
            p.drawLine(ctr, stop);
        }

        if (positive || lit) {
            // FILLED, with a dark letter — Blender's positive ball.
            p.setPen(lit ? QPen(QColor(0xFF, 0xFF, 0xFF), 1.4) : Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(at, r, r);
            p.setPen(QColor(0x18, 0x18, 0x1C));
            QString label = QLatin1String(axisLetter(i));
            if (!positive) label = QStringLiteral("-") + label;
            p.drawText(QRectF(at.x() - 10, at.y() - 7, 20, 14),
                       Qt::AlignCenter, label);
        } else {
            // HOLLOW — Blender's negative ball: a ring in the axis colour with
            // the render showing through, and no letter until you hover it.
            QColor fill = c;
            fill.setAlphaF(0.20);
            p.setPen(QPen(c, 1.8));
            p.setBrush(fill);
            p.drawEllipse(at, r - 0.9, r - 0.9);
        }
    }

}

}  // namespace fox
