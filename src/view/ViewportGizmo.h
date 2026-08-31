// ViewportGizmo.h — Blender's axis gizmo, as an overlay widget.
//
// Six balls on three lines from a centre: +X +Y +Z filled and lettered,
// −X −Y −Z hollow. Hover one and it lights up and shows its letter; click it
// and the camera snaps to look down that axis; double-click the CENTRE and the
// projection toggles between perspective and orthographic. That is exactly the
// control the user asked for, and it is the one control in a 3D viewport that
// everyone already knows how to use.
//
// A QWidget, not geometry drawn inside the GL scene. Two reasons, and both are
// about correctness rather than convenience:
//
//  1. HIT TESTING. A gizmo drawn in the scene has to be picked out of the
//     scene, which means either a second colour-id pass reserved for it or
//     un-projecting the cursor — and the existing pick pass answers "which
//     submesh", so the gizmo would have had to fight it for the same click.
//     A widget gets the mouse events directly and the viewport never sees them.
//  2. IT MUST NOT BE IN A CAPTURE. The gizmo is a control, not part of the
//     picture. Drawn in the scene it would land in every screenshot, every
//     turntable GIF and every exported still, and would have to be switched
//     off around each of them — a switch that is one forgotten call away from
//     shipping a gizmo in someone's render.
//
// It is a SIBLING of the shading balls on the viewport's own furniture, so it
// positions itself under the ViewportBar rather than at the very top-right.
#pragma once
#include <QPointer>
#include <QRect>
#include <QVector3D>
#include <QWidget>

class GLModelWidget;

namespace fox {

class ViewportGizmo : public QWidget {
    Q_OBJECT
public:
    // Parents itself to `view` and follows its resizes. Starts visible.
    explicit ViewportGizmo(GLModelWidget* view);

    // Where the top edge of the gizmo sits, measured from the viewport's top.
    // The bar above it is a different height on different platforms, so the
    // owner says rather than this guessing.
    void setTopMargin(int px);

    // Dev harness: what is under a point in THIS widget's coordinates —
    // 0..5 for an axis (+X +Y +Z −X −Y −Z), -1 the RING (which is the
    // projection toggle), -2 nothing.
    int hitTest(const QPoint& at) const;
    // Dev harness: the projection button, exactly as a click on it.
    bool toggleProjection();
    // Dev harness: a point ON THE RING and not on any ball, so a hit test can
    // be checked against the same geometry the paint uses. Straight up from
    // the centre by most of the radius: +Y sits at the top only when the
    // camera is level, and the harness must not depend on where it is.
    QPoint ringPointForShot() const;
    // Dev harness: click an axis by index, exactly as the mouse would.
    bool activate(int axis);
    // Dev harness: which axis is currently lit.
    int hovered() const { return m_hover; }
    // Dev harness: where one ball is, in this widget's coordinates. Exposed so
    // a harness hover lands on the SAME point the paint draws — a hover test
    // that guessed the position would be testing its own guess.
    QPoint ballPos(int axis) const { return ballAt(axis).toPoint(); }

protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    // Where each ball sits in widget coordinates, for the CURRENT camera.
    // Recomputed on every paint and every hit test from the same function, so
    // what you see and what you can click cannot drift apart.
    // Where an axis points in the camera's frame: x/y are screen directions
    // (y already flipped), z is DEPTH, positive toward the viewer. The depth
    // is what sorts the balls and what scales them.
    QVector3D axisDir(int axis) const;
    QPointF dialCentre() const;
    QPointF ballAt(int axis) const;
    qreal ballScale(int axis) const;
    // The dial's outer radius — the ring, and the projection toggle's target.
    static qreal dialRadius();
    void reposition();

    QPointer<GLModelWidget> m_view;
    int m_hover = -2;
    int m_top = 4;
};

}  // namespace fox
