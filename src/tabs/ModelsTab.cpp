// ModelsTab.cpp — see ModelsTab.h.
#include "tabs/ModelsTab.h"
#include <cmath>

#include "util/HoverPreview.h"
#include "util/RowShading.h"
#include "index/ModelTags.h"
#include "util/PanelPersist.h"
#include "view/TipBar.h"
#include "util/SearchBox.h"
#include "util/SearchQuery.h"
#include "util/MenuContext.h"
#include "util/TagFilterPopup.h"

#include <algorithm>

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QAbstractItemView>
#include <QListView>
#include <QSignalBlocker>
#include <QMouseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSet>
#include <QSlider>
#include <QSplitter>
#include <QTimer>
#include <QClipboard>
#include <QMessageBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QCoreApplication>
#include <QEventLoop>
#include <QPainter>
#include <QSettings>
#include <QActionGroup>
#include "index/IconCatalog.h"
#include "index/TexThumbCache.h"
#include <QScrollBar>
#include <QWheelEvent>

#include "anim/AnimPose.h"
#include "app/Config.h"
#include "app/StatusLine.h"
#include "util/ExportLayout.h"
#include "util/MenuText.h"
#include "app/ExportNotifier.h"
#include "app/Hotkeys.h"
#include "fox/BcDecode.h"
#include "fox/FmdlFile.h"
#include "fox/FoxHash.h"
#include "gl/GLModelWidget.h"
#include "view/ViewGlyphs.h"
#include "export/ExportOptions.h"
#include "export/ViewCapture.h"
#include "view/ViewportBar.h"
#include "view/ViewportPanel.h"
#include "index/AnimCatalog.h"
#include "util/AnimCombo.h"
#include "index/ArchiveIndex.h"
#include "gl/ThumbnailRenderer.h"
#include "index/GameId.h"
#include "util/ShadowDisplay.h"
#include "model/GlbExporter.h"
#include "anim/RigBind.h"
#include "preview/ModelLoader.h"
#include "util/Extract.h"
#include "util/ExportActions.h"

#include <QItemSelection>
#include <QMenu>
#include <QSaveFile>

using fox::ArchiveIndex;
using fox::IndexedFile;

// ── FmdlListModel ────────────────────────────────────────────────────────────

FmdlListModel::FmdlListModel(QObject* parent) : QAbstractListModel(parent) {}

void FmdlListModel::refresh(const QString& query)
{
    beginResetModel();
    m_rows.clear();
    m_total = 0;
    const ArchiveIndex& index = ArchiveIndex::instance();
    if (index.ready()) {
        const searchq::Query q(query);
        const fox::ModelTags& tags = fox::ModelTags::instance();
        const QStringList mustTags = q.mustTags();
        const auto& files = index.files();
        for (int i = 0; i < files.size(); ++i) {
            if (ArchiveIndex::extensionOf(files[i]) != QLatin1String("fmdl")) continue;
            // Every model in the index, counted BEFORE any filter — so the
            // footer's denominator means one thing always. Counting after the
            // game gate made the total grow when a game tag was ticked, which
            // is the one direction a denominator must never move.
            ++m_total;
            // The game switches ARE tags now, so a game tag in the query
            // overrides the app-wide filter for this list — asking for #mgo3
            // while MGO is switched off should show MGO, not nothing. With no
            // game tag typed, the app-wide filter applies as it always did.
            if (!tags.categoryHasAny(mustTags, QStringLiteral("game"))
                && !fox::GameFilter::instance().enabled(index.gameOf(files[i])))
                continue;
            if (!fox::queryMatchesFile(q, i, files[i])) continue;
            m_rows.append(i);
        }
    }
    endResetModel();
}

int FmdlListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant FmdlListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const fox::IndexedFile& f = ArchiveIndex::instance().files()[m_rows[index.row()]];
    if (role == FileIdxRole) return m_rows[index.row()];
    if (role == StemRole)
        return f.path.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
    if (role == DirRole) return f.path.section(QLatin1Char('/'), 0, -2);
    if (role == Qt::ToolTipRole) return f.path;
    const QVariant sh = shadowui::roleFor(f, role, f.path);
    if (sh.isValid()) return sh;
    if (role == Qt::DisplayRole) return f.path;
    return {};
}

// How long a scroll must settle before anything is queued, and how long
// finished thumbnails are collected before the grid repaints. Repainting per
// arrival would mean a full viewport repaint forty times a page for no visible
// difference.
static constexpr int kThumbSettleMs = 90;
static constexpr int kThumbRepaintMs = 40;

ModelGridDelegate::ModelGridDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

QSize ModelGridDelegate::cellSize() const
{
    // Icon, then two lines of caption plus padding. Fixed rather than measured
    // per cell so the grid stays on a regular pitch while it scrolls.
    return QSize(m_icon + 18, m_icon + 40);
}

QSize ModelGridDelegate::sizeHint(const QStyleOptionViewItem&,
                                  const QModelIndex&) const
{
    return cellSize();
}

void ModelGridDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const
{
    painter->save();
    // NO BANDING. A wrapped icon view has no notion of a visual row, so this
    // used to derive one — columns from the viewport width, row from the model
    // index — and paint every second line of tiles a shade darker, the way a
    // list bands its rows.
    //
    // It is wrong for a grid, and the reason is what the tiles CONTAIN. Every
    // cell holds a render on its own background; a band behind half of them
    // means the same model reads as sitting on a different colour depending on
    // where it happens to land in the wrap, and the wrap moves every time the
    // panel is resized. In a list the band is a reading aid across a long row;
    // in a grid there is nothing to read across, and it is just two
    // backgrounds for one kind of thing.
    const bool sel = option.state & QStyle::State_Selected;
    if (sel) {
        // A tint and an outline, not a solid fill. The cell's whole point is
        // the render inside it, and painting the highlight colour edge to edge
        // buries it — the selection has to read without competing with the
        // thing being selected.
        painter->setRenderHint(QPainter::Antialiasing, true);
        QColor fill = option.palette.highlight().color();
        fill.setAlphaF(0.22);
        QColor edge = option.palette.highlight().color();
        edge.setAlphaF(0.85);
        painter->setPen(QPen(edge, 1.0));
        painter->setBrush(fill);
        painter->drawRoundedRect(
            QRectF(option.rect.adjusted(2, 2, -2, -2)).adjusted(0.5, 0.5, -0.5, -0.5),
            4, 4);
    }
    const int fileIdx = index.data(FmdlListModel::FileIdxRole).toInt();
    const QRect iconRect(option.rect.left() + (option.rect.width() - m_icon) / 2,
                         option.rect.top() + 6, m_icon, m_icon);
    // ── WHICH PICTURE ─────────────────────────────────────────────────
    // Mode 0 draws none, 2 draws the game's own UI icon, 3 draws the render
    // with the game's icon inset in the corner, 1 (the default) draws the
    // render alone. IconCatalog is the tool's one resolver for the game's own
    // art and is keyed by model stem.
    QPixmap pm;
    if (m_iconMode != 2 && m_iconMode != 0)
        pm = fox::ThumbnailRenderer::instance().cached(fileIdx, m_icon);
    QPixmap gameIcon;
    if (m_iconMode >= 2) {
        const QString stem =
            index.data(FmdlListModel::StemRole).toString();
        if (!stem.isEmpty())
            gameIcon = fox::IconCatalog::instance().iconFor(stem, m_icon);
    }
    if (m_iconMode == 0) {
        // Nothing drawn, and no request made further down either.
    } else if (m_iconMode == 2) {
        if (!gameIcon.isNull())
            painter->drawPixmap(
                iconRect.center() - QPoint(gameIcon.width() / 2,
                                           gameIcon.height() / 2),
                gameIcon);
    } else if (!pm.isNull()) {
        painter->drawPixmap(iconRect.topLeft(), pm);
        if (m_iconMode == 3 && !gameIcon.isNull()) {
            // A quarter-size inset, bottom-right: the render says what the
            // shape is and the game's icon says what the game calls it, and
            // one has to be subordinate or the cell reads as two assets.
            const int q = qMax(16, m_icon / 3);
            painter->drawPixmap(
                QRect(iconRect.right() - q, iconRect.bottom() - q, q, q),
                gameIcon.scaled(q, q, Qt::KeepAspectRatio,
                                Qt::SmoothTransformation));
        }
    } else if (fox::ThumbnailRenderer::instance().has(fileIdx, m_icon)) {
        // Rendered and failed — a cached null. Leave the cell empty rather than
        // drawing a frame that suggests something is still coming.
    } else {
        // Not rendered yet — a faint frame rather than a hole, so scrolling
        // does not look broken while the renders catch up.
        QColor c = option.palette.text().color();
        c.setAlphaF(0.16);
        painter->setPen(c);
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(iconRect.adjusted(4, 4, -4, -4), 3, 3);
        // Ask for it HERE, not only from the tab's visible-range sweep. Paint
        // is the one event that fires for every cell that is actually on
        // screen, however it got there — first show, re-index, a new search, a
        // window resize, a splitter drag. Driving the queue from it means none
        // of those cases needs its own hook, and none of them can be forgotten.
        // The request is two hash lookups when it is already queued or cached,
        // so this is safe to do on every repaint.
        //
        // EXCEPT mid-fling. A scroll repaints every page it passes through, and
        // queueing those would spend the render thread on rows that are already
        // gone — which shows up as the page you stopped on filling in LAST.
        // While the settle timer is running the sweep owns the queue.
        if (!m_settle || !m_settle->isActive())
            fox::ThumbnailRenderer::instance().request(fileIdx, m_icon);
    }

    const QColor base = option.palette.text().color();
    QFont f = option.font;
    const QFontMetrics fm(f);
    painter->setFont(f);
    painter->setPen(base);
    const QRect nameRect(option.rect.left() + 4, iconRect.bottom() + 3,
                         option.rect.width() - 8, fm.height());
    painter->drawText(nameRect, Qt::AlignHCenter | Qt::AlignVCenter,
                      fm.elidedText(index.data(FmdlListModel::StemRole).toString(),
                                    Qt::ElideMiddle, nameRect.width()));
    QFont sf = f;
    sf.setPointSizeF(f.pointSizeF() > 0 ? f.pointSizeF() * 0.8 : f.pointSize() * 0.8);
    const QFontMetrics sfm(sf);
    painter->setFont(sf);
    QColor sub = base;
    sub.setAlphaF(sel ? 0.8 : 0.55);
    painter->setPen(sub);
    // Elided from the LEFT: the tail of a Fox path is what identifies it, the
    // /Assets/tpp/ head never does. The full path is the tooltip.
    const QRect dirRect(option.rect.left() + 4, nameRect.bottom() + 1,
                        option.rect.width() - 8, sfm.height());
    painter->drawText(dirRect, Qt::AlignHCenter | Qt::AlignVCenter,
                      sfm.elidedText(index.data(FmdlListModel::DirRole).toString(),
                                     Qt::ElideLeft, dirRect.width()));
    painter->restore();
}

int FmdlListModel::fileIdxOf(int row) const
{
    return (row >= 0 && row < m_rows.size()) ? m_rows[row] : -1;
}

int FmdlListModel::fileIdxAt(const QModelIndex& index) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) return -1;
    return m_rows[index.row()];
}

// ── ModelsTab ────────────────────────────────────────────────────────────────

