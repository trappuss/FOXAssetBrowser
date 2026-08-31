// TagFilterPopup.h — the Filter button's dropdown: every tag the current index
// defines, grouped by category, with a count beside each.
//
// The popup owns NO filter state. Ticking a box rewrites the search box's text
// to contain or not contain "#tag", and the check marks are read back out of
// that same text whenever the popup opens. One source of truth, so typing
// "#chara" by hand and ticking Chara are the same act and can never disagree —
// which is the failure mode a second, parallel filter model would have.
//
// Layout is deliberately dense: two columns per category, small type, tight
// rows, because the family list on a full install runs to dozens of entries and
// a comfortable checkbox list would be several screens tall. The filter field
// at the top narrows the whole popup at once, and categories that end up empty
// hide themselves rather than leaving a header over nothing.
#pragma once
#include <QElapsedTimer>
#include <QFrame>
#include <QHash>
#include <QStringList>
#include <QVector>

#include "util/SearchQuery.h"

class QCheckBox;
class QHideEvent;
class QScrollArea;
class QLabel;
class QLineEdit;
class QSpacerItem;
class QVBoxLayout;
class QWidget;

namespace fox {

class TagFilterPopup : public QFrame {
    Q_OBJECT
public:
    explicit TagFilterPopup(QWidget* parent = nullptr);

    // Rebuild from the current ModelTags vocabulary. Cheap to call again; the
    // index changing is the only reason it needs to be.
    void rebuild();
    // Tick the boxes named by `text`'s "#tag" terms (and cross out its "-#tag"
    // ones), then show under `anchor`.
    void showFor(QWidget* anchor, const QString& text);
    // Footer line — the tab fills this in with how many models survive.
    void setResultText(const QString& s);
    // Milliseconds since this popup last closed itself. The Filter button uses
    // it to tell "the user wants to close me" from "the user wants to open me"
    // — see the note in TagFilterPopup.cpp.
    qint64 msSinceClosed() const;

protected:
    void hideEvent(QHideEvent* e) override;

signals:
    // The tab turns this into a search-box rewrite. Three states, not two:
    // a tick requires the tag, a filled square excludes it, empty removes it.
    void tagStateChanged(const QString& tag, searchq::Query::TagState state);
    void clearRequested();

private:
    void applyFilterText(const QString& needle);

    QLineEdit* m_find = nullptr;
    QLabel* m_result = nullptr;
    // Kept because a QScrollArea's own sizeHint is a small fixed default that
    // has nothing to do with what is inside it — the popup's height has to be
    // driven from the CONTENT, through this.
    QScrollArea* m_scroll = nullptr;
    QWidget* m_body = nullptr;
    QVBoxLayout* m_bodyLayout = nullptr;
    // Rebuilt with the body; every row so the find field can hide and show them.
    struct Row {
        QWidget* cell = nullptr;
        QCheckBox* box = nullptr;
        QString tag;
        QString label;
    };
    struct Group {
        QWidget* header = nullptr;
        QLabel* count = nullptr;
        QWidget* grid = nullptr;
        QVector<Row> rows;
    };
    QVector<Group> m_groups;
    // The trailing stretch has to be remembered so rebuild() can take it out
    // again: Qt removes a deleted WIDGET's layout item automatically, but a
    // spacer has no widget and would survive to sit ABOVE the next rebuild's
    // content, pushing everything into the middle of the popup.
    QSpacerItem* m_tail = nullptr;
    QElapsedTimer m_closed;
    bool m_settingChecks = false;
};

}  // namespace fox
