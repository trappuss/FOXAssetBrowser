#include "view/FilterChips.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QToolButton>
#include <QWidgetItem>

#include "util/SearchQuery.h"

namespace fox {

// ── ChipFlowLayout ──────────────────────────────────────────────────────────

ChipFlowLayout::ChipFlowLayout(QWidget* parent) : QLayout(parent)
{
    setContentsMargins(0, 0, 0, 0);
    setSpacing(4);
}

ChipFlowLayout::~ChipFlowLayout()
{
    while (QLayoutItem* it = takeAt(0)) delete it;
}

void ChipFlowLayout::addItem(QLayoutItem* item) { m_items.append(item); }
int ChipFlowLayout::count() const { return m_items.size(); }

QLayoutItem* ChipFlowLayout::itemAt(int i) const
{
    return (i >= 0 && i < m_items.size()) ? m_items[i] : nullptr;
}

QLayoutItem* ChipFlowLayout::takeAt(int i)
{
    return (i >= 0 && i < m_items.size()) ? m_items.takeAt(i) : nullptr;
}

int ChipFlowLayout::heightForWidth(int w) const
{
    return layout(QRect(0, 0, w, 0), false);
}

void ChipFlowLayout::setGeometry(const QRect& r)
{
    QLayout::setGeometry(r);
    layout(r, true);
}

QSize ChipFlowLayout::sizeHint() const { return minimumSize(); }

QSize ChipFlowLayout::minimumSize() const
{
    QSize s;
    for (QLayoutItem* it : m_items) s = s.expandedTo(it->minimumSize());
    const QMargins m = contentsMargins();
    return s + QSize(m.left() + m.right(), m.top() + m.bottom());
}

// The whole layout, run either to MEASURE (apply=false, returns the height) or
// to place. One function for both so a measured height and a placed row can
// never disagree — the classic flow-layout bug is two copies of this loop.
int ChipFlowLayout::layout(const QRect& r, bool apply) const
{
    const QMargins m = contentsMargins();
    const QRect eff = r.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    int x = eff.x();
    int y = eff.y();
    int lineH = 0;
    for (QLayoutItem* it : m_items) {
        const QSize hint = it->sizeHint();
        int next = x + hint.width();
        if (next > eff.right() + 1 && lineH > 0) {
            x = eff.x();
            y += lineH + spacing();
            next = x + hint.width();
            lineH = 0;
        }
        if (apply) it->setGeometry(QRect(QPoint(x, y), hint));
        x = next + spacing();
        lineH = qMax(lineH, hint.height());
    }
    return y + lineH - r.y() + m.bottom();
}

// ── FilterChips ─────────────────────────────────────────────────────────────

FilterChips::FilterChips(QWidget* parent) : QWidget(parent)
{
    m_flow = new ChipFlowLayout(this);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    hide();
}

void FilterChips::clearChips()
{
    while (QLayoutItem* it = m_flow->takeAt(0)) {
        if (QWidget* w = it->widget()) w->deleteLater();
        delete it;
    }
    m_count = 0;
}

void FilterChips::addChip(const QString& text, const QString& term,
                          const QString& tip, const QColor& accent)
{
    auto* chip = new QWidget(this);
    auto* h = new QHBoxLayout(chip);
    h->setContentsMargins(6, 1, 2, 1);
    h->setSpacing(2);
    chip->setStyleSheet(
        // 3px, not 8. A pill-shaped chip is a different visual language from
        // everything else in this window — every other control here is a
        // square-cornered Qt widget, and eight rounded pills under a
        // square-cornered search box read as pasted in from another program.
        QStringLiteral("background:%1;border:1px solid %2;border-radius:3px;")
            .arg(accent.name(QColor::HexArgb), accent.darker(160).name()));
    auto* label = new QLabel(text, chip);
    label->setStyleSheet(QStringLiteral("border:none;background:transparent;"));
    label->setToolTip(tip);
    h->addWidget(label);
    auto* x = new QToolButton(chip);
    x->setText(QStringLiteral("✕"));
    x->setAutoRaise(true);
    x->setFixedSize(14, 14);
    x->setFocusPolicy(Qt::NoFocus);
    x->setCursor(Qt::PointingHandCursor);
    x->setToolTip(QStringLiteral("Remove this filter"));
    x->setStyleSheet(QStringLiteral(
        "QToolButton{border:none;background:transparent;color:#666;}"
        "QToolButton:hover{color:#000;}"));
    connect(x, &QToolButton::clicked, this,
            [this, term] { Q_EMIT removeRequested(term); });
    h->addWidget(x);
    m_flow->addWidget(chip);
    ++m_count;
}

void FilterChips::setQuery(const QString& text,
                           const std::function<QString(const QString&)>& labelFor)
{
    clearChips();
    const searchq::Query q(text);

    // Tags first — they are the ones the funnel sets, and the ones a user is
    // most likely to want gone. Colour carries the SENSE: a required filter is
    // neutral, an exclusion is red, because "-#mgo3" reading like "#mgo3" at a
    // glance is how someone ends up staring at an empty list.
    for (const QString& t : q.mustTags())
        addChip(labelFor ? labelFor(t) : t, QStringLiteral("#") + t,
                QStringLiteral("Tag \"%1\" is required. Click ✕ to drop it.")
                    .arg(t),
                QColor(0x4a, 0x8f, 0xe8, 0x38));
    for (const QString& t : q.mustNotTags())
        addChip(QStringLiteral("−") + (labelFor ? labelFor(t) : t),
                QStringLiteral("-#") + t,
                QStringLiteral("Tag \"%1\" is EXCLUDED. Click ✕ to drop it.")
                    .arg(t),
                QColor(0xe8, 0x53, 0x53, 0x38));
    // …then the words. They are in the box in front of the user, so they are
    // shown for completeness and to give them a ✕ too, not because they are
    // hard to see.
    for (const QString& t : q.must())
        addChip(t, t,
                QStringLiteral("The word \"%1\" must appear. Click ✕ to drop "
                               "it.").arg(t),
                QColor(0x88, 0x88, 0x90, 0x30));
    for (const QString& t : q.mustNot())
        addChip(QStringLiteral("−") + t, QStringLiteral("-") + t,
                QStringLiteral("Rows containing \"%1\" are excluded. Click ✕ "
                               "to drop it.").arg(t),
                QColor(0xe8, 0x53, 0x53, 0x30));

    if (m_count > 1) {
        auto* clear = new QToolButton(this);
        clear->setText(QStringLiteral("Clear all"));
        clear->setAutoRaise(true);
        clear->setFocusPolicy(Qt::NoFocus);
        clear->setCursor(Qt::PointingHandCursor);
        clear->setToolTip(QStringLiteral("Empty the search box."));
        connect(clear, &QToolButton::clicked, this,
                [this] { Q_EMIT clearRequested(); });
        m_flow->addWidget(clear);
        // NOT counted: it is not a filter, and the funnel's tint reads count().
    }

    setVisible(m_count > 0);
    updateGeometry();
}

}  // namespace fox
