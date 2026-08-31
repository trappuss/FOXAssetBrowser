// FilesTab.cpp — see FilesTab.h.
#include "tabs/FilesTab.h"

#include <QBrush>
#include <QColor>

#include "util/ExportActions.h"
#include "util/MenuText.h"
#include "util/ShadowDisplay.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QCoreApplication>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QStackedWidget>
#include <QTimer>
#include <QTreeView>
#include "view/AssetIcons.h"
#include <QVBoxLayout>

#include "app/Config.h"
#include "fox/BcDecode.h"
#include "fox/FoxHash.h"
#include "index/ArchiveIndex.h"
#include "index/ModelTags.h"
#include "view/FileInfoPanel.h"
#include "view/NPanel.h"
#include "util/MenuContext.h"
#include "util/PanelPersist.h"
#include "util/SearchBox.h"
#include "util/SearchQuery.h"
#include "preview/PreviewPane.h"
#include "view/StringsPanel.h"
#include "util/ExportLayout.h"
#include "util/Extract.h"

using fox::ArchiveIndex;
using fox::IndexedFile;

// ── FileTreeModel ────────────────────────────────────────────────────────────

FileTreeModel::FileTreeModel(QObject* parent) : QAbstractItemModel(parent)
{
    m_nodes.append(Node{});   // root at id 0
}

int FileTreeModel::folderChild(int parentNode, const QString& name,
                               QHash<quint64, int>& lookup)
{
    // Key on (parent, name-hash) — collisions resolved by an exact-name walk.
    const quint64 key =
        (static_cast<quint64>(parentNode) << 32) ^ qHash(name, 0x9E3779B9u);
    const auto it = lookup.constFind(key);
    if (it != lookup.constEnd() && m_nodes[it.value()].name == name)
        return it.value();

    const int id = m_nodes.size();
    Node n;
    n.name = name;
    n.parent = parentNode;
    m_nodes.append(n);
    m_nodes[parentNode].children.append(id);
    lookup.insert(key, id);
    return id;
}

void FileTreeModel::rebuildFromIndex()
{
    beginResetModel();
    m_nodes.clear();
    m_nodeOfFile.clear();
    m_nodes.append(Node{});

    const ArchiveIndex& index = ArchiveIndex::instance();
    QHash<quint64, int> lookup;

    // Archive roots first so the tree groups by .dat.
    QVector<int> archiveNodes;
    archiveNodes.reserve(index.archives().size());
    for (const auto& a : index.archives())
        archiveNodes.append(folderChild(0, a.shortName, lookup));

    const auto& files = index.files();
    for (int i = 0; i < files.size(); ++i) {
        const IndexedFile& f = files[i];
        if (f.archiveId < 0 || f.archiveId >= archiveNodes.size()) continue;
        int at = archiveNodes[f.archiveId];

        const QString rel = extract::relativePathFor(f);
        const QStringList parts = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (int p = 0; p < parts.size() - 1; ++p)
            at = folderChild(at, parts[p], lookup);

        const int id = m_nodes.size();
        Node leaf;
        leaf.name = parts.isEmpty() ? rel : parts.last();
        leaf.parent = at;
        leaf.fileIdx = i;
        m_nodes.append(leaf);
        m_nodes[at].children.append(id);
        m_nodeOfFile.insert(i, id);

        // Stamp this file's game up its ancestry, for the folder icons. A
        // folder whose files agree takes their colour; one holding two games
        // goes back to neutral and stays there, because a folder that is half
        // TPP is not a TPP folder.
        const quint8 g = quint8(fox::GameId(index.gameOf(f)));
        for (int up = at; up > 0; up = m_nodes[up].parent) {
            Node& n = m_nodes[up];
            if (!n.gameSet) { n.game = g; n.gameSet = true; }
            else if (n.game != g) { n.game = quint8(fox::GameId::Unknown); }
        }
    }
    endResetModel();
}

QModelIndex FileTreeModel::index(int row, int column, const QModelIndex& parent) const
{
    const int parentId = parent.isValid() ? static_cast<int>(parent.internalId()) : 0;
    if (parentId < 0 || parentId >= m_nodes.size()) return {};
    const Node& p = m_nodes[parentId];
    if (row < 0 || row >= p.children.size() || column < 0 || column > 1) return {};
    return createIndex(row, column, static_cast<quintptr>(p.children[row]));
}