ModelsTab::ModelsTab(QWidget* parent) : QWidget(parent)
{
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    auto* left = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    // Search box with the Filter button immediately to its right — the button
    // opens the tag vocabulary, and everything it does it does by writing
    // "#tag" into this same box.
    {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        m_search = new QLineEdit(left);
        m_search->setPlaceholderText(
            QStringLiteral("Search models…  (-word excludes, #tag filters)"));
        m_search->setToolTip(searchq::tooltip());
        m_search->setClearButtonEnabled(true);
        // Esc clears, the down arrow recalls the last ten searches, and a
        // committed search is remembered — the same four behaviours in every
        // search box in the application, from one place (template §4/§15).
        fox::searchbox::attach(m_search, QStringLiteral("models/searchHistory"));
        // ── THE ROW: funnel, box, display ───────────────────────────
        // The funnel goes on the LEFT of the search box and the display switch
        // on its right, which is the arrangement every file browser and photo
        // app uses: what narrows the list is in front of the list's own input,
        // and what changes how the results are SHOWN is after it. Both are
        // icons — the words "Filter" and "List" were a third of the row's
        // width, in a pane that is mostly a narrow column of paths.
        m_filterBtn = new QToolButton(left);
        m_filterBtn->setIcon(foxglyph::toolIcon(18));
        m_filterBtn->setIconSize(QSize(foxglyph::kSize, foxglyph::kSize));
        m_filterBtn->setAutoRaise(true);
        m_filterBtn->setFocusPolicy(Qt::NoFocus);
        // NOT setPopupMode(InstantPopup): the button has no QMenu, so today
        // that does nothing, and the day anyone gives it one the press would be
        // hijacked and openFilterPopup() would stop being called.
        m_filterBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        m_filterBtn->setToolTip(QStringLiteral(
            "Filter\n\n"
            "Tags for everything the index knows about these models — game, "
            "asset type, family, variant, source archive and status.\n"
            "Ticking one writes #tag into the search box, so the button and "
            "the box are always the same filter."));
        row->insertWidget(0, m_filterBtn);
        row->addWidget(m_search, 1);
        leftLayout->addLayout(row);
        m_searchRow = row;
    }
    // ── The chips (§4, §15) ─────────────────────────────────────────────
    // A COUNT on the funnel button said three filters were in force. Three
    // what? The only way to find out was to open the popup and read the ticks,
    // and the only way to drop one was to find it in there. Each chip says
    // what it is and carries its own ✕.
    m_chips = new fox::FilterChips(left);
    leftLayout->addWidget(m_chips);
    connect(m_chips, &fox::FilterChips::removeRequested, this,
            [this](const QString& term) {
                // Through the SEARCH BOX, which is where filter state lives —
                // the chips are a view of it and the popup owns none of it
                // either, so there is one thing to keep right.
                m_search->setText(
                    searchq::Query::withoutTerm(m_search->text(), term));
                refreshList();
            });
    connect(m_chips, &fox::FilterChips::clearRequested, this, [this] {
        m_search->clear();
        refreshList();
    });
    m_listModel = new FmdlListModel(this);
    m_list = new QListView(left);
    m_list->setModel(m_listModel);
    m_list->setUniformItemSizes(true);
    // Multi-select (§4, and the context-sensitive export rule): several models
    // selected means one "Export 12 models…" entry that says so, rather than a
    // menu acting on whichever row happens to be current.
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_gridDelegate = new ModelGridDelegate(this);
    // The LIST-mode delegate, kept because there has to be one. setGridMode()
    // used to leave list mode with setItemDelegate(nullptr), which does not
    // restore Qt's built-in delegate — it removes the delegate entirely. A
    // QListView with no delegate has no sizeHint, every row comes back
    // 155 x -1, and the view paints nothing at all: 26 rows in the model, a
    // working search, a working selection, and a blank white pane. That is
    // what this tab looked like for anyone who had not turned the grid on.
    // (Measured: visualRect(index(0,0)) = 170x-1 with itemDelegate() == 0.)
    m_listDelegate = new QStyledItemDelegate(this);
    m_list->setItemDelegate(m_listDelegate);

    // THE DISPLAY DROPDOWN (§4). It was a checkbox labelled "Grid": a
    // two-state control standing in for a three-state one, which named the
    // other state only by not being ticked. Three modes now, and a submenu for
    // the per-mode options — which is what a checkbox had nowhere to put.
    m_display = new fox::DisplayModeButton(QStringLiteral("models/display"),
                                           left);
    // ICON-ONLY, and in the search row rather than on a line of its own: a
    // whole row of the pane spent on one short word is a row the list does not
    // get. The mode it is on is in the tooltip and shown by the icon's own
    // tick in the menu.
    m_display->setToolButtonStyle(Qt::ToolButtonIconOnly);
    if (m_searchRow) m_searchRow->addWidget(m_display);
    m_outliner = new fox::ModelOutliner(left);
    m_outliner->setSource(m_listModel);
    m_outliner->treeViewport()->installEventFilter(this);
    m_outliner->hide();
    leftLayout->addWidget(m_list, 1);
    leftLayout->addWidget(m_outliner, 1);

    // ── The display options, REBUILT EVERY TIME THE MENU OPENS ──────────
    // Two complaints, one cause. "Hard to read what's toggled on/off" and
    // "settings should be display context appropriate": the menu was built
    // once in the constructor and held every option for every mode at all
    // times, so half of it did nothing in whichever mode you were in and the
    // ticks had to be read against options that were not live.
    //
    // aboutToShow, not on every mode change: the mode changes are a handful of
    // events, the menu opening is the ONE that matters, and it cannot be
    // missed because the user cannot read the menu without firing it. Same
    // argument §6 makes for the Export menu.
    connect(m_display->optionsMenu(), &QMenu::aboutToShow, this,
            [this] { rebuildDisplayOptions(); });
    // THE MODE ITSELF, which nothing was listening to. The button stored the
    // choice, wrote it to settings and emitted modeChanged — and this tab
    // connected only the OPTIONS menu, so picking List, Outliner or Grid
    // updated the tick and changed nothing on screen. The Textures tab has
    // always connected this; the Models tab never did, so its display button
    // was inert from the day it was added and only --viewmode (which calls
    // applyDisplayMode directly) could switch the view.
    connect(m_display, &fox::DisplayModeButton::modeChanged, this,
            [this](const QString& id) {
                applyDisplayMode(id);
                if (id == fox::displaymode::grid()) renderVisibleThumbnails();
            });
    rebuildDisplayOptions();

    connect(m_outliner, &fox::ModelOutliner::modelActivated, this,
            [this](int fileIdx) { loadModel(fileIdx); });
    // A clip row in the outliner plays it — THROUGH THE COMBOS, exactly as the
    // ANIMATIONS panel does and for exactly the same reason: the combos are
    // what every other path reads to know what is loaded, and a view that
    // loaded a clip behind their back would leave them naming the previous
    // one, including in the export the user pressed next.
    connect(m_outliner, &fox::ModelOutliner::clipActivated, this,
            [this](int archiveFileIdx, int clipIdx) {
                if (m_mtarCombo->currentPayload().toInt() != archiveFileIdx)
                    m_mtarCombo->selectPayload(archiveFileIdx);
                m_clipCombo->selectPayload(clipIdx);
            });
    connect(m_outliner, &fox::ModelOutliner::partSelected, this,
            [this](int meshId) {
                if (m_view) m_view->setPickedMesh(meshId);
                // The WHOLE set, not just the active one: selectLeaf selects a single
        // row, the tree answers with its own selection, and the viewport was
        // handed back a set of one — which collapsed every Shift/Ctrl click in
        // the viewport. Measured with --selseq.
        if (m_sceneTree)
            m_sceneTree->selectLeaves(m_view->selectedMeshes(), meshId);
                refreshInfoPanel();
            });

    // The popup is built once and refilled when the index changes.
    m_filterPopup = new fox::TagFilterPopup(this);
    connect(m_filterBtn, &QToolButton::clicked, this,
            [this] { openFilterPopup(); });
    connect(m_filterPopup, &fox::TagFilterPopup::tagStateChanged, this,
            [this](const QString& tag, searchq::Query::TagState st) {
                m_search->setText(
                    searchq::Query::withTag(m_search->text(), tag, st));
                refreshList();
                m_filterPopup->setResultText(filterSummary());
            });
    connect(m_filterPopup, &fox::TagFilterPopup::clearRequested, this, [this] {
        QString text = m_search->text();
        for (const fox::TagCategory& cat : fox::ModelTags::instance().categories())
            for (const fox::TagInfo& t : cat.tags)
                text = searchq::Query::withTag(text, t.tag,
                                               searchq::Query::TagState::Off);
        m_search->setText(text);
        refreshList();
        m_filterPopup->showFor(m_filterBtn, m_search->text());
        m_filterPopup->setResultText(filterSummary());
    });

    // Thumbnails are rendered a few at a time on a timer, for the rows that are
    // actually on screen — a list of forty thousand models renders forty.
    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setSingleShot(true);
    connect(m_thumbTimer, &QTimer::timeout, this,
            [this] { renderVisibleThumbnails(); });
    m_gridDelegate->setSettleTimer(m_thumbTimer);
    m_thumbRepaint = new QTimer(this);
    m_thumbRepaint->setSingleShot(true);
    connect(m_thumbRepaint, &QTimer::timeout, this,
            [this] { if (m_gridMode) m_list->viewport()->update(); });
    connect(&fox::ThumbnailRenderer::instance(), &fox::ThumbnailRenderer::ready,
            this, [this](int, int) {
                if (m_gridMode && !m_thumbRepaint->isActive())
                    m_thumbRepaint->start(kThumbRepaintMs);
                // A preview opened before its thumbnail finished rendering
                // fills itself in rather than staying a text box.
                hover::Preview::instance().refresh();
            });
    m_list->viewport()->installEventFilter(this);
    // Without this the viewport only reports moves while a button is held, and
    // a hover preview would need a drag to appear.
    m_list->setMouseTracking(true);
    m_list->viewport()->setMouseTracking(true);
    // Scrolling only ever RESTARTS the timer. Rendering rows that are already
    // flying past is the worst thing this can do — it makes the scroll itself
    // stutter and throws the work away — so a fling does nothing until it
    // settles, and then fills what is actually on screen.
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { if (m_gridMode) m_thumbTimer->start(kThumbSettleMs); });
    m_gridDelegate->setIconSize(
        QSettings().value(QStringLiteral("models/gridIcon"), 112).toInt());
    setRowZoom(QSettings().value(QStringLiteral("models/rowZoom"), 0).toInt());
    applyDisplayMode(m_display->mode());
    splitter->addWidget(left);

    auto* right = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    // ── THE TOOLBAR IS GONE ─────────────────────────────────────────────
    // Nine controls used to sit in a strip above the viewport, and every one
    // of them was a second way to reach something that already had a home:
    //
    //   Wireframe, Skeleton   → the shading balls and the Graphics popover's
    //                           overlay list, both on the viewport itself.
    //   Normal maps, PBR      → the Graphics popover's Materials group. The
    //                           reload PBR can trigger is this tab's job and
    //                           is wired below, off displayChanged.
    //   Parts, Animations,    → the N-panel's icon strip. They were never
    //   Materials               view controls; they were panel switches, and
    //                           panel switches belong on the panel column.
    //   View settings         → removed inside attachViewportPanel: it opened
    //                           the same three popovers as the buttons a few
    //                           pixels below it.
    //   Reset view            → middle-click in the viewport, the Camera
    //                           popover, and the right-click menu.
    //   Export .glb…          → the Export menu, the model list's right-click
    //                           menu and the viewport's, all of which now name
    //                           what they will export.
    //
    // What is left above the viewport is nothing, so the viewport starts at
    // the top of its pane and the model is the biggest thing on the tab.

    // The tip bar (§5's fullscreen argument, one step earlier): the viewport
    // has a keyboard now, and none of it is discoverable from the screen.
    m_tip = new fox::TipBar(
        QStringLiteral("modelsViewport"),
        QStringLiteral("Tip: double-click a part to select it, Shift or Ctrl to add "
                       "· H / Alt+H / Shift+H hide, show all, isolate · . "
                       "frames it · F fullscreen · middle-click resets the "
                       "camera · F1 lists everything"),
        right);
    rightLayout->addWidget(m_tip);

    m_view = new GLModelWidget(right);
    rightLayout->addWidget(m_view, 1);
    // The page's own head of the viewport's right-click menu. Only the tab
    // can resolve a submesh to its material name — the viewport knows it as an
    // integer — so the title row and the per-part export are built here and
    // handed to the one shared menu, rather than each tab installing a second
    // handler on the same signal.
    // §4: this page fills in what only it can resolve and the SHARED builder
    // composes the menu. It used to append its own header, its own export
    // entries and its own Copy material — three rows the builder now owns, and
    // three chances for this tab's part menu to differ from the Customize
    // tab's, which is exactly what had happened.
    auto describeSubject = [this](partmenu::Context& ctx, int part) {
        if (!m_hasModel) return;
        ctx.modelName = modelDisplayName();
        ctx.partName = part >= 0 ? submeshLabel(part) : QString();
        ctx.partTris = part >= 0 ? submeshTris(part) : 0;
        // The scene's triangle count, from the meshes themselves — there is
        // no accessor for it and inventing one for a label would be a second
        // place the number could be wrong.
        {
            qint64 tris = 0;
            for (const auto& mesh : m_model.meshes())
                tris += mesh.triangles.size() / 3;
            ctx.modelTris = tris;
        }
        const auto& files = fox::ArchiveIndex::instance().files();
        if (m_currentFile >= 0 && m_currentFile < files.size()) {
            ctx.filePath = files[m_currentFile].path;
            ctx.fileHash = QStringLiteral("0x%1")
                               .arg(files[m_currentFile].hash, 16, 16,
                                    QLatin1Char('0'));
        }
        ctx.exportModel = [this] { exportCurrent(); };
        ctx.exportModelLast = [this] { exportCurrentToLast(); };
        // ── THE WHOLE SELECTION, NOT THE PART UNDER THE CURSOR ──────────
        // This was `{part}`, and the menu ABOVE it already said "Export 3
        // parts" (partmenu scopes its labels by ctx.subject.size()). So a
        // three-part selection offered an action counting three and wrote
        // one — the last one picked. The parts panel got this right because
        // SceneTree rebinds exportPart to its own selection afterwards; the
        // viewport does not rebind, so the viewport was wrong.
        //
        // ctx.subject is filled by BOTH callers before this hook runs, so
        // reading it here fixes every entry point at once.
        if (part >= 0) {
            const QSet<int> subj =
                ctx.subject.isEmpty() ? QSet<int>{part} : ctx.subject;
            ctx.exportPart = [this, subj] { exportSubmeshes(subj); };
            ctx.exportPartLast = [this, subj] { exportSubmeshesToLast(subj); };
        }
    };
    fox::attachViewportPanel(m_view, describeSubject);
    // The parts panel gets THE SAME describer — but m_sceneTree is built a
    // hundred lines further down, so the call is down there with it. Written
    // here first, where it read perfectly and did nothing at all: the member
    // was still null and `if (m_sceneTree)` swallowed it silently.
    m_describeSubject = describeSubject;
    // This tab implements fullscreen, so its viewport may offer the key.
    m_view->setFullscreenSupported(true);

    // ── Two-way part selection (§4) ─────────────────────────────────────
    connect(m_view, &GLModelWidget::meshPicked, this, [this](int meshId) {
        // Opening the tree when a pick lands is the point: a selection you
        // cannot see is not feedback, and the tree is where a part's name,
        // its triangle count and its checkbox live.
        // NOT while fullscreen. Fullscreen hides the tree without unchecking
        // its button, so this either did nothing (the button was already
        // checked) or re-opened a pane beside a "fullscreen" viewport that
        // nothing could then close, because the toggle is hidden too.
        if (meshId >= 0 && m_sceneTree && !m_sceneTree->isVisible()
            && !m_view->viewportFullscreen())
            setSubmeshTreeVisible(true);
        // The WHOLE set. selectLeaf() picks ONE row, the tree answers with
        // its own selection, and the viewport is handed back a set of one —
        // which collapsed every Shift and Ctrl click made in the viewport.
        // Measured with --selseq: "pick 14, shift 12" came back {12}.
        if (m_sceneTree)
            m_sceneTree->selectLeaves(m_view->selectedMeshes(), meshId);
        if (m_outliner && listViewActive())
            m_outliner->selectPart(meshId);
        // …and the MATERIALS list, which was the missing third of §4's
        // two-way selection: a picked submesh lit up in the parts list and
        // left the material panel showing whatever was last clicked, so the
        // one question a pick is usually asked in order to answer — "which
        // material is this" — still needed a hunt through the cards.
        selectMaterialFor(meshId);
        // INFO describes the model AND what is selected in it, so a pick is a
        // reason to rewrite it. Cheap: the panel refuses to build when closed.
        refreshInfoPanel();
    });
    connect(m_view, &GLModelWidget::meshVisibilityChanged, this, [this] {
        // The H keys changed the viewport's own visibility set; the tree shows
        // the same state and would otherwise drift out of step with it.
        if (m_sceneTree) m_sceneTree->setHiddenLeaves(m_view->hiddenMeshes());
    });
    connect(m_view, &GLModelWidget::fullscreenChanged, this,
            &ModelsTab::applyViewportFullscreen);

    // Animation bar: mtar picker · clip picker · play · frame slider.
    auto* animBar = new QHBoxLayout();
    m_mtarCombo = new SearchableCombo(right);
    m_mtarCombo->setMinimumWidth(200);
    m_mtarCombo->setMaximumWidth(360);
    m_mtarCombo->setToolTip(QStringLiteral(
        "Animation archive (.mtar) — grouped by game and shelf. Type to "
        "filter by name, file or path."));
    m_clipCombo = new SearchableCombo(right);
    m_clipCombo->setMinimumWidth(200);
    m_clipCombo->setMaximumWidth(400);
    m_clipCombo->setToolTip(QStringLiteral(
        "Clip (.gani) — grouped by category. Type to filter; the raw asset "
        "name is on the second line of every row."));
    m_clipCombo->setEnabled(false);
    m_playBtn = new QToolButton(right);
    m_playBtn->setText(QStringLiteral("▶"));
    m_playBtn->setCheckable(true);
    m_playBtn->setEnabled(false);
    m_frameSlider = new QSlider(Qt::Horizontal, right);
    m_frameSlider->setEnabled(false);
    m_frameLabel = new QLabel(QStringLiteral("—"), right);
    m_frameLabel->setMinimumWidth(64);
    animBar->addWidget(m_mtarCombo);
    animBar->addWidget(m_clipCombo);
    animBar->addWidget(m_playBtn);
    animBar->addWidget(m_frameSlider, 1);
    animBar->addWidget(m_frameLabel);
    rightLayout->addLayout(animBar);

    // THE LABEL UNDER THE VIEWPORT IS GONE. It word-wrapped, so a long path
    // took two lines of window height permanently — for a message that is
    // interesting for about four seconds after a load. The same text goes to
    // the status bar now (app/StatusLine.h), beside the index's own line,
    // which is the bar this window already has for exactly this.
    //
    // m_info survives as a NON-PARENTED sink so the hundred-odd
    // setStatus() call sites keep compiling and keep working: setStatus()
    // writes to it and forwards to the bar. It is never shown.
    m_info = new QLabel;
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    splitter->addWidget(right);

    // ── The N-panel column (template §6) ────────────────────────────────
    // These three used to be three SEPARATE panes in this splitter, each with
    // its own button on a toolbar above the viewport. Three panes wide is how
    // a 1080p window ends up with a viewport narrower than the lists beside
    // it, and every one of those buttons was a panel switch pretending to be
    // a view control. One column now, opened by the arrow on the viewport's
    // right edge, with the icon strip down its far side.
    //
    // The minimum widths went with them. They were per-pane and additive —
    // 240 + 280 + 400 — and in one column they would have made the column
    // itself 400px wide before anything was in it. The column has one floor
    // and the panels scroll inside it.
    m_npanel = new fox::NPanel(QStringLiteral("models/npanel"), splitter);

    // Built whether or not its panel is SHOWN, because it holds state the
    // export depends on: what is unticked here is not in the .glb.
    m_sceneTree = new SceneTree(m_npanel);
    // §4's "one builder, six entry points" is only true if the entry points
    // also agree on the CONTEXT they hand it. Same describer as the viewport.
    m_sceneTree->setContextHook(m_describeSubject);
    // …and the OUTLINER is the seventh entry point to the same builder. It
    // gets the menu already built, because unlike the parts panel it is not a
    // partmenu client — it just needs the rows a submesh is owed.
    if (m_outliner)
        m_outliner->setPartMenuHook([this](QMenu* menu, int meshId) {
            if (!menu || !m_describeSubject) return;
            partmenu::Context ctx;
            m_describeSubject(ctx, meshId);
            partmenu::build(menu, ctx);
        });
    m_npanel->addPanel(
        QStringLiteral("parts"), QStringLiteral("PARTS"), 4, m_sceneTree,
        QStringLiteral(
            "Every mesh group in this model and every submesh inside it, with "
            "its triangle count, material slot and material name, switchable "
            "individually.\n\nA Fox mesh group routinely holds several "
            "submeshes — a face, its eyelashes and a welded-on bandanna can "
            "share one — so the switches are per submesh and a group's own box "
            "turns all of them at once. This is the export scope too."));

    m_inspector = new MaterialInspector(m_npanel);
    m_npanel->addPanel(
        QStringLiteral("materials"), QStringLiteral("MATERIALS"), 5,
        m_inspector,
        QStringLiteral(
            "Every material in this scene, the shader it asks for, the meshes "
            "and mesh groups that use it, and every texture map split into its "
            "channels with the mean value of each."));

    // Built EMPTY — rebuild() walks thousands of rows and a session that never
    // opens this should never pay for them. The tab builds it on first open.
    m_animPanel = new AnimationsPanel(m_npanel);
    m_npanel->addPanel(
        QStringLiteral("animations"), QStringLiteral("ANIMATIONS"), 14,
        m_animPanel,
        QStringLiteral(
            "Every clip in the install as a list: one filter across all of "
            "them, a category filter, and multi-select — which is what an "
            "export of nine clips needs and what the two combos beside the "
            "viewport cannot express."));

    m_attachPanel = new fox::AttachmentsPanel(m_npanel);
    m_npanel->addPanel(
        QStringLiteral("attachments"), QStringLiteral("ATTACHMENTS"), 15,
        m_attachPanel,
        QStringLiteral(
            "The .fv2 variation tables that address THIS model: the mesh "
            "groups they turn on and off, and the extra models they bring with "
            "them — a hat, a bag, a hair mesh.\n\nMatched by hash out of the "
            "files themselves, never by filename."));

    m_infoPanel = new fox::InfoPanel(m_npanel);
    m_npanel->addPanel(
        QStringLiteral("info"), QStringLiteral("INFO"), 16, m_infoPanel,
        QStringLiteral(
            "Everything this tool knows about the loaded model, and about the "
            "submesh selected inside it. Ctrl+C copies the selected rows."));

    // The default column, before the user has arranged one: PARTS, which is
    // the only per-part visibility control this tab has, and INFO, which is
    // what the old status line under the viewport was trying to be.
    m_npanel->restoreState({QStringLiteral("parts"), QStringLiteral("info")});
    // The open/close control the user asked for: a small arrow with a grey
    // background on the viewport's right edge. On the VIEWPORT, not in the
    // column — a control that only exists inside the thing it opens is not a
    // way in.
    m_npanel->attachToggle(m_view);
    splitter->addWidget(m_npanel);

    splitter->setStretchFactor(0, 2);   // the model list
    splitter->setStretchFactor(1, 5);   // the viewport
    splitter->setStretchFactor(2, 2);   // the N-panel column
    // Stretch factors alone are not the initial layout — they are how SPARE
    // room is shared once every pane has its size hint, and the viewport's
    // hint is large enough that a 1280px window came up with a 155px model
    // list and a 240px column whose INFO rows all elided to "avf0_t…".
    // Measured, and fixed by saying what the list, the viewport and the
    // column should start at; the
    // splitter scales these to the real width, and PanelPersist below leaves
    // them alone until the user has actually dragged something.
    splitter->setSizes({300, 660, 320});

    PanelPersist::bind(splitter, QStringLiteral("models/splitter"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(splitter);

    auto* debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(200);
    connect(m_search, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
    connect(debounce, &QTimer::timeout, this, [this] {
        m_listModel->refresh(m_search->text());
        // The outliner reads the SAME model, so a narrowed list narrows the
        // tree — there is no second filtering path here, which is why §4's
        // one-matcher rule is not at risk.
        if (m_outliner && listViewActive()) {
            m_outliner->refresh();
            m_outliner->setLoadedModel(m_currentFile,
                                       m_hasModel ? &m_model : nullptr);
        }
        updateFilterButton();
        // The popup stays open while you type into ITS find field, not this
        // one, so it is only refreshed when it happens to be visible.
        if (m_filterPopup && m_filterPopup->isVisible())
            m_filterPopup->setResultText(filterSummary());
    });

    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& cur, const QModelIndex&) {
                const int fi = m_listModel->fileIdxAt(cur);
                if (fi >= 0) loadModel(fi);
            });
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListView::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                const int fi = m_listModel->fileIdxAt(m_list->indexAt(pos));
                if (fi < 0) return;
                // §2's selection rule, from util/MenuContext.h. The list and
                // the grid are one view here with two delegates, so they cannot
                // disagree — but the rule itself was written out twice, here
                // and in the Files tab, and now is not.
                const QVector<int> sel = menuctx::contextFiles(
                    m_list, pos,
                    [this](const QModelIndex& ix) {
                        return m_listModel->fileIdxAt(ix);
                    });
                QMenu menu(this);
                exportactions::addFileSetActions(&menu, sel, this);
                // §15: thumbnails are rendered by the tool and cached, and a
                // cache needs a way to be wrong out loud. After a mod install
                // or a re-extract a thumbnail is a picture of the OLD model and
                // looks exactly like a picture of the new one.
                menu.addSeparator();
                QAction* re = menu.addAction(
                    sel.size() == 1
                        ? QStringLiteral("Re-render thumbnail")
                        : QStringLiteral("Re-render %1 thumbnails")
                              .arg(sel.size()),
                    this, [this, sel] {
                        int n = 0;
                        for (int f : sel)
                            n += fox::ThumbnailRenderer::instance().refresh(f);
                        m_list->viewport()->update();
                        setStatus(
                            n > 0
                                ? QStringLiteral("Re-rendering %1 thumbnail(s).")
                                      .arg(n)
                                : QStringLiteral(
                                      "Nothing to re-render — none of those "
                                      "have been drawn yet."));
                    });
                re->setToolTip(QStringLiteral(
                    "Draw these again from the model as it is on disk now. "
                    "Only sizes already in the cache are redrawn."));
                menu.exec(m_list->viewport()->mapToGlobal(pos));
            });
    // ── The reload PBR asks for ─────────────────────────────────────────
    // The switch itself is on the Graphics popover now, with the rest of the
    // shading state. What could not go there is this: choosing Rendered, or
    // ticking PBR, is a request for a MAP SET, and the viewport widget cannot
    // fetch one — it knows nothing about archives. The tab does. So the tab
    // watches the viewport for a shading state it cannot currently honour and
    // reloads the model once to satisfy it.
    //
    // Guarded on hasPbrMaps() and on m_reloadingForPbr: loadModel() re-asserts
    // the shading state as it finishes, which emits displayChanged again, and
    // without the guard that second emission asked for another reload.
    connect(m_view, &GLModelWidget::displayChanged, this, [this] {
        if (m_reloadingForPbr || m_currentFile < 0) return;
        const bool wants = m_view->pbrShading()
                        || m_view->shadingMode() == ShadingMode::Rendered;
        if (!wants || m_view->hasPbrMaps()) { refreshInspector(); return; }
        m_reloadingForPbr = true;
        reloadKeepingParts();
        m_reloadingForPbr = false;
    });
    connect(m_view, &GLModelWidget::shadingModeChanged, this,
            [this](ShadingMode mode) {
                if (mode != ShadingMode::Rendered) return;
                if (m_view->hasPbrMaps() || m_currentFile < 0) return;
                if (m_reloadingForPbr) return;
                m_reloadingForPbr = true;
                m_view->setPbrShading(true);
                reloadKeepingParts();
                m_reloadingForPbr = false;
            });
    connect(m_sceneTree, &SceneTree::meshVisibilityChanged, this,
            [this](int meshId, bool on) { m_view->setMeshVisible(meshId, on); });
    connect(m_sceneTree, &SceneTree::exportRequested, this,
            [this](const QSet<int>& meshIds) { exportSubmeshes(meshIds); });
    // The panel header carries the live count, so it has to follow every
    // change to the hidden set. HERE, and not up beside the viewport wiring:
    // the tree is built with the N-panel column, further down, and a connect
    // against a null pointer is a warning at startup and a signal that never
    // arrives.
    connect(m_sceneTree, &SceneTree::meshVisibilityChanged, this,
            [this](int, bool) { updatePartsTitle(); });
    connect(m_inspector, &MaterialInspector::exportImagesRequested, this,
            [this](const QVector<MaterialInspector::CardInfo>& mats) {
                exportMaterialImages(mats);
            });
    connect(m_animPanel, &AnimationsPanel::clipChosen, this,
            [this](int archiveFileIdx, int clipIdx) {
                // Through the COMBOS, not straight into loadMtar/loadClip: the
                // combos are what every other path reads to know what is
                // loaded, and a panel that loaded a clip behind their back left
                // them naming the previous one — including in the export the
                // user pressed next.
                if (m_mtarCombo->currentPayload().toInt() != archiveFileIdx)
                    m_mtarCombo->selectPayload(archiveFileIdx);
                m_clipCombo->selectPayload(clipIdx);
            });
    // The scope moved: the bar's archive list is a mirror of it.
    connect(m_animPanel, &AnimationsPanel::scopeChanged, this, [this] {
        populateAnimCombo();
        syncAnimPanel();
    });
    connect(m_animPanel, &AnimationsPanel::exportRequested, this,
            [this](bool separateFiles) { exportAnimationsInteractive(separateFiles); });
    connect(m_attachPanel, &fox::AttachmentsPanel::meshGroupToggled, this,
            [this](quint32 nameHash32, bool on) {
                // The panel speaks in StrCode32, because that is what a .fv2
                // addresses a group by; the viewport speaks in submesh id.
                // This is the only place the two meet.
                if (!m_sceneTree || !m_hasModel) return;
                const auto& groups = m_model.meshGroups();
                int group = -1;
                for (int g = 0; g < groups.size(); ++g)
                    if (groups[g].nameHash32 == nameHash32) { group = g; break; }
                if (group < 0) return;
                // Through the TREE's hidden set, not straight at the viewport:
                // the tree is this tab's per-part visibility state and the
                // export scope, and a group switched off behind its back would
                // still be in the .glb.
                QSet<int> hidden = m_sceneTree->hiddenLeaves();
                const auto& meshes = m_model.meshes();
                for (int i = 0; i < meshes.size(); ++i) {
                    if (meshes[i].meshGroupIndex != group) continue;
                    if (on) hidden.remove(i);
                    else hidden.insert(i);
                }
                m_sceneTree->setHiddenLeaves(hidden);
            });
    connect(m_attachPanel, &fox::AttachmentsPanel::openModelRequested, this,
            [this](int fileIdx) { loadModel(fileIdx); });
    connect(m_attachPanel, &fox::AttachmentsPanel::summaryChanged, this,
            [this] {
                if (QLabel* t = m_npanel->titleLabel(QStringLiteral("attachments"))) {
                    const QString sum = m_attachPanel->summary();
                    t->setText(sum.isEmpty()
                                   ? QStringLiteral("ATTACHMENTS")
                                   : QStringLiteral("ATTACHMENTS · %1").arg(sum));
                }
            });

    // Opening a panel is what builds it. The N-panel does not know what its
    // contents cost, so the tab answers panelOpenChanged rather than the
    // panels doing work every time the column is shown.
    connect(m_npanel, &fox::NPanel::panelOpenChanged, this,
            [this](const QString& key, bool on) {
                if (!on) {
                    if (key == QLatin1String("materials")) refreshInspector();
                    return;
                }
                fillPanel(key);
            });
    // ── AND ONCE, NOW, FOR WHATEVER IS ALREADY OPEN ──────────────────────
    //
    // m_npanel->restoreState() ran two hundred lines above this connect, so
    // every panel the user left open emitted panelOpenChanged into a signal
    // nobody was listening to yet — and was therefore never filled for the
    // whole session. That is the bug the user reported as "the animation
    // player at the bottom doesn't always match the ANIMATIONS panel": with
    // ANIMATIONS remembered open, the panel was never rebuilt and sat EMPTY
    // while the transport played a clip. Same for MATERIALS, INFO and
    // ATTACHMENTS; the only way to fill any of them was to close and reopen.
    //
    // Fixed HERE rather than by moving restoreState below the connect,
    // because ordering is exactly what failed: a fill that runs off the
    // panel's actual open state cannot be broken again by moving a line.
    fillOpenPanels();

    // The tree-to-viewport half of the same rule: picking a row in the tree
    // highlights it in the viewport, so the H keys and "." act on what the
    // user just clicked rather than on whatever they last double-clicked.
    connect(m_sceneTree, &SceneTree::leavesSelected, this,
            [this](const QSet<int>& ids, int active) {
                // The panel's multi-select IS the viewport's selection. One
                // set, two views of it — §4's two-way rule, which until now
                // only carried one id in this direction.
                if (m_view) m_view->setSelectedMeshes(ids, active);
            });
    connect(m_sceneTree, &SceneTree::contextLeaves, this,
            [this](const QSet<int>& ids) {
                if (m_view) m_view->setContextMeshes(ids);
            });
    // NOT setPickedMesh here any more: leavesSelected above already handed the
    // viewport the whole set, and setPickedMesh REPLACES a selection with one
    // id — so this line, firing second, undid every multi-select the panel
    // made. What is left is what only the active row decides.
    connect(m_sceneTree, &SceneTree::leafSelected, this, [this](int meshId) {
        selectMaterialFor(meshId);
        refreshInfoPanel();
    });

    // ── animation wiring ────────────────────────────────────────────────────
    m_animTimer = new QTimer(this);
    // ── PLAYBACK IS TIME-BASED, NOT TICK-BASED ──────────────────────────
    // This used to advance the clip by exactly ONE FRAME per timer tick, at a
    // 33 ms interval. That is only 30 fps if every tick costs nothing — and a
    // tick here rebuilds a 120-bone palette, solves IK, rebuilds the skeleton
    // overlay and repaints the viewport. When that takes longer than 33 ms
    // QTimer simply fires late, so playback ran at whatever the renderer could
    // manage: on a heavy character, slow motion, and slower the heavier the
    // model. That is the user's "animation playback seems like it's in slow
    // motion and doesn't match how fast it should be playing".
    //
    // Now the clip advances by ELAPSED WALL-CLOCK TIME, so a slow frame skips
    // clip frames instead of stretching them and playback runs at the same
    // speed on any machine and any model.
    //
    // 16 ms so the sampler is asked more often than the clip changes — a
    // 30 fps clip driven by a 30 Hz timer beats against itself and judders.
    m_animTimer->setInterval(16);
    connect(m_animTimer, &QTimer::timeout, this, [this] {
        if (!m_hasAnim || m_anim.frameCount <= 0) return;
        const qint64 ns = m_animClock.isValid() ? m_animClock.nsecsElapsed() : 0;
        m_animClock.restart();
        // A first tick, a resume, or a stall longer than a quarter second
        // advances one frame rather than jumping — a hitch must not teleport
        // the pose.
        const double dt = (ns <= 0 || ns > 250000000LL)
                              ? 1.0 / double(kPlaybackFps)
                              : double(ns) / 1e9;
        float f = m_frame + float(dt * kPlaybackFps);
        // fmod, not a single wrap: a long stall can be more than one loop.
        const float span = float(m_anim.frameCount);
        if (span > 0.0f) f = std::fmod(f, span);
        if (f < 0.0f) f += span;
        setFrame(f);
    });
    connect(m_mtarCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int row) {
                // Switching to another ARCHIVE keeps playing too, for the same
                // reason as the clip list. Only "— none —" stops it: there is
                // then nothing to play and the timer would tick on an empty
                // clip forever.
                Q_UNUSED(row);
                const QVariant pv = m_mtarCombo->currentPayload();
                const int fi = pv.isValid() ? pv.toInt() : -1;
                if (fi >= 0) loadMtar(fi);
                else {
                    stopPlayback();
                    m_hasMtar = m_hasAnim = false;
                    m_clipCombo->clear();
                    m_clipCombo->setEnabled(false);
                    m_playBtn->setEnabled(false);
                    m_frameSlider->setEnabled(false);
                    m_frameLabel->setText(QStringLiteral("—"));
                    m_view->clearPose();
                    syncAnimProvider();
                    syncAnimPanel();   // …nothing is playing; say so
                }
            });
    connect(m_clipCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int row) {
                // Playback is NOT stopped. Flicking through a long clip list
                // to find the one you want means watching each of them, and
                // pausing on every change turned that into two clicks per
                // clip. loadClip() rewinds to frame 0, so a running timer just
                // starts playing the new one.
                Q_UNUSED(row);
                if (!m_hasMtar) return;
                const QVariant pv = m_clipCombo->currentPayload();
                if (pv.isValid()) loadClip(pv.toInt());
            });
    connect(m_playBtn, &QToolButton::toggled, this, [this](bool on) {
        m_playBtn->setText(on ? QStringLiteral("⏸") : QStringLiteral("▶"));
        if (on) {
            // Restarted on PLAY, not left running: the elapsed time since the
            // last tick of a previous playback is not a frame delta, and
            // feeding it in would jump the pose on every resume.
            m_animClock.restart();
            m_animTimer->start();
        } else {
            m_animTimer->stop();
            m_animClock.invalidate();
        }
    });
    connect(m_frameSlider, &QSlider::valueChanged, this, [this](int v) {
        if (m_hasAnim) setFrame(static_cast<float>(v), /*fromSlider=*/true);
    });

}

