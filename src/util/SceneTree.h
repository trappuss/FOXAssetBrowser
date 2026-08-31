// SceneTree.h — the 3D viewports' submesh tree.
//
// Both viewports already had a flat list of switches: mesh groups in the
// Models tab, equipped parts in the Customize tab. Neither can reach a single
// submesh, and a Fox mesh group routinely holds several — the face, the
// eyelashes and the bandanna in one group, a vehicle's body and its glass in
// another. Turning one of those off meant turning off everything beside it.
//
// This is the same switches in a tree: part → mesh group → submesh, checkable
// at every level, with a parent showing partially-checked when its children
// disagree. Toggling an interior node applies to everything under it.
//
// The tree does NOT own the scene. The tab describes it as a nested Node list
// and gets back one signal per LEAF whose state changed, keyed by the
// GLMeshUpload::meshId it supplied. That keeps the widget ignorant of parts,
// variations and attachments, all of which mean different things in the two
// tabs.
#pragma once
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

namespace partmenu { struct Context; }

class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QLabel;

class SceneTree : public QWidget {
    Q_OBJECT
public:
    explicit SceneTree(QWidget* parent = nullptr);

    struct Node {
        QString label;
        // Leaf only: the id the viewport knows this submesh by. Interior nodes
        // leave it at -1 and take their state from their children.
        int meshId = -1;
        // Leaf only, for the caption: triangles and the material slot.
        int tris = 0;
        int materialSlot = -1;
        // Leaf only, optional: the material instance's own name. A slot number
        // says which material and nothing about it; the name is what the
        // material inspector and the .fv2 tables call the same thing, so a row
        // carrying it can be matched against either by eye.
        QString material;
        // Optional hover text for THIS row. The Models tab's mesh groups are
        // named by a hash the FMDL only sometimes carries a string for, so the
        // label is "Group 3" and the hash lives here — one naming function,
        // two lines of output, and they cannot disagree.
        QString tip;
        QVector<Node> children;
    };

    // Replace the tree. Everything starts checked, which is the state the
    // viewport is in after setModel().
    void setScene(const QVector<Node>& roots);
    void clear();

    // Check or uncheck everything. Emits meshVisibilityChanged for every leaf
    // whose state actually changed, which is what the caller needs — there is
    // no way to read the tree back, so a "something changed, go look" signal
    // would be one the receiver could not act on.
    void setAllVisible(bool on);

    // Show ONLY the rows under the current selection — everything else off,
    // in one action. "Turn twenty things off to look at the twenty-first" is
    // the commonest thing anyone does in a scene tree, and doing it by hand is
    // both slow and hard to undo, which is why "Show all" sits beside it.
    // Does nothing when nothing is selected.
    void isolateSelected();

    // Uncheck the first row whose label contains `needle`, exactly as clicking
    // its box would — the same propagation and the same signals. Returns false
    // when nothing matches OR when the match was already unchecked, so a
    // caller reporting "ok" is reporting something that happened. For the
    // screenshot harness, which cannot click.
    bool uncheckMatching(const QString& needle);
    // Whether the tree currently holds any rows. Lets a caller tell "no such
    // submesh" apart from "the panel is closed and there are no rows at all".
    bool isEmpty() const;

    // The tree IS the per-part visibility state now — the Models tab's flat
    // mesh-group list beside the viewport is gone, and the export paths that
    // read its check boxes read this instead. Two calls, and they are exact
    // inverses of each other so a reload can put the user's hidden set back:
    //   hiddenLeaves()      the meshIds switched OFF, and only those
    //   setHiddenLeaves()   switch exactly those off and everything else on,
    //                       emitting one signal per leaf that actually moved
    // Ids the current scene does not carry are ignored rather than remembered:
    // a set kept across a model change would hide meshes of the NEXT model
    // that happen to share an index, which is every model.
    QSet<int> hiddenLeaves() const;
    void setHiddenLeaves(const QSet<int>& meshIds);

    // Select and reveal the leaf carrying this meshId. -1 clears the
    // selection. This is the viewport-to-tree half of §4's "selecting a part
    // node selects it in the viewport and vice versa" — double-clicking a part
    // on screen has to land somewhere the user can see what they hit.
    bool selectLeaf(int meshId);
    // The same, for a SET — how a viewport multi-select reaches the panel.
    // Suppresses the echo, so it is a one-way write.
    bool selectLeaves(const QSet<int>& meshIds, int active);
    // Every leaf under the current selection. A group row counts as all of its
    // children, which is what "these submeshes" means when a group is what was
    // clicked. Multi-select is EXTENDED now: shift for a range, Ctrl to add.
    QSet<int> selectedLeaves() const;
public:
    // The page fills §4's model-side context (name, path, hash) — the tree
    // does not know which asset it is a tree of. Same hook shape the viewport
    // uses, so a page describes its subject once for both menus.
    void setContextHook(std::function<void(partmenu::Context&, int)> hook)
    {
        m_context = std::move(hook);
    }
    bool anyHidden() const;
    bool allHidden(const QSet<int>& ids) const;
    // Show or hide exactly these leaves, through the hidden-set pair.
    void setLeavesVisible(const QSet<int>& meshIds, bool on);

signals:
    // One per leaf whose visibility actually changed.
    void meshVisibilityChanged(int meshId, bool visible);
    // "Export these submeshes…" from the right-click menu. The tree does not
    // know how to export anything — the owning tab does — so it says which
    // ones and stops there.
    void exportRequested(const QSet<int>& meshIds);
    // A leaf row was selected BY THE USER. The viewport follows it — the other
    // half of the same rule.
    void leafSelected(int meshId);
    // The WHOLE selection, and which of it is current. Emitted alongside
    // leafSelected on every selection change; the viewport outlines all of
    // them, because a multi-select in the panel that showed one part on screen
    // was a list disagreeing with the picture beside it.
    void leavesSelected(const QSet<int>& meshIds, int active);
    // The subject of a context menu that is open right now, or an empty set
    // when it closes. Drawn in the context colour.
    void contextLeaves(const QSet<int>& meshIds);

private:
    void showContextMenu(const QPoint& at);
    void applyFilter();
    void onItemChanged(QTreeWidgetItem* item, int column);
    void propagateDown(QTreeWidgetItem* item, Qt::CheckState state);
    void refreshParents(QTreeWidgetItem* item);

    QTreeWidget* m_tree = nullptr;
    QLineEdit* m_filter = nullptr;
    QLabel* m_header = nullptr;
    // Collect every leaf's current state, for the bulk paths.
    QVector<QPair<int, bool>> leafStates() const;
    // Guards the recursive check-state updates: setting a child's state inside
    // itemChanged re-enters this slot, and without this the first toggle of an
    // interior node walks its subtree once per descendant.
    bool m_applying = false;
    std::function<void(partmenu::Context&, int)> m_context;
    int m_leafCount = 0;
};