QModelIndex FileTreeModel::parent(const QModelIndex& child) const
{
    if (!child.isValid()) return {};
    const int id = static_cast<int>(child.internalId());
    if (id <= 0 || id >= m_nodes.size()) return {};
    const int parentId = m_nodes[id].parent;
    if (parentId <= 0) return {};
    const int grand = m_nodes[parentId].parent;
    const int row = m_nodes[grand].children.indexOf(parentId);
    return createIndex(row, 0, static_cast<quintptr>(parentId));
}

QModelIndex FileTreeModel::indexOfFile(int fileIdx) const
{
    const auto it = m_nodeOfFile.constFind(fileIdx);
    if (it == m_nodeOfFile.constEnd()) return {};
    const int id = it.value();
    if (id <= 0 || id >= m_nodes.size()) return {};
    const int parentId = m_nodes[id].parent;
    if (parentId < 0 || parentId >= m_nodes.size()) return {};
    const int row = m_nodes[parentId].children.indexOf(id);
    if (row < 0) return {};
    return createIndex(row, 0, static_cast<quintptr>(id));
}

int FileTreeModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid() && parent.column() != 0) return 0;
    const int id = parent.isValid() ? static_cast<int>(parent.internalId()) : 0;
    if (id < 0 || id >= m_nodes.size()) return 0;
    return m_nodes[id].children.size();
}

int FileTreeModel::columnCount(const QModelIndex&) const
{
    return 2;
}

QVariant FileTreeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) return {};
    const int id = static_cast<int>(index.internalId());
    if (id <= 0 || id >= m_nodes.size()) return {};
    const Node& n = m_nodes[id];

    // Overridden duplicate (mod install): dim + label, via the shared rule so
    // every pane agrees. The name suffix belongs to column 0 only — column 1
    // keeps its size formatting — but the dimming applies to the whole row.
    if (n.fileIdx >= 0) {
        const fox::IndexedFile& sf = ArchiveIndex::instance().files()[n.fileIdx];
        if (sf.shadowed && !(role == Qt::DisplayRole && index.column() != 0)) {
            const QVariant sh = shadowui::roleFor(sf, role, n.name);
            if (sh.isValid()) return sh;
        }
    }

    // ── THE ICON ────────────────────────────────────────────────────────
    // A folder coloured by the game whose assets are under it; a file coloured
    // and drawn by what KIND of asset it is. One vocabulary, shared with the
    // Models views — see view/AssetIcons.h for why the two axes are split that
    // way. Cached by (glyph, colour, size), because this is data() and it is
    // called once per visible row per repaint.
    if (role == Qt::DecorationRole && index.column() == 0) {
        const int px = m_iconPx;
        if (n.fileIdx < 0) return fox::asseticon::folder(fox::GameId(n.game), px);
        const fox::IndexedFile& f = ArchiveIndex::instance().files()[n.fileIdx];
        return fox::asseticon::file(ArchiveIndex::extensionOf(f), px);
    }
    if (role == Qt::ToolTipRole && index.column() == 0 && n.fileIdx >= 0) {
        const fox::IndexedFile& f = ArchiveIndex::instance().files()[n.fileIdx];
        const QString kind =
            fox::asseticon::kindName(ArchiveIndex::extensionOf(f));
        return kind.isEmpty() ? f.path
                              : QStringLiteral("%1\n%2").arg(f.path, kind);
    }

    if (role == Qt::DisplayRole) {
        if (index.column() == 0) return n.name;
        if (n.fileIdx >= 0) {
            const IndexedFile& f = ArchiveIndex::instance().files()[n.fileIdx];
            if (f.size >= 1024 * 1024)
                return QStringLiteral("%1 MB").arg(f.size / (1024.0 * 1024.0), 0, 'f', 1);
            if (f.size >= 1024)
                return QStringLiteral("%1 KB").arg(f.size / 1024.0, 0, 'f', 1);
            return QStringLiteral("%1 B").arg(f.size);
        }
        return QString();
    }
    return {};
}