// The noun for what this tab exports. §6's exportNoun, and the answer to "the
// export menu needs to be context appropriate": on this tab the subject is
// always ONE model, and the interesting variable is whether a submesh is
// picked — because then "Export part" is a real, different action.
QString ModelsTab::exportSubjectNoun() const
{
    return QStringLiteral("model");
}

void ModelsTab::populateExportMenu(QMenu* menu)
{
    const QString subject = exportSubjectNoun();
    QAction* glbAct = menu->addAction(
        MenuText::prompts(MenuText::exportSubject(
            subject, m_hasAnim ? QStringLiteral("posed, current frame")
                               : QStringLiteral("rigged, bind pose"))),
        this, &ModelsTab::exportCurrent);
    glbAct->setEnabled(m_hasModel);
    Hotkeys::Role::set(glbAct, Hotkeys::Role::exportSelection());

    // ── Export PART ─────────────────────────────────────────────────────
    // The part menu in the viewport and the parts panel has had this since
    // 9d; the Export MENU did not, so the one place §6 says everything is
    // reachable from could not export the thing the user had just clicked on.
    // Named after the part, because "Export part" over a scene of nineteen is
    // not context-appropriate either.
    const int picked = m_view ? m_view->pickedMesh() : -1;
    const bool haveP = m_hasModel && picked >= 0
                       && picked < m_model.meshes().size();
    QString partName;
    if (haveP) {
        const fox::FmdlMesh& m = m_model.meshes()[picked];
        if (m.materialInstanceIndex >= 0
            && m.materialInstanceIndex < m_model.materials().size())
            partName = m_model.materials()[m.materialInstanceIndex].name;
        if (partName.isEmpty())
            partName = QStringLiteral("submesh %1").arg(picked);
    }
    QAction* partAct = menu->addAction(
        MenuText::prompts(MenuText::exportSubject(
            QStringLiteral("part"),
            haveP ? MenuText::preview(partName) : QString())),
        this, [this] {
            const int p = m_view ? m_view->pickedMesh() : -1;
            // ONE implementation, the same the part menu calls. A second
            // "export just this submesh" here would be a second answer to
            // what the export scope is.
            if (p >= 0) exportSubmeshes({p});
        });
    // DISABLED, not hidden (§7): this tab can always do it, just not while
    // nothing is picked — and a sometimes-grey entry teaches that a pick is
    // what it needs.
    partAct->setEnabled(haveP);

    // Animated export is a DIFFERENT file from the two above — rigged, with
    // curves — so it is its own entry rather than a mode of theirs.
    QAction* animAct = menu->addAction(
        MenuText::prompts(MenuText::exportSubject(
            QStringLiteral("animated %1").arg(subject),
            QStringLiteral("selected clips"))),
        this, [this] { exportAnimationsInteractive(false); });
    animAct->setEnabled(m_hasModel && m_hasMtar);
    Hotkeys::Role::set(animAct, Hotkeys::Role::exportAnimations());
    QAction* animEach = menu->addAction(
        MenuText::prompts(MenuText::exportSubject(
            QStringLiteral("animated %1").arg(subject),
            QStringLiteral("one file per clip"))),
        this, [this] { exportAnimationsInteractive(true); });
    animEach->setEnabled(m_hasModel && m_hasMtar);
    fox::addViewportCaptureActions(menu, this, m_view, m_hasModel);

    if (m_hasMtar && m_clipCombo->currentPayload().isValid()) {
        // The ASSET's name, not the row's headline. The headline is our
        // readable expansion and a file called "Walk · start · right.gani"
        // would not round-trip back into the game.
        const auto rawNameOf = [this](int ci) {
            QString n = (ci >= 0 && ci < m_mtar.clips().size())
                ? m_mtar.clips()[ci].name.section(QLatin1Char('/'), -1)
                : QString();
            const int dot = n.lastIndexOf(QLatin1Char('.'));
            if (dot > 0) n.truncate(dot);
            return n;
        };
        QAction* clipAct = menu->addAction(
            QStringLiteral("Export clip \"%1\" (.gani)…")
                .arg(rawNameOf(m_clipCombo->currentPayload().toInt())),
            this, [this, rawNameOf] {
                const int ci = m_clipCombo->currentPayload().toInt();
                const QByteArray raw = m_mtar.readClip(ci);
                if (raw.isEmpty()) return;
                QString base = rawNameOf(ci);
                if (!base.endsWith(QLatin1String(".gani")))
                    base += QStringLiteral(".gani");
                const QString out = QFileDialog::getSaveFileName(
                    this, QStringLiteral("Export clip"),
                    QDir(Config::exportDir()).filePath(base),
                    QStringLiteral("Fox animation (*.gani)"));
                if (out.isEmpty()) return;
                Config::setExportDir(QFileInfo(out).absolutePath());
                QSaveFile f(out);
                if (f.open(QIODevice::WriteOnly) && f.write(raw) == raw.size())
                    f.commit();
            });
        clipAct->setEnabled(true);
        menu->addAction(
            QStringLiteral("Export all %1 clips (.gani)…")
                .arg(m_mtar.clips().size()),
            this, [this] {
                const QString dir = QFileDialog::getExistingDirectory(
                    this, QStringLiteral("Export all clips to…"),
                    Config::exportDir());
                if (dir.isEmpty()) return;
                Config::setExportDir(dir);
                int written = 0;
                for (int i = 0; i < m_mtar.clips().size(); ++i) {
                    const QByteArray raw = m_mtar.readClip(i);
                    if (raw.isEmpty()) continue;
                    QString name = m_mtar.clips()[i].name;
                    if (!name.endsWith(QLatin1String(".gani")))
                        name += QStringLiteral(".gani");
                    QSaveFile f(QDir(dir).filePath(name));
                    if (f.open(QIODevice::WriteOnly)
                        && f.write(raw) == raw.size() && f.commit())
                        ++written;
                }
                setStatus(
                    QStringLiteral("Exported %1 clip(s) to %2").arg(written).arg(dir));
            });
    }

    if (m_currentFile >= 0) {
        menu->addSeparator();
        // §12's Variants ▸ and Show dependencies… need a way to navigate;
        // this tab has one, so it offers them.
        exportactions::addFileActions(menu, m_currentFile, this,
                                      [this](int fi) { loadModel(fi); });
    }
}

// --filemenu: build the canonical file context menu for whatever is selected,
// list what is in it, and leave it on screen for the screenshot.
//
// §12's whole claim is that every list, tile, tree node and viewport click
// offers the SAME menu. That claim was made in 8y and never checked, because a
// context menu cannot be opened from a devshot run — so "Variants ▸" and
// "Show dependencies…" were shipped as code nobody had seen. The log line is
// the stronger half: a screenshot shows the menu is there, the list of entries
// shows exactly WHICH entries, elision and all.
// --selectrows N: select the first N rows of the list, so the SET form of the
// context menu can be photographed. §3's multi-selection block — the "— N rows"
// copies and the counted export pair — is a different menu from the single-row
// one and had no way to be reached from a shot.
int ModelsTab::selectRowsForShot(int n)
{
    if (!m_list || !m_listModel || n <= 0) return 0;
    QItemSelection sel;
    const int rows = qMin(n, m_listModel->rowCount(QModelIndex()));
    for (int r = 0; r < rows; ++r) {
        const QModelIndex ix = m_listModel->index(r, 0);
        sel.select(ix, ix);
    }
    m_list->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents();
    return rows;
}

QString ModelsTab::openFileMenuForShot()
{
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    // The SET menu when several rows are selected, exactly as the real
    // right-click handler composes it — otherwise this flag could only ever
    // photograph the single-row form, which is the half that already worked.
    const QVector<int> sel = selectedModelFiles();
    if (sel.size() > 1) {
        exportactions::addFileSetActions(menu, sel, this);
    } else if (m_currentFile >= 0) {
        populateExportMenu(menu);
    } else {
        return QStringLiteral("no file selected");
    }
    QStringList labels;
    for (QAction* a : menu->actions()) {
        if (a->isSeparator()) { labels << QStringLiteral("---"); continue; }
        QString l = a->text();
        l.remove(QLatin1Char('&'));
        if (a->menu()) l += QStringLiteral(" [%1 item(s)]")
                                .arg(a->menu()->actions().size());
        if (!a->isEnabled()) l += QStringLiteral(" (disabled)");
        labels << l;
    }
    // Popped up where a right-click in the middle of the list would put it, so
    // the grab shows it over the tab rather than off the window edge.
    menu->popup(mapToGlobal(QPoint(width() / 3, height() / 3)));
    for (int i = 0; i < 6; ++i) QCoreApplication::processEvents();
    return labels.join(QLatin1String(" | "));
}

void ModelsTab::syncGameFilter()
{
    // The per-game checkboxes live in the Filter popup now, and the popup reads
    // its state from the search box every time it opens, so there is nothing to
    // write back here — another tab changing the app-wide game filter just
    // means this list has to be re-run.
    refreshList();
}

// ── The Filter button ────────────────────────────────────────────────────────

void ModelsTab::refreshList()
{
    m_listModel->refresh(m_search->text());
    updateFilterButton();
    renderVisibleThumbnails();
}

QString ModelsTab::filterSummary() const
{
    if (!m_listModel) return {};
    const searchq::Query q(m_search ? m_search->text() : QString());
    const int tags = q.mustTags().size() + q.mustNotTags().size();
    QString s = QStringLiteral("%1 of %2 models")
                    .arg(m_listModel->shown())
                    .arg(m_listModel->total());
    if (tags > 0)
        s += QStringLiteral(" · %1 tag%2").arg(tags).arg(tags == 1 ? "" : "s");
    return s;
}

void ModelsTab::updateFilterButton()
{
    if (!m_filterBtn || !m_search) return;
    const searchq::Query q(m_search->text());
    const int n = q.mustTags().size() + q.mustNotTags().size();
    // A count on the button is the only way to see, without opening it, that a
    // filter is in force at all — which is the classic way a hidden filter
    // ends up wasting somebody's afternoon.
    // The button no longer carries the count — the chips below it say what the
    // filters ARE, which is what the count was standing in for. What it keeps
    // is the TINT (§4: "active filters tint the funnel icon"), because that is
    // visible from across the window and a row of chips is not.
    m_filterBtn->setText(QStringLiteral("Filter"));
    QFont f = m_filterBtn->font();
    f.setBold(n > 0);
    m_filterBtn->setFont(f);
    m_filterBtn->setStyleSheet(
        n > 0 ? QStringLiteral("QToolButton{color:#2f7fd0;}") : QString());
    if (m_chips) {
        const fox::ModelTags& tags = fox::ModelTags::instance();
        m_chips->setQuery(m_search->text(), [&tags](const QString& t) {
            return tags.labelFor(t);
        });
    }
}

int ModelsTab::listCount() const
{
    return m_listModel ? m_listModel->shown() : 0;
}

bool ModelsTab::openFilterForShot()
{
    if (!m_filterPopup || !m_filterBtn) return false;
    openFilterPopup();
    return m_filterPopup->isVisible();
}

void ModelsTab::openFilterPopup()
{
    if (!m_filterPopup) return;
    // Clicking the button while the popup is open has to CLOSE it, and on
    // Windows that takes explicit work. A Qt::Popup closes itself on the press
    // that lands outside it, and the Windows platform plugin then REPLAYS that
    // press onto whatever was underneath — which is this button, so the popup
    // reopens and looks impossible to dismiss with its own control. (XCB does
    // not replay, so this cannot be reproduced on Linux; the styleHint is
    // ReplayMousePressOutsidePopup, true on Windows and false on XCB.) A click
    // arriving within a few frames of the popup closing is that replay.
    if (m_filterPopup->msSinceClosed() < 200) return;
    m_filterPopup->showFor(m_filterBtn, m_search->text());
    m_filterPopup->setResultText(filterSummary());
}

