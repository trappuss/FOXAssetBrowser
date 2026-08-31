#include "view/NPanel.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QCoreApplication>

#include "util/PanelPersist.h"
#include "view/PanelBox.h"
#include "view/ViewGlyphs.h"

namespace fox {

namespace {
constexpr int kStripW = 26;
constexpr int kArrowW = 13;
constexpr int kArrowH = 46;
}  // namespace

NPanel::NPanel(const QString& prefix, QWidget* parent)
    : QWidget(parent), m_prefix(prefix)
{
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(2);

    // ── The icon strip goes FIRST, on the side nearest the viewport ─────
    // It was on the far side, on the argument that the collapse arrow and the
    // strip should not be two columns of small controls pressed together. In
    // practice that put the switches for these panels as far from the viewport
    // as it is possible to be, and every one of them is about the model you
    // are looking at. Beside the arrow they read as one cluster — the way
    // Blender's tab strip sits against its own region edge.
    m_strip = new QWidget(this);
    m_strip->setFixedWidth(kStripW);
    m_stripLayout = new QVBoxLayout(m_strip);
    m_stripLayout->setContentsMargins(2, 2, 2, 2);
    m_stripLayout->setSpacing(2);
    m_stripLayout->addStretch(1);
    row->addWidget(m_strip);

    m_stack = new QSplitter(Qt::Vertical, this);
    m_stack->setChildrenCollapsible(false);
    // ── THE HANDLES HAVE TO LOOK LIKE HANDLES ───────────────────────────
    // This was 4px and unstyled, which on a dark column is a hairline nobody
    // finds — the user reported the panels as not resizable at all, and an
    // affordance that cannot be seen is the same thing as one that is not
    // there. 8px with a painted grip and a resize cursor, which is what every
    // splitter the user has ever dragged looks like.
    m_stack->setHandleWidth(8);
    m_stack->setStyleSheet(QStringLiteral(
        "QSplitter::handle:vertical {"
        "  background: palette(mid);"
        "  border-top: 1px solid palette(dark);"
        "  border-bottom: 1px solid palette(dark);"
        "  image: none;"
        "}"
        "QSplitter::handle:vertical:hover { background: palette(highlight); }"));
    // The floor lives on the STACK, not on the column: the column shrinks to
    // its icon strip when nothing is open, and a minimum width on the column
    // would fight the maximum that does the shrinking.
    m_stack->setMinimumWidth(240);
    row->addWidget(m_stack, 1);

    // AFTER the panels are added the owner calls restoreState; the splitter's
    // own sizes are bound here because binding is idempotent and this is the
    // one place that knows the key.
    PanelPersist::bind(m_stack, m_prefix + QStringLiteral("/stack"));
}

void NPanel::addPanel(const QString& key, const QString& title, int glyph,
                      QWidget* content, const QString& tip)
{
    if (!content || find(key)) return;
    auto* box = new PanelBox(title, content, m_stack);
    box->key = key;
    box->hide();
    m_stack->addWidget(box);

    auto* b = new QToolButton(m_strip);
    b->setIcon(foxglyph::toolIcon(glyph));
    b->setIconSize(QSize(foxglyph::kSize, foxglyph::kSize));
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setFixedSize(kStripW - 4, kStripW - 4);
    b->setToolTip(tip.isEmpty() ? title
                                : (title + QStringLiteral("\n\n") + tip));
    b->setAccessibleName(title);
    // insertWidget, not addWidget: the trailing stretch must stay last or the
    // buttons centre themselves in the column and drift as panels are added.
    m_stripLayout->insertWidget(m_stripLayout->count() - 1, b);

    m_entries.append(Entry{key, box, b});

    connect(b, &QToolButton::toggled, this,
            [this, key](bool on) { setPanelOpen(key, on); });
    connect(box->close, &QToolButton::clicked, this,
            [this, key] { setPanelOpen(key, false); });
    connect(box->up, &QToolButton::clicked, this,
            [this, key] { movePanel(key, -1); });
    connect(box->down, &QToolButton::clicked, this,
            [this, key] { movePanel(key, 1); });
}

NPanel::Entry* NPanel::find(const QString& key)
{
    for (Entry& e : m_entries)
        if (e.key == key) return &e;
    return nullptr;
}

const NPanel::Entry* NPanel::find(const QString& key) const
{
    for (const Entry& e : m_entries)
        if (e.key == key) return &e;
    return nullptr;
}

QStringList NPanel::panelKeys() const
{
    QStringList out;
    for (const Entry& e : m_entries) out << e.key;
    return out;
}

// isHidden(), not isVisible()/isVisibleTo(): those answer "is it on screen",
// which is false for every panel while the COLUMN is collapsed — and the
// column collapses precisely when nothing is open, so reading them made
// "open a panel" a no-op that could never undo itself. isHidden() is the
// widget's own explicit state and is exactly what a panel toggle means.
bool NPanel::isPanelOpen(const QString& key) const
{
    const Entry* e = find(key);
    return e && e->box && !e->box->isHidden();
}

QLabel* NPanel::titleLabel(const QString& key) const
{
    const Entry* e = find(key);
    return e && e->box ? e->box->label : nullptr;
}

QWidget* NPanel::panelContent(const QString& key) const
{
    const Entry* e = find(key);
    return e && e->box ? e->box->body : nullptr;
}

void NPanel::setPanelOpen(const QString& key, bool on)
{
    Entry* e = find(key);
    if (!e || !e->box) return;
    if (!e->box->isHidden() == on && e->strip->isChecked() == on) {
        // Already there. Still sync the strip: a panel can be opened from the
        // owning tab as well, and a button left unlit over an open panel is the
        // kind of small lie that makes a UI feel broken.
        return;
    }
    const bool wasHidden = e->box->isHidden();
    e->box->setVisible(on);
    if (e->strip->isChecked() != on) {
        QSignalBlocker blocker(e->strip);
        e->strip->setChecked(on);
    }
    // Opening a panel while the column is collapsed is a request for the
    // column: the alternative is a lit strip button over nothing.
    if (on && !m_columnOpen) setColumnOpen(true);
    if (!m_restoring) saveOpen();
    updateColumnWidth();
    // The SIGNAL FIRST, then the sizing. The owner fills a panel when it
    // opens, and panelBoxArrive reads the content's asked-for height — so
    // arriving before the owner has had its turn sizes every panel from an
    // empty one. Measured: ATTACHMENTS, whose empty state asks for 100px, came
    // up 280px tall and squeezed INFO to a slit.
    Q_EMIT panelOpenChanged(key, on);
    if (on && wasHidden) panelBoxArrive(m_stack, e->box);
}

void NPanel::movePanel(const QString& key, int delta)
{
    Entry* e = find(key);
    if (!e || !e->box) return;
    const int at = m_stack->indexOf(e->box);
    if (at < 0) return;
    // Step over HIDDEN neighbours rather than swapping with them: ▲ next to a
    // closed panel otherwise looked like a dead button, because the visible
    // order did not change.
    int to = at;
    for (int i = at + delta; i >= 0 && i < m_stack->count(); i += delta) {
        if (!m_stack->widget(i)->isHidden()) { to = i; break; }
    }
    if (to == at) return;
    const QList<int> sizes = m_stack->sizes();
    m_stack->insertWidget(to, e->box);
    m_stack->setSizes(sizes);   // insertWidget re-parents and drops the sizes
    saveOrder();
}

void NPanel::saveOpen() const
{
    QStringList open;
    for (const Entry& e : m_entries)
        if (e.box && !e.box->isHidden()) open << e.key;
    QSettings().setValue(m_prefix + QStringLiteral("/open"), open.join(QLatin1Char(',')));
}

void NPanel::saveOrder() const
{
    QStringList order;
    // PanelBox is deliberately not a QObject subclass with Q_OBJECT (no moc,
    // header-only), so the widget at index i is matched against the registry
    // by pointer rather than cast down to.
    for (int i = 0; i < m_stack->count(); ++i) {
        const QWidget* w = m_stack->widget(i);
        for (const Entry& e : m_entries)
            if (e.box == w) { order << e.key; break; }
    }
    QSettings().setValue(m_prefix + QStringLiteral("/order"), order.join(QLatin1Char(',')));
}

QString NPanel::probeSizesForShot()
{
    if (!m_stack) return QStringLiteral("no stack");
    const auto describe = [this] {
        QStringList out;
        for (int i = 0; i < m_stack->count(); ++i) {
            QWidget* w = m_stack->widget(i);
            if (w->isHidden()) continue;
            // The BOX height and the CONTENT's height, because "the panel
            // resizes but does not adjust" is a claim about the second one
            // following the first, and only one of those two numbers was
            // being reported.
            // static_cast: PanelBox has no Q_OBJECT (it is a plain QFrame
            // subclass in a header), and every widget in this stack IS one.
            const auto* box = static_cast<const PanelBox*>(w);
            const QWidget* body = box ? box->body : nullptr;
            out << QStringLiteral("%1:%2px(body %3, floor %4)")
                       .arg(box && box->label ? box->label->text()
                                              : QStringLiteral("?"))
                       .arg(w->height())
                       .arg(body ? body->height() : -1)
                       .arg(panelBoxFloor(w));
        }
        return out.join(QStringLiteral(" · "));
    };
    const QString before = describe();
    // Move the boundary the way a drag would, then read it back. If the sizes
    // do not change, the panels are not draggable however good the handle
    // looks — the floors have eaten all the slack.
    QList<int> sizes = m_stack->sizes();
    int first = -1, second = -1;
    for (int i = 0; i < m_stack->count(); ++i) {
        if (m_stack->widget(i)->isHidden()) continue;
        if (first < 0) first = i;
        else if (second < 0) { second = i; break; }
    }
    if (first < 0 || second < 0)
        return QStringLiteral("%1 — only one panel open, nothing to drag against")
            .arg(before);
    const int move = qMax(24, sizes[first] / 3);
    sizes[first] -= move;
    sizes[second] += move;
    m_stack->setSizes(sizes);
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents();
    const QString after = describe();
    return QStringLiteral("handle %1px · before %2 · asked to move %3px · after "
                          "%4 · %5")
        .arg(m_stack->handleWidth())
        .arg(before)
        .arg(move)
        .arg(after,
             before == after ? QStringLiteral("DID NOT MOVE")
                             : QStringLiteral("moved"));
}

void NPanel::restoreState(const QStringList& fallback)
{
    m_restoring = true;
    QSettings s;

    // ── Order ──────────────────────────────────────────────────────────────
    // Stored keys first, in their stored order; then anything registered that
    // the store has never heard of, in registration order. A build that adds a
    // panel appends it rather than scrambling what the user arranged.
    const QStringList stored =
        s.value(m_prefix + QStringLiteral("/order")).toString()
            .split(QLatin1Char(','), Qt::SkipEmptyParts);
    QStringList order;
    for (const QString& k : stored)
        if (find(k) && !order.contains(k)) order << k;
    for (const Entry& e : m_entries)
        if (!order.contains(e.key)) order << e.key;
    for (int i = 0; i < order.size(); ++i) {
        Entry* e = find(order[i]);
        if (e && e->box && m_stack->indexOf(e->box) != i) m_stack->insertWidget(i, e->box);
    }

    // ── Open set ───────────────────────────────────────────────────────────
    const QString openKey = m_prefix + QStringLiteral("/open");
    QStringList open;
    if (s.contains(openKey)) {
        // An EMPTY stored value is a real state — the user closed everything —
        // so it is distinguished from "never saved" by contains(), not by
        // whether the string is empty.
        open = s.value(openKey).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    } else {
        open = fallback;
    }
    for (const Entry& e : m_entries) setPanelOpen(e.key, open.contains(e.key));

    m_columnOpen = s.value(m_prefix + QStringLiteral("/column"), true).toBool();
    updateColumnWidth();
    syncArrow();

    m_restoring = false;
}

// A column with nothing open, or one the user collapsed, is an icon strip —
// and it should take an icon strip's WIDTH. Without this the splitter went on
// holding 315px for a 26px strip: 289px of nothing beside the viewport, and
// the log read "0 open, 315 px wide", which is how it was noticed.
void NPanel::updateColumnWidth()
{
    bool anyOpen = false;
    for (const Entry& e : m_entries)
        if (e.box && !e.box->isHidden()) { anyOpen = true; break; }
    const bool strip = !m_columnOpen || !anyOpen;
    // The stack is hidden either way, so the strip is all there is to show.
    m_stack->setVisible(m_columnOpen && anyOpen);
    setMaximumWidth(strip ? kStripW + 2 : QWIDGETSIZE_MAX);
}

void NPanel::setColumnOpen(bool on)
{
    if (m_columnOpen == on) return;
    m_columnOpen = on;
    updateColumnWidth();
    QSettings().setValue(m_prefix + QStringLiteral("/column"), on);
    syncArrow();
    Q_EMIT columnOpenChanged(on);
}

void NPanel::attachToggle(QWidget* viewport)
{
    if (!viewport) return;
    m_viewport = viewport;
    auto* b = new QToolButton(viewport);
    m_arrow = b;
    b->setFixedSize(kArrowW, kArrowH);
    b->setFocusPolicy(Qt::NoFocus);
    b->setCursor(Qt::PointingHandCursor);
    // The grey background is not decoration: this button floats over rendered
    // geometry, and a transparent glyph on a light model is invisible.
    b->setStyleSheet(QStringLiteral(
        "QToolButton{border:none;border-top-left-radius:4px;"
        "border-bottom-left-radius:4px;background:rgba(58,58,62,0.86);"
        "color:#d0d0d6;font-size:11px;}"
        "QToolButton:hover{background:rgba(84,84,90,0.94);color:#ffffff;}"));
    connect(b, &QToolButton::clicked, this, [this] { toggleColumn(); });
    viewport->installEventFilter(this);
    syncArrow();
    repositionArrow();
    b->show();
    b->raise();
}

void NPanel::setToggleVisible(bool on)
{
    if (m_arrow) m_arrow->setVisible(on);
}

void NPanel::syncArrow()
{
    if (!m_arrow) return;
    m_arrow->setText(m_columnOpen ? QStringLiteral("▶") : QStringLiteral("◀"));
    m_arrow->setToolTip(m_columnOpen
        ? QStringLiteral("Hide the panel column\n\nThe icon strip stays: every "
                         "panel is still one click away.")
        : QStringLiteral("Show the panel column\n\nParts, materials, "
                         "animations, attachments and info for this model."));
}

void NPanel::repositionArrow()
{
    if (!m_arrow || !m_viewport) return;
    m_arrow->move(m_viewport->width() - kArrowW,
                  (m_viewport->height() - kArrowH) / 2);
    m_arrow->raise();
}

bool NPanel::eventFilter(QObject* o, QEvent* e)
{
    if (o == m_viewport && (e->type() == QEvent::Resize || e->type() == QEvent::Show))
        repositionArrow();
    return QWidget::eventFilter(o, e);
}

}  // namespace fox
