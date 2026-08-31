// FilterChips.h — the removable chips for the filters currently in force
// (template §4, §15's "chips for active filters").
//
// What was here instead was a COUNT on the funnel button: "Filter (3)". Three
// what? The only way to find out was to open the popup and read the ticks, and
// the only way to remove one was to find it in there — which is a round trip
// through a modal-ish popup to undo a thing you can see the effect of.
//
// A chip says what the filter IS and carries its own ✕. Clicking that ✕ takes
// exactly that term out of the search box, which is the one place filter state
// lives (see TagFilterPopup.h: the popup owns none of it either). So the chips
// are a VIEW of the query string and nothing more — there is no chip state to
// drift out of step with the box.
//
// The layout wraps, because a search of four tags and two words is six chips
// and a single row would elide them into uselessness — and §15 says no elided
// labels anywhere.
#pragma once
#include <QLayout>
#include <QString>
#include <QVector>

#include <functional>
#include <QWidget>

class QLabel;

namespace fox {

// A minimal flow layout: left to right, wrapping to a new line when the next
// item will not fit. Qt ships one only as an example, so here it is, small.
class ChipFlowLayout : public QLayout {
public:
    explicit ChipFlowLayout(QWidget* parent);
    ~ChipFlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int count() const override;
    QLayoutItem* itemAt(int i) const override;
    QLayoutItem* takeAt(int i) override;
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
    void setGeometry(const QRect& r) override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;

private:
    int layout(const QRect& r, bool apply) const;
    QVector<QLayoutItem*> m_items;
};

class FilterChips : public QWidget {
    Q_OBJECT
public:
    explicit FilterChips(QWidget* parent = nullptr);

    // Rebuild from a search string. `labelFor` turns a tag id into what the
    // chip shows ("chara" → "Chara"); pass a null function to show the id.
    // Hides itself when there is nothing in force, so an unfiltered list has
    // no empty strip above it.
    void setQuery(const QString& text,
                  const std::function<QString(const QString&)>& labelFor = {});

    // How many chips are up. The funnel button's tint reads this rather than
    // counting tags a second time.
    int count() const { return m_count; }

Q_SIGNALS:
    // Take this exact term out of the query. The owner rewrites the search
    // box; this widget never touches it, because the box is the state.
    void removeRequested(const QString& term);
    void clearRequested();

private:
    void clearChips();
    void addChip(const QString& text, const QString& term, const QString& tip,
                 const QColor& accent);

    ChipFlowLayout* m_flow = nullptr;
    int m_count = 0;
};

}  // namespace fox