QVariant FileTreeModel::headerData(int section, Qt::Orientation orientation,
                                   int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return section == 0 ? QStringLiteral("Name") : QStringLiteral("Size");
}

int FileTreeModel::fileIdxAt(const QModelIndex& index) const
{
    if (!index.isValid()) return -1;
    const int id = static_cast<int>(index.internalId());
    if (id <= 0 || id >= m_nodes.size()) return -1;
    return m_nodes[id].fileIdx;
}

QVector<int> FileTreeModel::fileIdxsUnder(const QModelIndex& index) const
{
    QVector<int> out;
    if (!index.isValid()) return out;
    QVector<int> stack{static_cast<int>(index.internalId())};
    while (!stack.isEmpty()) {
        const int id = stack.takeLast();
        if (id <= 0 || id >= m_nodes.size()) continue;
        const Node& n = m_nodes[id];
        if (n.fileIdx >= 0) out.append(n.fileIdx);
        for (const int c : n.children) stack.append(c);
    }
    return out;
}

// ── FileSearchModel ──────────────────────────────────────────────────────────

FileSearchModel::FileSearchModel(QObject* parent) : QAbstractListModel(parent) {}

void FileSearchModel::setQuery(const QString& query)
{
    beginResetModel();
    m_matches.clear();
    m_totalMatches = 0;
    const QString needle = query.trimmed();
    // Guard on the PARSED query, not the raw text: a stray quote parses to no
    // terms at all, and a query with no terms matches everything — which would
    // enumerate the whole index instead of nothing.
    const searchq::Query parsed(needle);
    if (!parsed.isEmpty()) {
        // Space-separated terms, ALL must match, and a -term must not — see
        // util/SearchQuery.h, which every search box in the app shares so the
        // syntax means one thing everywhere.
        const searchq::Query& q = parsed;
        const auto& files = ArchiveIndex::instance().files();
        constexpr int kMaxShown = 100000;
        for (int i = 0; i < files.size(); ++i) {
            // Id-aware: a term that reads as a 64-bit path hash matches the
            // entry carrying it, which is the only way to find one of the
            // entries in this tree that no dictionary names.
            if (!fox::queryMatchesFile(q, i, files[i])) continue;
            ++m_totalMatches;
            if (m_matches.size() < kMaxShown) m_matches.append(i);
        }
    }
    endResetModel();
}

int FileSearchModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_matches.size();
}

QVariant FileSearchModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_matches.size()) return {};
    const IndexedFile& f = ArchiveIndex::instance().files()[m_matches[index.row()]];
    const QVariant sh = shadowui::roleFor(f, role, f.path);
    if (sh.isValid()) return sh;
    if (role == Qt::DisplayRole) return f.path;
    // PARITY WITH THE TREE. The search results are the same files seen a
    // different way, and a result list with no icons beside a tree that has
    // them is two answers to "what kind of thing is this".
    if (role == Qt::DecorationRole)
        return fox::asseticon::file(ArchiveIndex::extensionOf(f), 16);
    if (role == Qt::ToolTipRole) {
        const QString kind = fox::asseticon::kindName(ArchiveIndex::extensionOf(f));
        return kind.isEmpty() ? f.path
                              : QStringLiteral("%1\n%2").arg(f.path, kind);
    }
    return {};
}

int FileSearchModel::fileIdxAt(const QModelIndex& index) const
{
    if (!index.isValid() || index.row() >= m_matches.size()) return -1;
    return m_matches[index.row()];
}

// ── FilesTab ─────────────────────────────────────────────────────────────────

