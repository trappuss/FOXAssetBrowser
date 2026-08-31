// RowShading.h — alternating row shading, for every list-style view at once.
//
// The tool has list views, tree views and combo popups, and a long list of
// same-shaped rows is hard to read without a band under every second one.
// `setAlternatingRowColors(true)` on its own is NOT enough and that is the
// whole reason this file exists: Qt then paints the odd rows with the
// palette's AlternateBase role, and on several platform styles — the Fusion
// style this application runs under included, and every dark theme built by
// deriving one colour — AlternateBase is either equal to Base or a shade away
// from it that no one can see. The switch is on, the code is "correct", and
// the list looks exactly as it did before.
//
// So both roles are written here from ONE derivation: the alternate is Base
// shifted in lightness by a fixed, visible amount, away from whichever end of
// the range Base sits at, so it is guaranteed to differ under a light theme,
// a dark theme and a black one alike.
//
// WHERE IT IS APPLIED. Not at each view's construction — that is a list of
// call sites to keep in step, and the one that gets forgotten is a combo popup
// created by Qt three layers down where there is no constructor to edit. It is
// applied from `XCheckStyle::polish()`, which the style calls for every widget
// the application ever creates, so a view added next year is shaded without
// anyone remembering this file exists. `setEnabled(view, false)` opts one out;
// exactly one thing does (the Models tab's thumbnail GRID, where the rows are
// tiles and the delegate paints their backgrounds itself — it uses
// `alternateFor()` directly to band its own visual rows instead).
#pragma once
#include <QColor>

class QAbstractItemView;
class QWidget;

namespace fox {
namespace rowshade {

// The alternate colour for a given Base. Deterministic, and never equal to
// its input: on a light Base it darkens, on a dark one it lightens, and at
// either extreme it moves the only way there is room to move.
QColor alternateFor(const QColor& base);

// Turn shading on (or off) for one view, writing the palette pair as well as
// the switch. Safe to call more than once on the same view.
void setEnabled(QAbstractItemView* view, bool on);

// The style hook. Applies the default (on) to `w` when it is an item view
// that has not opted out. Called from XCheckStyle::polish().
void polish(QWidget* w);

}  // namespace rowshade
}  // namespace fox
