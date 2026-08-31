// StringsPanel.h — the game's own text, as the PREVIEW of a language table.
//
// This was a top-level "Strings" tab. It is now a right-hand panel in the
// Files tab (template §6): a `.lng2` is a FILE, its strings are what that file
// contains, and a tab of its own said otherwise. Folding it in cost one tab
// and gained the thing a separate tab could never have — selecting a table in
// the file tree and reading it is one action, and everything the Files tab
// already does to a file (extract, copy, context menu, search) applies to a
// language table without a second copy of any of it.
//
// The table LIST survives as the combo in this panel's header: it lists every
// .lng2 in the install, in the same sorted order the tab's list used, and it
// is two-way — picking a table here selects that file in the tree, and
// selecting one in the tree moves this combo. One state, two controls
// (template §3.1).
//
// Every name, description and menu label the game shows lives in .lng2 tables
// (fox::LangFile), and until now this tool only ever read them sideways: the
// name catalogue joined a handful of them to weapon parts and gear ids, and
// nothing showed the tables themselves. That left two questions unanswerable
// from inside the browser — "does this install have any text at all?" (the
// difference between an install with no language tables and one whose tables
// hold nothing this tool looks up) and "what is the label for this string?",
// which is what naming ANY new asset family starts with.
//
// So: the tables down the left, their strings on the right, one filter across
// either the open table or all of them at once, and a TSV of whatever is on
// screen.
//
// THE KEYS ARE HASHES, and StrCode32 is not invertible — the table stores
// StrCode32(label), never the label. So a label column can only be filled from
// labels this build already knows (the weapon-parts message ids, the MGO gear
// name tags); everything else shows its hash and says so. Guessing a label
// from the text would be a fabrication, and this tool does not do that.
#pragma once
#include <QHash>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QMenu;
class QLabel;
class QLineEdit;
class QComboBox;
class QPushButton;
class QTreeWidget;

class StringsPanel : public QWidget {
    Q_OBJECT
public:
    explicit StringsPanel(QWidget* parent = nullptr);

    // Show one table, by its index into ArchiveIndex::files(). Called by the
    // preview pane when a .lng2 is selected. Turns "All tables" off, because
    // the user just asked for a specific one. A file that is not a language
    // table, or is not in the list, leaves the panel as it was and returns
    // false — the caller then knows not to show it.
    bool showTableFile(int fileIdx);
    // Which table is on screen (-1 for "all tables" or none). Lets the owner
    // avoid re-selecting the table the user just clicked.
    int currentTableFile() const;
    // Whether the index holds any language table at all.
    bool hasTables() const { return !m_tableFiles.isEmpty(); }
    // Every .lng2 in the index, sorted — the file tree uses this to jump to
    // the first one.
    const QVector<int>& tableFiles() const { return m_tableFiles; }

signals:
    // The user picked a table from this panel's combo. The Files tab selects
    // that row in the tree, so the two never disagree about what is open.
    void tableChosen(int fileIdx);

public:

    // The archives changed: drop everything and re-list the tables.
    void rescan();
    // The Export menu's contextual section for this tab.
    void populateExportMenu(QMenu* menu);

    // Dev harness: type `filter` into the search box (optionally across every
    // table) and report what it found. Returns the number of rows shown.
    int applyDevFilter(const QString& filter, bool allTables);
    // Dev harness: every string of every table, as TSV. Returns rows written.
    int dumpAll(const QString& tsvPath);

private:
    struct Row {
        quint32 key = 0;
        QString label;   // empty when this build does not know one
        QString text;
        int fileIdx = -1;
    };

    void refreshTables();
    void showTable(int fileIdx);      // -1 = every table
    bool m_selecting = false;   // guards the two-way combo/tree wiring
    void applyFilter();
    void exportTsv();
    const QVector<Row>& rowsFor(int fileIdx);
    const QHash<quint32, QString>& knownLabels();

    QComboBox* m_tables = nullptr;
    QLineEdit* m_filter = nullptr;
    QCheckBox* m_allTables = nullptr;
    QTreeWidget* m_rows = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_exportBtn = nullptr;

    QVector<int> m_tableFiles;                 // parallel to m_tables' rows
    QHash<int, QVector<Row>> m_parsed;         // fileIdx → its strings, cached
    QVector<Row> m_allRows;                    // every table, built on demand
    QHash<quint32, QString> m_labels;          // StrCode32 → label, when known
    bool m_labelsBuilt = false;
    int m_shown = 0;
};
