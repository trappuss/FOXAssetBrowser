// TexturesTab.cpp — see TexturesTab.h.
#include "tabs/TexturesTab.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSettings>
#include <QWheelEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

#include <QComboBox>
#include <QSpinBox>
#include <QDir>
#include <QDateTime>
#include <QDrag>
#include <QMimeData>
#include <QUrl>

#include "fox/FtexFile.h"
#include "index/ArchiveIndex.h"
#include "index/TextureUsers.h"
#include "util/Extract.h"
#include "fox/BcDecode.h"
#include "view/FilterPopup.h"
#include <QToolButton>
#include "view/ViewGlyphs.h"
#include "view/ChannelStrip.h"
#include "view/TextureInfoPanel.h"
#include "index/ModelTags.h"
#include "index/GameId.h"
#include "index/TexThumbCache.h"
#include "util/PanelPersist.h"
#include "util/SearchBox.h"
#include "util/RowShading.h"
#include "util/SearchQuery.h"
#include "util/ShadowDisplay.h"
#include "preview/PreviewPane.h"
#include "util/ExportActions.h"

using fox::ArchiveIndex;
using fox::IndexedFile;

TexListModel::TexListModel(QObject* parent) : QAbstractListModel(parent) {}

void TexListModel::refresh(const QString& query, bool namedOnly, int usedFilter,
                           const QString& userTag)
{
    beginResetModel();
    m_rows.clear();
    const ArchiveIndex& index = ArchiveIndex::instance();
    fox::TextureUsers& tu = fox::TextureUsers::instance();
    // The used/orphan and used-by-tag filters are only meaningful once the
    // sweep has finished. Half a map would call a texture an orphan because
    // the model that uses it has not been read yet, which is a WRONG answer
    // wearing the clothes of a provisional one.
    const bool haveUsers = tu.ready();
    if (index.ready()) {
        const searchq::Query q(query);
        const auto& files = index.files();
        // "The tags of the assets that USE the texture" (§7) — not the
        // texture's own tags, which is a different and much less useful
        // question. Resolved ONCE, into the set of texture hashes that qualify,
        // rather than per row: a model carries dozens of textures and its tags
        // do not change between them.
        const bool wantTag = haveUsers && !userTag.isEmpty();
        QSet<quint64> tagged;
        if (wantTag) {
            const fox::ModelTags& tags = fox::ModelTags::instance();
            tagged = tu.texturesWhere([&](quint64 modelHash) {
                const fox::IndexedFile* m = index.findByHash(modelHash);
                if (!m) return false;
                const int mi = int(m - index.files().constData());
                return tags.tagsOf(mi).contains(userTag);
            });
        }
        for (int i = 0; i < files.size(); ++i) {
            const IndexedFile& f = files[i];
            if (namedOnly && !f.named) continue;
            if (ArchiveIndex::extensionOf(f) != QLatin1String("ftex")) continue;
            if (!fox::GameFilter::instance().enabled(index.gameOf(f))) continue;
            if (!fox::queryMatchesFile(q, i, f)) continue;
            if (haveUsers && usedFilter != 0) {
                const bool used = tu.userCount(f.hash) > 0;
                if (usedFilter == 1 && !used) continue;
                if (usedFilter == 2 && used) continue;
            }
            if (wantTag && !tagged.contains(f.hash)) continue;
            m_rows.append(i);
        }
    }
    endResetModel();
}

// ── Drag-out ────────────────────────────────────────────────────────────────

Qt::ItemFlags TexListModel::flags(const QModelIndex& index) const
{
    Qt::ItemFlags f = QAbstractListModel::flags(index);
    if (index.isValid()) f |= Qt::ItemIsDragEnabled;
    return f;
}

QStringList TexListModel::mimeTypes() const
{
    return {QStringLiteral("text/uri-list"), QStringLiteral("image/png")};
}

QMimeData* TexListModel::mimeData(const QModelIndexList& indexes) const
{
    // BOUNDED. This decodes and writes a PNG per row, on the GUI thread, with
    // no progress and no cancel — Ctrl+A on a quarter of a million rows and one
    // drag gesture is a hang and a filled temp volume. A drag is a gesture for
    // a handful of things; past that the Bulk Extract tab is the answer.
    const int kMaxDrag = 32;
    auto* mime = new QMimeData();
    QList<QUrl> urls;
    // One folder PER DRAG. The name used to be the file's stem alone, and Fox
    // ships the same stem in many directories — dragging two of them wrote both
    // to <temp>/foo.png, so the drop delivered one file twice.
    const QString dir = QDir(QDir::tempPath())
                            .filePath(QStringLiteral("foxab-drag-%1")
                                          .arg(QDateTime::currentMSecsSinceEpoch()));
    QDir().mkpath(dir);
    int taken = 0;
    for (const QModelIndex& ix : indexes) {
        if (taken >= kMaxDrag) break;
        const int fi = fileIdxAt(ix);
        if (fi < 0) continue;
        const fox::IndexedFile& f = ArchiveIndex::instance().files()[fi];
        const QByteArray dds = extract::assembleFtexToDds(f);
        if (dds.isEmpty()) continue;
        const QImage img = fox::bc::decodeDds(dds);
        if (img.isNull()) continue;
        // A REAL FILE, in the temp folder. A drop target that takes pixels gets
        // them from image/png below; a file manager, or an engine's asset
        // browser, wants something on disk with a name — and a drag that
        // carries only pixels lands in those as nothing at all.
        const QString stem = f.named
            ? f.path.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0)
            : QStringLiteral("%1").arg(f.hash, 16, 16, QLatin1Char('0'));
        const QString out = QDir(dir).filePath(stem + QStringLiteral(".png"));
        if (img.save(out, "PNG")) {
            urls.append(QUrl::fromLocalFile(out));
            ++taken;
        }
        if (indexes.size() == 1) mime->setImageData(img);
    }
    if (urls.isEmpty() && !mime->hasImage()) {
        // NOTHING to carry — every decode failed. Returning an empty QMimeData
        // still starts a drag, which then drops nothing anywhere with no
        // explanation; a null one is Qt's way of saying there is no drag.
        QDir(dir).removeRecursively();
        delete mime;
        qWarning("textures: nothing in the selection could be decoded to drag");
        return nullptr;
    }
    if (!urls.isEmpty()) mime->setUrls(urls);
    return mime;
}

