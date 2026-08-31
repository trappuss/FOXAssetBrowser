// SceneTree.cpp — see SceneTree.h.
#include "util/SceneTree.h"

#include "util/PartMenu.h"
#include "util/TableCopy.h"

#include <QLabel>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QMenu>
#include <QList>
#include <QHash>
#include <QSet>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <functional>

#include "view/ViewGlyphs.h"

namespace {
// Item data: the leaf's viewport id, and whether this row is a leaf at all.
constexpr int kMeshIdRole = Qt::UserRole + 1;
constexpr int kIsLeafRole = Qt::UserRole + 2;
constexpr int kSearchRole = Qt::UserRole + 3;

// The three row icons, built once. toolIcon() paints two fresh pixmaps per
// call, and a composed character is several hundred rows.
const QIcon& kindIcon(int depthKind)
{
    static const QIcon kSource = foxglyph::toolIcon(13);
    static const QIcon kGroup = foxglyph::toolIcon(4);
    static const QIcon kMesh = foxglyph::toolIcon(0);
    switch (depthKind) {
        case 0: return kSource;
        case 1: return kGroup;
        default: return kMesh;
    }
}

void buildInto(QTreeWidgetItem* parent, QTreeWidget* tree,
               const SceneTree::Node& n, int* leaves, int depth)
{
    auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
    QString label = n.label;
    if (n.meshId >= 0) {
        label += QStringLiteral("   %1 tri").arg(n.tris);
        if (n.materialSlot >= 0)
            label += QStringLiteral(" · slot %1").arg(n.materialSlot);
        // The material's own NAME where the caller knew it. A slot number says
        // which material and nothing about it; the name is what the material
        // inspector and the .fv2 tables call the same thing.
        if (!n.material.isEmpty())
            label += QStringLiteral(" · %1").arg(n.material);
    }
    item->setText(0, label);
    // A KIND GLYPH per row, so the three levels are told apart at a glance
    // rather than by counting indents. By DEPTH, not by "does it have a
    // parent": the Models tab's tree is two deep (group → submesh) and the
    // Customize tab's is three (part → group → submesh), and a test on
    // parenthood gave the same kind of row a different icon in the two tabs.
    // A leaf is a leaf whatever its depth.
    item->setIcon(0, kindIcon(n.children.isEmpty() ? 2 : depth));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(0, Qt::Checked);
    item->setData(0, kMeshIdRole, n.meshId);
    item->setData(0, kIsLeafRole, n.children.isEmpty());
    item->setData(0, kSearchRole, label.toLower());
    if (!n.tip.isEmpty()) item->setToolTip(0, n.tip);
    if (n.children.isEmpty()) ++*leaves;
    for (const SceneTree::Node& c : n.children)
        buildInto(item, tree, c, leaves, depth + 1);
    item->setExpanded(true);
}
}  // namespace

SceneTree::SceneTree(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(3);

    m_header = new QLabel(this);
    v->addWidget(m_header);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(QStringLiteral("Filter submeshes…"));
    m_filter->setClearButtonEnabled(true);
    v->addWidget(m_filter);
    connect(m_filter, &QLineEdit::textChanged, this,
            [this](const QString&) { applyFilter(); });

    // THE FIVE BUTTONS ARE GONE. Show all, Hide all, Isolate, Expand and
    // Collapse took two rows across the top of this panel — in a column now
    // shared with four other panels, that was a third of its height spent on
    // controls used once a session. They are in the right-click menu, which is
    // where every other bulk action in this application already lives (§12),
    // and where they can act on the SELECTION rather than on "whatever is
    // current".
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    // EXTENDED selection. It was single, on the argument that the check boxes
    // are the multi-select mechanism — but the check boxes mean VISIBLE, and
    // "export these four submeshes" is a different question from "show these
    // four". Shift-click a range, Ctrl-click to add, and the menu acts on all
    // of it.
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    v->addWidget(m_tree, 1);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& at) { showContextMenu(at); });
    // §12: every detail table gets Copy / Copy all. A parts list nobody can
    // get the triangle counts out of is a list you retype.
    tablecopy::install(m_tree);
    connect(m_tree, &QTreeWidget::itemChanged, this, &SceneTree::onItemChanged);
    // The tree-to-viewport half of §4's two-way selection. On the SELECTION
    // signal, not on currentItemChanged: with extended selection those are
    // different events — Ctrl-clicking a second row changes the selection and
    // not the current item, so the viewport saw nothing at all for every
    // multi-select gesture.
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (m_applying) return;
        const QSet<int> leaves = selectedLeaves();
        QTreeWidgetItem* cur = m_tree->currentItem();
        const int active =
            (cur && cur->data(0, kIsLeafRole).toBool())
                ? cur->data(0, kMeshIdRole).toInt()
                : (leaves.isEmpty() ? -1 : *leaves.constBegin());
        Q_EMIT leavesSelected(leaves, active);
        // The old single-id signal still fires, for the two receivers that
        // only ever wanted the active one (the INFO panel and the material
        // panel). Removing it would have made this change bigger than it is.
        Q_EMIT leafSelected(leaves.contains(active) ? active : -1);
    });
}