void ModelsTab::onIndexReady(bool ready)
{
    // The panel describes a scene addressed by file index, and every one of
    // those indices has just been invalidated. Its decode cache is keyed by
    // asset path, which now resolves to a different install's bytes. Both go.
    if (m_inspector) m_inspector->clear();

    // Every cached render is keyed by file index, and those have just been
    // reassigned.
    fox::ThumbnailRenderer::instance().reset();
    if (ready) {
        // The tag vocabulary is derived from the index, so it has to be rebuilt
        // before anything asks the list to filter by it.
        if (m_filterPopup) m_filterPopup->rebuild();
        m_listModel->refresh(m_search->text());
        updateFilterButton();
        // ── THE OUTLINER, WHICH THIS NEVER TOUCHED ──────────────────────
        // The tab restores its display mode in the constructor, so when the
        // remembered mode is Outliner the tree is BUILT THERE — against an
        // index that has not loaded yet — and nothing rebuilt it when the
        // index arrived. The result is the user's "the outliner isn't
        // populated with anything until a filter option is used": the filter
        // path rebuilds it, so toggling any filter on and off was the only way
        // to get a tree at all.
        if (m_outliner && listViewActive()) {
            m_outliner->refresh();
            m_outliner->setLoadedModel(m_currentFile,
                                       m_hasModel ? &m_model : nullptr);
        }
        populateAnimCombo();
        // The catalogues these panels read only exist once the index does, and
        // the panels were filled in the constructor against nothing. Refill
        // what is open now that there is something to fill it with —
        // AnimationsPanel::isBuilt() reports itself stale, so this rebuilds
        // rather than skipping.
        fillOpenPanels();
        m_frigSearched = false;   // re-locate against the new index
        m_hasFrig = false;
        // Explicit rather than emergent: the rows all changed, so queue the
        // visible page instead of waiting for something to trigger a repaint.
        if (m_gridMode) m_thumbTimer->start(0);
    }
}

void ModelsTab::populateAnimCombo()
{
    // THE BAR FOLLOWS THE PANEL'S SCOPE. It used to list every archive in the
    // catalogue while the ANIMATIONS panel beside it showed only the ones that
    // can pose the loaded model — two controls for "what can I play here",
    // giving different answers, and the one under the viewport was the wrong
    // one. The panel owns the resolution (it is the thing that reads the rig);
    // this asks it rather than resolving a second time, because a second
    // resolution is a second answer waiting to happen.
    QSet<int> scopeFiles;
    bool scoped = false;
    QString note;
    if (m_animPanel && !m_animPanel->scopeIsAll()) {
        scopeFiles = m_animPanel->scopeArchiveFiles();
        scoped = true;
        note = QStringLiteral("Narrowed to %1 — %2")
                   .arg(m_animPanel->scope() == QLatin1String("model")
                            ? QStringLiteral("this model's rig")
                            : QStringLiteral("the pinned model's rig"),
                        m_animPanel->scopeSummary());
    }
    // What is playing must survive the repopulation when it is still in scope;
    // when it is not, the bar falls back to "No animation" rather than
    // silently selecting a different archive.
    const QVariant keep = m_mtarCombo->currentPayload();
    m_mtarCombo->blockSignals(true);
    animcombo::fillArchives(m_mtarCombo, scoped ? &scopeFiles : nullptr, note);
    const bool kept = keep.isValid() && keep.toInt() >= 0
                      && m_mtarCombo->selectPayload(keep.toInt());
    m_mtarCombo->blockSignals(false);
    // The archive that was playing is no longer in scope — which happens the
    // moment a model is loaded that its rig cannot pose. Stopping is the
    // honest outcome: the transport would otherwise go on playing a clip that
    // is posing nothing, naming an archive the list beside it does not have.
    // Through the combo WITH signals live, so the one stop path runs.
    if (!kept && m_hasMtar) {
        qInfo("models: the playing archive is outside the new animation scope "
              "— stopping");
        m_mtarCombo->selectPayload(-1);
    }
}

void ModelsTab::locateFrig()
{
    // One shared resolver (rigbind): engine-authoritative ModelDescription
    // binding, humanoid heuristic as fallback.
    if (m_frigSearched) return;
    m_frigSearched = true;
    const ArchiveIndex& index = ArchiveIndex::instance();
    QString modelPath;
    if (m_currentFile >= 0 && m_currentFile < index.files().size())
        modelPath = index.files()[m_currentFile].path;
    QString via;
    m_hasFrig = rigbind::loadFrigFor(modelPath, &m_frig, &via);
    if (m_hasFrig)
        qInfo("models: rig bound (%s) — %d units, %d bones", qUtf8Printable(via),
              m_frig.rigUnitCount(), static_cast<int>(m_frig.bones().size()));
}

void ModelsTab::loadMtar(int fileIdx)
{
    const ArchiveIndex& index = ArchiveIndex::instance();
    if (fileIdx < 0 || fileIdx >= index.files().size()) return;
    const IndexedFile& f = index.files()[fileIdx];
    const QByteArray data = index.readFile(f);
    m_hasMtar = m_hasAnim = false;
    m_clipCombo->blockSignals(true);
    m_clipCombo->clear();
    if (data.isEmpty() || !m_mtar.parse(data)) {
        // Same reason as the failed clip decode: with playback no longer
        // stopping on every combo change, an archive that does not parse would
        // leave the timer ticking over a frozen model.
        stopPlayback();
        m_view->clearPose();
        m_clipCombo->blockSignals(false);
        m_clipCombo->setEnabled(false);
        m_playBtn->setEnabled(false);
        m_frameSlider->setEnabled(false);
        syncAnimProvider();
        syncAnimPanel();
        setStatus(QStringLiteral("Failed to load %1: %2")
                            .arg(f.path, m_mtar.errorString()));
        return;
    }
    m_hasMtar = true;
    locateFrig();
    animcombo::fillClips(m_clipCombo, m_mtar, fileIdx);
    m_clipCombo->blockSignals(false);
    qInfo("models: mtar %s (%s, %d clips)", qUtf8Printable(f.path),
          m_mtar.isV2() ? "v2" : "v1/GZ",
          static_cast<int>(m_mtar.clips().size()));
    if (m_clipCombo->count() > 0) {
        // Through selectPayload, so the combo lands on clip 0 rather than on
        // whatever row 0 turned out to be — which, with captions, is a header.
        m_clipCombo->blockSignals(true);
        m_clipCombo->selectPayload(0);
        m_clipCombo->blockSignals(false);
        loadClip(0);   // …which syncs the panel itself
    } else {
        // An archive with no clips at all. loadClip is never reached, so
        // without this the panel keeps highlighting the previous archive's
        // clip while the transport plays nothing.
        syncAnimPanel();
    }
}

void ModelsTab::loadClip(int clipIdx)
{
    if (!m_hasMtar || clipIdx < 0 || clipIdx >= m_mtar.clips().size()) return;
    m_hasAnim = false;
    m_anim = m_mtar.decodeClip(clipIdx);
    if (!m_anim.valid()) {
        // Now that a clip change no longer stops playback, a clip that fails
        // to decode would leave the timer running against nothing — the button
        // saying "playing" over a still model, forever.
        stopPlayback();
        m_view->clearPose();
        m_frameSlider->setEnabled(false);
        m_playBtn->setEnabled(false);
        m_frameLabel->setText(QStringLiteral("—"));
        syncAnimProvider();
        syncAnimPanel();
        setStatus(QStringLiteral("Clip decode failed: %1")
                            .arg(m_mtar.clips()[clipIdx].name));
        return;
    }
    m_hasAnim = true;
    m_frameSlider->blockSignals(true);
    m_frameSlider->setRange(0, qMax(0, m_anim.frameCount - 1));
    m_frameSlider->setValue(0);
    m_frameSlider->blockSignals(false);
    m_frameSlider->setEnabled(true);
    m_playBtn->setEnabled(true);
    m_recenterPending = true;
    setFrame(0.0f);
    syncAnimProvider();
    syncAnimPanel();
}

void ModelsTab::setFrame(float f, bool fromSlider)
{
    m_frame = f;
    if (!m_hasModel || !m_hasAnim) return;
    int driven = 0;
    const QVector<animmath::Mat4> pal = animpose::buildPalette(
        m_model, m_anim, f, m_hasFrig ? &m_frig : nullptr,
        m_hasFrdv ? &m_frdv : nullptr, &driven);

    // Reposed skeleton overlay: animate each bind endpoint by its skin matrix.
    QVector<float> lines;
    const auto& bones = m_model.bones();
    lines.reserve(bones.size() * 6);
    for (int b = 0; b < bones.size(); ++b) {
        const int p = bones[b].parentIndex;
        if (p < 0 || p >= bones.size()) continue;
        const animmath::Vec3 pw = animmath::transform(
            animmath::Vec3(bones[p].worldPos[0], bones[p].worldPos[1],
                           bones[p].worldPos[2]), pal[p]);
        const animmath::Vec3 bw = animmath::transform(
            animmath::Vec3(bones[b].worldPos[0], bones[b].worldPos[1],
                           bones[b].worldPos[2]), pal[b]);
        lines << pw.x << pw.y << pw.z << bw.x << bw.y << bw.z;
    }
    m_view->applyPose(pal, lines);

    if (m_recenterPending && !lines.isEmpty()) {
        m_recenterPending = false;
        QVector3D mn(1e9f, 1e9f, 1e9f), mx(-1e9f, -1e9f, -1e9f);
        for (int i = 0; i + 2 < lines.size(); i += 3) {
            mn.setX(qMin(mn.x(), lines[i]));     mx.setX(qMax(mx.x(), lines[i]));
            mn.setY(qMin(mn.y(), lines[i + 1])); mx.setY(qMax(mx.y(), lines[i + 1]));
            mn.setZ(qMin(mn.z(), lines[i + 2])); mx.setZ(qMax(mx.z(), lines[i + 2]));
        }
        m_view->centerOn((mn + mx) * 0.5f, (mx - mn).length() * 0.62f);
    }

    if (!fromSlider) {
        m_frameSlider->blockSignals(true);
        m_frameSlider->setValue(qRound(f));
        m_frameSlider->blockSignals(false);
    }
    m_frameLabel->setText(QStringLiteral("%1 / %2 · %3 bones")
                              .arg(qRound(f))
                              .arg(qMax(0, m_anim.frameCount - 1))
                              .arg(driven));
}

void ModelsTab::stopPlayback()
{
    if (m_playBtn->isChecked()) m_playBtn->setChecked(false);
    // …and stop the TIMER directly, not only through the button's toggled
    // signal. Unchecking a box that is already unchecked emits nothing, and
    // the timer is the thing that actually has to stop.
    if (m_animTimer) m_animTimer->stop();
    m_frame = 0.0f;
}

void ModelsTab::syncAnimProvider()
{
    if (!m_view) return;
    // Cleared, not left installed and returning nothing. The Camera page and
    // the viewport's own context menu both decide whether to offer "Save
    // animation GIF…" purely on whether a provider is there; an installed
    // provider that answers with an empty list is a live button that opens a
    // dialog and then fails at the end of it.
    if (!m_hasModel || !m_hasAnim || m_anim.frameCount <= 0) {
        m_view->setAnimationFrameProvider({});
        return;
    }
    m_view->setAnimationFrameProvider(
        [this](int frames) { return renderClipFrames(frames); });
}

QVector<QImage> ModelsTab::renderClipFrames(int frames)
{
    QVector<QImage> out;
    if (!m_hasModel || !m_hasAnim || m_anim.frameCount <= 0) return out;
    // CAPPED AT THE CLIP'S OWN LENGTH. Asking for 120 frames of a 30-frame
    // clip does not make a smoother GIF — it makes 90 duplicated poses and a
    // file four times the size, and the capture dialog says this is the tab's
    // job because only the tab knows how long the clip is.
    const int n = qBound(2, frames, qMax(2, m_anim.frameCount));
    const int last = qMax(0, m_anim.frameCount - 1);
    // Playback has to stop for the duration or the timer keeps advancing the
    // pose between grabs and the frames come out of order.
    const bool wasPlaying = m_playBtn && m_playBtn->isChecked();
    if (wasPlaying) m_playBtn->setChecked(false);
    const float saved = m_frame;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        // Sampled across last+1, not last: a GIF loops, so ending ON the last
        // frame when frame 0 is the same pose holds that pose for two frames
        // every cycle — a visible stutter at the loop point.
        const float f =
            last > 0 ? qMin(float(last),
                            float(i) * float(last + 1) / float(n))
                     : 0.0f;
        setFrame(f);
        out.push_back(m_view->grabFramebuffer());
    }
    // Put the viewport back exactly where the user left it, playing included.
    setFrame(saved);
    if (wasPlaying) m_playBtn->setChecked(true);
    return out;
}

bool ModelsTab::openAnimPopup(const QString& which)
{
    SearchableCombo* target = nullptr;
    if (which.compare(QLatin1String("mtar"), Qt::CaseInsensitive) == 0
        || which.compare(QLatin1String("archive"), Qt::CaseInsensitive) == 0)
        target = m_mtarCombo;
    else if (which.compare(QLatin1String("clip"), Qt::CaseInsensitive) == 0)
        target = m_clipCombo;
    if (!target || !target->isEnabled()) return false;
    target->showPopup();
    return true;
}

bool ModelsTab::selectAnim(const QString& mtarFilter, const QString& clipFilter,
                           float frame)
{
    if (m_mtarCombo->count() <= 1) populateAnimCombo();
    // Searched by PAYLOAD, not by row: the combo now carries group captions,
    // so row N and archive N are different things and walking rows would have
    // matched a heading.
    int wantFile = -1;
    const auto& files = ArchiveIndex::instance().files();
    for (int i = 0; i < m_mtarCombo->count(); ++i) {
        const QVariant pv = m_mtarCombo->itemData(i, richcombo::PayloadRole);
        if (!pv.isValid()) continue;
        const int fi = pv.toInt();
        if (fi >= 0 && fi < files.size()
            && files[fi].path.contains(mtarFilter, Qt::CaseInsensitive)) {
            wantFile = fi;
            break;
        }
    }
    if (wantFile < 0) return false;
    if (!m_mtarCombo->selectPayload(wantFile)) return false;  // loads + clip 0
    if (!m_hasMtar) return false;
    if (!clipFilter.isEmpty()) {
        bool isNum = false;
        const int asNum = clipFilter.toInt(&isNum);
        int clipIdx = -1;
        if (isNum && asNum >= 0 && asNum < m_mtar.clips().size()) {
            clipIdx = asNum;
        } else {
            // Matched against the ASSET name and our label both, so a harness
            // line written against either spelling keeps working.
            for (int i = 0; i < m_mtar.clips().size(); ++i) {
                const QString raw = m_mtar.clips()[i].name;
                if (raw.contains(clipFilter, Qt::CaseInsensitive)
                    || fox::animLabelFor(raw).contains(clipFilter,
                                                       Qt::CaseInsensitive)) {
                    clipIdx = i;
                    break;
                }
            }
        }
        if (clipIdx < 0) return false;
        if (!m_clipCombo->selectPayload(clipIdx)) return false;
    }
    if (m_hasAnim && frame > 0.0f) {
        m_recenterPending = true;   // reframe on the requested frame's pose
        setFrame(qBound(0.0f, frame, float(qMax(0, m_anim.frameCount - 1))));
    }
    return m_hasAnim;
}

bool ModelsTab::selectModel(const QString& nameFilter)
{
    m_search->setText(nameFilter);
    m_listModel->refresh(nameFilter);
    if (m_listModel->rowCount(QModelIndex()) == 0) return false;
    const QModelIndex first = m_listModel->index(0, 0);
    m_list->setCurrentIndex(first);
    return true;
}

void ModelsTab::setSearchText(const QString& text)
{
    if (!m_search) return;
    m_search->setText(text);
    // The interactive path debounces; the harness has no time to wait for it,
    // and queueing thumbnails against the UNFILTERED list is exactly the bug
    // this papers over. refreshList() applies it now, synchronously.
    refreshList();
}

