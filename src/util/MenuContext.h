// MenuContext.h — WHICH rows a context menu acts on (docs/CONTEXT_MENUS.md §2).
//
// "The single most bug-prone thing in the whole system." Implemented exactly as
// the spec states it, because every refinement below was a shipped bug in D4:
//
//     hit      = view->indexAt(pos)
//     selected = view->selectionModel()->selectedRows()
//     if (hit valid && hit NOT in selected) -> the CLICKED ROW ALONE
//     else                                  -> the WHOLE SELECTION
//
// Right-clicking outside the selection acts on what you clicked; right-clicking
// inside it acts on all of it. Right-clicking a row you had not selected must
// never silently operate on a selection elsewhere in the list — that reads as
// the menu simply being wrong.
//
// Three refinements, each of which was a real bug:
//
//  1. THE VIEW IS A PARAMETER. A hard-coded list view hit-tested with grid
//     coordinates returns a PLAUSIBLE WRONG ROW — list rows are one column and
//     ~18px, grid cells are multi-column and ~122px, and a hidden view inside a
//     QStackedWidget keeps its geometry, so it answers confidently either way.
//     Every copy and export then acted on an asset the user had not clicked.
//  2. SUBTREE NODES ARE REJECTED. If the hit has a parent, the click landed on
//     an outliner child — a part, a look, a clip — whose row number is not a
//     browse row. Child rows are stripped out of the selection too: same
//     aliasing hazard, and a child row 3 is not asset 3.
//  3. SINGLE-SUBJECT ACTIONS TAKE THE CLICKED ROW. The clipboard holds ONE
//     image, so "Copy image" inside a thirty-row selection must copy the row
//     you right-clicked, not selection.first(). That is what clickedFile() is
//     for; list actions still take the whole set.
#pragma once
#include <QAbstractItemView>
#include <QModelIndex>
#include <QPoint>
#include <QVector>

#include <functional>

namespace menuctx {

// How a view turns a row into a file index. Every model in this application
// already has one — FileTreeModel::fileIdxAt, TexListModel::fileIdxAt — so the
// resolver is a parameter rather than a data role: asking the tabs to also
// agree on a role number would be a second thing to keep in step, which is the
// class of bug this whole file exists to remove.
using Resolve = std::function<int(const QModelIndex&)>;

// The SET a list action operates on.
inline QVector<int> contextFiles(QAbstractItemView* view, const QPoint& pos,
                                 const Resolve& resolve)
{
    QVector<int> out;
    if (!view || !resolve) return out;
    const QModelIndex hit = view->indexAt(pos);
    // Refinement 2: a click on a child node is not a click on a browse row.
    if (hit.isValid() && hit.parent().isValid()) return out;

    QVector<int> selRows;
    if (view->selectionModel()) {
        for (const QModelIndex& ix : view->selectionModel()->selectedIndexes()) {
            if (ix.column() != 0) continue;        // one entry per row, not per cell
            if (ix.parent().isValid()) continue;   // refinement 2, on the selection
            const int fi = resolve(ix);
            if (fi >= 0 && !selRows.contains(fi)) selRows.append(fi);
        }
    }

    const int hitFile = hit.isValid() ? resolve(hit) : -1;
    if (hitFile >= 0 && !selRows.contains(hitFile)) {
        out.append(hitFile);       // the clicked row alone; selection untouched
        return out;
    }
    return selRows;
}

// The ONE subject a single-subject action operates on: the clicked row when
// there is one, else the first of the set.
inline int clickedFile(QAbstractItemView* view, const QPoint& pos,
                       const Resolve& resolve, const QVector<int>& fallback)
{
    if (view && resolve) {
        const QModelIndex hit = view->indexAt(pos);
        if (hit.isValid() && !hit.parent().isValid()) {
            const int fi = resolve(hit);
            if (fi >= 0) return fi;
        }
    }
    return fallback.isEmpty() ? -1 : fallback.first();
}

}  // namespace menuctx
