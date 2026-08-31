#include "view/ModelOutliner.h"
#include <QPixmap>
#include <QPainter>
#include <QImage>
#include <QLabel>
#include <QFile>
#include <QTextStream>
#include <QIcon>
#include "gl/ThumbnailRenderer.h"
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QHeaderView>
#include <QScrollBar>
#include "util/ExportActions.h"
#include "util/TableCopy.h"
#include <algorithm>
#include <QLocale>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QFontMetrics>
#include "view/ViewGlyphs.h"
#include "index/AnimCatalog.h"
#include "index/TexThumbCache.h"
#include "index/ArchiveIndex.h"
#include "index/IconCatalog.h"
#include "index/GameId.h"
#include "fox/FrigFile.h"
#include "anim/RigBind.h"

#include <QAbstractItemModel>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "fox/FmdlFile.h"
#include "fox/FoxMaterial.h"
#include "tabs/ModelsTab.h"

namespace fox {

namespace {
constexpr int kKindRole = Qt::UserRole + 1;
constexpr int kFileRole = Qt::UserRole + 2;
constexpr int kMeshRole = Qt::UserRole + 3;
constexpr int kPathRole = Qt::UserRole + 4;
constexpr int kClipRole = Qt::UserRole + 5;
constexpr int kRoleRole = Qt::UserRole + 6;
// Insertion order, so the loaded model's Mesh / Materials / Armature /
// Animations do not get alphabetised when the user sorts the list by name.
constexpr int kOrderRole = Qt::UserRole + 7;

constexpr int kModel = 2, kPart = 3;
constexpr int kCategory = 4, kLeaf = 5;
constexpr int kMaterial = 6, kTexture = 7, kAnimArchive = 8, kClip = 9;

// ── HOW BIG THE ICONS ARE ────────────────────────────────────────────────
// Derived from the row font so the Ctrl+wheel zoom scales the whole row, and
// quantised so thirteen zoom steps do not become thirteen cache keys per
// model. 1.2 rather than 2.2: at 2.2 a row was ~50px and twelve filled the
// panel, which the user's word for was "bulky".
constexpr qreal kIconOfRow = 1.2;
// Up to 256. It stopped at 64, so dragging the icon column past ~70px did
// nothing — "size is limited". The cache is keyed by (file, size) and these
// are the only sizes it will ever be asked for, so the list can be long
// without the cache becoming one entry per pixel.
constexpr int kIconSteps[] = {16,  20,  24,  32,  40,  48,
                              64,  80,  96, 128, 160, 192, 256};
int quantiseIcon(int px)
{
    for (int s : kIconSteps)
        if (px <= s) return s;
    return kIconSteps[int(sizeof(kIconSteps) / sizeof(kIconSteps[0])) - 1];
}

// A row that sorts by its column text for MODELS and by insertion order for
// everything under a loaded model.
//
// QTreeWidget sorts children as happily as it sorts roots, so turning sorting
// on alphabetised "Animations / Armature / Materials / Mesh" — four headings
// whose order is deliberate — and scrambled the submesh list, whose order IS
// the submesh index. A row that is not a model keeps the order it was built in.
class Row : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem& other) const override
    {
        int col = treeWidget() ? treeWidget()->sortColumn() : ModelOutliner::ColName;
        // Sorting BY the icon column would compare empty strings and leave the
        // list in whatever order it was built in, which reads as "sorting is
        // broken". Clicking it sorts by name instead.
        if (col == ModelOutliner::ColIcon) col = ModelOutliner::ColName;
        const bool meModel = data(0, kKindRole).toInt() == kModel;
        const bool itModel = other.data(0, kKindRole).toInt() == kModel;
        if (!meModel || !itModel)
            return data(0, kOrderRole).toInt() < other.data(0, kOrderRole).toInt();
        const QString a = text(col);
        const QString b = other.text(col);
        const int c = a.compare(b, Qt::CaseInsensitive);
        // A stable tiebreak, so two models with the same name do not swap
        // places every time the list is rebuilt.
        return c != 0 ? c < 0
                      : data(0, kFileRole).toInt() < other.data(0, kFileRole).toInt();
    }
};

const char* const kColKey[ModelOutliner::ColCount] = {
    "icon", "name", "file", "path", "game",
};
const char* const kColTitle[ModelOutliner::ColCount] = {
    "", "Name", "Filename", "Path", "Game",
};
}  // namespace