int TexListModel::rowOfFile(int fileIdx) const
{
    if (fileIdx < 0) return -1;
    for (int r = 0; r < m_rows.size(); ++r)
        if (m_rows[r] == fileIdx) return r;
    return -1;
}

void TexListModel::setRows(const QVector<int>& rows)
{
    beginResetModel();
    m_rows = rows;
    endResetModel();
}

int TexListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant TexListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const fox::IndexedFile& f = ArchiveIndex::instance().files()[m_rows[index.row()]];
    const QVariant sh = shadowui::roleFor(f, role, f.path);
    if (sh.isValid()) return sh;
    if (role == Qt::DisplayRole) return f.path;
    if (role == FileIdxRole) return m_rows[index.row()];
    if (role == StemRole)
        return f.path.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
    if (role == DirRole) return f.path.section(QLatin1Char('/'), 0, -2);
    if (role == Qt::ToolTipRole) return f.path;
    return {};
}

// ── Grid cell ────────────────────────────────────────────────────────────────

TexGridDelegate::TexGridDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

QSize TexGridDelegate::cellSize() const
{
    // Icon, then two lines of caption plus padding — fixed rather than measured
    // per cell so the grid keeps a regular pitch while it scrolls. Wider than
    // the icon by more than the models grid is: texture names differ in the
    // MIDDLE (avf0_type0_v01_c02_bsm), which is exactly what a tight cell
    // elides away, so the extra characters are worth the column.
    return QSize(m_icon + 46, m_icon + 40);
}

QSize TexGridDelegate::sizeHint(const QStyleOptionViewItem&,
                                const QModelIndex&) const
{
    return cellSize();
}

void TexGridDelegate::paint(QPainter* p, const QStyleOptionViewItem& o,
                            const QModelIndex& i) const
{
    p->save();
    const bool sel = o.state & QStyle::State_Selected;
    if (sel) {
        // A tint and an outline rather than a solid fill: the cell exists to
        // show the texture, and painting the highlight edge to edge buries it.
        p->setRenderHint(QPainter::Antialiasing, true);
        QColor fill = o.palette.highlight().color();
        fill.setAlphaF(0.22);
        QColor edge = o.palette.highlight().color();
        edge.setAlphaF(0.85);
        p->setPen(QPen(edge, 1.0));
        p->setBrush(fill);
        p->drawRoundedRect(
            QRectF(o.rect.adjusted(2, 2, -2, -2)).adjusted(0.5, 0.5, -0.5, -0.5),
            4, 4);
    }
    const int fileIdx = i.data(TexListModel::FileIdxRole).toInt();
    const QRect iconRect(o.rect.left() + (o.rect.width() - m_icon) / 2,
                         o.rect.top() + 6, m_icon, m_icon);
    fox::TexThumbCache& tc = fox::TexThumbCache::instance();
    const QPixmap pm = tc.cached(fileIdx, m_icon);
    if (!pm.isNull()) {
        // A texture is not square; centre it in its box.
        p->drawPixmap(iconRect.left() + (m_icon - pm.width()) / 2,
                      iconRect.top() + (m_icon - pm.height()) / 2, pm);
    } else if (tc.has(fileIdx, m_icon)) {
        // Decoded and failed — a cached null. Leave the cell empty rather than
        // drawing a frame that suggests something is still coming.
    } else {
        QColor c = o.palette.text().color();
        c.setAlphaF(0.16);
        p->setPen(c);
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(iconRect.adjusted(4, 4, -4, -4), 3, 3);
        // Ask from HERE as well as from the tab's sweep: paint is the one event
        // that fires for every cell actually on screen, however it got there —
        // first show, re-index, new search, resize, splitter drag. Except
        // mid-fling, when the sweep owns the queue: queueing pages that are
        // already flying past makes the page you stop on fill in last.
        if (!m_settle || !m_settle->isActive()) tc.request(fileIdx, m_icon);
    }

    const QColor base = o.palette.text().color();
    const QFontMetrics fm(o.font);
    p->setFont(o.font);
    p->setPen(base);
    const QRect nameRect(o.rect.left() + 4, iconRect.bottom() + 3,
                         o.rect.width() - 8, fm.height());
    p->drawText(nameRect, Qt::AlignHCenter | Qt::AlignVCenter,
                fm.elidedText(i.data(TexListModel::StemRole).toString(),
                              Qt::ElideMiddle, nameRect.width()));
    QFont sf = o.font;
    sf.setPointSizeF(sf.pointSizeF() > 0 ? sf.pointSizeF() * 0.8
                                         : sf.pointSize() * 0.8);
    const QFontMetrics sfm(sf);
    p->setFont(sf);
    QColor sub = base;
    sub.setAlphaF(sel ? 0.8 : 0.55);
    p->setPen(sub);
    // Elided from the LEFT: the tail of a Fox path identifies it, the
    // /Assets/tpp/ head never does. The tooltip carries it in full.
    const QRect dirRect(o.rect.left() + 4, nameRect.bottom() + 1,
                        o.rect.width() - 8, sfm.height());
    p->drawText(dirRect, Qt::AlignHCenter | Qt::AlignVCenter,
                sfm.elidedText(i.data(TexListModel::DirRole).toString(),
                               Qt::ElideLeft, dirRect.width()));
    p->restore();
}

int TexListModel::fileIdxAt(const QModelIndex& index) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) return -1;
    return m_rows[index.row()];
}