void ModelsTab::showGrid(bool on, int iconPx)
{
    if (iconPx > 0) setIconSize(iconPx);
    // Through the display dropdown, so the harness drives the same switch the
    // user does. Off means LIST, not "whatever was up before" — the harness
    // asks for a grid or for no grid, and the outliner is a third answer it
    // has never asked for.
    m_display->setMode(on ? fox::displaymode::grid() : fox::displaymode::list());
    applyDisplayMode(m_display->mode());
    // The harness has no event loop between here and the grab. Pump it so the
    // view actually lays out — visualRect() is empty until it has — then queue
    // the visible page and wait for the render thread to drain it.
    for (int pass = 0; pass < 8; ++pass) {
        QCoreApplication::processEvents();
        renderVisibleThumbnails();
    }
    // Then drain. A round queues everything visible, waits for the render
    // thread to empty the queue, and repaints; when a round finds nothing left
    // to do the page is complete. Rounds rather than one wait, because paint
    // itself queues — the queue can be empty a moment before the next repaint
    // discovers a cell that still has no render.
    fox::ThumbnailRenderer& thumbs = fox::ThumbnailRenderer::instance();
    QElapsedTimer wait;
    wait.start();
    for (int round = 0; round < 40 && wait.elapsed() < 120000; ++round) {
        renderVisibleThumbnails();
        m_list->viewport()->update();
        QCoreApplication::processEvents();
        if (round > 0 && thumbs.outstanding() == 0) break;
        while (thumbs.outstanding() > 0 && wait.elapsed() < 120000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    m_list->viewport()->update();
    QCoreApplication::processEvents();
}

// One switch for the three views, so the list, the outliner and the thumbnail
// pump can never be in two states at once. setGridMode() still exists and
// still means what it meant — it is the GRID half of this — because the dev
// harness and the settings replay both call it.
// ── LIST AND OUTLINER ARE ONE WIDGET ─────────────────────────────────────
//
// The user asked three times for the List view to have toggleable, sortable
// columns — "so much information in one line that isn't even organized" — and
// separately asked for the outliner to become exactly that. Building both
// would be two implementations of one list, and they would drift apart the
// first time either was touched.
//
// So List and Outliner are the SAME columned widget, and the only difference
// between them is whether the loaded model's row grows into Mesh / Materials /
// Armature / Animations. That is one switch. GRID keeps the QListView, because
// a wall of thumbnails genuinely is a different thing.
//
// This also fixes "list view: icon options not working, no icons show up" by
// construction: the list is now the view whose icon modes already worked.
void ModelsTab::applyDisplayMode(const QString& id)
{
    const bool grid = (id == fox::displaymode::grid());
    const bool outliner = (id == fox::displaymode::outliner());
    setGridMode(grid);
    m_list->setVisible(grid);
    m_outliner->setVisible(!grid);
    if (!grid) {
        m_outliner->setShowLoadedTree(outliner);
        // Built on DEMAND: rebuilding on every keystroke while this is not on
        // screen is work nobody asked for.
        m_outliner->refresh();
        m_outliner->setLoadedModel(m_currentFile,
                                   m_hasModel ? &m_model : nullptr);
    }
    rebuildDisplayOptions();
}

void ModelsTab::setGridMode(bool on)
{
    m_gridMode = on;
    // Qt's alternating rows are a LIST-mode idea: in icon mode it bands by
    // model row, which in a wrapped grid is every second TILE, not every
    // second line of tiles — a chequerboard. So the view opts out here and the
    // delegate bands the visual rows itself, from the same derivation, in
    // paint().
    fox::rowshade::setEnabled(m_list, !on);
    if (on) {
        m_list->setViewMode(QListView::IconMode);
        m_list->setResizeMode(QListView::Adjust);
        m_list->setMovement(QListView::Static);
        m_list->setWrapping(true);
        m_list->setSpacing(2);
        m_list->setItemDelegate(m_gridDelegate);
        m_list->setGridSize(m_gridDelegate->cellSize());
        // Build the context and thread now, not on the first painted cache
        // miss — that would happen inside a paint event.
        fox::ThumbnailRenderer::instance().prewarm();
        m_thumbTimer->start(0);
    } else {
        m_list->setViewMode(QListView::ListMode);
        m_list->setWrapping(false);
        m_list->setSpacing(0);
        m_list->setGridSize(QSize());
        m_list->setItemDelegate(m_listDelegate);   // never nullptr — see above
    }
    m_list->setUniformItemSizes(true);
    // ── SCROLLING ───────────────────────────────────────────────────────
    // Qt's default for a QListView is ScrollPerItem, and in ICON MODE an
    // "item" is one TILE — so a wheel notch moves three tiles, which in a grid
    // eight tiles wide is three eighths of a row. That is the user's "scroll
    // wheel is extremely slow in grid": it is not slow in list mode, where an
    // item is a whole row, and it gets slower the wider the grid.
    //
    // Per-pixel, with the single step set to a fraction of a row, so a notch
    // moves the same visual distance in both modes.
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    if (m_gridMode) {
        const int row = qMax(24, m_gridDelegate->cellSize().height());
        m_list->verticalScrollBar()->setSingleStep(qMax(8, row / 3));
    } else {
        m_list->verticalScrollBar()->setSingleStep(
            qMax(8, m_list->sizeHintForRow(0) > 0 ? m_list->sizeHintForRow(0)
                                                  : 18));
    }
}

// Row zoom for the List and Outliner views: a point-size delta on the view's
// own font, clamped, remembered. A delta rather than an absolute size because
// the base is the system font and the user may have changed it — storing
// "11pt" would fight their own setting on the next machine.
void ModelsTab::setRowZoom(int delta)
{
    delta = qBound(-3, delta, 12);
    if (delta == m_rowZoom && m_rowZoomApplied) return;
    m_rowZoom = delta;
    m_rowZoomApplied = true;
    QSettings().setValue(QStringLiteral("models/rowZoom"), m_rowZoom);
    const QFont base = QApplication::font();
    QFont f = base;
    // pointSizeF() is -1 for a font built from a PIXEL size, and handing that
    // back with a delta makes Qt warn and ignore the whole font — the same
    // trap the animations panel's small-font code documents.
    if (base.pointSizeF() > 2.0)
        f.setPointSizeF(qMax(5.0, base.pointSizeF() + m_rowZoom));
    else if (base.pixelSize() > 2)
        f.setPixelSize(qMax(7, base.pixelSize() + m_rowZoom));
    if (m_list) m_list->setFont(f);
    if (m_outliner) m_outliner->setRowFont(f);
    qInfo("models: row zoom %+d -> %.1fpt", m_rowZoom, f.pointSizeF());
}

void ModelsTab::setIconSize(int px)
{
    px = qBound(48, px, 320);
    if (px == m_gridDelegate->iconSize()) return;
    m_gridDelegate->setIconSize(px);
    m_list->setGridSize(m_gridDelegate->cellSize());
    QSettings().setValue(QStringLiteral("models/gridIcon"), px);
    // The cache is keyed by size, so the old renders stay valid if the user
    // scrolls back to a size they have already used.
    m_list->viewport()->update();
    if (m_gridMode) m_thumbTimer->start(kThumbSettleMs);
}

// Queue the thumbnails for the rows currently on screen. This costs a hash
// lookup per visible cell and NOTHING else — every render happens on the
// thumbnail thread, and the results arrive back through ThumbnailRenderer's
// ready() signal. The GUI thread never parses a model.
void ModelsTab::renderVisibleThumbnails()
{
    m_thumbTimer->stop();
    if (!m_gridMode || !m_listModel || m_listModel->rowCount(QModelIndex()) == 0)
        return;
    // Anything still waiting from a previous page has scrolled away. Dropping
    // it first means the thread always works on what is actually on screen.
    fox::ThumbnailRenderer::instance().cancelQueued();
    const int size = m_gridDelegate->iconSize();
    const QRect vp = m_list->viewport()->rect();
    // Find the first row on screen. indexAt() on a corner can land in the gap
    // between cells, so probe across the top edge before giving up — and never
    // fall back to "the whole list", which is exactly what this is here to
    // avoid.
    QModelIndex first;
    for (int x = 0; x < vp.width() && !first.isValid(); x += 16)
        first = m_list->indexAt(QPoint(vp.left() + x, vp.top() + 2));
    if (!first.isValid()) first = m_list->indexAt(vp.center());
    if (!first.isValid()) return;

    const int total = m_listModel->rowCount(QModelIndex());
    // Walk forward while the cell still touches the viewport. A hard cap keeps
    // a pathological layout from turning this into a full-list queue.
    for (int row = first.row(), seen = 0; row < total && seen < 512; ++row, ++seen) {
        const QModelIndex idx = m_listModel->index(row, 0, QModelIndex());
        if (!idx.isValid()) break;
        const QRect r = m_list->visualRect(idx);
        if (r.top() > vp.bottom()) break;          // past the bottom edge
        if (!r.intersects(vp)) continue;
        const int fileIdx = idx.data(FmdlListModel::FileIdxRole).toInt();
        fox::ThumbnailRenderer::instance().request(fileIdx, size);
    }
}

namespace {
// What the hover popup shows for one indexed model. Built only when the pointer
// has actually stopped: the thumbnail comes from the renderer's cache when it
// is there (never blocking on a render), and the facts come from the index,
// which is already in memory.
hover::Content modelHoverContent(int fileIdx)
{
    hover::Content c;
    const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
    if (fileIdx < 0 || fileIdx >= ix.files().size()) return c;
    const fox::IndexedFile& f = ix.files()[fileIdx];
    c.name = f.path.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
    c.path = f.path;
    // The cache is keyed by SIZE, so the grid's own icon size is the one that
    // is already there; the bigger sizes are only present if a preview has
    // been open before. Take whatever exists, then ask for a big one.
    // UP TO THE PREVIEW'S OWN MAXIMUM, not 512. The popup can be wheeled to
    // 768 and this clamped what it asked the renderer for at 512, so the two
    // largest sizes showed a 512px render stretched into a bigger box — which
    // is what "the icon should not be cropped at all" looks like from outside.
    const int want =
        qBound(hover::kMinImage, hover::Preview::instance().imageSize(),
               hover::kMaxImage);
    for (int sz : {want, 768, 512, 384, 256, 192, 160, 128, 96, 64}) {
        c.image = fox::ThumbnailRenderer::instance().cached(fileIdx, sz);
        if (!c.image.isNull()) break;
    }
    fox::ThumbnailRenderer::instance().request(fileIdx, want);
    QStringList bits;
    if (f.size > 0)
        bits << QStringLiteral("%1 KB").arg((f.size + 1023) / 1024);
    if (f.archiveId >= 0 && f.archiveId < ix.archives().size())
        bits << ix.archives()[f.archiveId].shortName;
    if (f.childIdx >= 0) bits << QStringLiteral("inside a container");
    if (!bits.isEmpty()) c.info << bits.join(QStringLiteral("  ·  "));
    return c;
}

// The same, for a texture row under a material. Decoded through the shared
// TexThumbCache — the same picture the row's own icon came from, at whatever
// size the preview is currently set to, so wheeling it bigger asks for a
// bigger decode rather than stretching the small one.
hover::Content textureHoverContent(int fileIdx)
{
    hover::Content c;
    const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
    if (fileIdx < 0 || fileIdx >= ix.files().size()) return c;
    const fox::IndexedFile& f = ix.files()[fileIdx];
    c.name = f.path.section(QLatin1Char('/'), -1);
    c.path = f.path;
    const int want =
        qBound(hover::kMinImage, hover::Preview::instance().imageSize(),
               hover::kMaxImage);
    for (int sz : {want, 768, 512, 384, 256, 192, 128}) {
        c.image = fox::TexThumbCache::instance().cached(fileIdx, sz);
        if (!c.image.isNull()) break;
    }
    fox::TexThumbCache::instance().request(fileIdx, want);
    if (f.size > 0)
        c.info << QStringLiteral("%1 KB").arg((f.size + 1023) / 1024);
    return c;
}
}  // namespace

bool ModelsTab::hoverGridCell(int row)
{
    const QModelIndex ix = m_list->model() ? m_list->model()->index(row, 0)
                                           : QModelIndex();
    if (!ix.isValid()) return false;
    m_list->scrollTo(ix);
    const QRect r = m_list->visualRect(ix);
    if (!r.isValid()) return false;
    const QPoint p = r.center();
    QMouseEvent me(QEvent::MouseMove, QPointF(p),
                   QPointF(m_list->viewport()->mapToGlobal(p)), Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(m_list->viewport(), &me);
    return true;
}

bool ModelsTab::eventFilter(QObject* obj, QEvent* ev)
{
    // Ctrl+wheel ZOOMS, in all three view modes. It only did in the grid,
    // where it resizes the thumbnails; List and Outliner had no zoom at all,
    // and §4 asks all three for one. In those two the thing to scale is the
    // ROW, so it is the view's font — which takes the row height, the icons
    // and the indentation with it, and is one setting rather than three.
    if (ev->type() == QEvent::Wheel
        && (obj == m_list->viewport()
            || (m_outliner && obj == m_outliner->treeViewport()))) {
        auto* we = static_cast<QWheelEvent*>(ev);
        if (we->modifiers() & Qt::ControlModifier) {
            const int dir = we->angleDelta().y() > 0 ? 1 : -1;
            if (m_gridMode) setIconSize(m_gridDelegate->iconSize() + dir * 16);
            else setRowZoom(m_rowZoom + dir);
            return true;
        }
    }
    // Rest the pointer on a cell and the model is shown big, with its name,
    // its asset path and what is actually in it. The content is built lazily —
    // crossing a grid must not decode anything.
    if (obj == m_list->viewport()) {
        if (ev->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(ev);
            const QModelIndex ix = m_list->indexAt(me->pos());
            if (!ix.isValid()) {
                hover::Preview::instance().cancel();
            } else {
                const int fileIdx =
                    ix.data(FmdlListModel::FileIdxRole).toInt();
                if (qEnvironmentVariableIsSet("FOXAB_HOVER_DEBUG"))
                    qInfo("hover: grid cell fileIdx=%d", fileIdx);
                hover::Preview::instance().requestLazy(
                    QStringLiteral("model:%1").arg(fileIdx),
                    m_list->viewport()->mapToGlobal(me->pos()),
                    [fileIdx] { return modelHoverContent(fileIdx); });
            }
        } else if (ev->type() == QEvent::Leave
                   || ev->type() == QEvent::MouseButtonPress) {
            hover::Preview::instance().cancel();
        }
    }
    // ── AND THE OUTLINER ────────────────────────────────────────────────
    // "Anytime there's an icon present the on-hover preview should work." It
    // was wired to the list/grid viewport only, so the outliner — the view
    // with the most icons in it — had none. Model rows show the model; texture
    // rows under a material show the texture, which is the one place in the
    // tool where a texture's full art is reachable while looking at a model.
    if (m_outliner && obj == m_outliner->treeViewport()) {
        if (ev->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(ev);
            int fileIdx = -1;
            bool isTexture = false;
            m_outliner->hoverSubjectAt(me->pos(), &fileIdx, &isTexture);
            if (fileIdx < 0) {
                hover::Preview::instance().cancel();
            } else if (isTexture) {
                hover::Preview::instance().requestLazy(
                    QStringLiteral("tex:%1").arg(fileIdx),
                    m_outliner->treeViewport()->mapToGlobal(me->pos()),
                    [fileIdx] { return textureHoverContent(fileIdx); });
            } else {
                hover::Preview::instance().requestLazy(
                    QStringLiteral("model:%1").arg(fileIdx),
                    m_outliner->treeViewport()->mapToGlobal(me->pos()),
                    [fileIdx] { return modelHoverContent(fileIdx); });
            }
        } else if (ev->type() == QEvent::Leave
                   || ev->type() == QEvent::MouseButtonPress) {
            hover::Preview::instance().cancel();
        }
    }
    return QWidget::eventFilter(obj, ev);
}

QSet<int> ModelsTab::hiddenSubmeshes() const
{
    // From the TREE, not from the viewport. The two agree while a model is
    // loaded, but the tree is the control the user touched and the viewport is
    // where the answer was applied — reading the control keeps the export
    // scope and the panel one statement rather than two.
    return m_sceneTree ? m_sceneTree->hiddenLeaves() : QSet<int>();
}

void ModelsTab::reloadKeepingParts()
{
    if (m_currentFile < 0) return;
    // loadModel() rebuilds the parts tree with every row ticked, because for a
    // DIFFERENT model that is right. Reloading the SAME model to pick up a
    // shading change is not a different model, and silently re-showing four
    // submeshes the user had hidden is the kind of thing that makes a viewport
    // feel unreliable.
    //
    // Keyed by the submesh's own id — the mesh's index in the FMDL — not by
    // its row or its label. The rows are rebuilt from scratch and their order
    // is not promised, and a model can carry two mesh groups with the SAME
    // name; keying on text hid both when the user had hidden one.
    const QSet<int> hidden = hiddenSubmeshes();
    loadModel(m_currentFile);
    if (hidden.isEmpty()) return;
    // Through the tree, so the check states, the parent tri-states and the
    // viewport all move together — setHiddenLeaves emits one signal per leaf
    // that actually changed and the viewport is wired to those.
    if (m_sceneTree) m_sceneTree->setHiddenLeaves(hidden);
}

void ModelsTab::setPbrShading(bool on)
{
    // Straight at the viewport, which is where the switch lives now. The
    // reload the maps may need is not skipped by going this way: the tab
    // watches displayChanged for exactly that and fetches them.
    if (m_view) m_view->setPbrShading(on);
}

void ModelsTab::setMaterialFilter(const QString& text)
{
    if (m_inspector) m_inspector->setFilterText(text);
}

void ModelsTab::syncPbrFromSettings()
{
    if (!m_view) return;
    const bool want = Config::pbrEnabled(Config::PbrView::Models);
    if (m_view->pbrShading() == want) return;
    // The VIEWPORT is the authority on this now, and the load path reads it
    // from there. Without this a change in Settings did nothing at all until
    // the app was restarted.
    m_view->setPbrShading(want);
}

void ModelsTab::setSubmeshTreeVisible(bool on)
{
    if (m_npanel) m_npanel->setPanelOpen(QStringLiteral("parts"), on);
}

bool ModelsTab::hideSubmesh(const QString& needle)
{
    return m_sceneTree && m_sceneTree->uncheckMatching(needle);
}

void ModelsTab::setDebugPanelVisible(bool on)
{
    // Through the COLUMN, not straight at the widget: opening a panel is what
    // makes the tab fill it, and setting the widget's visibility alone would
    // open an empty one.
    if (m_npanel) m_npanel->setPanelOpen(QStringLiteral("materials"), on);
}

// What to call a mesh group. The FMDL stores a HASH and only sometimes a
// string for it, so a model that names none of its groups made both panels
// read 0x619a9e…, 0xfbae1a…, 0x67e490…. The hash is still the truth and goes
// in the tooltip; the label is the group's own index, the SAME number the
// tooltip quotes, because two numbers for one group is worse than a hash.
static QString meshGroupLabel(const fox::FmdlFile& model, int g)
{
    const auto& groups = model.meshGroups();
    if (g < 0 || g >= groups.size())
        return QStringLiteral("(no group %1)").arg(g);
    const QString raw = groups[g].name;
    return raw.isEmpty() || raw.startsWith(QLatin1String("0x"))
        ? QStringLiteral("Group %1").arg(g)
        : raw;
}

static QString meshGroupTip(const fox::FmdlFile& model, int g)
{
    const auto& groups = model.meshGroups();
    const QString raw = g >= 0 && g < groups.size() ? groups[g].name : QString();
    return raw.isEmpty()
        ? QStringLiteral("Mesh group %1 — this group carries no name of its own")
              .arg(g)
        : QStringLiteral("Mesh group %1 — %2").arg(g).arg(raw);
}

void ModelsTab::refreshSceneTree()
{
    if (!m_sceneTree) return;
    // The tree is about to be rebuilt with every row checked, so the
    // viewport's per-mesh switches have to go with it. Without this, closing
    // the panel with a submesh hidden and reopening it showed a ticked box
    // over an invisible mesh — and clicking that box changed no state, so it
    // emitted nothing and the mesh stayed gone.
    m_view->clearMeshVisibility();
    // NOT gated on the pane being open. The tree is this tab's per-part
    // visibility state and the export reads it; a scope that existed only
    // while a pane happened to be showing would make the same Ctrl+E write two
    // different files.
    if (!m_hasModel) {
        m_sceneTree->clear();
        return;
    }
    // One root per mesh GROUP, one leaf per submesh in it. Meshes whose group
    // index is out of range get a root of their own rather than being dropped:
    // a submesh you cannot see in the tree is one you cannot switch off, and
    // "the model has a group the tree does not" is worth seeing.
    QVector<SceneTree::Node> roots;
    QHash<int, int> rootOf;   // group index -> position in `roots`
    const auto& groups = m_model.meshGroups();
    for (int mi = 0; mi < m_model.meshes().size(); ++mi) {
        const fox::FmdlMesh& mesh = m_model.meshes()[mi];
        // The same test buildUploads() applies. A mesh with no vertices or no
        // triangles never becomes an upload, so a row for it would be a switch
        // wired to nothing and would inflate the submesh count in the header.
        if (mesh.positions.isEmpty() || mesh.triangles.isEmpty()) continue;
        const int g = mesh.meshGroupIndex;
        if (!rootOf.contains(g)) {
            SceneTree::Node r;
            // The same label the Parts list uses — the two panels sit side by
            // side and named the same group two different ways.
            r.label = meshGroupLabel(m_model, g);
            r.tip = meshGroupTip(m_model, g);
            rootOf.insert(g, roots.size());
            roots.append(r);
        }
        SceneTree::Node leaf;
        leaf.label = QStringLiteral("mesh %1").arg(mi);
        leaf.meshId = mi;                     // buildUploads uses the same id
        leaf.tris = int(mesh.triangles.size() / 3);
        leaf.materialSlot = mesh.materialInstanceIndex;
        if (mesh.materialInstanceIndex >= 0
            && mesh.materialInstanceIndex < m_model.materials().size())
            leaf.material = m_model.materials()[mesh.materialInstanceIndex].name;
        roots[rootOf.value(g)].children.append(leaf);
    }
    m_sceneTree->setScene(roots);
}

void ModelsTab::refreshInspector()
{
    if (!m_inspector) return;
    if (!m_npanel || !m_npanel->isPanelOpen(QStringLiteral("materials"))) {
        // The maps are NOT dropped here. This tab holds one model, so keeping
        // them costs four half-resolution maps per material; dropping them
        // meant opening the panel had to reload the model, which cleared every
        // per-group visibility checkbox the user had set. A debug panel that
        // changes what you are looking at in order to describe it is worse
        // than no panel.
        m_inspector->clear();
        return;
    }
    // m_currentFile can be STALE after a re-index — onIndexReady leaves it
    // alone — so it is bounds-checked here the way exportCurrent() already
    // checks it. Reading files()[30000] of a 12,000-entry list because the
    // user removed a game folder is not a thing a debug panel should do.
    const auto& files = ArchiveIndex::instance().files();
    if (!m_hasModel || m_currentFile < 0 || m_currentFile >= files.size()) {
        m_inspector->clear();
        return;
    }
    MaterialInspector::Source src;
    src.label = files[m_currentFile].path.section(QLatin1Char('/'), -1);
    src.materials = MaterialInspector::entriesFor(m_model);
    src.base = m_textures;
    src.normals = m_normalMaps;
    src.pbr = m_pbrForPanel;
    src.slotBase = 0;
    src.gz = files[m_currentFile].gz;
    m_inspector->setSources({src});
}

void ModelsTab::loadModel(int fileIdx)
{
    // BOUNDS FIRST, before m_currentFile is committed. onIndexReady() does not
    // reset m_currentFile or m_hasModel, so an index that shrank under us —
    // the user removes a game folder and rescans — leaves this holding a file
    // number the list no longer has. Every path that reloads the CURRENT model
    // rather than a freshly clicked one goes through here: the PBR toggle, the
    // settings sync, reloadKeepingParts(). exportCurrent() has checked this
    // for the same reason since long before those existed.
    const ArchiveIndex& index = ArchiveIndex::instance();
    if (fileIdx < 0 || fileIdx >= index.files().size()) {
        m_currentFile = -1;
        m_hasModel = false;
            m_view->clearModel();
        // …and the capture name with it, or the next screenshot from an empty
        // viewport is offered under the last model's name.
        m_view->setProperty("foxabCaptureName", QString());
        m_view->setProperty("foxabCaptureFileIdx", -1);
        m_hasModel = false;
        refreshSceneTree();      // clears it, and the viewport's mesh switches
        refreshInspector();
        setStatus(QStringLiteral(
            "That model is no longer in the index — rescan or pick another."));
        return;
    }
    m_currentFile = fileIdx;
    m_frigSearched = false;   // rig binding is per model (via its .parts)
    m_hasFrig = false;
    const IndexedFile& f = index.files()[fileIdx];

    // Connect points, from the sibling .fcnp — the same lookup the composer
    // does. Cleared first, or a model with no sockets would export the last
    // one's.
    m_hasFcnp = false;
    m_fcnp = fox::FcnpFile();
    if (f.path.endsWith(QLatin1String(".fmdl"))) {
        QString cnpPath = f.path;
        cnpPath.chop(5);
        cnpPath += QStringLiteral(".fcnp");
        for (const IndexedFile& c : index.files()) {
            if (c.path != cnpPath) continue;
            const QByteArray cd = index.readFile(c);
            m_hasFcnp = !cd.isEmpty() && m_fcnp.parse(cd);
            break;
        }
    }

    QElapsedTimer timer;
    timer.start();
    const QByteArray data = index.readFile(f);
    fox::FmdlFile& model = m_model;
    m_hasModel = false;
    if (data.isEmpty() || !model.parse(data)) {
        setStatus(QStringLiteral("Failed to load %1: %2")
                            .arg(f.path, model.errorString()));
        m_view->clearModel();
        // …and the capture name with it, or the next screenshot from an empty
        // viewport is offered under the last model's name.
        m_view->setProperty("foxabCaptureName", QString());
        m_view->setProperty("foxabCaptureFileIdx", -1);
        refreshSceneTree();      // clears it, and the viewport's mesh switches
        return;
    }

    // Shared pipeline with the Files preview / Customize composer.
    int texturesFound = 0;
    QVector<QImage>& textures = m_textures;
    textures = modelload::loadBaseTextures(model, f.gz, &texturesFound);
    int normalsFound = 0;
    m_normalMaps = modelload::loadNormalMaps(model, f.gz, &normalsFound);
    // The rest of the PBR set, when this viewport is configured for it. Loaded
    // with no FOVA overrides because the Models tab shows a model as shipped;
    // the Customize tab is where a variation is chosen.
    // THE VIEWPORT is the authority here, not the setting. The setting is its
    // default; once the user has switched PBR on for this session — from the
    // Graphics popover's Materials group, or by choosing the Rendered shading
    // ball — reloading a model must respect that rather than quietly dropping
    // back to the stored preference.
    QVector<GLPbrMaterial> pbr;
    if (m_view->pbrShading())
        pbr = modelload::loadPbrMaps(model, f.gz, nullptr);
    const QVector<GLMeshUpload> uploads = modelload::buildUploads(model);
    const GLSkeletonUpload skeleton = modelload::buildSkeleton(model);

    // The inspector is fed BEFORE setModel takes the maps: it reduces them to
    // thumbnails and keeps nothing full-size, and the viewport frees its own
    // CPU copies as soon as they are on the GPU.
    m_pbrForPanel = pbr;
    // Which game this model came out of, so the Auto environment can follow
    // it. One model, one answer — no vote needed here.
    m_view->setSceneGame(index.gameOf(f));
    m_view->setModel(uploads, textures, skeleton, m_normalMaps, std::move(pbr));
    // What a capture from this viewport should be called. A property rather
    // than a call into the panel: the panel reads it when a file dialog opens
    // and never otherwise, so there is nothing to react to.
    QString capName = QStringLiteral("model");
    {
        const auto& files = ArchiveIndex::instance().files();
        if (m_currentFile >= 0 && m_currentFile < files.size()) {
            capName = files[m_currentFile].path.section(QLatin1Char('/'), -1);
            if (capName.endsWith(QLatin1String(".fmdl"))) capName.chop(5);
        }
    }
    m_view->setProperty("foxabCaptureName", capName);
    m_view->setProperty("foxabCaptureFileIdx", m_currentFile);
    // Re-assert the shading mode across the load, in MODE terms. The old
    // setPbrShading(box) here clobbered Wireframe and Flat as well — every
    // model load dropped the viewport back to a lit mode — and it is what made
    // the Rendered ball unselectable, because it ran after the reload that
    // ball had just asked for.
    if (m_view->pbrShading())
        m_view->setShadingMode(ShadingMode::Rendered);
    else if (m_view->shadingMode() == ShadingMode::Rendered)
        m_view->setShadingMode(ShadingMode::Shaded);
    // Nothing to toggle when this model has no normal maps at all.
    // The Normal maps switch lives on the Graphics popover, which re-reads
    // hasNormalMaps() on every sceneChanged and greys itself out — so there is
    // nothing for the load path to enable here any more.

    // Help-bone driver: the .frdv beside the .fmdl (same stem).
    m_hasFrdv = false;
    if (f.path.endsWith(QLatin1String(".fmdl"))) {
        QString frdvPath = f.path;
        frdvPath.chop(5);
        frdvPath += QStringLiteral(".frdv");
        const int at = index.fileIndexForPath(frdvPath);
        if (at >= 0) {
            const QByteArray fd = index.readFile(index.files()[at]);
            if (!fd.isEmpty() && m_frdv.parse(fd)) {
                m_hasFrdv = true;
                qInfo("models: help bones %s (%d ops)", qUtf8Printable(frdvPath),
                      static_cast<int>(m_frdv.ops().size()));
            }
        }
    }

    m_hasModel = true;

    // The sockets, for the hardpoints overlay (template §5). Fox's connect
    // points ARE this engine's hardpoints, and they were already loaded above
    // for the exporter — this hands the same records to the viewport. The
    // position is the record unchanged, because a .fcnp point is authored in
    // its parent bone's BIND frame and an FMDL bind pose is translation-only;
    // the bone index lets the overlay carry the socket through a pose.
    {
        QVector<GLConnectPoint> points;
        if (m_hasFcnp) {
            QHash<QString, int> boneOf;
            const auto& bones = model.bones();
            for (int b = 0; b < bones.size(); ++b) boneOf.insert(bones[b].name, b);
            points.reserve(m_fcnp.points().size());
            for (const fox::ConnectPoint& cp : m_fcnp.points()) {
                GLConnectPoint g;
                g.name = cp.name;
                g.bone = cp.parentBone.isEmpty() ? -1
                                                 : boneOf.value(cp.parentBone, -1);
                // Its world position in bind pose is the record plus the
                // parent bone's world position — the point is stored relative
                // to that bone, and a bone with no parent entry is stored
                // relative to the model origin.
                QVector3D at(cp.pos[0], cp.pos[1], cp.pos[2]);
                if (g.bone >= 0)
                    at += QVector3D(bones[g.bone].worldPos[0],
                                    bones[g.bone].worldPos[1],
                                    bones[g.bone].worldPos[2]);
                g.pos = at;
                points.append(g);
            }
        }
        m_view->setConnectPoints(points);
    }

    // After m_hasModel, not before: the panel refuses to build without it, and
    // building it earlier meant the first model opened with an empty panel.
    refreshInspector();
    refreshSceneTree();
    m_lastMatMesh = -2;   // a new scene: the last answer is about another model
    refreshInfoPanel();
    refreshAttachments();
    updatePartsTitle();
    if (m_outliner && listViewActive())
        m_outliner->setLoadedModel(m_currentFile, &m_model);
    if (m_hasMtar) locateFrig();        // rebind the rig for THIS model
    if (m_hasAnim) setFrame(m_frame);   // keep the active clip posed on swap
    // A clip alone is not enough to record: the provider renders THIS model
    // posed, so it only becomes live once a model is loaded under the clip.
    syncAnimProvider();
    syncAnimPanel();   // …and the animation scope follows the new model
    setStatus(
        QStringLiteral("%1 — %2 · %3 base textures · %4 normal maps · loaded in %5 ms")
            .arg(f.path, model.describe())
            .arg(texturesFound)
            .arg(normalsFound)
            .arg(timer.elapsed()));
    qInfo("models: %s — %s (%d textures, %d normal maps, %lld ms)",
          qUtf8Printable(f.path), qUtf8Printable(model.describe()), texturesFound,
          normalsFound, static_cast<long long>(timer.elapsed()));
}

// What to call the loaded model on screen: its file name without the
// extension, or "model". One function, because the export dialog's suggested
// name, the menu's label and the INFO panel were deriving it three times.
QString ModelsTab::modelDisplayName() const
{
    const auto& files = ArchiveIndex::instance().files();
    if (m_currentFile < 0 || m_currentFile >= files.size())
        return QStringLiteral("model");
    QString base = files[m_currentFile].path.section(QLatin1Char('/'), -1);
    if (base.endsWith(QLatin1String(".fmdl"), Qt::CaseInsensitive)) base.chop(5);
    return base.isEmpty() ? QStringLiteral("model") : base;
}

// A submesh has no authored name of its own in an FMDL — the format stores a
// material instance index and a mesh group — so its MATERIAL name is the only
// human-readable label there is, and it is what the parts tree already shows.
// Falling back to the group and then to the index keeps the label honest
// rather than inventing one.
QString ModelsTab::submeshLabel(int meshId) const
{
    if (!m_hasModel || meshId < 0 || meshId >= m_model.meshes().size())
        return QString();
    const fox::FmdlMesh& m = m_model.meshes()[meshId];
    const auto& mats = m_model.materials();
    if (m.materialInstanceIndex >= 0 && m.materialInstanceIndex < mats.size()
        && !mats[m.materialInstanceIndex].name.isEmpty())
        return mats[m.materialInstanceIndex].name;
    if (m.meshGroupIndex >= 0) return meshGroupLabel(m_model, m.meshGroupIndex);
    return QStringLiteral("submesh %1").arg(meshId);
}

int ModelsTab::submeshTris(int meshId) const
{
    if (!m_hasModel || meshId < 0 || meshId >= m_model.meshes().size()) return 0;
    return m_model.meshes()[meshId].triangles.size() / 3;
}

// Export a SET of submeshes as one .glb, by hiding everything else for the
// duration of the write. It goes through exportTo() like every other export
// path in this tab — the colour bake, the PBR maps and the connect points all
// landed on that function, and a second copy of the export call is how a path
// misses them.
// The stem an export of `meshIds` gets. One place, so the prompting path and
// the silent one cannot name the same export differently.
QString ModelsTab::submeshExportStem(const QSet<int>& meshIds) const
{
    const int n = meshIds.size();
    return n == 1
               ? QStringLiteral("%1_%2").arg(modelDisplayName(),
                                             submeshLabel(*meshIds.constBegin()))
               : QStringLiteral("%1_%2parts").arg(modelDisplayName()).arg(n);
}

// "Export part to last folder" — the silent twin §12 asks for beside every
// export that prompts. Same writer, no dialog.
bool ModelsTab::exportSubmeshesToLast(const QSet<int>& meshIds)
{
    const QString dir = Config::exportDir();
    if (dir.isEmpty() || meshIds.isEmpty()) return false;
    return exportSubmeshesTo(
        QDir(dir).filePath(submeshExportStem(meshIds) + QStringLiteral(".glb")),
        meshIds);
}

bool ModelsTab::exportSubmeshes(const QSet<int>& meshIds)
{
    if (!m_hasModel || meshIds.isEmpty() || !m_sceneTree) return false;
    const int n = meshIds.size();
    const QString one = n == 1 ? submeshLabel(*meshIds.constBegin()) : QString();
    const QString out = QFileDialog::getSaveFileName(
        this,
        MenuText::exportLabel(QStringLiteral("Export"), n, one,
                              QStringLiteral("submesh")),
        QDir(Config::exportDir())
            .filePath(submeshExportStem(meshIds) + QStringLiteral(".glb")),
        QStringLiteral("glTF binary (*.glb)"));
    if (out.isEmpty()) return false;
    Config::setExportDir(QFileInfo(out).absolutePath());
    return exportSubmeshesTo(out, meshIds);
}

bool ModelsTab::exportSubmeshesTo(const QString& out, const QSet<int>& meshIds)
{
    if (!m_hasModel || meshIds.isEmpty() || !m_sceneTree) return false;

    // The user's own hidden set is RESTORED afterwards whatever happens. An
    // export that silently reshapes the scene it was asked about is the same
    // class of bug as a debug panel that reloads the model to describe it.
    const QSet<int> keep = m_sceneTree->hiddenLeaves();
    QSet<int> only;
    for (int i = 0; i < m_model.meshes().size(); ++i)
        if (!meshIds.contains(i)) only.insert(i);
    m_sceneTree->setHiddenLeaves(only);
    QString err;
    const bool ok = exportTo(out, &err);
    m_sceneTree->setHiddenLeaves(keep);
    setStatus(ok ? QStringLiteral("Exported %1").arg(out)
                       : QStringLiteral("Export failed: %1").arg(err));
    return ok;
}

// The stem this model exports under — one place, so the prompting path and the
// silent one cannot disagree about the file name.
QString ModelsTab::modelExportStem() const
{
    const ArchiveIndex& index = ArchiveIndex::instance();
    QString base = QStringLiteral("model");
    if (m_currentFile >= 0 && m_currentFile < index.files().size()) {
        base = index.files()[m_currentFile].path.section(QLatin1Char('/'), -1);
        if (base.endsWith(QLatin1String(".fmdl"))) base.chop(5);
    }
    return fox::templatedStem(base, m_currentFile);
}

// "Export model to last folder" (§12's silent twin). No dialog, and no
// message box either — an action whose whole promise is "writes where I said
// without asking" must not then interrupt. The status line says where it went.
bool ModelsTab::exportCurrentToLast()
{
    if (!m_hasModel) return false;
    const QString dir = Config::exportDir();
    if (dir.isEmpty()) return false;
    const QString out =
        QDir(dir).filePath(modelExportStem() + QStringLiteral(".glb"));
    QString err;
    const bool ok = exportTo(out, &err);
    setStatus(ok ? QStringLiteral("Exported %1").arg(out)
                 : QStringLiteral("Export failed: %1").arg(err));
    return ok;
}

void ModelsTab::exportCurrent()
{
    if (!m_hasModel) return;
    const ArchiveIndex& index = ArchiveIndex::instance();
    // The model itself is already loaded (m_model); the index is only needed
    // for the suggested file name — and m_currentFile can be stale after a
    // rescan, so bounds-check and fall back to a generic name.
    QString base = QStringLiteral("model");
    if (m_currentFile >= 0 && m_currentFile < index.files().size()) {
        base = index.files()[m_currentFile].path.section(QLatin1Char('/'), -1);
        if (base.endsWith(QLatin1String(".fmdl"))) base.chop(5);
    }
    base = fox::templatedStem(base, m_currentFile);
    const QString out = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export glTF binary"),
        QDir(Config::exportDir()).filePath(base + QStringLiteral(".glb")),
        QStringLiteral("glTF binary (*.glb)"));
    if (out.isEmpty()) return;
    Config::setExportDir(QFileInfo(out).absolutePath());
    // THROUGH exportTo(), not through a second copy of the export call. The
    // button and the harness were two separate paths into the exporter and
    // they drifted the moment one of them gained anything: the colour bake,
    // the occlusion/roughness maps and the hidden-group filter all landed on
    // exportTo() and none of them reached the button anyone actually presses.
    QString err;
    // The REPORT is inside exportTo(), not here: the harness, the shared
    // context-menu actions and the Export menu all call that function
    // directly, and a notify at the dialog would have covered only the one
    // path that opens a dialog — which is the same drift the two paths were
    // merged to stop.
    if (exportTo(out, &err))
        setStatus(QStringLiteral("Exported %1").arg(out));
    else
        setStatus(QStringLiteral("Export failed: %1").arg(err));
}

// ── INFO (template §6) ──────────────────────────────────────────────────────
// "Absolutely all info possible about the selected model or submesh" was the
// ask, and the honest reading of it is: everything this tool has ALREADY
// parsed, said once, in one place. Nothing here computes anything new — every
// row is a field of the FMDL, of the index entry, or of a sibling file the
// load path already read. What used to carry a fraction of it was a one-line
// QLabel under the viewport that had to choose between the counts and the
// path, and chose differently on each of the four code paths that wrote it.
void ModelsTab::refreshInfoPanel()
{
    if (!m_infoPanel) return;
    if (!m_npanel || !m_npanel->isPanelOpen(QStringLiteral("info"))) return;
    m_infoPanel->clear();
    if (!m_hasModel) { m_infoPanel->finish(); return; }

    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();
    const auto loc = QLocale();

    // ── FILE ────────────────────────────────────────────────────────────
    if (m_currentFile >= 0 && m_currentFile < files.size()) {
        const IndexedFile& f = files[m_currentFile];
        m_infoPanel->beginSection(QStringLiteral("FILE"));
        m_infoPanel->addRow(QStringLiteral("Name"),
                            f.path.section(QLatin1Char('/'), -1));
        m_infoPanel->addRow(QStringLiteral("Path"), f.path);
        m_infoPanel->addRow(QStringLiteral("Size"),
                            QStringLiteral("%1 bytes").arg(loc.toString(f.size)));
        m_infoPanel->addRow(QStringLiteral("Game"),
                            fox::gameLongName(index.gameOf(f)));
        m_infoPanel->addRow(
            QStringLiteral("Hash"),
            QStringLiteral("0x%1").arg(f.hash, 16, 16, QLatin1Char('0')),
            f.gz ? QStringLiteral("The Ground Zeroes legacy hash scheme.")
                 : QStringLiteral("PathFileNameCode — the 64-bit code Fox "
                                  "addresses this file by."));
        m_infoPanel->addRow(
            QStringLiteral("Name source"),
            f.named ? QStringLiteral("dictionary / stored path")
                    : QStringLiteral("hash only — no name in the dictionary"));
        if (f.archiveId >= 0 && f.archiveId < index.archives().size())
            m_infoPanel->addRow(QStringLiteral("Archive"),
                                index.archives()[f.archiveId].shortName);
        if (f.shadowed)
            m_infoPanel->addRow(
                QStringLiteral("Shadowed"), QStringLiteral("yes"),
                QStringLiteral("Another archive with higher mount priority "
                               "carries this same hash — the game would load "
                               "that copy, not this one."));
        const QStringList tags = fox::ModelTags::instance().tagsOf(m_currentFile);
        if (!tags.isEmpty())
            m_infoPanel->addRow(QStringLiteral("Tags"),
                                tags.join(QStringLiteral(", ")));
    }

    // ── MODEL ───────────────────────────────────────────────────────────
    {
        const auto& meshes = m_model.meshes();
        int tris = 0, verts = 0;
        for (const fox::FmdlMesh& m : meshes) {
            tris += m.triangles.size() / 3;
            verts += m.positions.size() / 3;
        }
        m_infoPanel->beginSection(QStringLiteral("MODEL"));
        m_infoPanel->addRow(QStringLiteral("FMDL version"),
                            QString::number(m_model.version(), 'f', 2));
        m_infoPanel->addRow(QStringLiteral("Mesh groups"),
                            QString::number(m_model.meshGroups().size()));
        m_infoPanel->addRow(QStringLiteral("Submeshes"),
                            QString::number(meshes.size()));
        m_infoPanel->addRow(QStringLiteral("Triangles"), loc.toString(tris));
        m_infoPanel->addRow(QStringLiteral("Vertices"), loc.toString(verts));
        m_infoPanel->addRow(QStringLiteral("Materials"),
                            QString::number(m_model.materials().size()));
        m_infoPanel->addRow(QStringLiteral("Bones"),
                            QString::number(m_model.bones().size()));
        int withTex = 0;
        for (const QImage& im : m_textures)
            if (!im.isNull()) ++withTex;
        m_infoPanel->addRow(
            QStringLiteral("Base textures"),
            QStringLiteral("%1 of %2 decoded").arg(withTex).arg(m_textures.size()),
            QStringLiteral("A slot with no decoded texture draws flat — the "
                           "file is missing from this install, or its stream "
                           "is."));
        int withNrm = 0;
        for (const QImage& im : m_normalMaps)
            if (!im.isNull()) ++withNrm;
        m_infoPanel->addRow(QStringLiteral("Normal maps"),
                            QStringLiteral("%1 decoded").arg(withNrm));
        m_infoPanel->addRow(
            QStringLiteral("PBR maps"),
            m_view && m_view->hasPbrMaps() ? QStringLiteral("loaded")
                                           : QStringLiteral("not loaded"),
            QStringLiteral("The SRM/TRM/layer set the Rendered shading mode "
                           "needs. Loaded on demand — see the Graphics "
                           "popover."));
    }

    // ── SIBLING FILES ───────────────────────────────────────────────────
    {
        m_infoPanel->beginSection(QStringLiteral("SIBLINGS"));
        m_infoPanel->addRow(
            QStringLiteral("Connect points (.fcnp)"),
            m_hasFcnp ? QStringLiteral("%1 sockets").arg(m_fcnp.points().size())
                      : QStringLiteral("none"),
            QStringLiteral("Fox's hardpoints: where an attachment is seated."));
        m_infoPanel->addRow(
            QStringLiteral("Help bones (.frdv)"),
            m_hasFrdv ? QStringLiteral("%1 ops").arg(m_frdv.ops().size())
                      : QStringLiteral("none"));
        m_infoPanel->addRow(
            QStringLiteral("Rig (.frig)"),
            m_hasFrig ? QStringLiteral("bound") : QStringLiteral("none"),
            QStringLiteral("The rig an animation archive is authored against. "
                           "Bound when a clip is loaded."));
        if (m_hasMtar)
            m_infoPanel->addRow(QStringLiteral("Motion archive"),
                                QStringLiteral("%1 clips").arg(m_mtar.clips().size()));
    }

    // ── SELECTION ───────────────────────────────────────────────────────
    // The submesh the viewport and the parts list agree is selected. A panel
    // that described only the model made the parts list's selection mean
    // nothing here, which is exactly the disconnect §4's two-way selection
    // rule exists to close.
    const int sel = m_view ? m_view->pickedMesh() : -1;
    if (sel >= 0 && sel < m_model.meshes().size()) {
        const fox::FmdlMesh& m = m_model.meshes()[sel];
        m_infoPanel->beginSection(QStringLiteral("SELECTED SUBMESH"));
        m_infoPanel->addRow(QStringLiteral("Index"), QString::number(sel));
        m_infoPanel->addRow(QStringLiteral("Mesh group"),
                            meshGroupLabel(m_model, m.meshGroupIndex));
        const auto& mats = m_model.materials();
        if (m.materialInstanceIndex >= 0
            && m.materialInstanceIndex < mats.size()) {
            const fox::FmdlMaterialInstance& mi = mats[m.materialInstanceIndex];
            m_infoPanel->addRow(QStringLiteral("Material"), mi.name);
            m_infoPanel->addRow(QStringLiteral("Shader"), mi.shader);
            m_infoPanel->addRow(QStringLiteral("Texture slots"),
                                QString::number(mi.textures.size()));
            for (const fox::FmdlTextureRef& t : mi.textures)
                m_infoPanel->addRow(
                    t.role,
                    t.path.isEmpty()
                        ? QStringLiteral("0x%1").arg(t.pathHash, 16, 16,
                                                     QLatin1Char('0'))
                        : t.path.section(QLatin1Char('/'), -1),
                    t.path);
        }
        m_infoPanel->addRow(QStringLiteral("Triangles"),
                            loc.toString(m.triangles.size() / 3));
        m_infoPanel->addRow(QStringLiteral("Vertices"),
                            loc.toString(m.positions.size() / 3));
        m_infoPanel->addRow(QStringLiteral("Bone palette"),
                            QString::number(m.palette.size()));
        m_infoPanel->addRow(
            QStringLiteral("Has tangents"),
            m.tangents.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes"),
            QStringLiteral("No tangents means the normal map cannot be applied "
                           "to this submesh whatever the switch says."));
        m_infoPanel->addRow(
            QStringLiteral("Visible"),
            m_view && m_view->hiddenMeshes().contains(sel)
                ? QStringLiteral("no — hidden, and out of the export scope")
                : QStringLiteral("yes"));
    }
    m_infoPanel->finish();
}

void ModelsTab::refreshAttachments()
{
    if (!m_attachPanel) return;
    if (!m_npanel || !m_npanel->isPanelOpen(QStringLiteral("attachments")))
        return;
    if (!m_hasModel) { m_attachPanel->clearModel(); return; }
    m_attachPanel->setModel(m_currentFile, &m_model);
}

// "PARTS · 12 of 14 shown" in the panel header. The PanelBox hands its title
// label back for exactly this: a count that lives in the header costs no row
// inside the panel, and the panel's own header line used to be spent on it.
// ── "Export this material's images" (§9) ────────────────────────────────────
// The shape the user asked for, exactly: one folder per material named for the
// material, and inside it one file per texture under the TEXTURE's own real
// name — `dog material/dogn.png, dogc.png`, not `material_0/Base_Tex_SRGB.png`.
// The role is not the file name: a Fox material binds the same texture under
// different roles in different shaders, and naming the file after the role
// loses which .ftex it actually was.
//
// ONE dialog for the whole set. A per-file dialog for a four-map material is
// four dialogs, and the panel's whole reason for offering this is that doing
// it a texture at a time is the slow way.
bool ModelsTab::exportMaterialImages(
    const QVector<MaterialInspector::CardInfo>& mats)
{
    if (mats.isEmpty()) return false;
    const QString what =
        mats.size() == 1 ? mats.first().material
                         : QStringLiteral("%1 materials").arg(mats.size());
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Export %1's images to…").arg(what),
        Config::exportDir());
    if (dir.isEmpty()) return false;
    Config::setExportDir(dir);

    const ArchiveIndex& index = ArchiveIndex::instance();
    int written = 0, failed = 0, unresolved = 0;
    for (const MaterialInspector::CardInfo& mi : mats) {
        // The folder is the material's name, through the SAME sanitiser the
        // rest of the export layout uses — a Fox material name can carry
        // characters no file system will take, and two sanitisers is how two
        // export paths end up writing to two different folders.
        const QString sub = ExportLayout::sanitizeFolder(mi.material, QStringLiteral("material"));
        const QString outDir = QDir(dir).filePath(
            sub);
        for (const MaterialInspector::CardTexture& t : mi.textures) {
            // Resolve the ref to a file in the index. By hash when the model
            // carries one, by path when it is a Ground Zeroes model (whose
            // refs are strings and whose pathHash is always 0).
            int fi = -1;
            if (t.pathHash != 0) {
                if (const IndexedFile* f = index.findByHash(t.pathHash)) {
                    const auto& files = index.files();
                    for (int i = 0; i < files.size(); ++i)
                        if (&files[i] == f) { fi = i; break; }
                }
            }
            if (fi < 0 && !t.path.isEmpty()) {
                const auto& files = index.files();
                for (int i = 0; i < files.size(); ++i)
                    if (files[i].path.compare(t.path, Qt::CaseInsensitive) == 0) {
                        fi = i;
                        break;
                    }
            }
            if (fi < 0) { ++unresolved; continue; }
            // The TEXTURE's own file name, extension swapped for .png.
            QString stem = index.files()[fi].path.section(QLatin1Char('/'), -1);
            if (stem.endsWith(QLatin1String(".ftex"), Qt::CaseInsensitive))
                stem.chop(5);
            if (stem.isEmpty())
                stem = QStringLiteral("0x%1").arg(t.pathHash, 16, 16,
                                                  QLatin1Char('0'));
            const QString out =
                QDir(outDir).filePath(stem + QStringLiteral(".png"));
            QString err;
            if (exportactions::writeFtexPng(fi, out, &err)) {
                ++written;
            } else {
                ++failed;
                qWarning("models: %s — %s", qUtf8Printable(out), qUtf8Printable(err));
            }
        }
    }
    QString msg = QStringLiteral("Wrote %1 image(s) into %2 folder(s).")
                      .arg(written).arg(mats.size());
    if (failed > 0)
        msg += QStringLiteral(" %1 failed (see log).").arg(failed);
    if (unresolved > 0)
        msg += QStringLiteral(" %1 texture(s) are not in this install.")
                   .arg(unresolved);
    setStatus(msg);
    QMessageBox::information(this, QStringLiteral("Export"), msg);
    return written > 0;
}

bool ModelsTab::setDisplayModeForShot(const QString& id)
{
    const QString want = id.trimmed().toLower();
    if (want != fox::displaymode::list() && want != fox::displaymode::outliner()
        && want != fox::displaymode::grid())
        return false;
    // setMode ALONE. It emits modeChanged, and the connection in the
    // constructor is what applies it — which is exactly the path a user takes
    // when they pick a mode from the button's menu.
    //
    // This used to call applyDisplayMode() itself as well, and that is why the
    // flag passed for months while the button did nothing: the harness was
    // driving the view directly and never touching the signal the UI depends
    // on. A test that takes a different route to the same place cannot see a
    // broken route.
    m_display->setMode(want);
    // The harness has no event loop between here and the grab, and both the
    // tree and the grid lay out lazily. Pump it so what is photographed is
    // what the mode actually produces.
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents();
    return true;
}

// ── ICON MODE, FOR ALL THREE VIEWS ───────────────────────────────────────
// One setting, one place. The outliner keeps its own copy because it draws its
// own rows; the grid and the list go through their delegates. Asked for as
// "needs option to turn on/off icons / true icons / both" across all display
// modes, so it cannot live inside any single view.
// What the display button's option menu holds, for the mode that is actually
// on. See the connect above for why it is rebuilt rather than filtered.
// Is the columned list the current view?
//
// NOT m_outliner->isVisible(). QWidget::isVisible() is false whenever the
// widget OR ANY ANCESTOR is hidden — and at startup the Models tab is not the
// front tab, so the whole tab is hidden and every one of these guards was
// false exactly when the index became ready. That is why the outliner "shows
// nothing on load until a filter is turned on and off": by the time the user
// touches a filter the tab IS on screen, isVisible() is finally true, and the
// filter path rebuilds it. Two batches tried to fix this by adding refreshes
// and every one of them was behind the same broken test.
//
// The display MODE is state this tab owns, is true whether or not anything is
// painted, and is the thing these guards actually meant.
bool ModelsTab::listViewActive() const
{
    return m_display && m_display->mode() != fox::displaymode::grid();
}

void ModelsTab::rebuildDisplayOptions()
{
    if (!m_display) return;
    // clearOptions(), NOT opts->clear(): optionsMenu() hands back the mode
    // menu itself, so clear() took List, Outliner and Grid with it and the
    // display button lost its display modes.
    m_display->clearOptions();
    QMenu* opts = m_display->optionsMenu();
    const QString id = m_display->mode();
    const bool grid = (id == fox::displaymode::grid());
    const bool outliner = (id == fox::displaymode::outliner());

    // ── ICONS — every mode has them ─────────────────────────────────────
    // A SECTION HEADER and a radio group, not four loose checkable rows. The
    // four are mutually exclusive and were drawn as four independent ticks,
    // which is why "what is toggled on" was hard to read: a tick that cannot
    // be off unless another is on is a radio button, and Qt draws it as one
    // once the actions share a QActionGroup.
    opts->addSection(QStringLiteral("Icons"));
    {
        static const char* const kModes[] = {
            "None", "Rendered", "The game's own", "Rendered + game",
        };
        static const char* const kTips[] = {
            "No pictures at all — the fastest the list can be.",
            "This tool's own render of the model.",
            "The icon MGSV itself draws for the asset. Weapon parts and "
            "equipment have one; most environment models do not.",
            "The game's own icon where there is one, and this tool's render "
            "everywhere else.",
        };
        auto* grp = new QActionGroup(opts);
        grp->setExclusive(true);
        const int cur = iconMode();
        for (int i = 0; i < 4; ++i) {
            QAction* a = opts->addAction(QString::fromLatin1(kModes[i]));
            a->setCheckable(true);
            a->setChecked(cur == i);
            // The mark is an ICON, for the same reason the mode rows use one —
            // see DisplayModeButton::syncFace. It always draws and it lines the
            // whole menu up.
            a->setIcon(cur == i ? foxglyph::toolIcon(29) : QIcon());
            a->setToolTip(QString::fromLatin1(kTips[i]));
            grp->addAction(a);
            connect(a, &QAction::triggered, this, [this, i] { setIconMode(i); });
        }
    }

    if (grid) {
        opts->addSection(QStringLiteral("Grid"));
        QAction* bigger = opts->addAction(QStringLiteral("Bigger icons"));
        bigger->setToolTip(QStringLiteral(
            "The same as Ctrl+wheel over the grid. 48 to 320 pixels."));
        connect(bigger, &QAction::triggered, this,
                [this] { setIconSize(m_gridDelegate->iconSize() + 24); });
        QAction* smaller = opts->addAction(QStringLiteral("Smaller icons"));
        connect(smaller, &QAction::triggered, this,
                [this] { setIconSize(m_gridDelegate->iconSize() - 24); });
    } else {
        // LIST and OUTLINER are the same widget, so they get the same options.
        opts->addSection(outliner ? QStringLiteral("Outliner")
                                  : QStringLiteral("List"));
        QAction* cols = opts->addAction(
            QStringLiteral("Columns and sorting are on the header"));
        cols->setEnabled(false);
        cols->setToolTip(QStringLiteral(
            "Right-click any column header to choose which columns show. "
            "Click a header to sort by it; click again to reverse. Drag the "
            "icon column's edge to resize the pictures."));
        QAction* bigger = opts->addAction(QStringLiteral("Bigger rows"));
        bigger->setToolTip(QStringLiteral(
            "The same as Ctrl+wheel over the list. Scales the text, the row "
            "height and the icons together."));
        connect(bigger, &QAction::triggered, this,
                [this] { setRowZoom(m_rowZoom + 1); });
        QAction* smaller = opts->addAction(QStringLiteral("Smaller rows"));
        connect(smaller, &QAction::triggered, this,
                [this] { setRowZoom(m_rowZoom - 1); });
    }
}

int ModelsTab::iconMode() const
{
    return QSettings().value(QStringLiteral("models/iconMode"), 1).toInt();
}

void ModelsTab::setIconMode(int mode)
{
    mode = qBound(0, mode, 3);
    QSettings().setValue(QStringLiteral("models/iconMode"), mode);
    if (m_outliner) m_outliner->setIconMode(mode);
    if (m_gridDelegate) m_gridDelegate->setIconMode(mode);
    if (m_list) m_list->viewport()->update();
    qInfo("models: icon mode -> %d (0 none, 1 rendered, 2 game, 3 both)", mode);
}

QString ModelsTab::displaySummary() const
{
    const QString id = m_display ? m_display->mode() : QString();
    if (id == fox::displaymode::outliner() && m_outliner)
        return QStringLiteral("outliner · %1 models, icons %2, %3 px wide")
            .arg(m_outliner->modelRows())
            .arg(m_outliner->iconMode())
            .arg(m_outliner->width());
    return QStringLiteral("%1 · %2 rows").arg(id).arg(listCount());
}

void ModelsTab::setRowZoomForShot(int delta)
{
    setRowZoom(delta);
    for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
}

QString ModelsTab::setAnimSortForShot(const QString& id)
{
    if (!m_animPanel || !m_npanel) return QStringLiteral("no panel");
    m_npanel->setPanelOpen(QStringLiteral("animations"), true);
    if (!m_animPanel->isBuilt()) m_animPanel->rebuild();
    if (!m_animPanel->setSortOrder(id))
        return QStringLiteral("NO SUCH ORDER (kept '%1')")
            .arg(m_animPanel->sortOrder());
    for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
    return QStringLiteral("%1 — first rows: %2")
        .arg(m_animPanel->sortOrder(), m_animPanel->firstRowsForShot(6));
}

QString ModelsTab::outlinerDumpForShot(const QString& outPath)
{
    if (!m_outliner) return QStringLiteral("no outliner");
    // NO isVisible() GUARD: the tree is built whether or not the outliner is
    // the current display mode, and a dump that refuses unless the user
    // happens to be looking at it is a dump nobody can take when it matters.
    m_outliner->refresh();
    m_outliner->setLoadedModel(m_currentFile, m_hasModel ? &m_model : nullptr);
    return m_outliner->dumpTreeForShot(outPath);
}

QString ModelsTab::outlinerProbeForShot(const QString& spec)
{
    if (!m_outliner) return QStringLiteral("no outliner");
    if (!m_outliner->isVisible())
        return QStringLiteral("the outliner is not the current view — pass "
                              "--viewmode outliner as well");
    const QString out = m_outliner->probeForShot(spec);
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents();
    return out;
}

QString ModelsTab::setPanelsForShot(const QString& keys)
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

void ModelsTab::updatePartsTitle()
{
    if (!m_npanel || !m_sceneTree) return;
    QLabel* t = m_npanel->titleLabel(QStringLiteral("parts"));
    if (!t) return;
    if (!m_hasModel) { t->setText(QStringLiteral("PARTS")); return; }
    const int total = m_model.meshes().size();
    const int hidden = m_sceneTree->hiddenLeaves().size();
    t->setText(hidden == 0
                   ? QStringLiteral("PARTS · %1").arg(total)
                   : QStringLiteral("PARTS · %1 of %2 shown")
                         .arg(total - hidden).arg(total));
}

// Light up the material of one submesh in the MATERIALS panel. Silent when
// the panel is closed or the material is filtered out — this follows a
// selection, and a selection must not force a panel open or clear a filter the
// user set.
void ModelsTab::selectMaterialFor(int meshId)
{
    if (!m_inspector || !m_npanel) return;
    if (!m_npanel->isPanelOpen(QStringLiteral("materials"))) return;
    // ONE pick, one lookup. A viewport pick reaches here twice — the pick asks
    // directly, and the parts tree's own selection signal asks again for the
    // same submesh — and the second pass logged a duplicate line for one click.
    if (meshId == m_lastMatMesh) return;
    m_lastMatMesh = meshId;
    const QString name = submeshMaterialName(meshId);
    if (name.isEmpty()) return;
    const bool got = m_inspector->selectMaterialNamed(name);
    // One line per pick, and only while the panel is open. "Did the third leg
    // of the two-way selection actually land" is not visible in a screenshot —
    // the card lights up somewhere below the fold as often as not — and a
    // material the filter is hiding is a real and reportable no.
    qInfo("models: submesh %d -> material '%s' %s", meshId, qUtf8Printable(name),
          got ? "selected" : "NOT in the panel (filtered out?)");
}

// The material INSTANCE name of a submesh, or empty. Distinct from
// submeshLabel(), which falls back to the mesh group and then to an index so a
// menu title always says something — a fallback that would make this select
// the wrong card.
QString ModelsTab::submeshMaterialName(int meshId) const
{
    if (!m_hasModel || meshId < 0 || meshId >= m_model.meshes().size())
        return QString();
    const int mi = m_model.meshes()[meshId].materialInstanceIndex;
    const auto& mats = m_model.materials();
    return (mi >= 0 && mi < mats.size()) ? mats[mi].name : QString();
}

// Everything this tab used to write under its viewport goes to the window's
// status bar. One function, so the eighteen call sites cannot drift into
// writing to a label nobody can see.
void ModelsTab::setStatus(const QString& text)
{
    if (m_info) m_info->setText(text);   // the offscreen sink, for the harness
    fox::StatusLine::instance().report(this, text);
}

QVector<int> ModelsTab::selectedModelFiles() const
{
    QVector<int> out;
    if (!m_list || !m_list->selectionModel()) return out;
    for (const QModelIndex& ix : m_list->selectionModel()->selectedIndexes()) {
        const int fi = m_listModel->fileIdxAt(ix);
        if (fi >= 0 && !out.contains(fi)) out.append(fi);
    }
    return out;
}


int ModelsTab::showAnimationsPanel(const QString& filter)
{
    if (!m_npanel || !m_animPanel) return 0;
    // Through the column: opening the panel is what builds it.
    m_npanel->setPanelOpen(QStringLiteral("animations"), true);
    if (!m_animPanel->isBuilt()) m_animPanel->rebuild();
    m_animPanel->setFilter(filter);
    const int n = m_animPanel->selectVisible();
    // The width is in the line on purpose. A pane whose minimum does not fit
    // in what the splitter has left is laid out past the right edge — visible
    // and correctly sized as far as Qt is concerned, and not on screen — and
    // that is invisible in a log that reports only "opened".
    qInfo("models: animations panel — filter '%s', %s, %d px wide",
          qUtf8Printable(filter), qUtf8Printable(m_animPanel->selectionSummary()),
          m_animPanel->width());
    return n;
}

QString ModelsTab::setAnimScopeForShot(const QString& spec)
{
    if (!m_npanel || !m_animPanel) return QStringLiteral("no panel");
    m_npanel->setPanelOpen(QStringLiteral("animations"), true);
    if (!m_animPanel->isBuilt()) m_animPanel->rebuild();
    syncAnimPanel();   // the panel must know the model before it is scoped
    bool ok = false;
    if (spec.startsWith(QLatin1String("other:")))
        ok = m_animPanel->setScopeModel(spec.mid(6));
    else
        ok = m_animPanel->setScope(spec);
    return QStringLiteral("%1 -> scope '%2' (%3)%4")
        .arg(spec, m_animPanel->scope(), m_animPanel->scopeSummary(),
             ok ? QString() : QStringLiteral(" [REFUSED]"));
}

QVector<QPair<int, int>> ModelsTab::panelSelection() const
{
    return m_animPanel ? m_animPanel->selectedClips()
                       : QVector<QPair<int, int>>();
}

// What OPENING a panel does — the one implementation, so the startup pass and
// the signal cannot drift apart.
void ModelsTab::fillPanel(const QString& key)
{
    if (key == QLatin1String("animations")) {
        if (m_animPanel && !m_animPanel->isBuilt()) m_animPanel->rebuild();
        syncAnimPanel();
    } else if (key == QLatin1String("materials")) {
        refreshInspector();
    } else if (key == QLatin1String("info")) {
        refreshInfoPanel();
    } else if (key == QLatin1String("attachments")) {
        refreshAttachments();
    }
}

void ModelsTab::fillOpenPanels()
{
    if (!m_npanel) return;
    QStringList filled;
    for (const QString& key : m_npanel->panelKeys()) {
        if (!m_npanel->isPanelOpen(key)) continue;
        fillPanel(key);
        filled << key;
    }
    // Always on and bounded: one line, at startup, naming what was restored
    // open. An empty list here after a session that left ANIMATIONS open is
    // the signature of this bug coming back.
    qInfo("models: panels restored open and filled — %s",
          filled.isEmpty() ? "none"
                           : qUtf8Printable(filled.join(QLatin1Char('+'))));
}

void ModelsTab::syncAnimPanel()
{
    if (!m_animPanel) return;
    // The MODEL is pushed whether or not the panel is open or built: the scope
    // is what the panel shows the moment it opens, and resolving it from a
    // model the panel was never told about is how it would open on the wrong
    // list once and be right ever after — the hardest kind of bug to see.
    const fox::ArchiveIndex& index = fox::ArchiveIndex::instance();
    m_animPanel->setCurrentModel(
        m_hasModel && m_currentFile >= 0 && m_currentFile < index.files().size()
            ? index.files()[m_currentFile].path
            : QString());
    // NO isVisible() GUARD, AND NO EARLY RETURN ON AN INVALID COMBO. Both
    // were here and both were the bug the user reported as "the animation
    // player at the bottom doesn't always match the ANIMATIONS panel":
    // returning early left the panel showing whatever it showed LAST, which
    // is a highlight meaning "this is playing" pointing at a clip that
    // stopped playing several clicks ago. showCurrent is cheap when the
    // panel is not built and it is the one that decides what it can show, so
    // it is told the truth on every path and left to act on it.
    //
    // m_hasAnim, not just a valid combo payload: a clip whose decode failed
    // leaves the combo naming it with nothing posed, and the panel must not
    // claim that clip is playing.
    const QVariant av = m_mtarCombo ? m_mtarCombo->currentPayload() : QVariant();
    const QVariant cv = m_clipCombo ? m_clipCombo->currentPayload() : QVariant();
    const bool playing = m_hasAnim && av.isValid() && cv.isValid();
    m_animPanel->showCurrent(playing ? av.toInt() : -1,
                             playing ? cv.toInt() : -1);
}

// The clip's own asset name, from the catalogue rather than by opening the
// archive: naming twenty output files should not mean parsing twenty .mtar.
static QString clipNameFromCatalog(int archiveFileIdx, int clipIdx)
{
    const fox::AnimCatalog& cat = fox::AnimCatalog::instance();
    for (const fox::AnimArchive& a : cat.archives()) {
        if (a.fileIdx != archiveFileIdx) continue;
        for (const fox::AnimClip& c : a.clips)
            if (c.index == clipIdx) return c.name;
        break;
    }
    return {};
}

void ModelsTab::exportAnimationsInteractive(bool separateFiles)
{
    if (!m_animPanel) return;
    // The panel's selection when it has one, otherwise the clip the combos
    // have loaded — so the Export menu's entry works without opening the panel
    // at all, and means the same thing when it is open.
    QVector<QPair<int, int>> sel = m_animPanel->selectedClips();
    if (sel.isEmpty()) sel = clipsMatching(QString());
    if (sel.isEmpty()) {
        setStatus(QStringLiteral(
            "No clips to export — pick one in the animation bar, or select "
            "some in the Animations list."));
        return;
    }
    if (!m_hasModel) {
        setStatus(QStringLiteral(
            "Load a model first — an animation export is a rigged model with "
            "the clips on it, not the clips on their own."));
        return;
    }

    if (!separateFiles) {
        QString base = QStringLiteral("model");
        const ArchiveIndex& ix = ArchiveIndex::instance();
        if (m_currentFile >= 0 && m_currentFile < ix.files().size()) {
            base = ix.files()[m_currentFile].path.section(QLatin1Char('/'), -1);
            if (base.endsWith(QLatin1String(".fmdl"))) base.chop(5);
        }
        base = fox::templatedStem(base, m_currentFile);
        const QString out = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export animated glTF binary"),
            QDir(Config::exportDir()).filePath(base + QStringLiteral(".glb")),
            QStringLiteral("glTF binary (*.glb)"));
        if (out.isEmpty()) return;
        Config::setExportDir(QFileInfo(out).absolutePath());
        QString err;
        if (exportAnimatedTo(out, sel, &err))
            setStatus(QStringLiteral("Exported %1 — %2 clip(s)")
                                .arg(out)
                                .arg(sel.size()));
        else
            setStatus(QStringLiteral("Export failed: %1").arg(err));
        return;
    }

    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Export one .glb per clip into…"),
        Config::exportDir());
    if (dir.isEmpty()) return;
    Config::setExportDir(dir);
    int written = 0, failed = 0;
    QString firstError;
    QSet<QString> used;
    for (const QPair<int, int>& one : sel) {
        QString stem = clipNameFromCatalog(one.first, one.second)
                           .section(QLatin1Char('/'), -1);
        const int dot = stem.lastIndexOf(QLatin1Char('.'));
        if (dot > 0) stem.truncate(dot);
        if (stem.isEmpty()) stem = QStringLiteral("clip%1").arg(one.second);
        stem = fox::templatedStem(stem, m_currentFile);
        // Unique within the run AND on disk — the same rule the Customize
        // per-part export follows, and for the same reason: two archives can
        // hold clips of one name, and quietly replacing a file in a folder the
        // user picked is a loss nobody notices until later.
        QString name = stem;
        for (int n = 2;
             used.contains(name)
             || QFile::exists(QDir(dir).filePath(name + QStringLiteral(".glb")));
             ++n)
            name = QStringLiteral("%1_%2").arg(stem).arg(n);
        used.insert(name);
        QString err;
        if (exportAnimatedTo(QDir(dir).filePath(name + QStringLiteral(".glb")),
                             {one}, &err))
            ++written;
        else {
            ++failed;
            if (firstError.isEmpty()) firstError = err;
        }
    }
    QString msg = QStringLiteral("Exported %1 clip(s) into %2").arg(written).arg(dir);
    if (failed)
        msg += QStringLiteral(" · %1 FAILED — %2").arg(failed).arg(firstError);
    setStatus(msg);
    qInfo("models: %s", qUtf8Printable(msg));
    fox::ExportNotifier::instance().notify(msg, dir);
}