ModelOutliner::ModelOutliner(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(ColCount);
    QStringList titles;
    for (int c = 0; c < ColCount; ++c)
        titles << QString::fromLatin1(kColTitle[c]);
    m_tree->setHeaderLabels(titles);
    m_tree->setUniformRowHeights(true);
    m_tree->setTextElideMode(Qt::ElideMiddle);
    m_tree->setIndentation(12);
    m_tree->setExpandsOnDoubleClick(true);
    m_tree->setAnimated(false);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(ColName, Qt::AscendingOrder);
    m_tree->header()->setSectionsClickable(true);
    m_tree->header()->setSortIndicatorShown(true);
    m_tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setMinimumSectionSize(16);
    // ── DRAGGING THE ICON COLUMN RESIZES THE PICTURES ───────────────────
    // The column width IS the icon size. Quantised to the cache's steps, so
    // a drag across the whole header does not ask the renderer for sixty
    // different sizes of every model on screen.
    connect(m_tree->header(), &QHeaderView::sectionResized, this,
            [this](int section, int, int now) {
                if (section != ColIcon || m_building) return;
                // The column carries the indent and the expand arrow as well
                // as the picture, so the icon is what is LEFT after those —
                // subtracting a flat 6 cropped every icon by the arrow's width.
                const int room = now - m_tree->indentation() - 10;
                const int want = quantiseIcon(qBound(16, room, 256));
                if (want == m_iconPx) return;
                m_iconPx = want;
                m_tree->setIconSize(QSize(m_iconPx, m_iconPx));
                rebuildGlyphs();
                reicon();
                qInfo("outliner: icon column %d px -> icons %d px", now, m_iconPx);
            });
    // THE HOVER PREVIEW NEEDS THIS. Without mouse tracking a viewport only
    // sends MouseMove while a button is held, so the preview never fired at
    // all — the tab's event filter was correct and was never called. The
    // list/grid view has set it since it had a preview; this did not.
    m_tree->viewport()->setMouseTracking(true);
    m_tree->setMouseTracking(true);
    // ── THE COLUMN TOGGLES ARE ON THE HEADER ────────────────────────────
    // Where every table the user has ever right-clicked keeps them.
    m_tree->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree->header(), &QWidget::customContextMenuRequested, this,
            [this](const QPoint& at) { buildHeaderMenu(at); });

    for (int c = 0; c < ColCount; ++c)
        m_colOn[c] = QSettings()
                         .value(QStringLiteral("models/outlinerCol/%1")
                                    .arg(QString::fromLatin1(kColKey[c])),
                                true)
                         .toBool();
    m_iconMode =
        QSettings().value(QStringLiteral("models/outlinerIcons"), 1).toInt();

    connect(&ThumbnailRenderer::instance(), &ThumbnailRenderer::ready, this,
            [this](int fileIdx, int size) {
                if (size != m_iconPx) return;
                if (!m_loggedFirstIcon) {
                    m_loggedFirstIcon = true;
                    qInfo("outliner: first rendered icon in (file %d, %d px)",
                          fileIdx, size);
                }
                const auto it = m_iconRows.constFind(fileIdx);
                if (it == m_iconRows.constEnd() || !*it) return;
                (*it)->setIcon(0, iconFor(fileIdx));
            });
    connect(&TexThumbCache::instance(), &TexThumbCache::ready, this,
            [this](int fileIdx, int size) {
                if (size == m_iconPx) {
                    const auto it = m_texRows.constFind(fileIdx);
                    if (it != m_texRows.constEnd()) {
                        const QPixmap pm =
                            TexThumbCache::instance().cached(fileIdx, m_iconPx);
                        if (!pm.isNull())
                            for (QTreeWidgetItem* row : *it)
                                if (row) row->setIcon(ColName, QIcon(pm));
                    }
                    // A model row in "the game's own icon" mode is waiting on
                    // this decode too.
                    const auto mr = m_iconRows.constFind(fileIdx);
                    if (mr != m_iconRows.constEnd() && *mr)
                        (*mr)->setIcon(0, iconFor(fileIdx));
                }
                if (size != 128) return;
                const auto it = m_texRows.constFind(fileIdx);
                if (it == m_texRows.constEnd()) return;
                for (QTreeWidgetItem* row : *it)
                    if (row && row->isExpanded()) showChannels(row);
            });

    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& at) {
                QTreeWidgetItem* it = m_tree->itemAt(at);
                QMenu m(this);
                const int kind = it ? it->data(0, kKindRole).toInt() : 0;
                const bool hasFile = it
                                     && (kind == kModel || kind == kTexture
                                         || kind == kAnimArchive || kind == kClip);
                const int fi = hasFile ? it->data(0, kFileRole).toInt() : -1;

                // §2's selection rule: a click inside a multi-row selection
                // acts on the SET.
                QVector<int> selFiles;
                {
                    bool clickedInSel = false;
                    for (QTreeWidgetItem* sel : m_tree->selectedItems()) {
                        if (sel == it) clickedInSel = true;
                        const int k = sel->data(0, kKindRole).toInt();
                        if (k != kModel && k != kTexture && k != kAnimArchive)
                            continue;
                        const int f = sel->data(0, kFileRole).toInt();
                        if (f >= 0 && !selFiles.contains(f)) selFiles.append(f);
                    }
                    if (!clickedInSel || selFiles.size() < 2) selFiles.clear();
                }
                if (!selFiles.isEmpty()) {
                    exportactions::addFileSetActions(&m, selFiles, this);
                    m.addSeparator();
                } else if (kind == kMaterial) {
                    QVector<int> tex;
                    for (int i = 0; i < it->childCount(); ++i) {
                        const QTreeWidgetItem* c = it->child(i);
                        if (c->data(0, kKindRole).toInt() != kTexture) continue;
                        const int f = c->data(0, kFileRole).toInt();
                        if (f >= 0 && !tex.contains(f)) tex.append(f);
                    }
                    if (tex.isEmpty())
                        m.addAction(QStringLiteral(
                                        "No texture of this material is in "
                                        "this install"))
                            ->setEnabled(false);
                    else
                        exportactions::addFileSetActions(&m, tex, this);
                    m.addSeparator();
                } else if (kind == kPart && m_partHook) {
                    m_partHook(&m, it->data(0, kMeshRole).toInt());
                    m.addSeparator();
                }
                if (fi >= 0 && selFiles.isEmpty()) {
                    if (kind == kModel)
                        exportactions::addFileActions(
                            &m, fi, this,
                            [this](int target) { Q_EMIT modelActivated(target); });
                    else
                        exportactions::addFileActions(&m, fi, this);
                    if (kind == kClip) {
                        m.addSeparator();
                        QAction* play = m.addAction(QStringLiteral("Play clip"));
                        const int clip = it->data(0, kClipRole).toInt();
                        connect(play, &QAction::triggered, this,
                                [this, fi, clip] { Q_EMIT clipActivated(fi, clip); });
                    }
                    m.addSeparator();
                }
                tablecopy::addMenuActions(&m, m_tree);
                m.exec(m_tree->viewport()->mapToGlobal(at));
            });
    tablecopy::install(m_tree);
    v->addWidget(m_tree);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    m_iconPx = quantiseIcon(int(QFontMetrics(m_tree->font()).height() * kIconOfRow));
    m_tree->setIconSize(QSize(m_iconPx, m_iconPx));
    rebuildGlyphs();
    applyColumns();

    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
                if (m_building || !cur) return;
                switch (cur->data(0, kKindRole).toInt()) {
                    case kModel:
                        Q_EMIT modelActivated(cur->data(0, kFileRole).toInt());
                        break;
                    case kPart:
                        Q_EMIT partSelected(cur->data(0, kMeshRole).toInt());
                        break;
                    default:
                        break;
                }
            });
    // Clips play on a CLICK, not on the current row changing: a clip costs a
    // decode and a rewind, so arrow-keying an archive would load every one.
    const auto playClipRow = [this](QTreeWidgetItem* it, int) {
        if (!it || it->data(0, kKindRole).toInt() != kClip) return;
        const int fi = it->data(0, kFileRole).toInt();
        const int clip = it->data(0, kClipRole).toInt();
        if (fi < 0 || clip < 0) return;
        qInfo("outliner: play clip %d of archive file %d", clip, fi);
        Q_EMIT clipActivated(fi, clip);
    };
    connect(m_tree, &QTreeWidget::itemClicked, this, playClipRow);
    connect(m_tree, &QTreeWidget::itemActivated, this, playClipRow);
    connect(m_tree, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem* it) {
                showChannels(it);
                requestVisibleIcons();
            });
    connect(m_tree->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { requestVisibleIcons(); });
}