TexturesTab::TexturesTab(QWidget* parent) : QWidget(parent)
{
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    auto* left = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    auto* bar = new QHBoxLayout();
    m_search = new QLineEdit(left);
    m_search->setPlaceholderText(QStringLiteral("Search textures…  (-word excludes)"));
    // Esc clears, the down arrow recalls the last ten searches, and a
    // committed search is remembered — the same four behaviours in every
    // search box in the application, from one place (template §4/§15).
    fox::searchbox::attach(m_search, QStringLiteral("textures/searchHistory"));
    m_search->setToolTip(searchq::tooltip());
    m_search->setClearButtonEnabled(true);
    // ── The funnel, LEFT of the search box (template §4) ────────────────
    // Same place and same icon as the Models tab, because it does the same
    // job: what narrows the list goes in front of the list's own input.
    // Everything that used to sit loose above the list — Named, the four game
    // toggles, and the used-by / user / format combos — is inside it now. Two
    // rows of controls came off the page and none of them was rebuilt: the
    // popup REPARENTS the widgets that already existed, so there is still one
    // `m_formatBox` and nothing to keep in step.
    m_filterBtn = new QToolButton(left);
    m_filterBtn->setIcon(foxglyph::toolIcon(18));
    m_filterBtn->setIconSize(QSize(foxglyph::kSize, foxglyph::kSize));
    m_filterBtn->setAutoRaise(true);
    m_filterBtn->setFocusPolicy(Qt::NoFocus);
    m_filterBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->addWidget(m_filterBtn);
    bar->addWidget(m_search, 1);
    m_namedOnly = new QCheckBox(QStringLiteral("Named"), left);
    m_namedOnly->setToolTip(QStringLiteral(
        "Only textures whose name this install can resolve. Everything else is "
        "shown by hash."));
    // THE DISPLAY SWITCH, on the right of the search box — the same place and
    // the same control as the Models tab, because it does the same job. It was
    // a checkbox labelled "Grid" here too. No Outliner: there is no per-texture
    // tree to grow, and a mode that resolves to nothing is worse than an
    // absent one.
    m_display = new fox::DisplayModeButton(QStringLiteral("textures/display"),
                                           left);
    m_display->setModes({fox::displaymode::list(), fox::displaymode::grid()});
    m_display->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->addWidget(m_display);
    leftLayout->addLayout(bar);
    // The chips, for the same reason the Models tab has them: a search of
    // "-lod normal" is two filters in force and the box shows them as a string
    // you have to parse. Each chip carries its own ✕.
    m_chips = new fox::FilterChips(left);
    leftLayout->addWidget(m_chips);
    connect(m_chips, &fox::FilterChips::removeRequested, this,
            [this](const QString& term) {
                m_search->setText(
                    searchq::Query::withoutTerm(m_search->text(), term));
                refresh();
            });
    connect(m_chips, &fox::FilterChips::clearRequested, this, [this] {
        m_search->clear();
        refresh();
    });
    m_count = new QLabel(left);
    leftLayout->addWidget(m_count);
    m_model = new TexListModel(this);
    m_list = new QListView(left);
    m_list->setModel(m_model);
    m_list->setUniformItemSizes(true);
    // An EXPLICIT delegate, always. A QListView that has never been given one
    // reports itemDelegate() == nullptr, and with setUniformItemSizes(true)
    // the row size is then cached as an INVALID QSize: every row comes back
    // height -1 and the view paints nothing at all — a full model, a working
    // search, a working selection, and a blank pane. Measured on this list:
    // visualRect(index(0,0)) went from 401x-1 to 421x14 the moment one was
    // installed. Three lists in this application had it.
    m_listDelegate = new QStyledItemDelegate(this);
    m_list->setItemDelegate(m_listDelegate);
    m_gridDelegate = new TexGridDelegate(this);
    m_gridDelegate->setIconSize(
        QSettings().value(QStringLiteral("textures/gridIcon"), 112).toInt());

    // Grid toggle and the per-game quick filters, above the list — the same
    // row the models tab has, in the same order, because they do the same job.
    {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        // The Grid checkbox was here; it is the display switch in the search
        // row now. What is left on this line is the per-game quick filters.
        static const fox::GameId kGames[] = {
            fox::GameId::Tpp, fox::GameId::Mgo, fox::GameId::GroundZeroes,
            fox::GameId::Survive,
        };
        for (fox::GameId g : kGames) {
            auto* box = new QCheckBox(QString::fromLatin1(fox::gameShortName(g)), left);
            box->setToolTip(QString::fromLatin1(fox::gameLongName(g)));
            box->setChecked(fox::GameFilter::instance().enabled(g));
            row->addWidget(box);
            m_gameBoxes.append({box, int(g)});
            connect(box, &QCheckBox::toggled, this, [this, g](bool on) {
                if (fox::GameFilter::instance().enabled(g) == on) return;
                fox::GameFilter::instance().setEnabled(g, on);
                refresh();
                Q_EMIT gameFilterChanged();
            });
        }
        row->addStretch(1);
        auto* host = new QWidget(left);
        host->setLayout(row);
        m_gameRow = host;   // placed in the funnel popup below, not on the page
    }

    // ── The three §7 filters ────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);

        // ONE CONTROL PER ROW, each under its own heading. These three were a
        // single line of unlabelled combos under a heading reading "Used by ·
        // user tag · format" — inside a popup, where there is no width to
        // spare and no page context to infer a combo's meaning from.
        m_usedBox = new QComboBox(left);
        m_usedBox->addItem(QStringLiteral("All textures"), 0);
        m_usedBox->addItem(QStringLiteral("Used by a model"), 1);
        m_usedBox->addItem(QStringLiteral("Orphans only"), 2);
        m_usedBox->setToolTip(QStringLiteral(
            "Whether any model in this install names this texture. An orphan "
            "is a real category, not an error: UI art, an effect map, or "
            "something whose model this install does not ship. Needs the "
            "texture→model sweep, which runs once in the background."));
        m_usedBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        m_userTagBox = new QComboBox(left);
        m_userTagBox->addItem(QStringLiteral("Any user"), QString());
        m_userTagBox->setToolTip(QStringLiteral(
            "Filter by the tags of the MODELS that use the texture, not by the "
            "texture's own — \"show me the character maps\" is a question "
            "about what wears them."));
        m_userTagBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        m_formatBox = new QComboBox(left);
        m_formatBox->addItem(QStringLiteral("Any format"), -1);
        m_formatBox->addItem(QStringLiteral("DXT1 (BC1)"), 2);
        m_formatBox->addItem(QStringLiteral("DXT5 (BC3)"), 4);
        m_formatBox->addItem(QStringLiteral("A8R8G8B8"), 0);
        m_formatBox->addItem(QStringLiteral("L8"), 1);
        m_formatBox->setToolTip(QStringLiteral(
            "The pixel format in the .ftex header. No index holds it, so this "
            "is applied last, over whatever the other filters left, and what it "
            "reads is remembered for the rest of the session. Narrow the search "
            "first if the list is very long."));
        m_formatBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        m_sweepLabel = new QLabel(left);
        m_sweepLabel->setWordWrap(true);
        // Its OWN label. The refusal used to share the sweep's progress label,
        // so it appeared and vanished every 256 models and was wiped for good
        // when the sweep finished.
        m_formatNote = new QLabel(left);
        m_formatNote->setWordWrap(true);
        m_formatNote->hide();
        // m_filterRow survives as the SWEEP-STATUS row: the progress line and
        // the format refusal, which belong together at the foot of the filters
        // because both explain why a filter has not answered yet.
        row->addWidget(m_sweepLabel, 1);
        auto* host = new QWidget(left);
        host->setLayout(row);
        m_filterRow = host;   // into the funnel popup, not onto the page
    }

    // ── Assemble the funnel ─────────────────────────────────────────────
    // The popup takes the rows that were on the page. Every connection made
    // above still holds, because these are the same widgets.
    m_filterPopup = new fox::FilterPopup(this);
    m_filterPopup->addHeading(QStringLiteral("Show"));
    m_filterPopup->addRow(m_namedOnly);
    m_filterPopup->addHeading(QStringLiteral("Game"));
    m_filterPopup->addRow(m_gameRow);
    m_filterPopup->addHeading(QStringLiteral("Used by a model"));
    m_filterPopup->addRow(m_usedBox);
    m_filterPopup->addHeading(QStringLiteral("Tag of the models that use it"));
    m_filterPopup->addRow(m_userTagBox);
    m_filterPopup->addHeading(QStringLiteral("Pixel format"));
    m_filterPopup->addRow(m_formatBox);
    m_filterPopup->addRow(m_formatNote);
    m_filterPopup->addRow(m_filterRow);
    m_filterPopup->addFooter();
    // Every filter back to its default IN ONE PLACE, which is the control a
    // stack of five independent filters most needs and did not have.
    connect(m_filterPopup, &fox::FilterPopup::clearRequested, this, [this] {
        m_namedOnly->setChecked(false);
        for (const auto& gb : m_gameBoxes) gb.first->setChecked(true);
        m_usedBox->setCurrentIndex(0);
        m_userTagBox->setCurrentIndex(0);
        m_formatBox->setCurrentIndex(0);
        if (m_search) m_search->clear();
        refresh();
        refreshFilterButton();
        m_filterPopup->setResultText(filterSummary());
        qInfo("textures: filters cleared");
    });
    connect(m_filterBtn, &QToolButton::clicked, this, [this] {
        m_filterPopup->setResultText(filterSummary());
        m_filterPopup->showFor(m_filterBtn);
    });
    connect(m_filterPopup, &fox::FilterPopup::closed, this,
            [this] { refreshFilterButton(); });
    m_filterBtn->setToolTip(QStringLiteral(
        "Filter\n\nNamed-only, the four games, whether a model uses the "
        "texture, the tags of the models that do, and the pixel format.\n"
        "The popup stays open while you tick."));
    refreshFilterButton();

    leftLayout->addWidget(m_list, 1);
    splitter->addWidget(left);

    // Decoding runs a page at a time, for the rows that are actually on screen.
    // Scrolling only ever RESTARTS the timer: decoding rows that are already
    // flying past makes the scroll stutter and throws the work away.
    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setSingleShot(true);
    m_thumbTimer->setInterval(120);
    connect(m_thumbTimer, &QTimer::timeout, this, &TexturesTab::decodeVisible);
    m_gridDelegate->setSettleTimer(m_thumbTimer);
    m_thumbRepaint = new QTimer(this);
    m_thumbRepaint->setSingleShot(true);
    connect(m_thumbRepaint, &QTimer::timeout, this,
            [this] { if (m_gridMode) m_list->viewport()->update(); });
    connect(&fox::TexThumbCache::instance(), &fox::TexThumbCache::ready, this,
            [this](int, int) {
                if (m_gridMode && !m_thumbRepaint->isActive())
                    m_thumbRepaint->start(40);
            });
    // Drag-out (§7). DragOnly, because nothing can be dropped INTO a read-only
    // view of the game's own files.
    // Multi-select, because the export vocabulary is context-sensitive now:
    // pick twelve textures and the menu offers "Extract 12 textures…", pick
    // one and it offers that one by name. A single-selection list could only
    // ever take the second branch.
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setDefaultDropAction(Qt::CopyAction);
    m_list->viewport()->installEventFilter(this);
    connect(m_list->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { if (m_gridMode) m_thumbTimer->start(); });
    connect(m_display, &fox::DisplayModeButton::modeChanged, this,
            [this](const QString& id) {
                setGridMode(id == fox::displaymode::grid());
            });

    // Middle: the viewer, with the channel strip under it. The strip is part
    // of the viewer rather than of the panel because it IS the viewer's own
    // state — it says which channel is on screen and switches it.
    auto* middle = new QWidget(splitter);
    {
        auto* mv = new QVBoxLayout(middle);
        mv->setContentsMargins(0, 0, 0, 0);
        mv->setSpacing(2);
        m_preview = new PreviewPane(middle);
        mv->addWidget(m_preview, 1);
        m_strip = new fox::ChannelStrip(middle);
        mv->addWidget(m_strip);
        // ── Volume slices (§7's face selector) ──────────────────────────
        // Hidden for the 2D textures that are nearly everything, so the row
        // costs nothing until there is something to step through.
        m_sliceRow = new QWidget(middle);
        {
            auto* sr = new QHBoxLayout(m_sliceRow);
            sr->setContentsMargins(4, 0, 4, 0);
            m_sliceLabel = new QLabel(m_sliceRow);
            sr->addWidget(m_sliceLabel);
            m_sliceBox = new QSpinBox(m_sliceRow);
            m_sliceBox->setMinimum(1);
            m_sliceBox->setToolTip(QStringLiteral(
                "This texture is a VOLUME — several 2D slices in one file. Fox "
                "writes these as a depth>1 DDS and never as a cubemap, so what "
                "there is to step through is slices. The channel you have "
                "chosen stays put as you step, which is what makes comparing "
                "them useful."));
            sr->addWidget(m_sliceBox);
            sr->addStretch(1);
            connect(m_sliceBox, &QSpinBox::valueChanged, this, [this](int v) {
                m_preview->showSlice(v - 1);
            });
            m_sliceRow->hide();
            mv->addWidget(m_sliceRow);
        }
        connect(m_preview, &PreviewPane::sourceChanged, this, [this] {
            const int n = m_preview->sliceCount();
            m_sliceRow->setVisible(n > 1);
            if (n <= 1) return;
            QSignalBlocker b(m_sliceBox);
            m_sliceBox->setMaximum(n);
            m_sliceBox->setValue(1);
            m_sliceLabel->setText(QStringLiteral("Slice (of %1):").arg(n));
        });
        ImageView* iv = m_preview->imageView();
        connect(m_strip, &fox::ChannelStrip::channelPicked, iv,
                [iv](ImageView::Channel c) { iv->setChannel(c); });
        // BOTH ways (template §3: one state, one owner). The strip mirrors the
        // view rather than keeping a second copy of the answer, so the toolbar
        // buttons, a keyboard shortcut and the strip cannot disagree.
        connect(iv, &ImageView::channelChanged, m_strip,
                [this](ImageView::Channel c) { m_strip->setCurrent(c); });
        connect(iv, &ImageView::imageChanged, m_strip, [this, iv] {
            m_strip->setImage(iv->image());
            m_strip->setCurrent(iv->channel());
        });
    }
    splitter->addWidget(middle);

    // ── Right: the N-panel column (template §6) ─────────────────────────
    // What this file IS and what it is ON — three panels now rather than three
    // bold headings stacked inside one pane. Each has its own header, its own
    // ▲▼✕, its own share of the column and a switch on the icon strip, and the
    // live counts the headings carried ("MIP LEVELS (10 — 2 not mounted)") are
    // in the panel headers, which costs no row inside the panel.
    m_panel = new fox::TextureInfoPanel(this);
    m_npanel = new fox::NPanel(QStringLiteral("textures/npanel"), splitter);
    m_npanel->addPanel(
        QStringLiteral("fileinfo"), QStringLiteral("FILE INFO"), 16,
        m_panel->fileInfoSection(),
        QStringLiteral("What this file IS — name, size, format, dimensions, "
                       "how many .ftexs stream files it needs, and its tags."));
    m_npanel->addPanel(
        QStringLiteral("mips"), QStringLiteral("MIP LEVELS"), 17,
        m_panel->mipsSection(),
        QStringLiteral(
            "A .ftex has no atlas; what it HAS is a mip chain split across "
            "<name>.N.ftexs files, and which mip lives in which file is the "
            "thing that explains a texture that decodes blurry on a partially "
            "downloaded install. Each row exports on its own."));
    m_npanel->addPanel(
        QStringLiteral("users"), QStringLiteral("ASSOCIATED MODELS"), 13,
        m_panel->usersSection(),
        QStringLiteral("What this texture is ON — model, material and role. "
                       "Double-click a model to open it in the Models tab."));
    m_npanel->restoreState({QStringLiteral("fileinfo"), QStringLiteral("mips"),
                            QStringLiteral("users")});
    splitter->addWidget(m_npanel);
    // The live counts into the panel headers.
    connect(m_panel, &fox::TextureInfoPanel::mipsTitleChanged, this,
            [this](const QString& t) {
                if (QLabel* l = m_npanel->titleLabel(QStringLiteral("mips")))
                    l->setText(t);
            });
    connect(m_panel, &fox::TextureInfoPanel::usersTitleChanged, this,
            [this](const QString& t) {
                if (QLabel* l = m_npanel->titleLabel(QStringLiteral("users")))
                    l->setText(t);
            });
    connect(m_panel, &fox::TextureInfoPanel::modelActivated, this,
            [this](quint64 hash, const QString& path) {
                if (!path.isEmpty()) Q_EMIT openModelRequested(path);
                else Q_EMIT unnamedModelActivated(hash);
            });
    // Explicit widths, not just stretch factors: the panel's trees have real
    // content and a stretch-only split gave them a column and a half. These are
    // the tab's DEFAULT — PanelPersist::bind below leaves them alone until the
    // user has dragged something.
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setStretchFactor(2, 2);
    splitter->setSizes({340, 620, 420});


    // AFTER the stretch factors above, which are this tab's chosen default:
    // PanelPersist leaves the sizes untouched when nothing has been
    // remembered, so the default stands until the user has actually dragged
    // something (template §6).
    PanelPersist::bind(splitter, QStringLiteral("textures/splitter"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(splitter);

    auto* debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(200);
    connect(m_search, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
    connect(m_namedOnly, &QCheckBox::toggled, debounce, qOverload<>(&QTimer::start));
    connect(m_usedBox, &QComboBox::currentIndexChanged, debounce,
            qOverload<>(&QTimer::start));
    connect(m_userTagBox, &QComboBox::currentIndexChanged, debounce,
            qOverload<>(&QTimer::start));
    connect(m_formatBox, &QComboBox::currentIndexChanged, debounce,
            qOverload<>(&QTimer::start));
    connect(debounce, &QTimer::timeout, this, &TexturesTab::refresh);

    // The sweep: started when the index is ready, and the two filters that
    // depend on it stay disabled until it lands rather than quietly returning
    // wrong sets in the meantime.
    connect(&fox::TextureUsers::instance(), &fox::TextureUsers::progress, this,
            [this](int done, int total) {
                m_sweepLabel->setText(
                    QStringLiteral("Reading models for \"used by\"… %1/%2")
                        .arg(done)
                        .arg(total));
            });
    connect(&fox::TextureUsers::instance(), &fox::TextureUsers::finished, this,
            [this](bool) {
                m_sweepLabel->clear();
                syncUsersUi();
                refresh();
            });
    m_usedBox->setEnabled(false);
    m_userTagBox->setEnabled(false);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& cur, const QModelIndex&) {
                m_current = m_model->fileIdxAt(cur);
                m_preview->showFile(m_current);
                m_panel->showTexture(m_current);
            });
    // A MODEL RESET does not emit currentChanged — QItemSelectionModel blocks
    // its own signals while it resets — so after every filter change the
    // selection was gone and the middle and right columns went on describing a
    // texture that was no longer in the list, with File ▸ Export still aimed at
    // it. Measured: the panel stayed fully populated over an empty selection.
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] {
        const QModelIndex cur = m_list->currentIndex();
        const int now = cur.isValid() ? m_model->fileIdxAt(cur) : -1;
        if (now == m_current) return;
        m_current = now;
        m_preview->showFile(m_current);
        m_panel->showTexture(m_current);
    });

    // Right-click: the shared export vocabulary on the clicked texture.
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListView::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                const int fi = m_model->fileIdxAt(m_list->indexAt(pos));
                if (fi < 0) return;
                QVector<int> sel;
                for (const QModelIndex& ix : m_list->selectionModel()->selectedIndexes()) {
                    const int f = m_model->fileIdxAt(ix);
                    if (f >= 0) sel.append(f);
                }
                // The row under the cursor wins when it is not part of the
                // selection — right-clicking outside a selection is how anyone
                // means "this one instead".
                if (!sel.contains(fi)) sel = {fi};
                QMenu menu(this);
                exportactions::addFileSetActions(&menu, sel, this);
                menu.exec(m_list->viewport()->mapToGlobal(pos));
            });

    if (m_display->mode() == fox::displaymode::grid()) setGridMode(true);
}