// ── The context menu (§12) ──────────────────────────────────────────────────
// Everything the button rows used to offer, plus the two things they could
// not: an action scoped to the SELECTION rather than to the whole tree, and an
// export of exactly the submeshes highlighted. Entries that would do nothing
// are left out rather than greyed — a menu of grey rows is a menu you have to
// read twice to find the live one.
void SceneTree::showContextMenu(const QPoint& at)
{
    QTreeWidgetItem* under = m_tree->itemAt(at);
    // THE SAME THREE CASES AS THE VIEWPORT (see installViewportContextMenu):
    // right-clicking outside the selection means "this one instead"; right-
    // clicking the only selected row DESELECTS it, which is how anyone says
    // "never mind" and was otherwise impossible without hiding something.
    if (under && !under->isSelected()) {
        m_tree->clearSelection();
        under->setSelected(true);
        m_tree->setCurrentItem(under);
    } else if (under && m_tree->selectedItems().size() == 1) {
        m_tree->clearSelection();
        under = nullptr;
    }
    const QSet<int> sel = selectedLeaves();
    // While the menu is up the subject wears the CONTEXT colour in the
    // viewport, exactly as a right-click in the viewport does. The tab is what
    // holds the viewport pointer, so the tree says what it means and lets the
    // tab paint it.
    Q_EMIT contextLeaves(sel);
    QMenu m(this);

    // §4's blocks, from THE shared builder — the same one the viewport's
    // right-click uses. This menu was written out separately and carried a
    // comment pointing at the viewport's version, which is a duplicate with a
    // cross-reference rather than a shared implementation. The two now differ
    // only in what each side can resolve.
    {
        partmenu::Context ctx;
        ctx.subject = sel;
        ctx.activePart =
            (under && under->data(0, kIsLeafRole).toBool())
                ? under->data(0, kMeshIdRole).toInt()
                : (sel.isEmpty() ? -1 : *sel.constBegin());
        ctx.hasGeometry = m_tree->topLevelItemCount() > 0;
        ctx.anyHidden = anyHidden();
        ctx.subjectHidden = !sel.isEmpty() && allHidden(sel);
        ctx.partName = under ? under->text(0) : QString();
        // The PAGE fills the model's name, path and hash — the tree does not
        // know which asset it is a tree OF.
        if (m_context) m_context(ctx, ctx.activePart);
        if (!sel.isEmpty()) {
            ctx.setHidden = [this, sel](bool hide) {
                setLeavesVisible(sel, !hide);
            };
            ctx.isolatePart = [this] { isolateSelected(); };
            ctx.exportPart = [this, sel] { Q_EMIT exportRequested(sel); };
        }
        ctx.showAll = [this] { setAllVisible(true); };
        ctx.hideAll = [this] { setAllVisible(false); };
        partmenu::build(&m, ctx);
        m.addSeparator();
    }
    m.addSeparator();
    m.addAction(QStringLiteral("Expand all"), m_tree, &QTreeWidget::expandAll);
    m.addAction(QStringLiteral("Collapse all"), m_tree,
                &QTreeWidget::collapseAll);
    m.addSeparator();
    tablecopy::addMenuActions(&m, m_tree);
    m.exec(m_tree->viewport()->mapToGlobal(at));
    // However it closed. The outline must not outlive the menu that raised it.
    Q_EMIT contextLeaves({});
}