// ── The header's own menu: which columns, and which icons ────────────────
void ModelOutliner::buildHeaderMenu(const QPoint& at)
{
    QMenu m(this);
    m.addSection(QStringLiteral("Columns"));
    for (int c = 0; c < ColCount; ++c) {
        QAction* a = m.addAction(QString::fromLatin1(kColTitle[c]));
        a->setCheckable(true);
        a->setChecked(m_colOn[c]);
        // NAME cannot be hidden: it carries the icon and the expand arrow, and
        // a list whose every row is blank is not a state worth being able to
        // reach.
        if (c == ColName) a->setEnabled(false);
        connect(a, &QAction::toggled, this,
                [this, c](bool on) { setColumnVisible(c, on); });
    }
    m.addSection(QStringLiteral("Icons"));
    static const char* const kModes[] = {
        "No icons", "Rendered", "The game's own", "Both",
    };
    auto* grp = new QActionGroup(&m);
    for (int i = 0; i < 4; ++i) {
        QAction* a = m.addAction(QString::fromLatin1(kModes[i]));
        a->setCheckable(true);
        a->setChecked(m_iconMode == i);
        grp->addAction(a);
        connect(a, &QAction::triggered, this, [this, i] { setIconMode(i); });
    }
    m.exec(m_tree->header()->mapToGlobal(at));
}

void ModelOutliner::setColumnVisible(int col, bool on)
{
    if (col < 0 || col >= ColCount || col == ColName) return;
    m_colOn[col] = on;
    QSettings().setValue(QStringLiteral("models/outlinerCol/%1")
                             .arg(QString::fromLatin1(kColKey[col])),
                         on);
    applyColumns();
}

bool ModelOutliner::columnVisible(int col) const
{
    return col >= 0 && col < ColCount && m_colOn[col];
}

// What the icon column has to be so the picture is not cropped: the icon, the
// indentation Qt applies to column 0, and the expand arrow drawn inside it.
int ModelOutliner::iconColumnWidth() const
{
    return m_iconPx + (m_tree ? m_tree->indentation() : 12) + 10;
}

void ModelOutliner::applyColumns()
{
    if (!m_tree) return;
    for (int c = 0; c < ColCount; ++c) m_tree->setColumnHidden(c, !m_colOn[c]);
    // Sized from the CONTENT once, not on every rebuild: resizeToContents on a
    // 67,000-row tree walks every row of every column.
    m_tree->header()->resizeSection(ColIcon, iconColumnWidth());
    m_tree->header()->resizeSection(ColName, 200);
    m_tree->header()->resizeSection(ColFile, 170);
    m_tree->header()->resizeSection(ColPath, 300);
    m_tree->header()->resizeSection(ColGame, 64);
}

void ModelOutliner::setIconMode(int mode)
{
    mode = qBound(0, mode, 3);
    if (mode == m_iconMode) return;
    m_iconMode = mode;
    QSettings().setValue(QStringLiteral("models/outlinerIcons"), mode);
    qInfo("outliner: icon mode -> %d (0 off, 1 rendered, 2 game, 3 both)", mode);
    for (auto it = m_iconRows.constBegin(); it != m_iconRows.constEnd(); ++it)
        if (it.value()) it.value()->setIcon(0, iconFor(it.key()));
    requestVisibleIcons();
}

// The icon for one model, in whichever mode is on.
//
// "The game's own icon" is the .ftex the game itself shows for the asset,
// which for gear and weapons is a real file in the index — the Customize tab
// already resolves them. It is a DECODE, not a render, so it comes from
// TexThumbCache like every other texture in this view.
QIcon ModelOutliner::iconFor(int fileIdx) const
{
    if (m_iconMode == 0) return QIcon();
    // Mode 2 is the game's icon ONLY, so it never asks for a render. Mode 3
    // needs one as its fallback, so it does.
    const QPixmap rendered =
        m_iconMode == 2 ? QPixmap()
                        : ThumbnailRenderer::instance().cached(fileIdx, m_iconPx);
    QPixmap game;
    if (m_iconMode >= 2) {
        // IconCatalog is the tool's ONE resolver for "the picture the game
        // itself shows for this asset" — the Customize tab has used it since
        // it had icons at all. Keyed by model STEM, because that is what
        // WeaponPartsUiSetting.lua and the equip tables name. It is decoded on
        // demand and cached by (stem, height), and a miss returns a null
        // pixmap rather than blocking a paint.
        const auto it = m_iconRows.constFind(fileIdx);
        QString stem;
        if (it != m_iconRows.constEnd() && *it) stem = (*it)->text(ColName);
        if (!stem.isEmpty())
            game = IconCatalog::instance().iconFor(stem, m_iconPx);
    }
    // MODE 3 IS "RENDERED + GAME", AND GAME WINS. Not two pictures side by
    // side, which is what this drew first: the game's own icon is the more
    // recognisable of the two wherever it exists, and the render is what
    // covers the assets the game ships no icon for. So the mode is a
    // PREFERENCE with a fallback, not a composite — which is also what the
    // user asked for once they saw the composite.
    if (m_iconMode == 3) {
        if (!game.isNull()) return QIcon(game);
        if (!rendered.isNull()) return QIcon(rendered);
        return m_gModel;
    }
    if (!rendered.isNull()) return QIcon(rendered);
    if (!game.isNull()) return QIcon(game);
    return m_gModel;
}

void ModelOutliner::hoverSubjectAt(const QPoint& pos, int* fileIdx,
                                   bool* isTexture) const
{
    if (fileIdx) *fileIdx = -1;
    if (isTexture) *isTexture = false;
    if (!m_tree) return;
    const QTreeWidgetItem* it = m_tree->itemAt(pos);
    if (!it) return;
    const int kind = it->data(0, kKindRole).toInt();
    if (kind != kModel && kind != kTexture) return;
    if (fileIdx) *fileIdx = it->data(0, kFileRole).toInt();
    if (isTexture) *isTexture = (kind == kTexture);
}

QWidget* ModelOutliner::treeViewport() const
{
    return m_tree ? m_tree->viewport() : nullptr;
}

void ModelOutliner::setRowFont(const QFont& f)
{
    if (!m_tree) return;
    m_tree->setFont(f);
    const int want = quantiseIcon(int(QFontMetrics(f).height() * kIconOfRow));
    if (want == m_iconPx) return;
    m_iconPx = want;
    m_tree->setIconSize(QSize(m_iconPx, m_iconPx));
    // …and the column that holds them grows with them, or a zoom makes the
    // icons bigger and then clips them.
    m_building = true;   // not a user drag; must not feed back into the size
    m_tree->header()->resizeSection(ColIcon, iconColumnWidth());
    m_building = false;
    rebuildGlyphs();
    qInfo("outliner: icon size -> %d px (row font height %d px)", m_iconPx,
          QFontMetrics(f).height());
    reicon();
}

void ModelOutliner::setSource(QAbstractItemModel* model)
{
    m_source = model;
    refresh();
}