void TexturesTab::setGridMode(bool on)
{
    m_gridMode = on;
    // NO ALTERNATING ROWS IN GRID MODE. Qt bands by MODEL row, which in a
    // wrapped icon view is every second TILE rather than every second line of
    // tiles — a chequerboard, and one whose pattern moves every time the pane
    // is resized. The same fix the Models grid got: the same texture must not
    // read as sitting on a different colour depending on where it lands in the
    // wrap.
    fox::rowshade::setEnabled(m_list, !on);
    if (on) {
        m_list->setViewMode(QListView::IconMode);
        m_list->setResizeMode(QListView::Adjust);
        m_list->setMovement(QListView::Static);
        m_list->setWrapping(true);
        m_list->setSpacing(2);
        m_list->setItemDelegate(m_gridDelegate);
        m_list->setGridSize(m_gridDelegate->cellSize());
        m_thumbTimer->start(0);
    } else {
        m_list->setViewMode(QListView::ListMode);
        m_list->setWrapping(false);
        m_list->setSpacing(0);
        m_list->setGridSize(QSize());
        m_list->setItemDelegate(m_listDelegate);   // never nullptr — see above
        fox::TexThumbCache::instance().cancelQueued();
    }
    m_list->setUniformItemSizes(true);
    m_list->viewport()->update();
}