// Every LEAF under the selection, which is what "these submeshes" means when a
// group row is what was clicked.

// Is anything switched off right now? "Show all" over a scene with nothing
// hidden is a row that exists to be grey, so the builder is told and leaves it
// out.
bool SceneTree::anyHidden() const
{
    QVector<QTreeWidgetItem*> stack;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        stack.append(m_tree->topLevelItem(i));
    while (!stack.isEmpty()) {
        QTreeWidgetItem* it = stack.takeLast();
        if (it->data(0, kIsLeafRole).toBool()
            && it->checkState(0) == Qt::Unchecked)
            return true;
        for (int i = 0; i < it->childCount(); ++i) stack.append(it->child(i));
    }
    return false;
}

// Is the whole subject hidden? The Hide/Show action is ONE action whose label
// reflects the current state, so it needs an answer for a set, not a row —
// and a mixed set counts as shown, because "Hide" is then the move that makes
// the state uniform.
bool SceneTree::allHidden(const QSet<int>& ids) const
{
    if (ids.isEmpty()) return false;
    QVector<QTreeWidgetItem*> stack;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        stack.append(m_tree->topLevelItem(i));
    int seen = 0;
    while (!stack.isEmpty()) {
        QTreeWidgetItem* it = stack.takeLast();
        if (it->data(0, kIsLeafRole).toBool()
            && ids.contains(it->data(0, kMeshIdRole).toInt())) {
            ++seen;
            if (it->checkState(0) != Qt::Unchecked) return false;
        }
        for (int i = 0; i < it->childCount(); ++i) stack.append(it->child(i));
    }
    return seen > 0;
}

QSet<int> SceneTree::selectedLeaves() const
{
    QSet<int> out;
    QVector<QTreeWidgetItem*> stack;
    for (QTreeWidgetItem* it : m_tree->selectedItems()) stack.append(it);
    while (!stack.isEmpty()) {
        QTreeWidgetItem* it = stack.takeLast();
        if (it->data(0, kIsLeafRole).toBool()) {
            const int id = it->data(0, kMeshIdRole).toInt();
            if (id >= 0) out.insert(id);
        }
        for (int i = 0; i < it->childCount(); ++i) stack.append(it->child(i));
    }
    return out;
}

void SceneTree::setLeavesVisible(const QSet<int>& meshIds, bool on)
{
    if (meshIds.isEmpty()) return;
    // Through the shared hidden-set pair, which is the documented exact
    // inverse and already emits one signal per leaf that actually moved.
    QSet<int> hidden = hiddenLeaves();
    for (int id : meshIds) {
        if (on) hidden.remove(id);
        else hidden.insert(id);
    }
    setHiddenLeaves(hidden);
}

void SceneTree::clear()
{
    m_applying = true;
    m_tree->clear();
    m_applying = false;
    m_leafCount = 0;
    m_header->setText(QStringLiteral("nothing in the scene"));
}

void SceneTree::setScene(const QVector<Node>& roots)
{
    // Blocked wholesale: building the tree sets a check state on every item,
    // and each of those is an itemChanged. Without this, showing a character
    // would emit several hundred visibility signals describing no change.
    m_applying = true;
    m_tree->clear();
    m_leafCount = 0;
    for (const Node& r : roots) buildInto(nullptr, m_tree, r, &m_leafCount, 0);
    m_applying = false;
    m_header->setText(QStringLiteral("%1 source(s) · %2 submesh(es)")
                          .arg(roots.size())
                          .arg(m_leafCount));
    applyFilter();
}

bool SceneTree::isEmpty() const
{
    return m_tree->topLevelItemCount() == 0;
}

QVector<QPair<int, bool>> SceneTree::leafStates() const
{
    QVector<QPair<int, bool>> out;
    QVector<QTreeWidgetItem*> stack;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        stack.append(m_tree->topLevelItem(i));
    while (!stack.isEmpty()) {
        QTreeWidgetItem* it = stack.takeLast();
        if (it->data(0, kIsLeafRole).toBool()) {
            const int id = it->data(0, kMeshIdRole).toInt();
            if (id >= 0) out.append({id, it->checkState(0) == Qt::Checked});
        }
        for (int i = 0; i < it->childCount(); ++i) stack.append(it->child(i));
    }
    return out;
}