// ── THE LIST ─────────────────────────────────────────────────────────────
// One row per model, flat. No folders: see the header.
void ModelOutliner::refresh()
{
    QElapsedTimer timer;
    timer.start();
    m_building = true;
    // Sorting OFF while filling. QTreeWidget re-sorts on every insertion with
    // it on, which turns a linear fill into an n log n one with a comparison
    // callback per step — measured at several seconds on an install-sized
    // list, for an order that is thrown away and recomputed at the end anyway.
    const int sortCol = m_tree->sortColumn();
    const Qt::SortOrder sortOrd = m_tree->header()->sortIndicatorOrder();
    m_tree->setSortingEnabled(false);
    m_tree->clear();
    m_iconRows.clear();
    m_texRows.clear();
    m_stripped.clear();
    m_partRows.clear();
    m_loadedRow = nullptr;
    m_models = 0;

    const ArchiveIndex& index = ArchiveIndex::instance();
    const int rows = m_source ? m_source->rowCount(QModelIndex()) : 0;
    QList<QTreeWidgetItem*> made;
    made.reserve(rows);
    for (int r = 0; r < rows; ++r) {
        const QModelIndex ix = m_source->index(r, 0);
        const int fileIdx = ix.data(FmdlListModel::FileIdxRole).toInt();
        if (fileIdx < 0 || fileIdx >= index.files().size()) continue;
        const QString path = index.files()[fileIdx].path;
        const QString file = path.section(QLatin1Char('/'), -1);
        auto* row = new Row(static_cast<QTreeWidget*>(nullptr));
        row->setText(ColName, file.section(QLatin1Char('.'), 0, 0));
        row->setText(ColFile, file);
        row->setText(ColPath, path.section(QLatin1Char('/'), 0, -2));
        row->setText(ColGame,
                     QString::fromLatin1(gameShortName(index.gameOf(index.files()[fileIdx]))));
        row->setToolTip(ColName, path);
        row->setToolTip(ColPath, path);
        row->setData(0, kKindRole, kModel);
        row->setData(0, kFileRole, fileIdx);
        row->setData(0, kPathRole, path);
        row->setData(0, kOrderRole, r);
        if (m_iconMode != 0) row->setIcon(0, iconFor(fileIdx));
        m_iconRows.insert(fileIdx, row);
        made.append(row);
        ++m_models;
    }
    // addTopLevelItems ONCE, not one insert per row: each insert is a model
    // reset on a view with a header, and 67,000 of them is the difference
    // between a list that appears and a window that stops responding.
    m_tree->addTopLevelItems(made);
    const qint64 msFill = timer.elapsed();
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(sortCol < 0 ? ColName : sortCol, sortOrd);
    m_building = false;
    qInfo("outliner: %d model row(s) — fill %lld ms, sort %lld ms, icons %d px "
          "(mode %d)",
          m_models, static_cast<long long>(msFill),
          static_cast<long long>(timer.elapsed() - msFill), m_iconPx, m_iconMode);
    if (m_loadedFile >= 0 && m_loadedPtr)
        setLoadedModel(m_loadedFile, m_loadedPtr);
    else
        requestVisibleIcons();
}

void ModelOutliner::rebuildGlyphs()
{
    constexpr qreal kQuiet = 0.66;
    m_gMesh     = foxglyph::toolIconInset(4,  m_iconPx, kQuiet);
    m_gMaterial = foxglyph::toolIconInset(5,  m_iconPx, kQuiet);
    m_gArmature = foxglyph::toolIconInset(1,  m_iconPx, kQuiet);
    m_gAnim     = foxglyph::toolIconInset(14, m_iconPx, kQuiet);
    m_gBone     = foxglyph::toolIconInset(23, m_iconPx, kQuiet * 0.85);
    m_gPart     = foxglyph::toolIconInset(24, m_iconPx, kQuiet);
    m_gClip     = foxglyph::toolIconInset(22, m_iconPx, kQuiet * 0.85);
    m_gModel    = foxglyph::toolIconAt(13, m_iconPx);
    m_gTexture  = foxglyph::toolIconAt(21, m_iconPx);
}

void ModelOutliner::reicon()
{
    for (auto it = m_iconRows.constBegin(); it != m_iconRows.constEnd(); ++it)
        if (it.value()) it.value()->setIcon(0, iconFor(it.key()));
    for (auto it = m_texRows.constBegin(); it != m_texRows.constEnd(); ++it) {
        const QPixmap pm = TexThumbCache::instance().cached(it.key(), m_iconPx);
        if (pm.isNull()) continue;
        for (QTreeWidgetItem* row : it.value())
            if (row) row->setIcon(ColName, QIcon(pm));
    }
    requestVisibleIcons();
}

// ── The RGBA strip under a texture row ───────────────────────────────────
//
// Six tiles: the image as it is, then R, G, B, A and luminance in isolation.
// That set is not decorative — a Fox SRM packs ambient occlusion, roughness
// and a reflection mask into three unrelated channels, and "which channel
// holds the mask" is a question you answer by LOOKING. The Textures tab has
// had this strip for a while (view/ChannelStrip.h); the outliner showed one
// composite thumbnail and left you to open the other tab to see inside it.
//
// One QLabel in a child row rather than six items: the tiles are one picture,
// they are read as one picture, and six rows of one tile each would be six
// rows of mostly empty column.
void ModelOutliner::showChannels(QTreeWidgetItem* texRow)
{
    if (!texRow || texRow->data(0, kKindRole).toInt() != kTexture) return;
    if (m_stripped.contains(texRow)) return;
    const int fileIdx = texRow->data(0, kFileRole).toInt();
    if (fileIdx < 0) return;
    // Decoded from the SOURCE size, not from the row icon: splitting a 20px
    // thumbnail into channels gives six 20px smudges. 128 is what the Textures
    // tab's own strip works from and is already in the cache for anything the
    // user has looked at there.
    constexpr int kStripSrc = 128;
    const QPixmap src = TexThumbCache::instance().cached(fileIdx, kStripSrc);
    if (src.isNull()) {
        // Not decoded yet. Ask, and let the `ready` handler come back for it —
        // the row stays open and empty for one round trip rather than
        // blocking the GUI thread on a BC7 decode.
        TexThumbCache::instance().request(fileIdx, kStripSrc);
        return;
    }
    m_stripped.insert(texRow);
    const QImage img = src.toImage().convertToFormat(QImage::Format_ARGB32);
    const int tile = qMax(16, m_iconPx);
    struct Chan { const char* label; int which; };   // -1 = as-is, 4 = luma
    static const Chan kChans[] = {{"RGB", -1}, {"R", 0}, {"G", 1},
                                  {"B", 2},    {"A", 3}, {"L", 4}};
    const int n = int(sizeof(kChans) / sizeof(kChans[0]));
    const int gap = 2;
    QPixmap strip(n * tile + (n - 1) * gap, tile);
    strip.fill(Qt::transparent);
    {
        QPainter p(&strip);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        for (int i = 0; i < n; ++i) {
            QImage one = img;
            if (kChans[i].which >= 0) {
                one = QImage(img.size(), QImage::Format_ARGB32);
                for (int y = 0; y < img.height(); ++y) {
                    const QRgb* in = reinterpret_cast<const QRgb*>(img.constScanLine(y));
                    QRgb* out = reinterpret_cast<QRgb*>(one.scanLine(y));
                    for (int x = 0; x < img.width(); ++x) {
                        const QRgb c = in[x];
                        int v = 0;
                        switch (kChans[i].which) {
                            case 0: v = qRed(c); break;
                            case 1: v = qGreen(c); break;
                            case 2: v = qBlue(c); break;
                            case 3: v = qAlpha(c); break;
                            default: v = qGray(c); break;
                        }
                        out[x] = qRgb(v, v, v);
                    }
                }
            }
            p.drawImage(QRect(i * (tile + gap), 0, tile, tile),
                        one.scaled(tile, tile, Qt::KeepAspectRatioByExpanding,
                                   Qt::SmoothTransformation));
        }
    }
    auto* row = new Row(texRow);
    row->setData(0, kKindRole, kLeaf);
    row->setFlags(Qt::ItemIsEnabled);
    auto* holder = new QLabel(m_tree);
    holder->setPixmap(strip);
    holder->setToolTip(QStringLiteral(
        "As-is, then red, green, blue, alpha and luminance.\n\nA Fox SRM packs "
        "ambient occlusion, roughness and a reflection mask into R, G and B — "
        "which channel holds what is a question you answer by looking."));
    // ColName, not column 0: the strip is ~110px of picture and the icon
    // column is icon-width. In column 0 it was clipped to a sliver.
    row->setSizeHint(ColName, QSize(strip.width() + 8, strip.height() + 4));
    m_tree->setItemWidget(row, ColName, holder);
}

