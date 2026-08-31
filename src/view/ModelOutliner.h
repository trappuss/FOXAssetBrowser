// ModelOutliner.h — the OUTLINER view of the model list (template §4).
//
// A FLAT, COLUMNED, SORTABLE LIST OF MODELS. No folders, no directories, no
// sub-trees of paths.
//
// It was a folder tree for three batches and the folder tree was broken twice
// in ways that only appeared on a real install — a merged chain that ate its
// own expand arrow, and folders that would not open at all. The user's verdict
// after the second one was the right call and is what this now is: "it should
// simply show ONLY models, no folders or directories, just icon, name,
// filename, path, game of origin in toggleable columns."
//
// That is not a retreat. A folder tree answers "what is in this directory",
// which the Files tab already answers; a columned list answers "which model is
// this, where did it come from, and which game" — sortable by any of those —
// which is what a model browser is for. And it deletes the entire class of bug
// the last two batches spent themselves on: there is no hierarchy left to fail
// to expand.
//
// ONE THING KEEPS ITS TREE: the LOADED model's row grows into Mesh, Materials,
// Armature and Animations — the Blender-style breakdown, with materials opening
// into their textures and textures into their RGBA channels. That is the part
// of this view nothing else in the tool does, and it is a tree because a model
// genuinely is one.
//
// It reads the SAME FmdlListModel the List and Grid views read, so whatever the
// search box and the tag filters have narrowed is what appears here, with no
// second filtering path.
#pragma once
#include <QHash>
#include <QSet>
#include <QVector>
#include <functional>
#include <QString>
#include <QFont>
#include <QIcon>
#include <QPoint>
#include <QWidget>

class QAbstractItemModel;
class QMenu;
class QTreeWidget;
class QTreeWidgetItem;

namespace fox {

class FmdlFile;

class ModelOutliner : public QWidget {
    Q_OBJECT
public:
    // The columns, as stable ids — never an index, because the visible set is
    // user-controlled and a stored index would mean something different after
    // a column was hidden. §3.1's rule for combos, applied to a header.
    // ICON IS ITS OWN COLUMN, and it is column 0 because that is the column
    // Qt indents and draws the expand arrow in. Dragging its edge resizes the
    // pictures with it, which is the only way to make "bigger icons" something
    // you do to the view rather than a setting you go and find.
    enum Column { ColIcon = 0, ColName, ColFile, ColPath, ColGame, ColCount };

    explicit ModelOutliner(QWidget* parent = nullptr);

    // Rebuild from `model` (an FmdlListModel). FileIdxRole is what this reads.
    void setSource(QAbstractItemModel* model);
    void refresh();

    // Grow the loaded model's row into its parts. -1 collapses the previous.
    void setLoadedModel(int fileIdx, const FmdlFile* model);
    // Select the part row for `meshId` — the viewport-to-outliner half of §4's
    // two-way selection.
    void selectPart(int meshId);

    // Display options.
    void setColumnVisible(int col, bool on);
    bool columnVisible(int col) const;
    // Icons: 0 off · 1 rendered · 2 the game's own · 3 game first, falling
    // back to the render when the game ships none.
    void setIconMode(int mode);
    // LIST mode and OUTLINER mode are the same widget: a columned list of
    // models. The only difference is whether the loaded model's row grows into
    // Mesh / Materials / Armature / Animations, so that is a switch rather
    // than a second implementation of the same list.
    void setShowLoadedTree(bool on);
    bool showLoadedTree() const { return m_showTree; }
    int iconMode() const { return m_iconMode; }

    // The tree's viewport, so the owning tab can filter its wheel events for
    // the Ctrl+wheel row zoom without this class knowing what a zoom is.
    QWidget* treeViewport() const;
    void setRowFont(const QFont& f);

    // The tab's part-menu builder. §4 has ONE builder and several entry
    // points; this is the outliner being one of them rather than growing its
    // own idea of what a submesh menu contains.
    void setPartMenuHook(std::function<void(QMenu*, int)> hook)
    {
        m_partHook = std::move(hook);
    }

    // Dev harness.
    int modelRows() const { return m_models; }
    int iconPx() const { return m_iconPx; }
    // What is under `pos` in the tree's viewport, for the hover preview: the
    // file index and whether it is a texture row. -1 for a row that stands for
    // no file. The tab owns the preview; this only answers what is there.
    void hoverSubjectAt(const QPoint& pos, int* fileIdx, bool* isTexture) const;

    QString probeForShot(const QString& spec);
    QString dumpTreeForShot(const QString& outPath);

Q_SIGNALS:
    // A MODEL row was chosen — the tab loads it.
    void modelActivated(int fileIdx);
    // A PART row under the loaded model was chosen.
    void partSelected(int meshId);
    // A CLIP row under the loaded model's Animations category was chosen —
    // the tab plays it, through the same combos the ANIMATIONS panel goes
    // through. See ModelsTab's clipChosen wiring for why it is never the
    // loader directly.
    void clipActivated(int archiveFileIdx, int clipIdx);

private:
    void rebuildGlyphs();
    void reicon();
    void requestVisibleIcons();
    void showChannels(QTreeWidgetItem* texRow);
    void applyColumns();
    int iconColumnWidth() const;
    void buildHeaderMenu(const QPoint& at);
    QIcon iconFor(int fileIdx) const;

    QTreeWidget* m_tree = nullptr;
    QAbstractItemModel* m_source = nullptr;
    QHash<int, QTreeWidgetItem*> m_partRows;   // meshId → row
    QHash<int, QTreeWidgetItem*> m_iconRows;   // fileIdx → its model row
    QHash<int, QVector<QTreeWidgetItem*>> m_texRows;
    QSet<QTreeWidgetItem*> m_stripped;
    QVector<QTreeWidgetItem*> m_texRowsExpanded;   // harness only
    bool m_loggedFirstIcon = false;
    QTreeWidgetItem* m_loadedRow = nullptr;
    int m_loadedFile = -1;
    const FmdlFile* m_loadedPtr = nullptr;
    std::function<void(QMenu*, int)> m_partHook;
    int m_models = 0;
    int m_iconPx = 20;
    // 0 off · 1 rendered · 2 the game's own icon · 3 both.
    int m_iconMode = 1;
    bool m_colOn[ColCount] = {true, true, true, true, true};
    bool m_showTree = true;
    bool m_building = false;

    QIcon m_gModel, m_gTexture, m_gClip, m_gBone, m_gPart;
    QIcon m_gMesh, m_gMaterial, m_gArmature, m_gAnim;
};

}  // namespace fox