void TexturesTab::setIconSize(int px)
{
    const int clamped = qBound(48, px, 320);
    if (clamped == m_gridDelegate->iconSize()) return;
    m_gridDelegate->setIconSize(clamped);
    // Remembered, the way the models grid remembers its own: the tooltip
    // advertises the resize, and losing it on every restart makes the feature
    // read as broken.
    QSettings().setValue(QStringLiteral("textures/gridIcon"), clamped);
    if (m_gridMode) {
        m_list->setGridSize(m_gridDelegate->cellSize());
        // The cache is keyed by size, so the old thumbnails are still valid for
        // the old size and useless for this one — drop the queue rather than
        // decoding a page at a size nothing will draw.
        fox::TexThumbCache::instance().cancelQueued();
        m_thumbTimer->start(0);
        m_list->viewport()->update();
    }
}

void TexturesTab::decodeVisible()
{
    if (!m_gridMode || !m_model->total()) return;
    const QRect vp = m_list->viewport()->rect();
    const QModelIndex first = m_list->indexAt(vp.topLeft());
    const QModelIndex last = m_list->indexAt(vp.bottomRight());
    const int a = first.isValid() ? first.row() : 0;
    const int b = last.isValid() ? last.row() : qMin(m_model->total() - 1, a + 60);
    fox::TexThumbCache& tc = fox::TexThumbCache::instance();
    tc.cancelQueued();
    for (int r = a; r <= b && r < m_model->total(); ++r)
        tc.request(m_model->fileIdxAt(m_model->index(r, 0)),
                   m_gridDelegate->iconSize());
}

