// TagFilterPopup.cpp — see TagFilterPopup.h.
#include "util/TagFilterPopup.h"

#include <QApplication>
#include <QCheckBox>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

#include "index/ModelTags.h"
#include "util/SearchQuery.h"

namespace fox {

namespace {
// Three columns and a wider panel: the Family list alone runs to dozens of
// entries on a real install, and two narrow columns meant scrolling past
// everything to reach Status. The names are short (the longest measured is
// "common_source"), so three fit comfortably at this width.
constexpr int kColumns = 3;
constexpr int kPopupWidth = 660;
// Both are clamped to the screen at show time — see showFor().
constexpr int kPopupMaxHeight = 760;
constexpr int kPopupMinHeight = 320;

// Qt's own tri-state cycle is off → partial → on, which would put "exclude"
// between "don't care" and "include" and make the common case take two clicks.
// This one goes off → include → exclude → off: the order people actually want
// them in, and the same order the syntax reads (#tag, then -#tag).
class TriTagBox : public QCheckBox {
public:
    using QCheckBox::QCheckBox;

protected:
    void nextCheckState() override
    {
        switch (checkState()) {
            case Qt::Unchecked: setCheckState(Qt::Checked); break;
            case Qt::Checked: setCheckState(Qt::PartiallyChecked); break;
            default: setCheckState(Qt::Unchecked); break;
        }
    }
};

searchq::Query::TagState stateOfBox(Qt::CheckState s)
{
    if (s == Qt::Checked) return searchq::Query::TagState::Include;
    if (s == Qt::PartiallyChecked) return searchq::Query::TagState::Exclude;
    return searchq::Query::TagState::Off;
}

Qt::CheckState boxForState(searchq::Query::TagState s)
{
    if (s == searchq::Query::TagState::Include) return Qt::Checked;
    if (s == searchq::Query::TagState::Exclude) return Qt::PartiallyChecked;
    return Qt::Unchecked;
}
}  // namespace

TagFilterPopup::TagFilterPopup(QWidget* parent) : QFrame(parent)
{
    setWindowFlags(Qt::Popup);
    setFrameShape(QFrame::StyledPanel);
    // The real width is set in showFor(), clamped to the screen.

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(8, 8, 8, 6);
    v->setSpacing(6);

    {
        auto* top = new QHBoxLayout();
        top->setContentsMargins(0, 0, 0, 0);
        top->setSpacing(6);
        m_find = new QLineEdit(this);
        m_find->setPlaceholderText(QStringLiteral("Find a tag…  #tag  +tag  -tag"));
        m_find->setClearButtonEnabled(true);
        m_find->setToolTip(QStringLiteral(
            "Narrow the tags shown below, and set them from the keyboard.\n\n"
            "  text     show tags whose name or label contains this\n"
            "  #tag     the same — the # the search box uses is accepted here\n"
            "  +tag     REQUIRE this tag (the same as ticking its box)\n"
            "  -tag     EXCLUDE it (the same as ticking its box twice)\n\n"
            "A + or - term is applied as you finish typing the tag's name and "
            "the box below moves with it, so the two are always the same "
            "filter — this popup holds no state of its own."));
        top->addWidget(m_find, 1);
        auto* clear = new QPushButton(QStringLiteral("Clear"), this);
        clear->setToolTip(QStringLiteral(
            "Remove every tag from the search box, leaving any typed words."));
        connect(clear, &QPushButton::clicked, this,
                [this] { Q_EMIT clearRequested(); });
        top->addWidget(clear);
        v->addLayout(top);
    }

    auto* scroll = new QScrollArea(this);
    m_scroll = scroll;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_body = new QWidget(scroll);
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(0, 0, 0, 0);
    m_bodyLayout->setSpacing(2);
    scroll->setWidget(m_body);
    v->addWidget(scroll, 1);

    m_result = new QLabel(this);
    QFont small = m_result->font();
    // A font built from a PIXEL size reports pointSizeF() as -1, and handing
    // that back with a subtraction makes Qt warn and ignore the whole font.
    if (small.pointSizeF() > 2.0) small.setPointSizeF(small.pointSizeF() - 1.0);
    m_result->setFont(small);
    m_result->setEnabled(false);
    v->addWidget(m_result);

    connect(m_find, &QLineEdit::textChanged, this,
            [this](const QString& s) { applyFilterText(s); });

    rebuild();
}

void TagFilterPopup::rebuild()
{
    for (const Group& g : m_groups) {
        delete g.header;
        delete g.grid;
    }
    m_groups.clear();
    if (m_tail) {
        m_bodyLayout->removeItem(m_tail);
        delete m_tail;
        m_tail = nullptr;
    }

    const ModelTags& tags = ModelTags::instance();
    // The rows keep the application's own font size. Density here comes from
    // tight row spacing and three columns, NOT from shrinking the text — a
    // filter you have to squint at is not a dense filter, it is an unreadable
    // one. Only the counts are dimmed, and only the header is emphasised.
    QFont headFont = font();
    headFont.setBold(true);
    const QFont rowFont = font();

    for (const TagCategory& cat : tags.categories()) {
        Group g;

        auto* head = new QWidget(m_body);
        auto* hl = new QHBoxLayout(head);
        hl->setContentsMargins(0, 6, 0, 1);
        hl->setSpacing(6);
        auto* title = new QLabel(cat.label.toUpper(), head);
        title->setFont(headFont);
        title->setToolTip(cat.hint);
        hl->addWidget(title);
        auto* n = new QLabel(QString::number(cat.tags.size()), head);
        n->setFont(rowFont);
        n->setEnabled(false);
        hl->addWidget(n);
        hl->addStretch(1);
        m_bodyLayout->addWidget(head);
        g.header = head;
        g.count = n;

        auto* grid = new QWidget(m_body);
        auto* gl = new QGridLayout(grid);
        gl->setContentsMargins(0, 0, 0, 0);
        gl->setHorizontalSpacing(10);
        gl->setVerticalSpacing(1);
        int r = 0, c = 0;
        for (const TagInfo& t : cat.tags) {
            auto* cell = new QWidget(grid);
            auto* cl = new QHBoxLayout(cell);
            cl->setContentsMargins(0, 0, 0, 0);
            cl->setSpacing(4);
            auto* box = new TriTagBox(t.label, cell);
            box->setTristate(true);
            box->setFont(rowFont);
            const QString cycle = QStringLiteral(
                "Click once to require it (#%1), again to exclude it (-#%1), "
                "again to clear.").arg(t.tag);
            box->setToolTip(t.hint.isEmpty()
                                ? cycle
                                : QStringLiteral("%1\n\n%2").arg(t.hint, cycle));
            cl->addWidget(box, 1);
            auto* cnt = new QLabel(QString::number(t.count), cell);
            cnt->setFont(rowFont);
            cnt->setEnabled(false);
            cnt->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            cl->addWidget(cnt);
            gl->addWidget(cell, r, c);

            const QString tag = t.tag;
            // clicked(), not stateChanged/checkStateChanged: the first is
            // deprecated in newer Qt and the second does not exist in older Qt,
            // and this build has to compile against both. nextCheckState() has
            // already run by the time clicked() fires, so the box carries the
            // state the user just chose, and programmatic changes never reach
            // here at all — which is the guard we want anyway.
            connect(box, &QAbstractButton::clicked, this, [this, tag, box] {
                if (m_settingChecks) return;
                Q_EMIT tagStateChanged(tag, stateOfBox(box->checkState()));
            });

            Row row;
            row.cell = cell;
            row.box = box;
            row.tag = t.tag;
            row.label = t.label;
            g.rows.append(row);
            if (++c >= kColumns) { c = 0; ++r; }
        }
        for (int i = 0; i < kColumns; ++i) gl->setColumnStretch(i, 1);
        m_bodyLayout->addWidget(grid);
        g.grid = grid;
        m_groups.append(g);
    }
    m_tail = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
    m_bodyLayout->addSpacerItem(m_tail);
}

// The find field speaks the SAME three prefixes as the search box, because a
// popup with a text field that ignored them was a text field people typed
// "#chara" into and watched do nothing. '#' is stripped and treated as a
// find; '+' and '-' SET the tag, through the same signal a click on the box
// emits — this popup owns no filter state and this is not the place to start.
void TagFilterPopup::applyFilterText(const QString& needle)
{
    QString n = needle.trimmed();
    if (n.size() > 1
        && (n.startsWith(QLatin1Char('+')) || n.startsWith(QLatin1Char('-')))) {
        const bool exclude = n.startsWith(QLatin1Char('-'));
        QString tag = n.mid(1);
        if (tag.startsWith(QLatin1Char('#'))) tag.remove(0, 1);
        // Only on an EXACT tag name. Applying a filter halfway through typing
        // one would set "ch", then "cha", then "chara" — three rewrites of the
        // search box for one word, two of which match nothing.
        for (const Group& g : m_groups) {
            for (const Row& row : g.rows) {
                if (row.tag.compare(tag, Qt::CaseInsensitive) != 0) continue;
                Q_EMIT tagStateChanged(row.tag,
                                       exclude
                                           ? searchq::Query::TagState::Exclude
                                           : searchq::Query::TagState::Include);
                return;
            }
        }
        n = tag;   // not a tag we know: fall through and treat it as a find
    }
    if (n.startsWith(QLatin1Char('#'))) n.remove(0, 1);
    for (Group& g : m_groups) {
        int shown = 0;
        for (Row& row : g.rows) {
            const bool hit =
                n.isEmpty() || row.label.contains(n, Qt::CaseInsensitive)
                || row.tag.contains(n, Qt::CaseInsensitive)
                // A tag that is DOING something stays visible even when it does
                // not match, or narrowing the list would hide what is currently
                // in force — including an exclusion, which is the easiest kind
                // of filter to forget you left on.
                || row.box->checkState() != Qt::Unchecked;
            row.cell->setVisible(hit);
            if (hit) ++shown;
        }
        // An empty category is a header over nothing. The header's number is
        // what is VISIBLE, not the category's total, or a search leaves
        // "FAMILY 87" sitting above two rows.
        if (g.count) g.count->setText(QString::number(shown));
        if (g.header) g.header->setVisible(shown > 0);
        if (g.grid) g.grid->setVisible(shown > 0);
    }
}

qint64 TagFilterPopup::msSinceClosed() const
{
    return m_closed.isValid() ? m_closed.elapsed() : 1 << 30;
}

void TagFilterPopup::hideEvent(QHideEvent* e)
{
    m_closed.restart();
    QFrame::hideEvent(e);
}

void TagFilterPopup::showFor(QWidget* anchor, const QString& text)
{
    // A find string left over from last time looks exactly like tags having
    // gone missing, so each opening starts from the whole vocabulary.
    {
        QSignalBlocker b(m_find);
        m_find->clear();
    }
    const searchq::Query q(text);
    m_settingChecks = true;
    for (Group& g : m_groups)
        for (Row& row : g.rows)
            row.box->setCheckState(boxForState(q.stateOf(row.tag)));
    m_settingChecks = false;
    applyFilterText(m_find->text());

    // Size from the CONTENT, not from sizeHint(). A QScrollArea reports a
    // small fixed default sizeHint that says nothing about what is inside it,
    // so asking the popup for its own hint collapsed the whole panel to about
    // a hundred pixels — one visible row and a clipped header. Giving the
    // scroll area a real minimum height is what lets the layout grow.
    QPoint anchorTopLeft =
        anchor ? anchor->mapToGlobal(QPoint(0, 0)) : QCursor::pos();
    QScreen* sc = QGuiApplication::screenAt(anchorTopLeft);
    if (!sc) sc = QGuiApplication::primaryScreen();
    const QRect avail = sc ? sc->availableGeometry() : QRect(0, 0, 1280, 800);

    setFixedWidth(qMin(kPopupWidth, avail.width() - 24));
    m_body->adjustSize();
    const int chrome = layout()->contentsMargins().top()
        + layout()->contentsMargins().bottom()
        + m_find->sizeHint().height() + m_result->sizeHint().height()
        + 2 * layout()->spacing();
    const int maxBody = qMax(120, qMin(kPopupMaxHeight, int(avail.height() * 0.75)) - chrome);
    const int wantBody = qBound(qMin(kPopupMinHeight, maxBody),
                                m_body->sizeHint().height(), maxBody);
    m_scroll->setMinimumHeight(wantBody);
    m_scroll->setMaximumHeight(maxBody);
    adjustSize();

    QPoint p = anchor ? anchor->mapToGlobal(QPoint(0, anchor->height() + 2))
                      : QCursor::pos();
    if (p.x() + width() > avail.right()) p.setX(avail.right() - width());
    if (p.x() < avail.left()) p.setX(avail.left());
    if (p.y() + height() > avail.bottom() && anchor) {
        const int above = anchorTopLeft.y() - height() - 2;
        // Flip above the button only if there is actually room up there;
        // otherwise sit against the bottom edge rather than off-screen.
        p.setY(above >= avail.top() ? above : avail.bottom() - height());
    }
    if (p.y() < avail.top()) p.setY(avail.top());
    move(p);
    show();
    m_find->setFocus();
}

void TagFilterPopup::setResultText(const QString& s)
{
    if (m_result) m_result->setText(s);
}

}  // namespace fox