FilesTab::FilesTab(QWidget* parent) : QWidget(parent)
{
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    // Left: search + tree/results.
    auto* left = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    m_search = new QLineEdit(left);
    m_search->setPlaceholderText(
        QStringLiteral("Search all files…  (space = AND, -word excludes)"));
    m_search->setToolTip(searchq::tooltip());
    m_search->setClearButtonEnabled(true);
    // Esc clears, the down arrow recalls the last ten searches, and a
    // committed search is remembered — the same four behaviours in every
    // search box in the application, from one place (template §4/§15).
    fox::searchbox::attach(m_search, QStringLiteral("files/searchHistory"));
    leftLayout->addWidget(m_search);

    m_matchCount = new QLabel(left);
    m_matchCount->hide();
    leftLayout->addWidget(m_matchCount);
    auto* headerBar = new QHBoxLayout();
    headerBar->setContentsMargins(0, 0, 0, 0);
    leftLayout->addLayout(headerBar);

    m_stack = new QStackedWidget(left);
    m_treeModel = new FileTreeModel(this);
    m_tree = new QTreeView(m_stack);
    m_tree->setModel(m_treeModel);
    m_tree->setUniformRowHeights(true);
    // Room for the kind icons. Small: this tree is for scanning hundreds of
    // rows, and the icon is a classification, not a picture of the asset.
    m_tree->setIconSize(QSize(16, 16));
    m_tree->setIndentation(14);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    m_searchModel = new FileSearchModel(this);
    m_results = new QListView(m_stack);
    m_results->setModel(m_searchModel);
    m_results->setUniformItemSizes(true);
    m_results->setIconSize(QSize(16, 16));
    // An EXPLICIT delegate, always. A QListView that has never been given one
    // reports itemDelegate() == nullptr, and with setUniformItemSizes(true)
    // the row size is then cached as an INVALID QSize: every row comes back
    // height -1 and the view paints nothing at all — a full model, a working
    // search, a working selection, and a blank pane. Measured on this list:
    // visualRect(index(0,0)) went from 401x-1 to 421x14 the moment one was
    // installed. Three lists in this application had it.
    m_results->setItemDelegate(new QStyledItemDelegate(this));
    m_results->setSelectionMode(QAbstractItemView::ExtendedSelection);

    m_stack->addWidget(m_tree);
    m_stack->addWidget(m_results);
    leftLayout->addWidget(m_stack, 1);
    splitter->addWidget(left);

    // Right: the contextual preview + actions.
    auto* right = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    m_preview = new PreviewPane(right);
    rightLayout->addWidget(m_preview, 1);

    m_extractBtn = new QPushButton(QStringLiteral("Extract…"), right);
    m_extractBtn->setEnabled(false);
    rightLayout->addWidget(m_extractBtn);

    // ── The N-panel column (template §6) ─────────────────────────────────
    // Files was the last tab not on the column, and it is a different shape of
    // job: its right-hand side is the PreviewPane — the main view, not a side
    // panel. So the preview stays where it is and the INFORMATION AROUND IT
    // becomes the column, following what TextureInfoPanel did in 8s.
    m_infoPanel = new fox::FileInfoPanel(this);
    m_npanel = new fox::NPanel(QStringLiteral("files/npanel"), splitter);
    m_npanel->addPanel(
        QStringLiteral("fileinfo"), QStringLiteral("FILE INFO"), 16,
        m_infoPanel->fileInfoSection(),
        QStringLiteral("What this file IS — name, type, size, hash, which "
                       "archive it came from, and whether this is the copy the "
                       "game would actually load."));
    m_npanel->addPanel(
        QStringLiteral("preview"), QStringLiteral("PREVIEW CONTROLS"), 20,
        m_preview->transportSection(),
        QStringLiteral("The controls for whatever the preview is showing. For "
                       "a .wem that is the transport; it says why it is "
                       "unavailable rather than only being greyed out."));
    m_npanel->addPanel(
        QStringLiteral("associated"), QStringLiteral("ASSOCIATED"), 13,
        m_infoPanel->associatedSection(),
        QStringLiteral("What this file is connected to — a model's textures "
                       "from its own material table, a texture's models from "
                       "the cached model→material→texture map. Double-click a "
                       "row to open it."));
    m_npanel->restoreState({QStringLiteral("fileinfo"),
                            QStringLiteral("associated")});

    // The one-click way into the language tables. Folding the Strings tab into
    // this one is right — a .lng2 is a file and its strings are its content —
    // but a tab in the tab bar is also how anyone found the feature, and
    // "select a .lng2 and the preview becomes a string browser" is not
    // discoverable on its own. So the entry point survives as a button, and
    // it disables itself on an install that ships no tables rather than
    // leading somewhere empty.
    m_stringsBtn = new QPushButton(QStringLiteral("Language tables…"), left);
    m_stringsBtn->setToolTip(QStringLiteral(
        "Jump to the game's .lng2 language tables — every name, description "
        "and menu label the game shows. The preview becomes a string browser: "
        "filter by text, label or key, search every table at once, export "
        "TSV.\n\n"
        "They are ordinary files, so everything else here works on them too."));
    m_stringsBtn->setEnabled(false);   // until the index says there are some
    splitter->addWidget(right);
    splitter->addWidget(m_npanel);
    // The live count into the panel header, rather than onto a row inside the
    // panel where it would cost a row and repeat the title.
    connect(m_infoPanel, &fox::FileInfoPanel::associatedTitleChanged, this,
            [this](const QString& t) {
                if (QLabel* l =
                        m_npanel->titleLabel(QStringLiteral("associated")))
                    l->setText(t);
            });
    // A row in ASSOCIATED navigates to the asset it names, in this tab.
    connect(m_infoPanel, &fox::FileInfoPanel::assetActivated, this,
            [this](int fi) { selectFile(fi); });

    // Explicit widths, not stretch alone: the preview is the main view and has
    // to keep the room, and a stretch-only split gave the column a sliver.
    // These are the tab's DEFAULT — PanelPersist::bind below leaves them alone
    // until the user has actually dragged something.
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 4);
    splitter->setStretchFactor(2, 2);
    splitter->setSizes({360, 640, 380});

    headerBar->addWidget(m_stringsBtn);
    headerBar->addStretch(1);

    // AFTER the stretch factors above, which are this tab's chosen default:
    // PanelPersist leaves the sizes untouched when nothing has been
    // remembered, so the default stands until the user has actually dragged
    // something (template §6).
    PanelPersist::bind(splitter, QStringLiteral("files/splitter"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(splitter);

    connect(m_stringsBtn, &QPushButton::clicked, this,
            [this] { showFirstStringTable(); });
    // The panel's table combo and this tree are ONE state shown twice, so the
    // combo drives the tree. Wired here rather than in the panel because the
    // panel knows nothing about file trees — it emits which table was asked
    // for and this tab decides what selecting one means.
    if (StringsPanel* sp = stringsPanel())
        connect(sp, &StringsPanel::tableChosen, this,
                [this](int fileIdx) { selectFile(fileIdx); });

    // Debounced search.
    auto* debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(180);
    connect(m_search, &QLineEdit::textChanged, debounce,
            qOverload<>(&QTimer::start));
    connect(debounce, &QTimer::timeout, this, [this] {
        const QString q = m_search->text().trimmed();
        if (q.isEmpty()) {
            m_stack->setCurrentWidget(m_tree);
            m_matchCount->hide();
            return;
        }
        m_searchModel->setQuery(q);
        m_stack->setCurrentWidget(m_results);
        const int shown = m_searchModel->rowCount(QModelIndex());
        const int total = m_searchModel->totalMatches();
        m_matchCount->setText(shown == total
                                  ? QStringLiteral("%1 matches").arg(total)
                                  : QStringLiteral("%1 matches (showing %2)")
                                        .arg(total)
                                        .arg(shown));
        m_matchCount->show();
    });

    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& cur, const QModelIndex&) {
                showFile(m_treeModel->fileIdxAt(cur));
            });
    connect(m_results->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& cur, const QModelIndex&) {
                showFile(m_searchModel->fileIdxAt(cur));
            });
    connect(m_extractBtn, &QPushButton::clicked, this,
            [this] { exportactions::extractSet(selectedFileIdxs(), this); });
    connect(m_tree, &QTreeView::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                const QModelIndex at = m_tree->indexAt(pos);
                if (!at.isValid()) return;
                QMenu menu(this);
                const int fi = m_treeModel->fileIdxAt(at);
                // §2's selection rule, from util/MenuContext.h — the one
                // implementation. This was hand-rolled here and again in the
                // Models tab, correctly in both places, which is exactly the
                // state a rule is in the day before it drifts.
                const QVector<int> sel = menuctx::contextFiles(
                    m_tree, pos,
                    [this](const QModelIndex& ix) {
                        return m_treeModel->fileIdxAt(ix);
                    });
                if (fi >= 0) {
                    exportactions::addFileSetActions(&menu, sel, this);
                } else {
                    const QVector<int> under = m_treeModel->fileIdxsUnder(at);
                    menu.addAction(
                        MenuText::prompts(MenuText::exportLabel(
                            QStringLiteral("Extract"), under.size(), QString(),
                            exportactions::nounForFiles(under))),
                        this,
                        [this, under] { exportactions::extractSet(under, this); });
                }
                menu.exec(m_tree->viewport()->mapToGlobal(pos));
            });

    // Search results get the same contextual menu.
    m_results->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_results, &QListView::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                const int fi = m_searchModel->fileIdxAt(m_results->indexAt(pos));
                if (fi < 0) return;
                QMenu menu(this);
                const QVector<int> sel = menuctx::contextFiles(
                    m_results, pos,
                    [this](const QModelIndex& ix) {
                        return m_searchModel->fileIdxAt(ix);
                    });
                exportactions::addFileSetActions(&menu, sel, this);
                menu.exec(m_results->viewport()->mapToGlobal(pos));
            });
}