bool TexturesTab::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_list->viewport() && ev->type() == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(ev);
        if (m_gridMode && (we->modifiers() & Qt::ControlModifier)) {
            setIconSize(m_gridDelegate->iconSize()
                        + (we->angleDelta().y() > 0 ? 16 : -16));
            return true;
        }
        if (m_gridMode) m_thumbTimer->start();
    }
    return QWidget::eventFilter(obj, ev);
}

void TexturesTab::setSearchText(const QString& text)
{
    m_search->setText(text);
    refresh();
    // Select the first hit, so the middle and right columns have something to
    // show. Without it a harness grab of this tab is three panes of "select a
    // texture", which photographs the layout and none of the content.
    if (m_model->total() > 0)
        m_list->setCurrentIndex(m_model->index(0, 0));
}

void TexturesTab::setGridForShot(bool on, int iconPx)
{
    if (iconPx > 0) m_gridDelegate->setIconSize(iconPx);
    // Blocked: setChecked() would fire the toggled handler, which PERSISTS the
    // setting — so a harness run without --grid would quietly clear the user's
    // saved preference.
    {
        m_display->setMode(on ? fox::displaymode::grid()
                              : fox::displaymode::list());
    }
    setGridMode(on);
}

int TexturesTab::matchCount() const { return m_model->total(); }

bool TexturesTab::usersReady() const
{
    return fox::TextureUsers::instance().ready();
}

bool TexturesTab::usersDone() const
{
    const auto s = fox::TextureUsers::instance().state();
    return s == fox::TextureUsers::State::Ready
           || s == fox::TextureUsers::State::Failed;
}

void TexturesTab::syncUsersUi()
{
    const bool ready = fox::TextureUsers::instance().ready();
    m_usedBox->setEnabled(ready);
    m_userTagBox->setEnabled(ready);
    if (ready) rebuildUserTagBox();
}

bool TexturesTab::setUsedFilter(const QString& name)
{
    int want = -1;
    if (name.compare(QLatin1String("all"), Qt::CaseInsensitive) == 0) want = 0;
    else if (name.compare(QLatin1String("used"), Qt::CaseInsensitive) == 0) want = 1;
    else if (name.compare(QLatin1String("orphans"), Qt::CaseInsensitive) == 0) want = 2;
    if (want < 0) return false;
    const int at = m_usedBox->findData(want);
    if (at < 0) return false;
    m_usedBox->setCurrentIndex(at);
    refresh();
    return true;
}