QSet<int> SceneTree::hiddenLeaves() const
{
    QSet<int> out;
    for (const auto& p : leafStates())
        if (!p.second) out.insert(p.first);
    return out;
}

void SceneTree::setHiddenLeaves(const QSet<int>& meshIds)
{
    const QVector<QPair<int, bool>> before = leafStates();
    m_applying = true;
    QVector<QTreeWidgetItem*> stack;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        stack.append(m_tree->topLevelItem(i));
    QVector<QTreeWidgetItem*> leaves;
    while (!stack.isEmpty()) {
        QTreeWidgetItem* it = stack.takeLast();
        if (it->data(0, kIsLeafRole).toBool()) {
            const int id = it->data(0, kMeshIdRole).toInt();
            if (id >= 0) {
                it->setCheckState(0, meshIds.contains(id) ? Qt::Unchecked
                                                          : Qt::Checked);
                leaves.append(it);
            }
        }
        for (int i = 0; i < it->childCount(); ++i) stack.append(it->child(i));
    }
    // Parents RECOMPUTED from the leaves, exactly as isolateSelected() does —
    // a restored set routinely leaves a group half on, which is what
    // PartiallyChecked is for.
    for (QTreeWidgetItem* it : leaves) refreshParents(it);
    m_applying = false;

    const QHash<int, bool> now = [&] {
        QHash<int, bool> h;
        for (const auto& p : leafStates()) h.insert(p.first, p.second);
        return h;
    }();
    for (const auto& b : before)
        if (now.value(b.first, b.second) != b.second)
            emit meshVisibilityChanged(b.first, now.value(b.first));
}

void SceneTree::setAllVisible(bool on)
{
    const Qt::CheckState st = on ? Qt::Checked : Qt::Unchecked;
    const QVector<QPair<int, bool>> before = leafStates();
    m_applying = true;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* it = m_tree->topLevelItem(i);
        it->setCheckState(0, st);
        propagateDown(it, st);
    }
    m_applying = false;
    // Only what actually changed. Emitting every leaf would be correct but
    // would also make a "Show all" on an already-visible scene look like a
    // scene-wide change in any log the receiver keeps.
    for (const auto& b : before)
        if (b.second != on) emit meshVisibilityChanged(b.first, on);
}

void SceneTree::isolateSelected()
{
    // The SELECTION, not the current item. Ctrl+click clears a single
    // selection while leaving the current item where it was, and isolating a
    // row nothing is highlighting is a scene-wide change nobody asked for.
    const QList<QTreeWidgetItem*> chosen = m_tree->selectedItems();
    QTreeWidgetItem* sel = chosen.isEmpty() ? nullptr : chosen.first();
    if (!sel || chosen.isEmpty()) {
        // Three silent no-ops on one button is two too many.
        m_header->setText(QStringLiteral(
            "Isolate: select a row first (a group isolates everything under "
            "it)."));
        return;
    }

    // Which leaves belong to the selection. Collected BEFORE anything is
    // switched, so the answer cannot change under the walk that follows.
    // EVERY selected row, not just the first. Isolating four shift-selected
    // submeshes used to isolate one of them and quietly hide the other three.
    const QSet<int> keep = selectedLeaves();
    if (keep.isEmpty()) {
        m_header->setText(
            QStringLiteral("Isolate: nothing under that row to show."));
        return;   // a row with no geometry under it
    }

    const QVector<QPair<int, bool>> before = leafStates();
    m_applying = true;
    QVector<QTreeWidgetItem*> stack;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        stack.append(m_tree->topLevelItem(i));
    while (!stack.isEmpty()) {
        QTreeWidgetItem* it = stack.takeLast();
        if (it->data(0, kIsLeafRole).toBool()) {
            const int id = it->data(0, kMeshIdRole).toInt();
            it->setCheckState(0, keep.contains(id) ? Qt::Checked
                                                   : Qt::Unchecked);
        }
        for (int i = 0; i < it->childCount(); ++i) stack.append(it->child(i));
    }
    // The parents are RECOMPUTED from the leaves rather than set: an isolate
    // routinely leaves a group half on, and that is exactly the state
    // PartiallyChecked exists to show. refreshParents walks up from a leaf, so
    // every leaf is visited — the walk is idempotent and the trees here are
    // hundreds of rows, not thousands.
    {
        QVector<QTreeWidgetItem*> st2;
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
            st2.append(m_tree->topLevelItem(i));
        while (!st2.isEmpty()) {
            QTreeWidgetItem* it = st2.takeLast();
            if (it->data(0, kIsLeafRole).toBool()) refreshParents(it);
            for (int i = 0; i < it->childCount(); ++i) st2.append(it->child(i));
        }
    }
    m_applying = false;

    // Only what actually changed, the same rule setAllVisible() follows.
    const QHash<int, bool> now = [&] {
        QHash<int, bool> h;
        for (const auto& p : leafStates()) h.insert(p.first, p.second);
        return h;
    }();
    for (const auto& b : before)
        if (now.value(b.first, b.second) != b.second)
            emit meshVisibilityChanged(b.first, now.value(b.first));
    m_header->setText(QStringLiteral("Isolated %1 of %2 submesh(es) — "
                                     "\"Show all\" puts the rest back")
                          .arg(keep.size())
                          .arg(m_leafCount));
}