// Ask for what is ON SCREEN, and nothing else. A flat list of every model in
// the install would otherwise queue one offscreen render per model.
void ModelOutliner::requestVisibleIcons()
{
    if (!m_tree || !m_tree->viewport() || m_iconMode == 0) return;
    const int h = m_tree->viewport()->height();
    if (h <= 0) return;
    QTreeWidgetItem* it = m_tree->itemAt(0, 0);
    int guard = 0, asked = 0;
    while (it && guard++ < 4000) {
        if (m_tree->visualItemRect(it).top() > h) break;
        const int kind = it->data(0, kKindRole).toInt();
        const int fi = it->data(0, kFileRole).toInt();
        if (kind == kModel && fi >= 0) {
            if (m_iconMode != 2
                && ThumbnailRenderer::instance().cached(fi, m_iconPx).isNull()) {
                ThumbnailRenderer::instance().request(fi, m_iconPx);
                ++asked;
            }
            // Mode 2/3's game icon needs no request: IconCatalog decodes on
            // demand and caches, and iconFor() above already asked for it.
        } else if (kind == kTexture && fi >= 0
                   && TexThumbCache::instance().cached(fi, m_iconPx).isNull()) {
            TexThumbCache::instance().request(fi, m_iconPx);
            ++asked;
        }
        it = m_tree->itemBelow(it);
    }
    if (asked > 0)
        qInfo("outliner: %d picture(s) requested at %d px for the rows on screen",
              asked, m_iconPx);
}

void ModelOutliner::setShowLoadedTree(bool on)
{
    if (on == m_showTree) return;
    m_showTree = on;
    // ── AND THEY HAVE TO LOOK DIFFERENT ─────────────────────────────────
    // With no model loaded the two modes were the same pixels, so switching
    // between them looked like nothing happening — "display modes not
    // switching". LIST is a plain table with no expand column at all;
    // OUTLINER keeps the arrows, because in it rows really do open.
    if (m_tree) m_tree->setRootIsDecorated(on);
    // Re-apply: turning it off must take an already-built subtree down, and
    // turning it on must put one up without waiting for the next model load.
    setLoadedModel(m_loadedFile, m_loadedPtr);
}

