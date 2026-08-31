// PanelBox.h — one panel in the N-panel's stack (template §6).
//
// TITLE ……… [▲][▼][✕] over its content. Panels are toggled from the vertical
// icon strip and live in a vertical QSplitter: several stack at once, drag the
// handles to size them, ▲▼ to reorder, ✕ to hide. There is no fold arrow and
// no pin — showing and hiding IS the toggle, and a second collapse mechanism
// on top of that is two ways to make a panel disappear with different
// aftermaths. Plain QWidget (no Q_OBJECT → no moc, header-only): the owning
// column connects to the public buttons directly.
//
// Ported from D4AssetBrowser's tabs/PanelBox.h, whose sizing contract is
// repeated here because it is the whole reason panels used to break when
// dragged:
//   · the panel is Expanding with a small floor, so a drag can shrink it to a
//     header plus a sliver but never erase it;
//   · GREEDY content (anything vertically Expanding — tables, tree lists)
//     fills the panel and scrolls past it;
//   · everything else (label grids) rides in a scroll area under a trailing
//     stretch, so a tall panel shows honest blank at the bottom and a short
//     one grows a scrollbar.
// What must NEVER happen is a fixed or maximum height on the content:
// QBoxLayout, handed spare room it cannot give to anything, springs the
// leftover out as gaps BETWEEN the items — which is exactly the 250px hole
// that opened between D4's PARTS title and its table.
#pragma once
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QScrollArea>
#include <QAbstractScrollArea>
#include <QSizePolicy>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

namespace fox {

constexpr int kPanelHeadH = 20;

// The title style. One string, so a panel header cannot drift from the
// popovers' titles by a hand-typed colour.
constexpr const char* kPanelHdrQss =
    "color:#c9c9d2;font-weight:600;font-size:10px;letter-spacing:0.6px;";

// How tall a panel's content would LIKE to be the first time it is opened,
// published as a dynamic property on the content widget and read by
// PanelBox::preferredHeight. A hint, never a constraint.
constexpr const char* kPanelWantH = "foxPanelWantH";

class PanelBox : public QWidget {
public:
    PanelBox(const QString& title, QWidget* content, QWidget* parent)
        : QWidget(parent), body(content)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(1);
        auto* head = new QWidget(this);
        head->setFixedHeight(kPanelHeadH);
        head->setStyleSheet(QStringLiteral("background:#2b2b2b;border-radius:3px;"));
        auto* h = new QHBoxLayout(head);
        h->setContentsMargins(5, 1, 3, 1);
        h->setSpacing(2);
        // The TITLE label is handed back to the owner, which keeps writing live
        // counts into it ("PARTS · 3 of 5 shown") — the header hosts the same
        // label the panes always used.
        label = new QLabel(title, head);
        label->setStyleSheet(QLatin1String(kPanelHdrQss));
        h->addWidget(label, 1);
        auto mk = [&](const QString& glyph, const QString& tip) {
            auto* b = new QToolButton(head);
            b->setText(glyph);
            b->setToolTip(tip);
            b->setAutoRaise(true);
            b->setFixedSize(16, 18);
            b->setFocusPolicy(Qt::NoFocus);
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(QStringLiteral(
                "QToolButton{border:none;background:transparent;color:#8a8a8a;}"
                "QToolButton:hover{color:#e0e0e0;}"));
            h->addWidget(b);
            return b;
        };
        up    = mk(QStringLiteral("▲"), QStringLiteral("Move this panel up"));
        down  = mk(QStringLiteral("▼"), QStringLiteral("Move this panel down"));
        close = mk(QStringLiteral("✕"), QStringLiteral("Hide this panel"));
        v->addWidget(head);
        content->setParent(this);
        // ── WHAT "GREEDY" MEANS, AND WHY IT IS NOT JUST THE POLICY ──────
        // A panel is greedy if it can usefully use extra height — that is,
        // if it scrolls or holds a list. Reading only the declared size
        // policy got MATERIALS wrong: MaterialInspector owns its own
        // QScrollArea and never declared Expanding, so it went down the
        // NON-greedy path and was wrapped in a SECOND scroll area. A scroll
        // area inside a scroll area sizes its child to the child's minimum
        // and then never moves it — measured, with --npanelsizes: dragging
        // the MATERIALS panel from 125px to 176px left its body at 136px
        // both times, which is the user's "I can only see half a slot and it
        // doesn't adjust".
        //
        // So: the declared policy, OR the presence of anything that scrolls
        // or lists. Structural, so a panel added later cannot get this wrong
        // by forgetting a line in its constructor.
        greedy = (content->sizePolicy().verticalPolicy() & QSizePolicy::ExpandFlag) != 0
                 || content->findChild<QAbstractScrollArea*>() != nullptr;
        if (greedy) {
            v->addWidget(content, 1);   // fills the panel, scrolls itself past it
            // …and it must be ABLE to take the height it is given. A widget
            // whose declared policy has no ExpandFlag sits at its sizeHint
            // inside a stretch, which is the same stuck body by another route.
            content->setSizePolicy(content->sizePolicy().horizontalPolicy(),
                                   QSizePolicy::Expanding);
            // …and it must not impose its OWN floor on the column. A
            // QTreeWidget's minimumSizeHint is tall, and a stack of four
            // panels each insisting on it leaves the splitter no slack — which
            // is what "the sidebar panels are not resizable" actually was.
            // The panel's floor is the one below, and it is the only one.
            content->setMinimumHeight(0);
        } else {
            // Label-grid content: hug the top, scroll when squeezed. Without
            // the stretch the form's rows spring apart to eat the spare height.
            auto* sc = new QScrollArea(this);
            sc->setWidgetResizable(true);
            sc->setFrameShape(QFrame::NoFrame);
            sc->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            auto* holder = new QWidget(sc);
            auto* hv = new QVBoxLayout(holder);
            hv->setContentsMargins(0, 0, 0, 0);
            hv->setSpacing(0);
            hv->addWidget(content);
            hv->addStretch(1);
            sc->setWidget(holder);
            v->addWidget(sc, 1);
        }
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        setMinimumHeight(kPanelHeadH + 38);   // a drag squeezes, never erases
    }