bool SceneTree::uncheckMatching(const QString& needle)
{
    const QString n = needle.trimmed().toLower();
    if (n.isEmpty()) return false;
    QVector<QTreeWidgetItem*> stack;
    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i)
        stack.append(m_tree->topLevelItem(i));
    while (!stack.isEmpty()) {
        QTreeWidgetItem* it = stack.takeLast();
        if (it->data(0, kSearchRole).toString().contains(n)) {
            // Already unchecked? setCheckState would emit nothing, and
            // returning true would report an action that did not happen.
            if (it->checkState(0) == Qt::Unchecked) return false;
            // Through setCheckState so onItemChanged runs: the point of this
            // hook is to exercise the real path, not to shortcut it.
            it->setCheckState(0, Qt::Unchecked);
            return true;
        }
        for (int i = it->childCount() - 1; i >= 0; --i) stack.append(it->child(i));
    }
    return false;
}

void SceneTree::propagateDown(QTreeWidgetItem* item, Qt::CheckState state)
{
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem* c = item->child(i);
        c->setCheckState(0, state);
        propagateDown(c, state);
    }
}

void SceneTree::refreshParents(QTreeWidgetItem* item)
{
    // A parent is checked when every child is, unchecked when none is, and
    // partially checked otherwise — computed rather than stored, so it can
    // never disagree with what is under it.
    for (QTreeWidgetItem* p = item->parent(); p; p = p->parent()) {
        int on = 0, off = 0, partial = 0;
        for (int i = 0; i < p->childCount(); ++i) {
            switch (p->child(i)->checkState(0)) {
            case Qt::Checked: ++on; break;
            case Qt::Unchecked: ++off; break;
            default: ++partial; break;
            }
        }
        p->setCheckState(0, partial > 0 || (on > 0 && off > 0)
                                ? Qt::PartiallyChecked
                                : (on > 0 ? Qt::Checked : Qt::Unchecked));
    }
}

void SceneTree::onItemChanged(QTreeWidgetItem* item, int column)
{
    if (m_applying || column != 0 || !item) return;
    m_applying = true;

    const Qt::CheckState st = item->checkState(0);
    const bool leaf = item->data(0, kIsLeafRole).toBool();
    if (!leaf && st != Qt::PartiallyChecked) propagateDown(item, st);
    refreshParents(item);

    // Report every leaf under the item that changed, including the item
    // itself when it is one. Collecting them here rather than emitting from
    // inside propagateDown keeps the signal order stable and the recursion
    // free of side effects.
    QVector<QTreeWidgetItem*> stack{item};
    QVector<QPair<int, bool>> changes;
    while (!stack.isEmpty()) {
        QTreeWidgetItem* it = stack.takeLast();
        if (it->data(0, kIsLeafRole).toBool()) {
            const int id = it->data(0, kMeshIdRole).toInt();
            if (id >= 0)
                changes.append({id, it->checkState(0) == Qt::Checked});
        }
        for (int i = 0; i < it->childCount(); ++i) stack.append(it->child(i));
    }
    // The guard is held ACROSS the emits. A receiver that touched a check
    // state would otherwise re-enter this slot mid-emit and interleave a
    // second walk — and `item` would dangle if it called setScene().
    for (const auto& c : changes) emit meshVisibilityChanged(c.first, c.second);
    m_applying = false;
}