void ModelOutliner::setLoadedModel(int fileIdx, const FmdlFile* model)
{
    m_building = true;
    // ── THE SUBTREE KEEPS ITS OWN ORDER ─────────────────────────────────
    // Sorting is on for the model list, and QTreeWidget sorts CHILDREN just
    // as happily — which alphabetised "Mesh · Materials · Armature ·
    // Animations" into "Animations · Armature · Materials · Mesh" and would
    // have scrambled the submesh list, whose order IS the submesh index.
    // Row::operator< falls back to this stamp for anything that is not a
    // model row, so creation order is sort order down here.
    int order = 0;
    const auto stamp = [&order](QTreeWidgetItem* it) {
        it->setData(0, kOrderRole, order++);
        return it;
    };
    // Take the previous model's parts back down. The row itself stays — it is
    // a model in the list like any other — but a tree carrying the parts of
    // three models at once is a tree that says three things are loaded.
    if (m_loadedRow) {
        while (m_loadedRow->childCount() > 0)
            delete m_loadedRow->takeChild(0);
        // …and it stops being the bold one. A list with two bold rows is a
        // list that claims two models are open.
        QFont f = m_loadedRow->font(ColName);
        f.setBold(false);
        for (int c = 0; c < ColCount; ++c) m_loadedRow->setFont(c, f);
    }
    m_partRows.clear();
    // The texture rows live under Materials, which is under the loaded row —
    // so they have just been deleted and every pointer in here is dangling.
    // Forgetting this is how the `ready` handler would write an icon into
    // freed memory the next time a decode landed.
    m_texRows.clear();
    m_stripped.clear();
    m_loadedFile = fileIdx;
    m_loadedPtr = model;
    m_loadedRow = nullptr;
    // Ask the INDEX which folder holds it and populate that one, rather than
    // walking the tree for a row that the lazy build may not have made yet.
    // The row is already in the flat list — the fill kept a fileIdx → row map,
    // so this is a lookup and not a walk of the tree.
    if (fileIdx >= 0) m_loadedRow = m_iconRows.value(fileIdx, nullptr);
    // ── THE LOADED MODEL IS THE BOLD ROW ────────────────────────────────
    // The category headings under it used to be bold, which put the emphasis
    // on four labels that read the same on every model and left the one row
    // saying WHICH model is open looking like all the others. The user's
    // words: "the model itself to expand from should be bold."
    //
    // OUTSIDE the subtree block, so it is true in List mode too, where there
    // is no subtree but there is still a model open.
    if (m_loadedRow) {
        QFont f = m_loadedRow->font(ColName);
        f.setBold(true);
        for (int c = 0; c < ColCount; ++c) m_loadedRow->setFont(c, f);
    }
    // "The outliner shows no model" has three causes — never told, told about
    // a file the current filter excludes, and told but the row could not be
    // made — and no log line separated them. Nothing loaded is not one of
    // them, so it does not get a line that reads like a failure.
    if (fileIdx >= 0)
        qInfo("outliner: model %d %s — its row was %s", fileIdx,
              model ? "loaded" : "cleared",
              m_loadedRow ? "found" : "NOT FOUND (outside this filter?)");
    if (m_loadedRow && model && m_showTree) {
        const auto& meshes = model->meshes();
        const auto& mats = model->materials();
        const auto& groups = model->meshGroups();
        const auto& bones = model->bones();

        // A category row: a heading that carries its own count, so the tree
        // answers "how many materials does this have" without being opened.
        const auto category = [this, &stamp](const QString& name, int n,
                                            const QIcon& glyph) {
            auto* c = new Row(m_loadedRow);
            stamp(c);
            c->setText(ColName, QStringLiteral("%1  ·  %2").arg(name).arg(n));
            c->setData(0, kKindRole, kCategory);
            // The cached glyph at the row's own size, quiet-weighted. It was
            // foxglyph::toolIcon(n) — an 18px pixmap, freshly painted per
            // call, upscaled by Qt into a 32-96px decoration box.
            c->setIcon(ColName, glyph);
            return c;
        };
        // Every list here is BOUNDED. A character has 290 bones and some Fox
        // models have far more; a tree that adds one row per bone turns the
        // outliner into a scroll wall and makes the categories below it
        // unreachable. The cap says how many it did not draw rather than
        // silently stopping — a truncated list that does not say so is a list
        // that looks complete.
        const auto leaf = [&stamp](QTreeWidgetItem* parent, const QString& text,
                                   const QString& tip = QString()) {
            auto* l = stamp(new Row(parent));
            l->setText(ColName, text);
            l->setData(0, kKindRole, kLeaf);
            if (!tip.isEmpty()) l->setToolTip(ColName, tip);
            return l;
        };
        constexpr int kMaxLeaves = 64;
        const auto capped = [&leaf](QTreeWidgetItem* parent, int shown, int total) {
            if (total > shown)
                leaf(parent, QStringLiteral("…and %1 more").arg(total - shown))
                    ->setDisabled(true);
        };

        // ── Mesh ────────────────────────────────────────────────────────
        // The part rows, which are the ones the viewport talks to. They keep
        // kPart and m_partRows, so two-way selection is unchanged — they have
        // simply moved one level down, under a heading.
        QTreeWidgetItem* meshCat =
            category(QStringLiteral("Mesh"), int(meshes.size()), m_gMesh);
        for (int i = 0; i < meshes.size(); ++i) {
            const FmdlMesh& m = meshes[i];
            // A submesh has no authored name in an FMDL, so its MATERIAL name
            // is the only human-readable label there is — the same string the
            // parts panel shows, from the same source, so the two views of one
            // submesh cannot call it different things.
            QString label;
            if (m.materialInstanceIndex >= 0
                && m.materialInstanceIndex < mats.size())
                label = mats[m.materialInstanceIndex].name;
            if (label.isEmpty() && m.meshGroupIndex >= 0
                && m.meshGroupIndex < groups.size())
                label = groups[m.meshGroupIndex].name;
            if (label.isEmpty()) label = QStringLiteral("submesh %1").arg(i);
            auto* row = new Row(meshCat);
            stamp(row);
            row->setText(ColName, QStringLiteral("%1  ·  %2 tris")
                                .arg(label)
                                .arg(QLocale().toString(
                                    qint64(m.triangles.size() / 3))));
            row->setIcon(ColName, m_gPart);
            row->setData(0, kKindRole, kPart);
            row->setData(0, kMeshRole, i);
            row->setToolTip(ColName, QStringLiteral("Submesh %1 · %2 triangles")
                                   .arg(i)
                                   .arg(m.triangles.size() / 3));
            m_partRows.insert(i, row);
        }

        // ── Materials ───────────────────────────────────────────────────
        QTreeWidgetItem* matCat = category(QStringLiteral("Materials"),
                                           int(mats.size()), m_gMaterial);
        {
            const ArchiveIndex& index = ArchiveIndex::instance();
            int texRows = 0, texResolved = 0, texCached = 0;
            for (int i = 0; i < mats.size() && i < kMaxLeaves; ++i) {
                const FmdlMaterialInstance& mat = mats[i];
                // A BRANCH now, not a leaf: "3 texture(s)" was a number you
                // could not act on, and the maps a material binds are the
                // thing anyone opening a material wants to see.
                auto* mrow = new Row(matCat);
                stamp(mrow);
                mrow->setText(ColName, QStringLiteral("%1  ·  %2 texture(s)")
                                     .arg(mat.name.isEmpty()
                                              ? QStringLiteral("material %1").arg(i)
                                              : mat.name)
                                     .arg(mat.textures.size()));
                mrow->setIcon(ColName, m_gMaterial);
                mrow->setData(0, kKindRole, kMaterial);
                mrow->setToolTip(ColName, mat.shader);
                for (const FmdlTextureRef& t : mat.textures) {
                    // findByHash, NOT fileIndexForPath — a texture ref's
                    // `path` is the resolved NAME and carries no extension,
                    // so the path lookup returns -1 for every row whose name
                    // DID resolve. This is the same scar FileInfoPanel.cpp
                    // records; the hash is what ModelLoader itself uses to
                    // fetch the pixels.
                    int fi = -1;
                    if (t.pathHash) {
                        const IndexedFile* f = index.findByHash(t.pathHash);
                        if (f) {
                            const qsizetype at = f - index.files().constData();
                            if (at >= 0 && at < index.files().size())
                                fi = int(at);
                        }
                    }
                    // The NAME, not the whole path: the row is already
                    // indented four levels and the path is on the tooltip.
                    QString name = t.path;
                    const int cut = name.lastIndexOf(QLatin1Char('/'));
                    if (cut >= 0) name = name.mid(cut + 1);
                    if (name.isEmpty())
                        name = t.pathHash
                                   ? QStringLiteral("0x%1").arg(t.pathHash, 0, 16)
                                   : QStringLiteral("(unnamed)");
                    auto* trow = new Row(mrow);
                    stamp(trow);
                    // WHAT THE SLOT IS FOR, then what is in it. The raw role
                    // ("SpecularMap_Tex_LIN") is the name the file uses and is
                    // the right thing to export under; it is the wrong thing
                    // to read down a column, which is what the user asked to
                    // have fixed. The raw name is on the tooltip.
                    trow->setText(ColName, QStringLiteral("%1  ·  %2")
                                         .arg(fox::texrole::roleDisplayName(t.roleHash32,
                                                    t.role),
                                              name));
                    trow->setIcon(ColName, m_gTexture);
                    trow->setData(0, kKindRole, kTexture);
                    trow->setData(0, kRoleRole, t.role);
                    trow->setData(0, kFileRole, fi);
                    trow->setToolTip(
                        0, QStringLiteral("%1\n%2\n%3")
                               .arg(t.role, t.path.isEmpty()
                                                ? QStringLiteral("(name not in "
                                                                 "the dictionary)")
                                                : t.path,
                                    fi >= 0
                                        ? index.files()[fi].path
                                        : QStringLiteral("not in this install")));
                    ++texRows;
                    // A texture this install does not carry gets NO preview
                    // and says so, rather than an empty square that reads as
                    // a failed decode.
                    if (fi < 0) {
                        trow->setDisabled(true);
                        continue;
                    }
                    ++texResolved;
                    // It opens — into its channels. Forced, because the child
                    // row does not exist until the user asks for it.
                    trow->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
                    m_texRows[fi].append(trow);
                    // Cache only, same as the model rows: a model with 41
                    // texture rows inside a collapsed Materials heading should
                    // decode nothing until the heading is opened.
                    const QPixmap pm =
                        TexThumbCache::instance().cached(fi, m_iconPx);
                    if (!pm.isNull()) { trow->setIcon(ColName, QIcon(pm)); ++texCached; }
                }
            }
            capped(matCat, kMaxLeaves, int(mats.size()));
            // Always on and bounded. "Resolved" is the number that matters:
            // a model whose every texture row is greyed out is either a GZ
            // model (whose refs carry no path hash at all) or an install
            // missing its texture archives, and those are different problems.
            qInfo("outliner: materials — %lld material(s), %d texture row(s), "
                  "%d resolved to a file, %d already decoded at %d px",
                  static_cast<long long>(mats.size()), texRows, texResolved,
                  texCached, m_iconPx);
        }

        // ── Armature ────────────────────────────────────────────────────
        // The BONE COUNT is the fact worth having at a glance — it is what
        // says whether a thing is rigged at all — so it is in the heading and
        // not only in the rows underneath.
        QTreeWidgetItem* boneCat = category(QStringLiteral("Armature"),
                                            int(bones.size()), m_gArmature);
        for (int i = 0; i < bones.size() && i < kMaxLeaves; ++i)
            leaf(boneCat, bones[i].name.isEmpty()
                              ? QStringLiteral("bone %1").arg(i)
                              : bones[i].name)
                ->setIcon(ColName, m_gBone);
        capped(boneCat, kMaxLeaves, int(bones.size()));

        // ── Animations ──────────────────────────────────────────────────
        // From the catalogue's own scope for this model, which is the same
        // answer the ANIMATIONS panel gives — asking it twice from two places
        // is how the panel and the tree come to disagree about how many clips
        // a model has.
        {
            // Scored through animBindScore/animBindCeiling — THE scoring
            // function, the same one the ANIMATIONS panel's "This model's
            // animations" scope uses. Asking the question a second way here is
            // how the panel and the tree come to disagree about how many clips
            // a model has.
            // modelBoneHashes(), not a hand-rolled set. I wrote the loop out
            // here first and it scored ZERO archives against a model that
            // --animbind scores 71 — the helper is the one implementation of
            // "which bones does this model have, as the animation system sees
            // them", and reproducing it is reproducing a bug.
            const QVector<quint32> boneHashes = modelBoneHashes(*model);
            // THE RIG. Without it this scored 0.000 against every one of 108
            // archives on a model --animbind scores 71 of them at ~1.0 — and
            // the reason is the whole point of the .frig: with no rig the
            // score is a direct bone-name match, and with one it goes through
            // resolveBoneDrives, which is what actually decides whether a clip
            // can pose a model. Passing nullptr is not "no rig available", it
            // is "pretend this model has none".
            //
            // Loaded the same way --animbind loads it, through rigbind, so the
            // tree and the probe answer the same question.
            fox::FrigFile frig;
            QString via;
            const bool haveFrig =
                m_loadedRow
                && rigbind::loadFrigFor(m_loadedRow->data(0, kPathRole).toString(),
                                        &frig, &via);
            const FrigFile* rig = haveFrig ? &frig : nullptr;
            const int ceiling = animBindCeiling(boneHashes, rig);
            const AnimCatalog& ac = AnimCatalog::instance();
            QVector<int> hits;
            int clips = 0;
            float best = 0.0f;
            if (ceiling > 0) {
                for (int i = 0; i < ac.archives().size(); ++i) {
                    const float sc = animBindScore(ac.archives()[i], boneHashes,
                                                   rig, nullptr, ceiling);
                    best = qMax(best, sc);
                    if (sc < animBindThreshold()) continue;
                    hits.append(i);
                    clips += int(ac.archives()[i].clips.size());
                }
            }
            QTreeWidgetItem* animCat =
                category(QStringLiteral("Animations"), clips, m_gAnim);
            // Each binding archive is a BRANCH of its clips, and a clip row
            // plays. The archive rows were inert leaves saying "42 clip(s)" —
            // the outliner could tell you an animation existed and gave you
            // nowhere to press.
            //
            // Bounded on both axes: kMaxLeaves archives, and kMaxClipRows
            // clips inside each. Some TPP archives carry several hundred
            // clips and a model that binds forty of those archives would
            // otherwise put tens of thousands of rows under one heading.
            constexpr int kMaxClipRows = 200;
            int clipRows = 0;
            for (int n = 0; n < hits.size() && n < kMaxLeaves; ++n) {
                const AnimArchive& a = ac.archives()[hits[n]];
                auto* arow = new Row(animCat);
                stamp(arow);
                arow->setText(ColName, QStringLiteral("%1  ·  %2 clip(s)")
                                     .arg(a.stem)
                                     .arg(a.clips.size()));
                arow->setIcon(ColName, m_gAnim);
                arow->setData(0, kKindRole, kAnimArchive);
                arow->setData(0, kFileRole, a.fileIdx);
                arow->setToolTip(ColName, a.path);
                for (int c = 0; c < a.clips.size() && c < kMaxClipRows; ++c) {
                    const AnimClip& clip = a.clips[c];
                    auto* crow = new Row(arow);
                    stamp(crow);
                    // The EXPANDED label, which is what the ANIMATIONS panel
                    // and the clip combo both show — the raw .gani stem is on
                    // the tooltip. Two views naming one clip differently is
                    // the bug this whole batch is also fixing elsewhere.
                    crow->setText(ColName, clip.label.isEmpty() ? clip.name
                                                          : clip.label);
                    crow->setIcon(ColName, m_gClip);
                    crow->setData(0, kKindRole, kClip);
                    crow->setData(0, kFileRole, a.fileIdx);
                    crow->setData(0, kClipRole, clip.index);
                    crow->setToolTip(ColName, QStringLiteral("%1\nclip %2 of %3")
                                            .arg(clip.name)
                                            .arg(clip.index)
                                            .arg(a.stem));
                    ++clipRows;
                }
                if (a.clips.size() > kMaxClipRows)
                    leaf(arow, QStringLiteral("…and %1 more")
                                   .arg(int(a.clips.size()) - kMaxClipRows))
                        ->setDisabled(true);
            }
            capped(animCat, kMaxLeaves, int(hits.size()));
            qInfo("outliner: anims — %d clip row(s) built under %lld archive(s)",
                  clipRows,
                  static_cast<long long>(qMin<qsizetype>(hits.size(), kMaxLeaves)));
            // Always on and bounded (convention 7). This category read 0
            // against a model --animbind scores 71 on, twice, for two
            // different reasons — so it says what it saw.
            qInfo("outliner: anims — %lld archive(s) in the catalogue, "
                  "ceiling %d, rig %s, best %.3f, %lld bind(s), %d clip(s)",
                  static_cast<long long>(ac.archives().size()), ceiling,
                  haveFrig ? qUtf8Printable(via) : "none", double(best),
                  static_cast<long long>(hits.size()), clips);

            // An empty category is a question, so it answers it. The two
            // reasons are different and the user can act on one of them:
            // a one-bone prop can never be posed, and a rigged model with no
            // matching archive means this install does not ship its motions.
            if (hits.isEmpty())
                leaf(animCat,
                     ceiling <= 0
                         ? QStringLiteral("no rig — nothing can pose this")
                         : QStringLiteral("no archive in this install binds to "
                                          "this rig"))
                    ->setDisabled(true);
        }

        m_loadedRow->setExpanded(true);
        // Its ancestors too: a loaded model whose parts are inside a collapsed
        // folder is a tree that appears not to have loaded anything.
        for (QTreeWidgetItem* p = m_loadedRow->parent(); p; p = p->parent())
            p->setExpanded(true);
        m_tree->scrollToItem(m_loadedRow);
    }
    m_building = false;
    requestVisibleIcons();
}