void FilesTab::populateExportMenu(QMenu* menu)
{
    // The strings panel's own contextual entry, when a language table is what
    // is on screen. This came off the menu bar with the Strings tab and had to
    // come back: the panel's "Export TSV…" button still worked, but the
    // application's Export menu offered nothing for a .lng2 at all.
    if (StringsPanel* sp = stringsPanel();
        sp && m_preview && m_preview->showingStrings()) {
        sp->populateExportMenu(menu);
        menu->addSeparator();
    }
    if (m_currentFile >= 0) {
        exportactions::addFileActions(menu, m_currentFile, this,
                                      [this](int fi) { showFile(fi); });
        menu->addSeparator();
    }
    const QVector<int> sel = selectedFileIdxs();
    // The same label the context menu uses, from the same builder — the menu
    // bar and the right-click menu offering differently-worded versions of one
    // action is the drift §12 is about.
    QAction* act = menu->addAction(
        MenuText::prompts(MenuText::exportLabel(
            QStringLiteral("Extract"), sel.size(), QString(),
            exportactions::nounForFiles(sel))),
        this, [this] { exportactions::extractSet(selectedFileIdxs(), this); });
    act->setEnabled(!sel.isEmpty());
}

void FilesTab::onIndexReady(bool ready)
{
    if (!ready) return;
    m_treeModel->rebuildFromIndex();
    m_tree->expandToDepth(0);   // open the archive roots
    if (!m_search->text().trimmed().isEmpty())
        m_searchModel->setQuery(m_search->text());
    // The strings panel re-lists its tables from the new index; the button
    // follows what it found. An install with no .lng2 gets a disabled button
    // and the panel's own explanation of where the tables live, rather than a
    // live button leading to an empty list.
    if (StringsPanel* sp = stringsPanel()) {
        sp->rescan();
        if (m_stringsBtn) m_stringsBtn->setEnabled(sp->hasTables());
    }
}

