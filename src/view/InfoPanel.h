// InfoPanel.h — the INFO panel: everything known about what is selected.
//
// Template §6 names INFO as a standard panel and lists what it should carry:
// filename, id, tags, size, LODs, bones, counts, what-uses-it, clickable
// variant links. This is the widget that holds those rows; the owning tab is
// what knows them, so this class deliberately knows nothing about models. It
// is a section list with a copy story, and that is all.
//
// Two scopes, one panel. A Fox model and the submesh selected inside it are
// different subjects with different rows, and a panel that showed only the
// model made the parts list's selection mean nothing here. So the owner
// writes both: SECTIONS for the model, then sections for the selection, in
// one pass through beginSection/addRow.
//
// COPY, per §12: Ctrl+C copies the selected rows as `key<TAB>value`, and the
// right-click menu offers that plus the value on its own and the whole panel.
// A read-only wall of text you cannot get out of the tool is a screenshot.
#pragma once
#include <QString>
#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;

namespace fox {

class InfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit InfoPanel(QWidget* parent = nullptr);

    // One rebuild is: clear(), then beginSection/addRow as often as needed.
    // Sections with no rows are dropped by finish(), so a caller can open one
    // unconditionally and let the data decide whether it appears.
    void clear();
    void beginSection(const QString& title);
    // `tip` goes on both columns. An empty value still adds the row — "(none)"
    // is information and a missing row is not.
    void addRow(const QString& key, const QString& value,
                const QString& tip = QString());
    void finish();

    // The line shown when nothing is loaded, so the panel is never a blank
    // rectangle with no explanation.
    void setPlaceholder(const QString& text);

    // Every row as `key<TAB>value`, sections as bare lines. What the copy
    // actions and the dev harness both read.
    QString asText(bool selectedOnly = false) const;
    int rowCount() const { return m_rows; }

private:
    void copy(bool selectedOnly);
    void copyValue();

    QTreeWidget* m_tree = nullptr;
    QTreeWidgetItem* m_section = nullptr;
    QString m_placeholder;
    int m_rows = 0;
};

}  // namespace fox