void SceneTree::applyFilter()
{
    const QString needle = m_filter->text().trimmed().toLower();
    // A row survives when it matches, when an ancestor matches (so a matched
    // group still shows what is in it), or when a descendant matches (so the
    // path to a matched submesh is not hidden).
    std::function<bool(QTreeWidgetItem*, bool)> walk =
        [&](QTreeWidgetItem* it, bool ancestorHit) -> bool {
        const bool self = needle.isEmpty()
            || it->data(0, kSearchRole).toString().contains(needle);
        bool childHit = false;
        for (int i = 0; i < it->childCount(); ++i)
            childHit = walk(it->child(i), ancestorHit || self) || childHit;
        const bool vis = self || childHit || ancestorHit;
        it->setHidden(!vis);
        return self || childHit;
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        walk(m_tree->topLevelItem(i), false);
}

// ── Selection, both ways (§4) ───────────────────────────────────────────────

// Mirror a whole viewport selection into the tree.
//
// This exists because selectLeaf() alone could not: the viewport's pick now
// hands the tree its ACTIVE part, the tree answers by selecting that one row,
// and the tree's own selection signal hands the viewport back a set of one —
// which collapsed every Shift and Ctrl click in the viewport to a single part.
// Measured with --selseq: "pick 14, shift 12" came back {12}.
//
// m_applying suppresses the echo, so this is a one-way write.
bool SceneTree::selectLeaves(const QSet<int>& meshIds, int active)
{
    if (!m_tree) return false;
    const bool was = m_applying;
    m_applying = true;
    m_tree->clearSelection();
    QTreeWidgetItem* activeRow = nullptr;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if (!(*it)->data(0, kIsLeafRole).toBool()) continue;
        const int id = (*it)->data(0, kMeshIdRole).toInt();
        if (!meshIds.contains(id)) continue;
        if ((*it)->isHidden() && m_filter && !m_filter->text().isEmpty())
            m_filter->clear();
        for (QTreeWidgetItem* p = (*it)->parent(); p; p = p->parent())
            p->setExpanded(true);
        (*it)->setSelected(true);
        if (id == active || !activeRow) activeRow = *it;
    }
    if (activeRow) {
        m_tree->setCurrentItem(activeRow);
        m_tree->scrollToItem(activeRow, QAbstractItemView::EnsureVisible);
    }
    m_applying = was;
    return activeRow != nullptr;
}

bool SceneTree::selectLeaf(int meshId)
{
    if (!m_tree) return false;
    if (meshId < 0) {
        const bool was = m_applying;
        m_applying = true;
        m_tree->clearSelection();
        m_tree->setCurrentItem(nullptr);
        m_applying = was;
        return true;
    }
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if (!(*it)->data(0, kIsLeafRole).toBool()) continue;
        if ((*it)->data(0, kMeshIdRole).toInt() != meshId) continue;
        // A row the FILTER has hidden is a row the user cannot see either, and
        // the same argument applies: they just clicked this part in the
        // viewport, so the filter that is hiding it has been overtaken by
        // events. Clearing it is the only outcome that shows them what they
        // picked. (The iterator walks hidden rows by default, which is how the
        // selection could land on one at all.)
        if ((*it)->isHidden() && m_filter && !m_filter->text().isEmpty())
            m_filter->clear();
        // Expand the ancestors: a row selected inside a collapsed group is a
        // selection the user cannot see, which is the same as no feedback.
        for (QTreeWidgetItem* p = (*it)->parent(); p; p = p->parent())
            p->setExpanded(true);
        const bool was = m_applying;
        m_applying = true;
        m_tree->clearSelection();
        (*it)->setSelected(true);
        m_tree->setCurrentItem(*it);
        m_applying = was;
        m_tree->scrollToItem(*it, QAbstractItemView::EnsureVisible);
        return true;
    }
    return false;
}

