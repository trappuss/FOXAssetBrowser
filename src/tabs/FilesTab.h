// FilesTab.h — the browser: virtual folder tree of every indexed file (archive
// roots → /Assets/... paths), instant search, and a preview/detail pane with
// one-click extraction. Textures (.ftex) preview decoded, with their streamed
// .ftexs mips auto-assembled from sibling archives.
#pragma once
#include <QAbstractItemModel>
#include <QHash>
#include <QAbstractListModel>
#include <QWidget>
#include <QVector>

class PreviewPane;
namespace fox { class FileInfoPanel; class NPanel; }
class StringsPanel;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QStackedWidget;
class QTreeView;

namespace fox {
struct IndexedFile;
}

// Virtual folder tree over ArchiveIndex's flat file list.
class FileTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit FileTreeModel(QObject* parent = nullptr);

    void rebuildFromIndex();

    QModelIndex index(int row, int column, const QModelIndex& parent) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent) const override;
    int columnCount(const QModelIndex& parent) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    // -1 for folders; otherwise an index into ArchiveIndex::files().
    int fileIdxAt(const QModelIndex& index) const;
    // Every file index at-or-under a node (for folder extraction).
    QVector<int> fileIdxsUnder(const QModelIndex& index) const;
    // The model index of one file's leaf node, so a caller holding a file
    // number can select it. Invalid when this index does not carry that file —
    // which is a real state, not an error: the tree is rebuilt from the
    // ArchiveIndex and a stale file number outlives a rescan.
    QModelIndex indexOfFile(int fileIdx) const;

private:
    struct Node {
        QString name;
        int parent = -1;
        int fileIdx = -1;          // -1 = folder
        QVector<int> children;     // node ids
        // Which game's assets are under this folder, for its icon colour.
        // Stamped upward from the files as the tree is built: a folder whose
        // files all agree takes their game, one holding two games takes
        // Unknown and draws neutral. Cached here rather than derived in
        // data(), which is called once per visible row per repaint.
        quint8 game = 0;           // GameId::Unknown
        bool gameSet = false;
    };
    int folderChild(int parentNode, const QString& name, QHash<quint64, int>& lookup);
    QVector<Node> m_nodes;
    // file index → leaf node id. Built with the tree, in the same pass, so it
    // can never describe a tree that is no longer there.
    QHash<int, int> m_nodeOfFile;
    // The icon box, so data() does not have to ask the view for it.
    int m_iconPx = 16;
};

// Flat search-results model (filtered view of the same flat list).
class FileSearchModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit FileSearchModel(QObject* parent = nullptr);
    void setQuery(const QString& query);
    int rowCount(const QModelIndex& parent) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    int fileIdxAt(const QModelIndex& index) const;
    int totalMatches() const { return m_totalMatches; }

private:
    QVector<int> m_matches;
    int m_totalMatches = 0;
};

class QMenu;

class FilesTab : public QWidget {
    Q_OBJECT
public:
    explicit FilesTab(QWidget* parent = nullptr);

    // Contextual entries for the menu-bar Export menu: the current file's
    // export set plus multi-selection extraction.
    void populateExportMenu(QMenu* menu);

public slots:
    void onIndexReady(bool ready);

    // Dev/screenshot harness: drive the search box programmatically.
    void setSearchText(const QString& text);

public:
    // The STRINGS panel, which lives in the preview pane — a .lng2 is a file
    // and its strings are what it contains (template §6). Null before the
    // preview is built. The Export menu and the devshot harness reach it here
    // rather than through a tab of its own, because there is no longer one.
    StringsPanel* stringsPanel() const;
    // Select the first .lng2 in the index — the file tree jumps to it and the
    // preview becomes the strings panel. False when the install carries none.
    // This is the one-click way in that the "Strings" tab used to be.
    bool showFirstStringTable();
    // Select a file in the tree/results by index, exactly as clicking it
    // would. Used by the strings panel's table combo, so the two controls
    // cannot end up naming different tables.
    bool selectFile(int fileIdx);
    // --npanel, for this tab. Same contract as the Models tab's.
    QString setPanelsForShot(const QString& keys);
    // Select the first row the current search matched. Used by the
    // harness so a Files screenshot has a file in hand.
    bool selectFirstResult();

private:
    void showFile(int fileIdx);
    QVector<int> selectedFileIdxs() const;

    QLineEdit* m_search = nullptr;
    QStackedWidget* m_stack = nullptr;
    QTreeView* m_tree = nullptr;
    QListView* m_results = nullptr;
    FileTreeModel* m_treeModel = nullptr;
    FileSearchModel* m_searchModel = nullptr;

    PreviewPane* m_preview = nullptr;
    // §6's right-hand column: the information AROUND the preview. The preview
    // itself stays the main view — see view/FileInfoPanel.h.
    fox::FileInfoPanel* m_infoPanel = nullptr;
    fox::NPanel* m_npanel = nullptr;
    QPushButton* m_extractBtn = nullptr;
    QPushButton* m_stringsBtn = nullptr;
    QLabel* m_matchCount = nullptr;
    int m_currentFile = -1;
};