    // The height this panel asks for when it first comes up. Tables and lists
    // publish their row count through the kPanelWantH property; everything else
    // falls back to its natural hint. Clamped at both ends: one fat panel must
    // not swallow the column, a thin one must not be a slit.
    int preferredHeight() const
    {
        int c = body ? body->property(kPanelWantH).toInt() : 0;
        if (c <= 0 && body) c = body->sizeHint().height();
        // The upper clamp was 400, and a rich-text INFO label asks for more
        // than that — so opening four panels gave INFO 402px and left the
        // other three sitting on their floors. 260 is still a useful panel and
        // leaves the column something to share.
        return kPanelHeadH + qBound(80, c, 260);
    }

    QToolButton* up    = nullptr;
    QToolButton* down  = nullptr;
    QToolButton* close = nullptr;
    QLabel*      label = nullptr;
    QWidget*     body  = nullptr;   // the registered content, NOT the wrapper
    bool         greedy = false;
    QString      key;               // the stable settings id
};

// The floor the splitter will actually enforce for a pane — whichever is
// larger, what we asked for or what its layout insists on.
// THE FLOOR IS THE ONE THE PANEL DECLARES, not the one its contents would
// like. It was max(minimumHeight, minimumSizeHint), and once the greedy branch
// started adding list content directly the hint climbed to 137-215px per
// panel — four open panels would have claimed 586px of column before anything
// was in them, and panelBoxArrive would have seen no slack anywhere to give a
// newcomer. Every greedy panel scrolls internally, so squeezing one below what
// its contents "want" costs a scrollbar, not a clipped widget.
inline int panelBoxFloor(QWidget* w)
{
    return w->minimumHeight();
}

// A panel that has just come up should ARRIVE at a height that fits what is in
// it, rather than the equal split QSplitter hands out blind — opening
// ANIMATIONS next to INFO should not cut the clip list to half a column, and
// opening INFO should not hand a dozen label rows 500px of nothing. The
// newcomer's preferred height is taken from the panels already up, in
// proportion to how much SLACK each has above its floor, so nothing is pushed
// below its minimum.
inline void panelBoxArrive(QSplitter* stack, PanelBox* box)
{
    if (!stack || !box) return;
    const int me = stack->indexOf(box);
    if (me < 0) return;
    QList<int> sizes = stack->sizes();
    QVector<int> others;
    int slack = 0;
    for (int i = 0; i < stack->count(); ++i) {
        if (i == me || stack->widget(i)->isHidden()) continue;
        const int s = qMax(0, sizes.value(i) - panelBoxFloor(stack->widget(i)));
        if (s > 0) { others << i; slack += s; }
    }
    if (others.isEmpty()) return;   // only panel up, or the rest are already at
                                    // their floor — the splitter's own
                                    // distribution is the best available
    const int want = qMin(box->preferredHeight() - sizes.value(me), slack);
    if (want <= 0) return;          // it already has at least what it asked for
    int left = want;
    for (int k = 0; k < others.size() && left > 0; ++k) {
        const int i = others[k];
        const int mine = qMax(0, sizes[i] - panelBoxFloor(stack->widget(i)));
        const int give = (k == others.size() - 1)
                             ? left
                             : qMin(left, qRound(double(want) * mine / slack));
        sizes[i] -= give;
        left     -= give;
    }
    sizes[me] += want - left;
    stack->setSizes(sizes);
}

}  // namespace fox