QVector<QPair<int, int>> ModelsTab::clipsMatching(const QString& spec) const
{
    QVector<QPair<int, int>> out;
    if (!m_hasMtar) return out;
    const QVariant av = m_mtarCombo ? m_mtarCombo->currentPayload() : QVariant();
    const int archive = av.isValid() ? av.toInt() : -1;
    if (archive < 0) return out;
    const auto& clips = m_mtar.clips();
    const QString s = spec.trimmed();
    if (s.isEmpty()) {
        const QVariant pv = m_clipCombo ? m_clipCombo->currentPayload() : QVariant();
        const int ci = pv.isValid() ? pv.toInt() : -1;
        if (ci >= 0 && ci < clips.size()) out.append({archive, ci});
        return out;
    }
    if (s.compare(QLatin1String("all"), Qt::CaseInsensitive) == 0) {
        out.reserve(clips.size());
        for (int i = 0; i < clips.size(); ++i) out.append({archive, i});
        return out;
    }
    // Deduplicated and kept in the order the spec asked for: two tokens that
    // both match one clip must not export it twice, and glTF animations with
    // the same name are a viewer's problem, not a viewer's feature.
    QSet<int> seen;
    const QStringList fields = s.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& fRaw : fields) {
        const QString f = fRaw.trimmed();
        if (f.isEmpty()) continue;
        bool numeric = false;
        const int asIndex = f.toInt(&numeric);
        if (numeric) {
            if (asIndex >= 0 && asIndex < clips.size() && !seen.contains(asIndex)) {
                seen.insert(asIndex);
                out.append({archive, asIndex});
            }
            continue;
        }
        const QString needle = f.toLower();
        for (int i = 0; i < clips.size(); ++i)
            if (clips[i].name.toLower().contains(needle) && !seen.contains(i)) {
                seen.insert(i);
                out.append({archive, i});
            }
    }
    return out;
}

