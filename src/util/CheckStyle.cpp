// CheckStyle.cpp — see CheckStyle.h.
#include "util/CheckStyle.h"

#include <QPainter>
#include <QStyleOption>
#include <QWidget>

#include "util/RowShading.h"

namespace fox {
namespace {

// The mark is inset from the box so the strokes do not touch the border, and
// the box is inset from the slot Qt hands us so two adjacent rows do not run
// their borders together.
constexpr qreal kBoxInset = 1.0;
constexpr qreal kMarkInset = 0.28;   // fraction of the box side

// One square, centred in whatever rectangle the layout gave the indicator.
// Qt's indicator rect is not always square (item views pad it), and an X drawn
// into a non-square rect leans.
//
// `inset` is spent only where there is room for it: a menu hands over an 8x16
// column, and taking a pixel off each side of that leaves a 6px mark next to a
// 16px row — the base style draws about eleven. Never returns an empty rect
// either: an indicator that paints nothing is still clickable, which is worse
// than a small one.
QRectF squareIn(const QRect& r, qreal inset)
{
    const qreal raw = qMin(r.width(), r.height());
    const qreal side = qMax(raw - 2 * inset, qMin(raw, qreal(3.0)));
    if (side <= 0) return {};
    return QRectF(r.x() + (r.width() - side) / 2.0,
                  r.y() + (r.height() - side) / 2.0, side, side);
}

}  // namespace

XCheckStyle::XCheckStyle(QStyle* base) : QProxyStyle(base) {}

void XCheckStyle::drawPrimitive(PrimitiveElement pe, const QStyleOption* opt,
                                QPainter* p, const QWidget* w) const
{
    const bool boxed =
        pe == PE_IndicatorCheckBox || pe == PE_IndicatorItemViewItemCheck;
    // A checkable menu item draws its mark with no box around it — that is the
    // established look for a menu and a box there would read as a second,
    // nested control.
    const bool bare = pe == PE_IndicatorMenuCheckMark;
    if (!boxed && !bare) {
        QProxyStyle::drawPrimitive(pe, opt, p, w);
        return;
    }

    const QRectF box = squareIn(opt->rect, bare ? 0.0 : kBoxInset);
    if (box.isEmpty()) return;

    const bool enabled = opt->state & State_Enabled;
    // A MENU mark is asked for only when the item is already checked — the base
    // styles call this primitive from inside their "if checked" branch, and
    // reuse State_On to mean HIGHLIGHTED. So the widget states cannot be read
    // the same way in both places: taking State_On as "checked" here drew the
    // mark only while the row was hovered and left every other checked item
    // looking unchecked.
    const bool on = bare || (opt->state & State_On);
    const bool tri = !bare && (opt->state & State_NoChange);
    const QPalette& pal = opt->palette;
    const QPalette::ColorGroup grp =
        enabled ? QPalette::Active : QPalette::Disabled;

    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);

    if (boxed) {
        // Hover and press feedback, because painting the indicator ourselves
        // means the base style no longer gets to provide any. Only for a real
        // check BOX: an item view forwards the whole row's hover state into its
        // check column, and the stock styles strip it before drawing — without
        // that, pointing anywhere on a row lit up its box.
        const bool hover = (opt->state & State_MouseOver)
            && pe == PE_IndicatorCheckBox;
        QColor fill = pal.color(grp, QPalette::Base);
        if (enabled && (opt->state & State_Sunken))
            fill = pal.color(grp, QPalette::Midlight);
        else if (enabled && hover)
            fill = fill.lighter(112);
        QColor edge = pal.color(grp, QPalette::Mid);
        if (enabled && hover) edge = pal.color(grp, QPalette::Highlight);
        // Half-pixel offset so a 1px border lands on the pixel rather than
        // straddling two and coming out grey.
        const QRectF r = box.adjusted(0.5, 0.5, -0.5, -0.5);
        p->setPen(QPen(edge, 1.0));
        p->setBrush(fill);
        p->drawRoundedRect(r, 2.0, 2.0);
    }

    if (!on && !tri) { p->restore(); return; }

    // The mark itself. The boxed form paints its own Base fill first, so Text
    // is the role that contrasts with it — including on a selected row, where
    // the box sits over the highlight. A menu mark has no fill of its own and
    // sits directly on the row, so it has to follow the row: HighlightedText
    // where the row is highlighted, WindowText otherwise.
    const QPalette::ColorRole inkRole = boxed
        ? QPalette::Text
        : ((opt->state & State_Selected) ? QPalette::HighlightedText
                                         : QPalette::WindowText);
    const QColor ink = pal.color(grp, inkRole);
    const qreal weight = qMax(1.3, box.width() / 7.0);
    p->setPen(QPen(ink, weight, Qt::SolidLine, Qt::RoundCap));

    const qreal in = box.width() * kMarkInset;
    const QRectF m = box.adjusted(in, in, -in, -in);
    if (tri) {
        // Partially checked: a bar, not an X. A dimmed X would read as
        // "disabled" rather than "some of these are on".
        p->drawLine(QPointF(m.left(), m.center().y()),
                    QPointF(m.right(), m.center().y()));
    } else {
        p->drawLine(m.topLeft(), m.bottomRight());
        p->drawLine(m.topRight(), m.bottomLeft());
    }
    p->restore();
}

void XCheckStyle::polish(QWidget* w)
{
    QProxyStyle::polish(w);
    rowshade::polish(w);
}

}  // namespace fox