bool TexturesTab::setFormatFilter(const QString& name)
{
    static const struct { const char* n; int fmt; } kNames[] = {
        {"any", -1}, {"dxt1", 2}, {"bc1", 2}, {"dxt5", 4}, {"bc3", 4},
        {"argb", 0}, {"a8r8g8b8", 0}, {"l8", 1},
    };
    for (const auto& k : kNames) {
        if (name.compare(QLatin1String(k.n), Qt::CaseInsensitive) != 0) continue;
        const int at = m_formatBox->findData(k.fmt);
        if (at < 0) return false;
        m_formatBox->setCurrentIndex(at);
        refresh();
        // The FILTER's verdict, not the combo's. A refusal (too many rows to
        // read headers for) puts the combo back to "Any format", and reporting
        // success there would have the harness log an unfiltered count as a
        // filtered one.
        return m_formatBox->currentData().toInt() == k.fmt;
    }
    return false;
}

bool TexturesTab::setUserTagFilter(const QString& tag)
{
    // The box is normally filled by the sweep's `finished` signal, which is a
    // QUEUED cross-thread connection — and the harness deliberately does not
    // pump the event loop while it waits, so the box still held only "Any user"
    // and every tag was rejected. Filling it here makes the flag work and costs
    // nothing when it is already filled.
    syncUsersUi();
    const int at = m_userTagBox->findData(tag);
    if (at < 0) return false;
    m_userTagBox->setCurrentIndex(at);
    refresh();
    return true;
}

bool TexturesTab::setChannel(const QString& name)
{
    static const struct { const char* n; ImageView::Channel c; } kNames[] = {
        {"rgb", ImageView::RGB}, {"r", ImageView::R}, {"g", ImageView::G},
        {"b", ImageView::B},     {"a", ImageView::A}, {"luma", ImageView::Luma},
    };
    for (const auto& k : kNames)
        if (name.compare(QLatin1String(k.n), Qt::CaseInsensitive) == 0) {
            m_preview->imageView()->setChannel(k.c);
            return true;
        }
    return false;
}

void TexturesTab::populateExportMenu(QMenu* menu)
{
    if (m_current >= 0) exportactions::addFileActions(menu, m_current, this);
    else menu->addAction(QStringLiteral("(select a texture)"))->setEnabled(false);
}

void TexturesTab::syncGameFilter()
{
    for (const auto& [box, g] : m_gameBoxes) {
        const bool on = fox::GameFilter::instance().enabled(fox::GameId(g));
        if (box->isChecked() == on) continue;
        QSignalBlocker b(box);   // do not write back what we just read
        box->setChecked(on);
    }
    refresh();
}

// The same contract ModelsTab::setPanelsForShot has. --npanel claimed to act
// on "whichever tab is on screen" and handled three of the five, so a run
// that selected Textures photographed the MODELS column instead — silently,
// because the fallback is a valid tab with valid panels.
QString TexturesTab::setPanelsForShot(const QString& keys)
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
        out += QStringLiteral(" — NO SUCH PANEL: %1 (this tab has: %2)")
                   .arg(unknown.join(QLatin1Char(',')),
                        known.join(QLatin1Char(',')));
    return out;
}


// The funnel says whether it is doing anything. A funnel that looks identical
// filtered and unfiltered is how a user ends up convinced the index is missing
// assets — §4 asks for the icon to be tinted, and the tooltip names WHICH
// filters are on, because "3 filters" answers nothing.
// --filterpopup, for this tab. Opening a popup is the only way to photograph
// what is inside it, and a popup that has never been photographed is a column
// of controls nobody has seen laid out.
bool TexturesTab::openFilterPopupForShot()
{
    if (!m_filterPopup || !m_filterBtn) return false;
    // The footer, here too. Setting it only on the BUTTON's click left every
    // headless shot of this popup showing an empty result line — which is
    // exactly how a footer nobody set would look in the real UI as well.
    m_filterPopup->setResultText(filterSummary());
    m_filterPopup->showFor(m_filterBtn);
    for (int i = 0; i < 6; ++i) QCoreApplication::processEvents();
    return true;
}

// What the filters left, and which ones are on — the footer line in the popup.
// The Models tab's popup has had one; this one had nothing, so the only way to
// find out what a filter combination did was to close the popup and look.
QString TexturesTab::filterSummary() const
{
    const int shown = m_model ? m_model->rowCount(QModelIndex()) : 0;
    QStringList on;
    if (m_namedOnly && m_namedOnly->isChecked()) on << QStringLiteral("named only");
    for (const auto& gb : m_gameBoxes)
        if (gb.first && !gb.first->isChecked())
            on << QStringLiteral("%1 off").arg(QString::fromLatin1(
                   fox::gameShortName(fox::GameId(gb.second))));
    if (m_usedBox && m_usedBox->currentIndex() > 0) on << m_usedBox->currentText();
    if (m_userTagBox && m_userTagBox->currentIndex() > 0)
        on << m_userTagBox->currentText();
    if (m_formatBox && m_formatBox->currentIndex() > 0)
        on << m_formatBox->currentText();
    if (m_search && !m_search->text().isEmpty())
        on << QStringLiteral("\"%1\"").arg(m_search->text());
    return on.isEmpty()
               ? QStringLiteral("%1 texture(s) — no filter")
                     .arg(QLocale().toString(shown))
               : QStringLiteral("%1 texture(s) · %2")
                     .arg(QLocale().toString(shown), on.join(QStringLiteral(" · ")));
}