StringsPanel* FilesTab::stringsPanel() const
{
    return m_preview ? m_preview->stringsPanel() : nullptr;
}

bool FilesTab::selectFile(int fileIdx)
{
    if (fileIdx < 0) return false;
    // The tree, always — the search results are a filtered view and the file
    // asked for may not survive the current query. Switching to the tree is
    // the honest answer to "show me this file".
    m_stack->setCurrentWidget(m_tree);
    const QModelIndex mi = m_treeModel->indexOfFile(fileIdx);
    if (!mi.isValid()) return false;
    // EVERY ancestor, not just the parent: a language table is five folders
    // deep and expanding one level leaves the row collapsed out of sight,
    // where setCurrentIndex selects something the user cannot see.
    for (QModelIndex up = mi.parent(); up.isValid(); up = up.parent())
        m_tree->expand(up);
    m_tree->setCurrentIndex(mi);
    m_tree->scrollTo(mi, QAbstractItemView::PositionAtCenter);
    return true;
}

bool FilesTab::showFirstStringTable()
{
    StringsPanel* sp = stringsPanel();
    if (!sp || sp->tableFiles().isEmpty()) return false;
    // Whichever table the panel is already on, so pressing the button twice
    // does not throw away the table the user had chosen.
    const int want = sp->currentTableFile() >= 0 ? sp->currentTableFile()
                                                 : sp->tableFiles().first();
    return selectFile(want);
}