// Export the model RIGGED, with one glTF animation per selected clip.
//
// Deliberately not a variant of exportTo: that one exports what the viewport
// shows, and what the viewport shows during playback is a single frame baked
// into the vertices. An animated export is the opposite shape — no baked pose,
// the skeleton always written, and the motion sampled per frame from the same
// solver the viewport runs.
bool ModelsTab::exportAnimatedTo(const QString& glbPath,
                                 const QVector<QPair<int, int>>& clipIdx,
                                 QString* errorOut)
{
    const auto fail = [&](const QString& why) {
        qWarning("models: animated export failed: %s", qUtf8Printable(why));
        if (errorOut) *errorOut = why;
        return false;
    };
    if (!m_hasModel) return fail(QStringLiteral("no model loaded"));
    if (clipIdx.isEmpty()) return fail(QStringLiteral("no clips selected"));
    if (m_model.bones().isEmpty())
        return fail(QStringLiteral("this model has no skeleton to animate"));

    // Archives are opened HERE rather than reusing the one the combos loaded,
    // because a selection made in the Animations panel can span archives —
    // "every walk cycle in the install" is nine archives, not one — and an
    // export that silently used the open archive's clip 7 for all of them
    // would look like it worked.
    QHash<int, fox::MtarFile> opened;
    const auto archiveFor = [&](int fileIdx) -> fox::MtarFile* {
        auto it = opened.find(fileIdx);
        if (it != opened.end()) return it->clips().isEmpty() ? nullptr : &*it;
        const ArchiveIndex& ix = ArchiveIndex::instance();
        if (fileIdx < 0 || fileIdx >= ix.files().size()) return nullptr;
        fox::MtarFile m;
        const QByteArray raw = ix.readFile(ix.files()[fileIdx]);
        if (raw.isEmpty() || !m.parse(raw)) {
            opened.insert(fileIdx, fox::MtarFile());   // negative cache
            return nullptr;
        }
        it = opened.insert(fileIdx, std::move(m));
        return &*it;
    };

    // Decoded UP FRONT and kept alive for the whole export: the exporter calls
    // back per sample, and a clip decoded inside that callback would be decoded
    // once per frame.
    QVector<fox::GaniAnim> decoded;
    QVector<QString> names;     // the name each decoded entry exports under
    QVector<glb::GlbAnimation> clips;
    decoded.reserve(clipIdx.size());
    names.reserve(clipIdx.size());
    clips.reserve(clipIdx.size());
    int failedDecode = 0;
    for (const QPair<int, int>& sel : clipIdx) {
        fox::MtarFile* mt = archiveFor(sel.first);
        if (!mt || sel.second < 0 || sel.second >= mt->clips().size()) {
            ++failedDecode;
            continue;
        }
        fox::GaniAnim a = mt->decodeClip(sel.second);
        if (!a.valid() || a.frameCount <= 0) { ++failedDecode; continue; }
        // The name is taken HERE, beside the decode that succeeded. Re-walking
        // the selection afterwards to name things by position only works while
        // nothing is skipped, and a clip that will not decode is exactly the
        // case where it is not: every later clip would be exported under its
        // neighbour's name.
        QString nm = mt->clips()[sel.second].name.section(QLatin1Char('/'), -1);
        const int dot = nm.lastIndexOf(QLatin1Char('.'));
        if (dot > 0) nm.truncate(dot);
        if (nm.isEmpty()) nm = QStringLiteral("clip%1").arg(sel.second);
        decoded.append(std::move(a));
        names.append(nm);
    }
    if (decoded.isEmpty())
        return fail(QStringLiteral("none of the %1 selected clip(s) decoded")
                        .arg(clipIdx.size()));

    // glTF animation names are how a viewer's clip menu reads, and two entries
    // called "wlk_lp" is a menu you cannot use. Uniqued here rather than in the
    // writer, which merges same-named clips across PARTS on purpose — and
    // uniqued by SEARCH, because "<stem>_2" is a name clips really have and
    // counting would have collided with it.
    QSet<QString> used;
    for (int k = 0; k < decoded.size(); ++k) {
        QString name = names[k];
        for (int n = 2; used.contains(name); ++n)
            name = QStringLiteral("%1_%2").arg(names[k]).arg(n);
        used.insert(name);
        glb::GlbAnimation ga;
        ga.name = name;
        ga.fps = 30.0f;   // Fox motion is authored at 30, as is the playback
        ga.sampleCount = decoded[k].frameCount;
        const fox::GaniAnim* anim = &decoded[k];
        const fox::FrigFile* frig = m_hasFrig ? &m_frig : nullptr;
        const frdv::FrdvFile* frdv = m_hasFrdv ? &m_frdv : nullptr;
        // One part in this scene, so the part index is ignored — the Models
        // tab exports one model and the exporter only ever asks for it.
        ga.pose = [this, anim, frig, frdv](int, int sample,
                                           QVector<animmath::Mat4>* out) {
            *out = animpose::buildWorld(m_model, *anim,
                                        static_cast<float>(sample), frig, frdv);
        };
        clips.append(ga);
    }

    glb::ScenePart sp;
    sp.model = &m_model;
    sp.textures = &m_textures;
    sp.normalMaps = &m_normalMaps;
    if (m_hasFcnp) sp.connectPoints = &m_fcnp.points();
    if (m_pbrForPanel.size() == m_model.materials().size())
        sp.pbr = &m_pbrForPanel;
    // Submeshes the user unticked are not on screen, so they are not in the
    // file. ONE set now, not a group set and a mesh set: turning a group off
    // in the tree turns off every leaf under it, so hiddenGroups would only
    // ever have said the same thing twice.
    sp.hiddenMeshes = hiddenSubmeshes();

    fox::ExportOptions eo = fox::loadExportOptions();
    // "No rig" and "with animation" cannot both be honoured — glTF animates
    // node transforms, and a file with no joint nodes has nothing to animate.
    // Overridden here WITH A LINE IN THE LOG rather than silently dropping
    // every clip inside the writer.
    if (!eo.skeleton) {
        qInfo("models: animated export overrides the \"no skeleton\" setting — "
              "an animation needs joints to drive");
        eo.skeleton = true;
    }
    qInfo("export: %s", qUtf8Printable(eo.describe()));
    QString err;
    int written = 0;
    const bool ok = glb::exportGlbScene({sp}, glbPath, &err,
                                        fox::sceneOptionsFrom(eo), &clips,
                                        &written);
    if (!ok) return fail(err);
    // The count from the WRITER, not the request: a clip the file does not
    // hold must not be reported as exported.
    fox::ExportNotifier::instance().notify(
        QStringLiteral("Exported %1 — %2 clip(s)%3")
            .arg(QFileInfo(glbPath).fileName())
            .arg(written)
            .arg(fox::ExportNotifier::glbOptionsLine(eo)),
        QFileInfo(glbPath).absolutePath());
    qInfo("models: exported %s — %d clip(s)%s%s", qUtf8Printable(glbPath), written,
          written < clips.size()
              ? qUtf8Printable(QStringLiteral(", %1 drove no bone of this model")
                               .arg(clips.size() - written))
              : "",
          failedDecode ? qUtf8Printable(QStringLiteral(", %1 would not decode")
                                        .arg(failedDecode))
                       : "");
    return true;
}