void TexturesTab::refreshFilterButton()
{
    if (!m_filterBtn) return;
    QStringList on;
    if (m_namedOnly && m_namedOnly->isChecked()) on << QStringLiteral("Named");
    for (const auto& gb : m_gameBoxes)
        if (gb.first && !gb.first->isChecked())
            on << QStringLiteral("%1 off")
                      .arg(QString::fromLatin1(
                          fox::gameShortName(fox::GameId(gb.second))));
    if (m_usedBox && m_usedBox->currentIndex() > 0) on << m_usedBox->currentText();
    if (m_userTagBox && m_userTagBox->currentIndex() > 0)
        on << m_userTagBox->currentText();
    if (m_formatBox && m_formatBox->currentIndex() > 0)
        on << m_formatBox->currentText();

    m_filterBtn->setDown(!on.isEmpty());
    // The popup's footer follows the same state this button does, so they are
    // written in the same place and cannot disagree.
    if (m_filterPopup) m_filterPopup->setResultText(filterSummary());
    m_filterBtn->setToolTip(
        on.isEmpty()
            ? QStringLiteral(
                  "Filter\n\nNamed-only, the four games, whether a model uses "
                  "the texture, the tags of the models that do, and the pixel "
                  "format.\nThe popup stays open while you tick.")
            : QStringLiteral("Filter — active\n\n%1").arg(on.join(QLatin1String("\n"))));
}

void TexturesTab::onIndexReady(bool ready)
{
    // Every cached thumbnail is keyed by file index, and those have just been
    // reassigned — the same reason the models tab drops its renders here.
    fox::TexThumbCache::instance().reset();
    if (!ready) return;
    refresh();
    // The texture→model sweep, in the background. Started HERE rather than at
    // construction because it needs a finished index, and started
    // unconditionally rather than on first use because the two filters that
    // need it are visible from the moment the tab opens — a control that only
    // starts working after you have pressed it once reads as broken.
    // Reset to neutral, not just disabled. A greyed combo still reading
    // "Orphans only" over a list showing every texture is the same wrong answer
    // the half-built map would have given, just wearing a different hat.
    {
        QSignalBlocker a(m_usedBox), b(m_userTagBox);
        m_usedBox->setCurrentIndex(0);
        m_userTagBox->setCurrentIndex(0);
    }
    m_usedBox->setEnabled(false);
    m_userTagBox->setEnabled(false);
    fox::TextureUsers::instance().build();
}

// The format filter's session cache: fileIdx → pixelFormatType, -2 = unreadable.
// A session cache rather than a disk one because a .ftex header is a cheap read
// and the set anyone filters is small; if this ever needs to run over a whole
// install it wants the same treatment TextureUsers got, not a bigger budget.
static QHash<int, int> g_formatCache;
static quint64 g_formatGeneration = 0;

bool TexturesTab::applyFormatFilter()
{
    const int want = m_formatBox->currentData().toInt();
    m_formatNote->hide();
    if (want < 0) return true;
    fox::ArchiveIndex& index = fox::ArchiveIndex::instance();
    // Keyed by file index, so a rescan invalidates the whole cache.
    if (g_formatGeneration != index.installGeneration()) {
        g_formatCache.clear();
        g_formatGeneration = index.installGeneration();
    }
    // BOUNDED, and refused rather than partially applied. Every uncached row
    // costs a decompress; over a quarter of a million textures that is a frozen
    // window, and a filter that silently gave up half way would be worse than
    // one that says it needs a narrower search.
    const int kBudget = 20000;
    int uncached = 0;
    for (int r = 0; r < m_model->total(); ++r) {
        const int fi = m_model->fileIdxAt(m_model->index(r, 0));
        if (!g_formatCache.contains(fi)) ++uncached;
    }
    if (uncached > kBudget) {
        // REFUSED, and the combo goes back to "Any format" with it. Returning
        // while leaving the unfiltered list on screen under a filter label was
        // indistinguishable from having applied it — and the harness happily
        // reported the unfiltered count as a filtered one.
        m_formatNote->setText(
            QStringLiteral("Reading the format of %1 textures would freeze the "
                           "window, so the format filter is off. Narrow the "
                           "search first — it needs about %2 or fewer.")
                .arg(m_model->total())
                .arg(kBudget));
        m_formatNote->show();
        QSignalBlocker b(m_formatBox);
        m_formatBox->setCurrentIndex(m_formatBox->findData(-1));
        return false;
    }
    QVector<int> keep;
    for (int r = 0; r < m_model->total(); ++r) {
        const int fi = m_model->fileIdxAt(m_model->index(r, 0));
        auto it = g_formatCache.constFind(fi);
        int fmt;
        if (it != g_formatCache.constEnd()) {
            fmt = *it;
        } else {
            fox::FtexFile ftex;
            fmt = ftex.parse(index.readFile(index.files()[fi]))
                      ? int(ftex.pixelFormatType())
                      : -2;
            g_formatCache.insert(fi, fmt);
        }
        if (fmt == want) keep.append(fi);
    }
    m_model->setRows(keep);
    return true;
}

void TexturesTab::rebuildUserTagBox()
{
    const QString had = m_userTagBox->currentData().toString();
    QSignalBlocker b(m_userTagBox);
    m_userTagBox->clear();
    m_userTagBox->addItem(QStringLiteral("Any user"), QString());
    // The MODEL vocabulary, not the texture one — this filter asks about what
    // uses the texture, so the tags it offers have to be the ones models carry.
    for (const fox::TagCategory& c : fox::ModelTags::instance().categories())
        for (const fox::TagInfo& tag : c.tags)
            m_userTagBox->addItem(
                QStringLiteral("%1: %2").arg(c.label, tag.label), tag.tag);
    const int at = m_userTagBox->findData(had);
    m_userTagBox->setCurrentIndex(at >= 0 ? at : 0);
}

void TexturesTab::refresh()
{
    // KEEP THE SELECTION when the new list still contains it. A model reset
    // clears the current index, so every debounced keystroke used to deselect
    // whatever you were looking at — which was invisible only because the panel
    // went on showing the old texture over an empty selection. Fixing that
    // exposed this; both are the same bug seen from two sides.
    const int had = m_current;
    m_model->refresh(m_search->text(), m_namedOnly->isChecked(),
                     m_usedBox ? m_usedBox->currentData().toInt() : 0,
                     m_userTagBox ? m_userTagBox->currentData().toString()
                                  : QString());
    applyFormatFilter();
    const int row = m_model->rowOfFile(had);
    if (row >= 0) m_list->setCurrentIndex(m_model->index(row, 0));
    m_count->setText(QStringLiteral("%1 texture(s)").arg(m_model->total()));
    // The chips are a VIEW of the search string and hold no state of their
    // own — the same contract as the Models tab's, so there is one thing to
    // keep right rather than two.
    if (m_chips) m_chips->setQuery(m_search->text());
    if (m_gridMode) m_thumbTimer->start(0);
}
