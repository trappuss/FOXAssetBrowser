// CheckStyle.h — every check indicator in the application, drawn as an X.
//
// The check boxes themselves are ordinary Qt widgets and always were; what
// changes here is only how their indicator is PAINTED. A style is the right
// place for that: it reaches QCheckBox, the check column of a QTreeWidget or
// QListWidget, a checkable QGroupBox and a checkable menu item alike, without
// any of those knowing about it, and without touching the toggle logic that
// makes them work.
//
// Why an X rather than a tick: the platform tick is a glyph out of the theme
// font on several styles, so it renders at whatever weight and shape that font
// happens to have — including, on some systems, as a colour emoji. Two straight
// strokes drawn by hand cannot do that. They also carry the same meaning at any
// size and in any theme, which a hinted glyph does not.
//
// Three states, all painted here rather than deferred to the base style:
//   checked            a box with an X through it
//   partially checked  a box with a horizontal bar (a tri-state parent whose
//                      children disagree — the submesh tree uses these)
//   unchecked          an empty box
#pragma once
#include <QProxyStyle>

class QWidget;

namespace fox {

class XCheckStyle : public QProxyStyle {
    Q_OBJECT
public:
    explicit XCheckStyle(QStyle* base = nullptr);

    void drawPrimitive(PrimitiveElement pe, const QStyleOption* opt,
                       QPainter* p, const QWidget* w) const override;

    // Every widget the application creates passes through here once, which is
    // the one place from which a rule can reach a view that no constructor of
    // ours ever sees — a combo popup, a completer's list, a file dialog's
    // tree. Alternating row shading is applied from here for exactly that
    // reason; see util/RowShading.h.
    void polish(QWidget* w) override;
};

}  // namespace fox