QVector<int> FilesTab::selectedFileIdxs() const
{
    QVector<int> out;
    if (m_stack->currentWidget() == m_tree) {
        const auto rows = m_tree->selectionModel()->selectedRows(0);
        for (const QModelIndex& mi : rows) {
            const int fi = m_treeModel->fileIdxAt(mi);
            if (fi >= 0) out.append(fi);
            else out += m_treeModel->fileIdxsUnder(mi);
        }
    } else {
        const auto rows = m_results->selectionModel()->selectedRows(0);
        for (const QModelIndex& mi : rows) {
            const int fi = m_searchModel->fileIdxAt(mi);
            if (fi >= 0) out.append(fi);
        }
    }
    if (out.isEmpty() && m_currentFile >= 0) out.append(m_currentFile);
    return out;
}

bool FilesTab::selectFirstResult()
{
    if (!m_searchModel || m_searchModel->rowCount({}) == 0) return false;
    const QModelIndex first = m_searchModel->index(0, 0);
    m_results->setCurrentIndex(first);
    const int fi = m_searchModel->fileIdxAt(first);
    if (fi < 0) return false;
    showFile(fi);
    return true;
}

// The same contract ModelsTab::setPanelsForShot has, so --npanel means
// exactly one thing whichever tab is in front.
QString FilesTab::setPanelsForShot(const QString& keys)
{
    if (!m_npanel) return QStringLiteral("no column");
    const QStringList want =
        keys.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0
            ? QStringList()
            : keys.split(QLatin1Char(','), Qt::SkipEmptyParts);
    QStringList unknown;
    const QStringList known = m_npanel->panelKeys();
    for (const QString& k : want)
        if (!known.contains(k.trimmed())) unknown << k.trimmed();
    // EXACTLY these: a run that asks for INFO alone and gets INFO beside
    // whatever the last session left open has photographed the wrong thing.
    for (const QString& k : known)
        m_npanel->setPanelOpen(k, want.contains(k));
    if (!want.isEmpty()) m_npanel->setColumnOpen(true);
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents();
    QStringList open;
    for (const QString& k : known)
        if (m_npanel->isPanelOpen(k)) open << k;
    QString out = QStringLiteral("%1 open (%2), column %3, %4 px wide")
                      .arg(open.size())
                      .arg(open.isEmpty() ? QStringLiteral("-")
                                          : open.join(QLatin1Char('+')))
                      .arg(m_npanel->columnOpen() ? QStringLiteral("open")
                                                  : QStringLiteral("collapsed"))
                      .arg(m_npanel->width());
    if (!unknown.isEmpty())
        out += QStringLiteral(" — NO SUCH PANEL: %1")
                   .arg(unknown.join(QLatin1Char(',')));
    return out;
}

void FilesTab::showFile(int fileIdx)
{
    m_currentFile = fileIdx;
    m_extractBtn->setEnabled(fileIdx >= 0);
    m_preview->showFile(fileIdx);
    // The column follows the preview from the SAME call, not from a signal the
    // preview emits — one producer, so the two cannot end up describing
    // different files.
    if (m_infoPanel) m_infoPanel->showFile(fileIdx);
}

void FilesTab::setSearchText(const QString& text)
{
    m_search->setText(text);
    // Dev harness: select the first hit so the preview shows something. AFTER
    // the 180 ms search debounce — there is nothing to select before it runs.
    // The selection itself is selectFirstResult(), not a second copy of it:
    // this used to set the current index and nothing else, which moved the
    // list cursor without ever calling showFile(), so the preview and the
    // panel column both stayed empty in every screenshot taken this way.
    QTimer::singleShot(400, this, [this] { selectFirstResult(); });
}