QString ModelOutliner::dumpTreeForShot(const QString& outPath)
{
    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QStringLiteral("could not write %1").arg(outPath);
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << "depth\tkind\tname\tfile\tpath\tgame\tchildren\texpanded\n";
    int rows = 0;
    QVector<QPair<QTreeWidgetItem*, int>> stack;
    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i)
        stack.append({m_tree->topLevelItem(i), 0});
    while (!stack.isEmpty()) {
        const auto [it, depth] = stack.takeLast();
        out << depth << '\t' << it->data(0, kKindRole).toInt() << '\t'
            << it->text(ColName) << '\t' << it->text(ColFile) << '\t'
            << it->text(ColPath) << '\t' << it->text(ColGame) << '\t'
            << it->childCount() << '\t' << (it->isExpanded() ? 1 : 0) << '\n';
        ++rows;
        // Only the loaded model has children now, so this cannot run away.
        if (depth < 6)
            for (int i = it->childCount() - 1; i >= 0; --i)
                stack.append({it->child(i), depth + 1});
    }
    f.close();
    return QStringLiteral("%1 (%2 row(s), %3 model(s))")
        .arg(outPath).arg(rows).arg(m_models);
}

QString ModelOutliner::probeForShot(const QString& spec)
{
    if (!m_loadedRow) return QStringLiteral("no model loaded in the outliner");
    const QStringList want = spec.split(QLatin1Char(','), Qt::SkipEmptyParts);
    m_texRowsExpanded.clear();
    QStringList opened;
    QTreeWidgetItem* firstClip = nullptr;
    int clipRows = 0, texRows = 0;
    for (int i = 0; i < m_loadedRow->childCount(); ++i) {
        QTreeWidgetItem* cat = m_loadedRow->child(i);
        // The heading text is "Materials  (2)" — match on the leading word so
        // the spec does not have to know the count.
        // ColName, not column 0 — the text moved off the icon column.
        const QString head =
            cat->text(ColName).section(QLatin1Char(' '), 0, 0).toLower();
        if (!want.contains(head, Qt::CaseInsensitive)) continue;
        cat->setExpanded(true);
        opened << head;
        for (int j = 0; j < cat->childCount(); ++j) {
            QTreeWidgetItem* row = cat->child(j);
            // One level further: the material and archive rows are themselves
            // branches now, and their children are the point of the probe.
            row->setExpanded(true);
            for (int k = 0; k < row->childCount(); ++k) {
                QTreeWidgetItem* leaf = row->child(k);
                const int kind = leaf->data(0, kKindRole).toInt();
                if (kind == kClip) { ++clipRows; if (!firstClip) firstClip = leaf; }
                else if (kind == kTexture) {
                    ++texRows;
                    // …and one level further for textures: the channel strip
                    // is what a shot of this needs to show, and it does not
                    // exist until the row is opened.
                    leaf->setExpanded(true);
                    // Only rows that HAVE a file: an unresolved texture never
                    // gets a strip, so counting it as pending waits the full
                    // three seconds for a decode that was never asked for.
                    if (leaf->data(0, kFileRole).toInt() >= 0)
                        m_texRowsExpanded.append(leaf);
                    showChannels(leaf);
                }
            }
        }
    }
    requestVisibleIcons();
    // A channel strip needs a 128px decode that showChannels ASKS for and does
    // not wait on — right, for the GUI, and useless for a screenshot, which is
    // taken before the worker answers. The probe waits; nothing else does.
    {
        QElapsedTimer wait;
        wait.start();
        int pending = 0;
        do {
            pending = 0;
            for (QTreeWidgetItem* row : std::as_const(m_texRowsExpanded))
                if (row && !m_stripped.contains(row)) {
                    showChannels(row);
                    if (!m_stripped.contains(row)) ++pending;
                }
            if (!pending) break;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        } while (wait.elapsed() < 3000);
        if (pending)
            qInfo("outliner: %d channel strip(s) still waiting on a decode "
                  "after %lld ms", pending, static_cast<long long>(wait.elapsed()));
    }
    QString played = QStringLiteral("no");
    if (want.contains(QLatin1String("play"), Qt::CaseInsensitive) && firstClip) {
        m_tree->setCurrentItem(firstClip);
        m_tree->scrollToItem(firstClip);
        Q_EMIT clipActivated(firstClip->data(0, kFileRole).toInt(),
                             firstClip->data(0, kClipRole).toInt());
        played = firstClip->text(ColName);
    }
    return QStringLiteral("expanded %1 · %2 clip row(s), %3 texture row(s) "
                          "reachable · played %4")
        .arg(opened.isEmpty() ? QStringLiteral("nothing")
                              : opened.join(QLatin1Char('+')))
        .arg(clipRows)
        .arg(texRows)
        .arg(played);
}

void ModelOutliner::selectPart(int meshId)
{
    auto it = m_partRows.constFind(meshId);
    if (it == m_partRows.constEnd()) return;
    m_building = true;   // this is the viewport telling US, not the reverse
    m_tree->setCurrentItem(*it);
    m_tree->scrollToItem(*it);
    m_building = false;
}

}  // namespace fox
