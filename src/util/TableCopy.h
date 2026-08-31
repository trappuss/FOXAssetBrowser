// TableCopy.h — Copy / Copy all on every detail table (template §12, §15).
//
// "Every detail table gets Copy / Copy all (Ctrl+C)" is the rule, and the
// reason it is a rule is that a read-only wall of numbers you cannot get out
// of the tool is a screenshot. The panels here list mip levels, submesh
// triangle counts, animation clips, material channel means — every one of
// them is something somebody wants in a spreadsheet or a bug report.
//
// Header-only and no Q_OBJECT: it installs an action and builds menu entries
// on a QTreeWidget the caller already owns. The format is TAB-separated,
// because that is what pastes into a spreadsheet as columns; a CSV would need
// quoting rules for the commas that are already all over Fox material names.
#pragma once
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QKeySequence>
#include <QMenu>
#include <QString>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <functional>

namespace tablecopy {

// One row as `col0<TAB>col1<TAB>…`, trailing empties trimmed — a two-column
// tree with a spanned header row should not paste as a line of lone tabs.
inline QString rowText(const QTreeWidgetItem* it, int columns)
{
    QStringList cells;
    for (int c = 0; c < columns; ++c) cells << it->text(c);
    while (!cells.isEmpty() && cells.last().isEmpty()) cells.removeLast();
    return cells.join(QLatin1Char('\t'));
}

// `selectedOnly` walks the selection; otherwise the whole tree. Children are
// INDENTED with two spaces per level, so a copied parts tree still reads as a
// tree rather than as a flat list in which every submesh looks top-level.
inline QString text(const QTreeWidget* tree, bool selectedOnly)
{
    if (!tree) return {};
    const int cols = tree->columnCount();
    QStringList out;
    std::function<void(const QTreeWidgetItem*, int)> walk =
        [&](const QTreeWidgetItem* it, int depth) {
            if (!selectedOnly || it->isSelected()) {
                QString line = rowText(it, cols);
                if (!line.isEmpty())
                    out << QString(depth * 2, QLatin1Char(' ')) + line;
            }
            for (int i = 0; i < it->childCount(); ++i)
                walk(it->child(i), depth + 1);
        };
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        walk(tree->topLevelItem(i), 0);
    return out.join(QLatin1Char('\n'));
}

inline void copy(const QTreeWidget* tree, bool selectedOnly)
{
    const QString t = text(tree, selectedOnly);
    if (!t.isEmpty()) QApplication::clipboard()->setText(t);
}

// Ctrl+C on the tree copies the selection. Scoped to the widget, so two of
// these on one page do not fight over the sequence.
inline void install(QTreeWidget* tree)
{
    if (!tree) return;
    auto* a = new QAction(tree);
    a->setShortcut(QKeySequence::Copy);
    a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(a, &QAction::triggered, tree,
                     [tree] { copy(tree, true); });
    tree->addAction(a);
}

// The two entries, for a menu the caller is already building. Named the same
// everywhere, which is the whole point of putting them here.
inline void addMenuActions(QMenu* menu, QTreeWidget* tree)
{
    if (!menu || !tree) return;
    QAction* sel = menu->addAction(QStringLiteral("Copy\tCtrl+C"), tree,
                                   [tree] { copy(tree, true); });
    sel->setEnabled(!tree->selectedItems().isEmpty());
    menu->addAction(QStringLiteral("Copy all"), tree,
                    [tree] { copy(tree, false); });
}

// install() PLUS a default context menu carrying the two entries, for a tree
// that has no menu of its own. Five of this application's ten trees had
// neither — the INFO panel, ATTACHMENTS, the Files tab's ASSOCIATED list, the
// outliner and the dependency dialog — so their contents could be read and
// not extracted, which §12's rule calls a screenshot.
//
// THE POLICY IS SET FIRST, and that ordering is the trap D4 records: a helper
// that installs its own menu must not do so over a caller's, and a caller that
// wants its own must set the policy before asking for this. Here the rule is
// simpler because it is explicit — call install() alone if you are building
// your own menu, and installWithMenu() only if you are not.
inline void installWithMenu(QTreeWidget* tree)
{
    if (!tree) return;
    install(tree);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(tree, &QWidget::customContextMenuRequested, tree,
                     [tree](const QPoint& at) {
                         QMenu m(tree);
                         addMenuActions(&m, tree);
                         m.exec(tree->viewport()->mapToGlobal(at));
                     });
}

}  // namespace tablecopy
