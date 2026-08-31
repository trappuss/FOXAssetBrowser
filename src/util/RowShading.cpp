// RowShading.cpp — see RowShading.h.
#include "util/RowShading.h"

#include <QAbstractItemView>
#include <QPalette>
#include <QVariant>
#include <QWidget>

namespace fox {
namespace rowshade {
namespace {

// How far the band sits from the row beside it, in 0..255 lightness. Chosen by
// looking at it rather than by arithmetic: 8 is invisible on a dark theme at a
// normal screen brightness, 24 reads as two alternating colours rather than as
// one list with a band.
constexpr int kShift = 16;

// The property a view sets on itself to say "not me". Read in polish(), so an
// opt-out survives a re-polish (a style change re-polishes every widget).
const char* const kOptOut = "foxabNoRowShading";

}  // namespace

QColor alternateFor(const QColor& base)
{
    if (!base.isValid()) return base;
    int h = 0, s = 0, l = 0, a = 0;
    base.getHsl(&h, &s, &l, &a);
    // Away from whichever end Base is nearer, so there is always room for the
    // full shift: a pure-black Base lightens, a pure-white one darkens, and
    // nothing clamps back onto the colour it started from.
    const int want = l > 127 ? l - kShift : l + kShift;
    QColor out = QColor::fromHsl(h, s, qBound(0, want, 255), a);
    // Belt and braces. Rounding through HSL can land back on the input for a
    // saturated colour at the very top or bottom of the range, and a shading
    // helper whose output equals its input is the exact failure this file is
    // here to prevent — so say so rather than shipping an invisible band.
    if (out == base)
        out = l > 127 ? base.darker(112) : base.lighter(128);
    return out;
}

void setEnabled(QAbstractItemView* view, bool on)
{
    if (!view) return;
    view->setProperty(kOptOut, !on);
    view->setAlternatingRowColors(on);
    if (!on) return;
    // BOTH roles, from one derivation. Writing only AlternateBase would leave
    // the pair at the mercy of whatever Base the theme hands the viewport,
    // which is how "the switch is on and nothing looks different" happens.
    QPalette p = view->palette();
    const QColor base = p.color(QPalette::Base);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alternateFor(base));
    // The disabled group too: a view that is greyed out while a background
    // index builds otherwise loses its banding and gets it back, which reads
    // as the list having been rebuilt.
    const QColor dbase = p.color(QPalette::Disabled, QPalette::Base);
    p.setColor(QPalette::Disabled, QPalette::AlternateBase, alternateFor(dbase));
    view->setPalette(p);
    // The viewport carries its own palette on some styles.
    if (QWidget* vp = view->viewport()) vp->setPalette(p);
}

void polish(QWidget* w)
{
    auto* view = qobject_cast<QAbstractItemView*>(w);
    if (!view) return;
    if (view->property(kOptOut).toBool()) return;
    setEnabled(view, true);
}

}  // namespace rowshade
}  // namespace fox