bool ModelsTab::exportTo(const QString& glbPath, QString* errorOut)
{
    if (!m_hasModel) return false;
    QString err;
    QVector<animmath::Mat4> pal;
    if (m_hasAnim)
        pal = animpose::buildPalette(m_model, m_anim, m_frame,
                                     m_hasFrig ? &m_frig : nullptr,
                                     m_hasFrdv ? &m_frdv : nullptr);

    glb::ScenePart sp;
    sp.model = &m_model;
    sp.textures = &m_textures;
    sp.normalMaps = &m_normalMaps;
    if (m_hasFcnp) sp.connectPoints = &m_fcnp.points();
    if (m_hasAnim) sp.pose = &pal;
    // The material set the viewport is shading with, when it has one. It is
    // what lets the exporter bake a runtime colour into the base map and write
    // the SRM out as glTF occlusion + roughness. Length-checked rather than
    // trusted: these two are indexed by the same material slot, and a set of
    // the wrong length would put one material's maps on another.
    if (m_pbrForPanel.size() == m_model.materials().size())
        sp.pbr = &m_pbrForPanel;
    // Submeshes the user unticked are not on screen, so they are not in the
    // file. Same rule the Customize export follows, and the reverse —
    // exporting them regardless — is how an export came to disagree with the
    // viewport that produced it. One model here, so the tree's mesh ids ARE
    // this model's own mesh indices, with no mapping step (unlike the composed
    // scene in Customize).
    sp.hiddenMeshes = hiddenSubmeshes();

    const fox::ExportOptions eo = fox::loadExportOptions();
    qInfo("export: %s", qUtf8Printable(eo.describe()));
    const bool ok = glb::exportGlbScene({sp}, glbPath, &err,
                                        fox::sceneOptionsFrom(eo));
    if (ok) {
        qInfo("models: exported %s (%lld submesh(es) hidden)",
              qUtf8Printable(glbPath), qint64(sp.hiddenMeshes.size()));
        fox::ExportNotifier::instance().notify(
            QStringLiteral("Exported %1%2")
                .arg(QFileInfo(glbPath).fileName(),
                     fox::ExportNotifier::glbOptionsLine(eo)),
            QFileInfo(glbPath).absolutePath());
    }
    else {
        qWarning("models: export failed: %s", qUtf8Printable(err));
        if (errorOut) *errorOut = err;
    }
    return ok;
}

// ── Fullscreen viewport (template §5) ───────────────────────────────────────
// Everything in the tab EXCEPT the viewport hides. The GL widget is never
// reparented: Qt re-creates a QOpenGLWidget's context when it changes parent
// and runs initializeGL again, and this one builds its overlay VAOs there —
// the file already carries a comment about the second run wiring a fresh VAO
// to a dead context's buffer id. Hiding the siblings gets the same screen for
// none of that risk, and leaving fullscreen restores exactly what was up
// rather than whatever the defaults are.
void ModelsTab::applyViewportFullscreen(bool on)
{
    if (!m_view) return;
    QWidget* keep = m_view;
    if (on) {
        m_fsHidden.clear();
        // Every sibling of the viewport, at every level up to this tab.
        for (QWidget* w = keep; w && w != this; w = w->parentWidget()) {
            QWidget* parent = w->parentWidget();
            if (!parent) break;
            for (QObject* o : parent->children()) {
                auto* sib = qobject_cast<QWidget*>(o);
                if (!sib || sib == w || !sib->isVisible()) continue;
                sib->hide();
                m_fsHidden.append(sib);
            }
        }
    } else {
        for (const QPointer<QWidget>& w : m_fsHidden)
            if (w) w->show();
        m_fsHidden.clear();
    }
    // The N-panel's arrow is a child of the VIEWPORT, so the sweep above —
    // which hides siblings — never reaches it. Left up, it is an arrow that
    // opens a column hidden behind the fullscreen viewport.
    if (m_npanel) m_npanel->setToggleVisible(!on);
    // The keyboard has to stay with the viewport, or Esc and F stop working
    // the moment anything else takes focus on the way in.
    m_view->setFocus(Qt::OtherFocusReason);
}
