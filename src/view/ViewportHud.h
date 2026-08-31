// ViewportHud.h — the TEXT half of the viewport overlays (template §5).
//
// Statistics, bone names and connect-point names. They are text, and text is
// the one overlay the line shader cannot draw. Mixing QPainter into paintGL()
// is possible and is a trap: QPainter saves and restores its own idea of the
// GL state, and every draw after it in that function then runs against
// something other than what it was written for.
//
// So this is a transparent child widget sitting on top of the viewport,
// painting with QPainter into the widget hierarchy where QPainter belongs. It
// is transparent for mouse events, so the camera still owns every pixel of the
// viewport, and it asks the viewport for already-projected screen positions
// rather than duplicating the camera maths.
//
// It also carries the two things §5 asks for that are TEXT over the viewport:
// the F1 shortcut list, and the floating "Exit fullscreen" button — which is a
// real button, and therefore the one part of this widget that does take mouse
// events, because §5 is explicit that fullscreen must never be something you
// can only get out of with a key you were never told about.
#pragma once
#include <QPointer>
#include <QWidget>

class QPainter;
class QPushButton;

class GLModelWidget;

namespace fox {

class ViewportHud : public QWidget {
    Q_OBJECT
public:
    // Parents itself to `view`, fills it, and follows its resizes. Starts
    // visible but paints nothing until an overlay is switched on.
    explicit ViewportHud(GLModelWidget* view);

protected:
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void drawHelp(QPainter& p);
    void layoutExitButton();
    // Where overlay text may start, given the bar above it.
    int barTop() const;

    QPointer<GLModelWidget> m_view;
    QPushButton* m_exit = nullptr;
};

}  // namespace fox
