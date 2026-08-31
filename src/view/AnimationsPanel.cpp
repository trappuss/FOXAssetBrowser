// AnimationsPanel.cpp — see AnimationsPanel.h.
#include "view/AnimationsPanel.h"
#include <QClipboard>
#include <QApplication>
#include <QMenu>

#include "util/MenuText.h"
#include "util/TableCopy.h"
#include <algorithm>
#include <QSettings>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QStackedWidget>
#include <QItemSelectionModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "util/SearchBox.h"
#include "view/PanelBox.h"

#include "anim/AnimBind.h"
#include "index/AnimCatalog.h"
#include "index/ArchiveIndex.h"

namespace {

// Row payloads. UserRole+1 is -1 on an archive row and the clip index on a
// clip row, which is what tells the two apart everywhere below — an archive
// with a clip index of 0 and a clip 0 would otherwise be the same row.
constexpr int kArchiveRole = Qt::UserRole;
constexpr int kClipRole = Qt::UserRole + 1;
constexpr int kHayRole = Qt::UserRole + 2;    // lower-cased search haystack
constexpr int kCatRole = Qt::UserRole + 3;    // int(AnimCategory) on clip rows

}  // namespace

AnimationsPanel::AnimationsPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(4);

    auto* head = new QHBoxLayout();
    // ── The scope, ABOVE the filter ─────────────────────────────────────
    // First control in the panel because it is the first decision: "which
    // clips am I looking at" comes before "which of those matches my text".
    m_scope = new QComboBox(this);
    m_scope->addItem(QStringLiteral("This model's animations"),
                     QStringLiteral("model"));
    m_scope->addItem(QStringLiteral("All animations"), QStringLiteral("all"));
    m_scope->addItem(QStringLiteral("From another model…"),
                     QStringLiteral("other"));
    m_scope->setToolTip(QStringLiteral(
        "Which clips this list holds.\n\n"
        "\"This model's\" is resolved from the RIG, not from the file name: a "
        "gani animates rig units, and the model's .frig is what says which "
        "bone each unit moves — so an archive is listed exactly when this "
        "model's own rig can play it. A prop with no drivable bones therefore "
        "lists nothing, which is the true answer for it.\n\n"
        "\"From another model…\" answers the same question about a model you "
        "name, without loading it."));
    {
        QSettings st;
        const QString want = st.value(QStringLiteral("animations/scope"),
                                      QStringLiteral("model")).toString();
        const int at = m_scope->findData(want);
        // "other" is never restored: the model it referred to was a choice
        // made in another session about a model that may not even be indexed
        // now, and a scope that silently means something else is worse than
        // the default.
        m_scope->setCurrentIndex(at >= 0 && want != QLatin1String("other")
                                     ? at
                                     : m_scope->findData(
                                           QStringLiteral("model")));
    }
    v->addWidget(m_scope);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(QStringLiteral("Filter every clip…"));
    // Esc clears, the down arrow recalls the last ten searches, and a
    // committed search is remembered — the same four behaviours in every
    // search box in the application, from one place (template §4/§15).
    fox::searchbox::attach(m_filter, QStringLiteral("animations/filterHistory"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setToolTip(QStringLiteral(
        "Search EVERY clip in the install at once, not just the archive that "
        "happens to be open. Matches the readable label, the asset's own name "
        "and the archive it lives in, so \"walk\", \"wk_lp\" and "
        "\"buddydog\" all find something."));
    v->addWidget(m_filter);
    // The filter gets a ROW OF ITS OWN. Sharing one with two combos left it
    // 40px wide in a docked column — "Filte…" with the placeholder elided,
    // which is a search box that cannot be read let alone typed into and
    // measured at 271px of panel.
    m_category = new QComboBox(this);
    m_category->addItem(QStringLiteral("All categories"), -1);
    for (int c = 0; c < int(fox::AnimCategory::Count); ++c)
        m_category->addItem(fox::animCategoryName(fox::AnimCategory(c)), c);
    m_category->setToolTip(QStringLiteral(
        "Our reading of the clip name's action token, not a field the data "
        "carries. \"Other\" is genuinely miscellaneous and is about a fifth of "
        "the corpus."));
    head->addWidget(m_category);
    m_sort = new QComboBox(this);
    // Stable string ids, restored with findData (§3.1). The old build stored
    // nothing here at all, so there is no index-keyed setting to migrate.
    m_sort->addItem(QStringLiteral("Archive order"), QStringLiteral("archive"));
    m_sort->addItem(QStringLiteral("Clip name"), QStringLiteral("name"));
    m_sort->addItem(QStringLiteral("Asset name"), QStringLiteral("asset"));
    m_sort->addItem(QStringLiteral("Category"), QStringLiteral("category"));
    m_sort->setToolTip(QStringLiteral(
        "How the clips inside each archive are ordered. The ARCHIVES keep "
        "their own order in every mode — they are the tree, and re-ordering "
        "them would move a row you were about to click.\n\n"
        "\"Archive order\" is the order the file itself stores, which is the "
        "order a clip index means; the other three are for finding something "
        "by eye. Sorting does not change what a clip index is, so an export "
        "still writes what you selected."));
    {
        QSettings st;
        const QString want =
            st.value(QStringLiteral("animations/sort"),
                     QStringLiteral("archive")).toString();
        const int at = m_sort->findData(want);
        m_sort->setCurrentIndex(at >= 0 ? at : 0);
    }
    head->addWidget(m_sort);
    v->addLayout(head);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({QStringLiteral("Clip"), QStringLiteral("Asset")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // This list had no context menu at all — the only way to act on a
    // selection was the two buttons at the bottom, and the only way to get a
    // clip's asset name out of it was to retype it. §12 and §15.
    tablecopy::install(m_tree);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& at) {
                QTreeWidgetItem* under = m_tree->itemAt(at);
                if (under && !under->isSelected()) {
                    m_tree->clearSelection();
                    under->setSelected(true);
                    m_tree->setCurrentItem(under);
                }
                const int n = selectedClips().size();
                QMenu m(this);
                // The same context-sensitive rule as everywhere else: one clip
                // is named, several are counted.
                const QString what =
                    n == 1 && under ? under->text(0)
                                    : QStringLiteral("%1 clips").arg(n);
                QAction* one = m.addAction(
                    QStringLiteral("Export %1 as one .glb…").arg(what), this,
                    [this] { Q_EMIT exportRequested(false); });
                one->setEnabled(n > 0);
                QAction* each = m.addAction(
                    QStringLiteral("Export %1, one file each…").arg(what), this,
                    [this] { Q_EMIT exportRequested(true); });
                each->setEnabled(n > 1);
                if (under && under->data(0, kClipRole).toInt() >= 0) {
                    m.addSeparator();
                    const QString asset = under->text(1);
                    m.addAction(MenuText::withCopyValue(MenuText::kCopyName, asset),
                                this, [asset] {
                                    QApplication::clipboard()->setText(asset);
                                });
                }
                m.addSeparator();
                tablecopy::addMenuActions(&m, m_tree);
                m.exec(m_tree->viewport()->mapToGlobal(at));
            });
    m_tree->setAllColumnsShowFocus(true);
    m_tree->header()->setStretchLastSection(true);
    // The readable label is the column people scan; the asset name is what
    // they check against. Both need room — at the default widths the label
    // column showed six characters, which for a clip called "fc5010102e49dbea"
    // is nothing at all.
    m_tree->setColumnWidth(0, 150);
    // The tree is the elastic part, and it has to be willing to GIVE. Left at
    // Qt's default minimum it out-argued the count label whenever the column
    // was short, and the label — the line that explains why a list is empty —
    // was squeezed to nothing.
    m_tree->setMinimumHeight(60);
    // ── The list and its empty state, as two PAGES ──────────────────────
    // The empty state replaces the tree rather than sitting inside it: as a
    // tree row the sentence was elided to "…has no bones any rig d…" in a
    // 270px column, and uniform row heights mean it cannot wrap there either.
    //
    // A stacked page rather than two widgets in the column, because a label
    // that shares a stretch factor with a hidden tree is positioned by the
    // layout, not by its own alignment — the sentence came out floating in
    // the vertical middle of an empty panel, and setAlignment on the layout
    // item did not move it. A page whose own layout ends in a stretch puts it
    // at the top by construction.
    m_pages = new QStackedWidget(this);
    m_pages->addWidget(m_tree);
    auto* emptyPage = new QWidget(m_pages);
    auto* ev = new QVBoxLayout(emptyPage);
    ev->setContentsMargins(4, 8, 4, 4);
    m_empty = new QLabel(emptyPage);
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_empty->setStyleSheet(QStringLiteral("color:#8a8a8a;"));
    ev->addWidget(m_empty);
    ev->addStretch(1);
    m_pages->addWidget(emptyPage);
    v->addWidget(m_pages, 1);

    m_count = new QLabel(this);
    // ONE LINE, never wrapped. A wrapped label between a stretching tree and a
    // button row is a fight it always loses: the layout hands it one line's
    // minimum and the rest of the sentence is clipped, or — once it has a
    // minimum of its own — the content no longer fits the pane and Qt lets
    // widgets overlap. So the counts go here, short, and the explanation goes
    // where there is actually room for it: the tooltip, and a row IN THE TREE
    // when the list is empty, which is the one case where the tree has space
    // going spare and is also where the user is looking.
    m_count->setTextFormat(Qt::PlainText);
    m_count->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    v->addWidget(m_count);

    auto* buttons = new QHBoxLayout();
    m_exportOne = new QPushButton(QStringLiteral("Export .glb…"), this);
    m_exportOne->setToolTip(QStringLiteral(
        "The model in the viewport, RIGGED, with every selected clip in the "
        "one file as its own glTF animation. Each clip is sampled per frame "
        "from the same solver the viewport plays, so what you get is what you "
        "were watching — IK, help bones and root motion included."));
    m_exportEach = new QPushButton(QStringLiteral("One file each…"), this);
    m_exportEach->setToolTip(QStringLiteral(
        "The same, but one .glb per clip in a folder you choose — which is "
        "what an engine import pipeline usually wants."));
    buttons->addWidget(m_exportOne);
    buttons->addWidget(m_exportEach);
    buttons->addStretch(1);
    v->addLayout(buttons);

    connect(m_filter, &QLineEdit::textChanged, this,
            [this] { applyFilter(); });
    connect(m_sort, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) {
                QSettings().setValue(QStringLiteral("animations/sort"),
                                     m_sort->currentData().toString());
                applySort();
            });
    connect(m_category, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { applyFilter(); });
    connect(m_scope, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) {
                if (m_syncing) return;
                if (m_scope->currentData().toString()
                    == QLatin1String("other")) {
                    openModelPicker();
                    return;   // the picker saves and re-filters, or reverts
                }
                QSettings().setValue(QStringLiteral("animations/scope"),
                                     m_scope->currentData().toString());
                m_scopeResolved = false;
                resolveScope();
                applyFilter();
                // The animation bar mirrors this; it has to be told.
                emit scopeChanged();
            });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            [this] { refreshButtons(); });
    // A single click plays it. Clip lists are browsed by ear, not by plan: the
    // point of a list of 2,855 clips is to walk down it watching each one, and
    // needing a double-click for that doubles every step.
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
                if (m_syncing || !cur) return;
                const int clip = cur->data(0, kClipRole).toInt();
                if (clip < 0) return;   // an archive row plays nothing
                emit clipChosen(cur->data(0, kArchiveRole).toInt(), clip);
            });
    connect(m_exportOne, &QPushButton::clicked, this,
            [this] { emit exportRequested(false); });
    connect(m_exportEach, &QPushButton::clicked, this,
            [this] { emit exportRequested(true); });
    refreshButtons();
}

bool AnimationsPanel::isBuilt() const
{
    return m_built
           && m_builtArchives == fox::AnimCatalog::instance().archives().size();
}

void AnimationsPanel::rebuild()
{
    const fox::AnimCatalog& cat = fox::AnimCatalog::instance();
    m_syncing = true;
    m_tree->clear();
    m_totalClips = 0;
    for (const int ai : cat.order()) {
        const fox::AnimArchive& a = cat.archives()[ai];
        auto* top = new QTreeWidgetItem(m_tree);
        top->setText(0, a.stem);
        top->setText(1, QStringLiteral("%1%2%3 clip%4")
                            .arg(a.game.toUpper(),
                                 a.group.isEmpty()
                                     ? QStringLiteral(" · ")
                                     : QStringLiteral(" · %1 · ").arg(a.group))
                            .arg(a.clips.size())
                            .arg(a.clips.size() == 1 ? QString()
                                                     : QStringLiteral("s")));
        top->setToolTip(0, a.path);
        top->setData(0, kArchiveRole, a.fileIdx);
        top->setData(0, kClipRole, -1);
        top->setData(0, kHayRole,
                     (a.stem + QLatin1Char(' ') + a.group + QLatin1Char(' ')
                      + a.game + QLatin1Char(' ') + a.path)
                         .toLower());
        for (const fox::AnimClip& c : a.clips) {
            auto* row = new QTreeWidgetItem(top);
            row->setText(0, c.label.isEmpty() ? c.name : c.label);
            row->setText(1, c.name);
            row->setData(0, kArchiveRole, a.fileIdx);
            row->setData(0, kClipRole, c.index);
            row->setData(0, kCatRole, int(c.category));
            // The archive's own words go INTO the clip's haystack: a filter of
            // "buddydog walk" is one query about two different rows, and a
            // clip that only knew its own name could never answer it.
            row->setData(0, kHayRole,
                         (c.label + QLatin1Char(' ') + c.name + QLatin1Char(' ')
                          + a.stem + QLatin1Char(' ') + a.group)
                             .toLower());
            ++m_totalClips;
        }
    }
    m_syncing = false;
    m_built = true;
    m_builtArchives = fox::AnimCatalog::instance().archives().size();
    // What this panel would like when it first opens: the scope row, the
    // filter, the two combos, a readable slice of the tree, the count and the
    // buttons. Without it the column sized the panel from an EMPTY tree and
    // the list opened four rows tall.
    setProperty(fox::kPanelWantH, 360);
    applySort();
    applyFilter();
}

// Reorder the CLIPS inside each archive. The archives themselves keep the
// catalogue's order in every mode: they are the tree's shape, and re-sorting
// them would move a row out from under a cursor that was about to click it.
//
// takeChildren/addChildren rather than QTreeWidget::sortItems, because sorting
// the widget sorts EVERY level — the archive rows included — and because a
// sort by category has to compare a stored role rather than the displayed
// text. Selection survives: the items are the same objects, only re-parented
// in order, and Qt keeps an item's selected flag across that.
void AnimationsPanel::applySort()
{
    if (!m_tree || !m_sort) return;
    const QString id = m_sort->currentData().toString();
    const bool syncWas = m_syncing;
    m_syncing = true;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = m_tree->topLevelItem(i);
        QList<QTreeWidgetItem*> kids = top->takeChildren();
        if (id == QLatin1String("name")) {
            std::sort(kids.begin(), kids.end(),
                      [](QTreeWidgetItem* a, QTreeWidgetItem* b) {
                          return a->text(0).compare(b->text(0),
                                                    Qt::CaseInsensitive) < 0;
                      });
        } else if (id == QLatin1String("asset")) {
            std::sort(kids.begin(), kids.end(),
                      [](QTreeWidgetItem* a, QTreeWidgetItem* b) {
                          return a->text(1).compare(b->text(1),
                                                    Qt::CaseInsensitive) < 0;
                      });
        } else if (id == QLatin1String("category")) {
            std::sort(kids.begin(), kids.end(),
                      [](QTreeWidgetItem* a, QTreeWidgetItem* b) {
                          const int ca = a->data(0, kCatRole).toInt();
                          const int cb = b->data(0, kCatRole).toInt();
                          if (ca != cb) return ca < cb;
                          return a->text(0).compare(b->text(0),
                                                    Qt::CaseInsensitive) < 0;
                      });
        } else {
            // "archive": the order the file stores, which is what a clip index
            // means. Restored from the index rather than remembered separately,
            // so this mode is exact rather than approximately undone.
            std::sort(kids.begin(), kids.end(),
                      [](QTreeWidgetItem* a, QTreeWidgetItem* b) {
                          return a->data(0, kClipRole).toInt()
                               < b->data(0, kClipRole).toInt();
                      });
        }
        top->addChildren(kids);
    }
    m_syncing = syncWas;
}

bool AnimationsPanel::setSortOrder(const QString& id)
{
    if (!m_sort) return false;
    const int at = m_sort->findData(id);
    if (at < 0) return false;
    m_sort->setCurrentIndex(at);   // the handler saves and re-sorts
    return true;
}

QString AnimationsPanel::sortOrder() const
{
    return m_sort ? m_sort->currentData().toString() : QString();
}

QString AnimationsPanel::firstRowsForShot(int n) const
{
    QStringList out;
    for (int i = 0; i < m_tree->topLevelItemCount() && out.size() < n; ++i) {
        QTreeWidgetItem* top = m_tree->topLevelItem(i);
        if (top->isHidden()) continue;
        for (int j = 0; j < top->childCount() && out.size() < n; ++j)
            if (!top->child(j)->isHidden()) out << top->child(j)->text(0);
    }
    return out.join(QStringLiteral(" | "));
}

void AnimationsPanel::setCurrentModel(const QString& modelPath)
{
    if (m_modelPath == modelPath) return;
    m_modelPath = modelPath;
    // Only the "this model" scope is affected — a scope pinned to another
    // model deliberately survives loading something else, which is the whole
    // point of pinning it.
    if (scope() == QLatin1String("model")) {
        m_scopeResolved = false;
        // Resolved WHETHER OR NOT the tree is built: the animation bar reads
        // this scope and the bar exists even when this panel is closed.
        resolveScope();
        if (m_built) applyFilter();
        emit scopeChanged();
    }
}

QString AnimationsPanel::scope() const
{
    return m_scope ? m_scope->currentData().toString() : QString();
}

bool AnimationsPanel::setScope(const QString& id)
{
    if (!m_scope) return false;
    // "other" means "a model was chosen"; setting it with none chosen would
    // leave a scope that names nothing, so it is refused rather than accepted
    // and quietly treated as "all". setScopeModel() is the way in.
    if (id == QLatin1String("other") && m_scopeLabel.isEmpty()) return false;
    const int at = m_scope->findData(id);
    if (at < 0) return false;
    m_syncing = true;
    m_scope->setCurrentIndex(at);
    m_syncing = false;
    QSettings().setValue(QStringLiteral("animations/scope"), id);
    m_scopeResolved = false;
    resolveScope();
    applyFilter();
    emit scopeChanged();
    return true;
}

bool AnimationsPanel::setScopeModel(const QString& modelNeedle)
{
    const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
    QString bestPath;
    for (const fox::IndexedFile& f : ix.files()) {
        if (fox::ArchiveIndex::extensionOf(f) != QLatin1String("fmdl")) continue;
        if (!f.path.contains(modelNeedle, Qt::CaseInsensitive)) continue;
        // Shortest match wins, so "avm0_body0_def" prefers the model with
        // that exact stem over a longer variant that merely contains it.
        if (bestPath.isEmpty() || f.path.size() < bestPath.size())
            bestPath = f.path;
    }
    if (bestPath.isEmpty()) return false;
    const animbind::Binding b = animbind::forModel(bestPath);
    if (!b.valid()) return false;
    m_scopeArchives = b.archives;
    m_scopeClips = b.clips;
    m_scopeCeiling = b.ceiling;
    m_scopeLabel = b.label;
    m_scopeResolved = true;
    m_syncing = true;
    const int at = m_scope->findData(QStringLiteral("other"));
    if (at >= 0) {
        m_scope->setItemText(
            at, QStringLiteral("From %1…").arg(b.label));
        m_scope->setCurrentIndex(at);
    }
    m_syncing = false;
    QSettings().setValue(QStringLiteral("animations/scope"),
                         QStringLiteral("other"));
    applyFilter();
    return true;
}

// Resolve the archive set the current scope allows. "all" resolves to an
// empty set AND m_scopeResolved = true, which applyFilter reads as "no
// restriction" — the set is only consulted when the scope is not "all".
void AnimationsPanel::resolveScope()
{
    if (m_scopeResolved) return;
    const QString id = scope();
    if (id == QLatin1String("all")) {
        m_scopeArchives.clear();
        m_scopeClips = m_totalClips;
        m_scopeCeiling = 0;
        m_scopeLabel.clear();
        m_scopeResolved = true;
        return;
    }
    if (id == QLatin1String("other")) {
        m_scopeResolved = true;   // set by setScopeModel; nothing to recompute
        return;
    }
    const animbind::Binding b = animbind::forModel(m_modelPath);
    m_scopeArchives = b.archives;
    m_scopeClips = b.clips;
    m_scopeCeiling = b.ceiling;
    m_scopeLabel = b.label;
    m_scopeResolved = true;
}

bool AnimationsPanel::scopeIsAll()
{
    resolveScope();
    return scope() == QLatin1String("all");
}

QSet<int> AnimationsPanel::scopeArchiveFiles()
{
    resolveScope();
    return m_scopeArchives;
}

QString AnimationsPanel::scopeSummary() const
{
    const QString id = scope();
    if (id == QLatin1String("all"))
        return QStringLiteral("all %1 clip(s)").arg(m_totalClips);
    if (m_modelPath.isEmpty() && id == QLatin1String("model"))
        return QStringLiteral("no model loaded");
    return QStringLiteral("%1 — %2 archive(s), %3 clip(s)")
        .arg(m_scopeLabel.isEmpty() ? QStringLiteral("(model)") : m_scopeLabel)
        .arg(m_scopeArchives.size())
        .arg(m_scopeClips);
}

// The picker for "From another model…". A plain filtered list of the models
// the index holds — no thumbnails and no renders, because which model this is
// is a question about its NAME and its rig, and an icon cannot answer it.
void AnimationsPanel::openModelPicker()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Animations from another model"));
    auto* v = new QVBoxLayout(&dlg);
    auto* note = new QLabel(
        QStringLiteral("Pick a model; the list will hold the clips ITS rig can "
                       "play. Nothing is loaded into the viewport."),
        &dlg);
    note->setWordWrap(true);
    v->addWidget(note);
    auto* box = new QLineEdit(&dlg);
    box->setPlaceholderText(QStringLiteral("Filter models…"));
    box->setClearButtonEnabled(true);
    v->addWidget(box);
    auto* list = new QListWidget(&dlg);
    v->addWidget(list, 1);
    const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
    QStringList paths;
    for (const fox::IndexedFile& f : ix.files())
        if (fox::ArchiveIndex::extensionOf(f) == QLatin1String("fmdl"))
            paths << f.path;
    paths.sort(Qt::CaseInsensitive);
    paths.removeDuplicates();
    const auto fill = [&](const QString& needle) {
        list->clear();
        int n = 0;
        for (const QString& p : paths) {
            if (!needle.isEmpty() && !p.contains(needle, Qt::CaseInsensitive))
                continue;
            auto* it = new QListWidgetItem(
                p.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0),
                list);
            it->setToolTip(p);
            it->setData(Qt::UserRole, p);
            // Capped rather than cut off silently: 20,000 rows is a list that
            // takes a second to build and cannot be read anyway.
            if (++n >= 2000) {
                auto* more = new QListWidgetItem(
                    QStringLiteral("… narrow the filter to see the rest"), list);
                more->setFlags(Qt::NoItemFlags);
                break;
            }
        }
    };
    fill(QString());
    QObject::connect(box, &QLineEdit::textChanged, &dlg,
                     [&fill](const QString& t) { fill(t.trimmed()); });
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg,
                     [&dlg] { dlg.accept(); });
    v->addWidget(buttons);

    const QString before =
        m_scopeLabel.isEmpty() ? QStringLiteral("model") : QStringLiteral("other");
    QListWidgetItem* chosen = nullptr;
    if (dlg.exec() == QDialog::Accepted) chosen = list->currentItem();
    if (!chosen || !chosen->data(Qt::UserRole).isValid()
        || !setScopeModel(chosen->data(Qt::UserRole).toString())) {
        // Backing out puts the combo back where it was rather than leaving it
        // sitting on a scope that was never chosen.
        m_syncing = true;
        const int at = m_scope->findData(before);
        m_scope->setCurrentIndex(at >= 0 ? at : 0);
        m_syncing = false;
    }
}

void AnimationsPanel::applyFilter()
{
    resolveScope();
    const bool scoped = scope() != QLatin1String("all");
    const QString needle = m_filter->text().trimmed().toLower();
    const int cat = m_category->currentData().toInt();
    int shown = 0;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = m_tree->topLevelItem(i);
        // The scope hides whole ARCHIVES, not clips: an archive either binds
        // to the model or it does not, and half of one is not a thing.
        const bool inScope =
            !scoped || m_scopeArchives.contains(top->data(0, kArchiveRole).toInt());
        const bool archiveMatches =
            needle.isEmpty() || top->data(0, kHayRole).toString().contains(needle);
        int visibleKids = 0;
        for (int k = 0; k < top->childCount(); ++k) {
            QTreeWidgetItem* row = top->child(k);
            const bool byCat =
                cat < 0 || row->data(0, kCatRole).toInt() == cat;
            // An archive that matches the text shows ALL of its clips: someone
            // who types an archive's name is asking for that archive, and
            // hiding the clips inside it because they do not repeat its name
            // would answer a question nobody asked.
            const bool byText = needle.isEmpty() || archiveMatches
                || row->data(0, kHayRole).toString().contains(needle);
            const bool vis = inScope && byCat && byText;
            row->setHidden(!vis);
            if (vis) ++visibleKids;
        }
        top->setHidden(visibleKids == 0);
        shown += visibleKids;
        // Expanded only while a filter is narrowing things down. Expanding
        // 159 archives by default is a list nobody can see the shape of.
        if (!needle.isEmpty() || cat >= 0)
            top->setExpanded(visibleKids > 0 && visibleKids <= 200);
    }
    QString countText =
        shown == m_totalClips
            ? QStringLiteral("%1 clip(s)").arg(m_totalClips)
            : QStringLiteral("%1 of %2 clip(s)").arg(shown).arg(m_totalClips);
    // WHY the list is short is as much of the answer as the number, and an
    // empty scope has three different causes. The long form goes in the
    // tooltip and, when nothing is left, into the tree.
    QString why;
    if (scoped) {
        if (m_modelPath.isEmpty() && scope() == QLatin1String("model"))
            why = QStringLiteral(
                "No model is loaded, so there is no skeleton to match "
                "against. Load one, or switch this to \"All animations\".");
        else if (m_scopeCeiling <= 0)
            why = QStringLiteral(
                "%1 has no bones any rig drives, so no motion archive in the "
                "install can pose it. That is the true answer for a prop — "
                "switch to \"All animations\" to browse everything.")
                      .arg(m_scopeLabel.isEmpty() ? QStringLiteral("This model")
                                                  : m_scopeLabel);
        else
            why = QStringLiteral(
                "%1 of the install's motion archives can pose %2: its rig "
                "drives %3 of its bones, and these are the archives with "
                "tracks for them. Resolved from the .frig, not from names.")
                      .arg(m_scopeArchives.size())
                      .arg(m_scopeLabel)
                      .arg(m_scopeCeiling);
        if (!m_scopeLabel.isEmpty())
            countText += QStringLiteral(" · %1").arg(m_scopeLabel);
    }
    m_countBase = countText;
    paintCount();
    m_count->setToolTip(why);
    const bool empty = shown == 0 && !why.isEmpty();
    m_empty->setText(why);
    m_pages->setCurrentIndex(empty ? 1 : 0);
    refreshButtons();
}

void AnimationsPanel::paintCount()
{
    if (!m_count) return;
    m_count->setText(m_countBase + m_playNote);
}

// The clip's own name, from the catalogue. Used for the "playing …" note when
// the row itself is not in the tree to be asked.
static QString catalogClipName(int archiveFileIdx, int clipIdx)
{
    const fox::AnimCatalog& cat = fox::AnimCatalog::instance();
    for (const fox::AnimArchive& a : cat.archives()) {
        if (a.fileIdx != archiveFileIdx) continue;
        for (const fox::AnimClip& c : a.clips)
            if (c.index == clipIdx)
                return c.label.isEmpty() ? c.name : c.label;
        break;
    }
    return {};
}

bool AnimationsPanel::showCurrent(int archiveFileIdx, int clipIdx)
{
    m_playArchive = archiveFileIdx;
    m_playClip = clipIdx;
    if (!m_built) {
        // Not built is not a disagreement — there is no list yet to disagree
        // with. The values are kept; the tab syncs again when it opens.
        m_playShown = false;
        return false;
    }
    m_syncing = true;
    QTreeWidgetItem* top = nullptr;
    QTreeWidgetItem* row = nullptr;
    for (int i = 0; i < m_tree->topLevelItemCount() && !row; ++i) {
        QTreeWidgetItem* t = m_tree->topLevelItem(i);
        if (t->data(0, kArchiveRole).toInt() != archiveFileIdx) continue;
        top = t;
        for (int k = 0; k < t->childCount(); ++k) {
            QTreeWidgetItem* r = t->child(k);
            if (r->data(0, kClipRole).toInt() != clipIdx) continue;
            row = r;
            break;
        }
        break;
    }
    // HIDDEN COUNTS AS ABSENT. A row the filter has hidden is one
    // scrollToItem cannot scroll to and the user cannot see, so selecting it
    // would leave the panel claiming a clip that is nowhere on screen.
    const bool shown = row && !row->isHidden() && top && !top->isHidden();
    if (shown) {
        top->setExpanded(true);
        // NoUpdate, not the default: setCurrentItem on an
        // ExtendedSelection view issues a real ClearAndSelect, so
        // following the animation bar wiped a nine-clip export selection
        // the moment the user changed clips — and the export button then
        // wrote one file instead of nine with nothing saying why.
        m_tree->setCurrentItem(row, 0, QItemSelectionModel::NoUpdate);
        m_tree->scrollToItem(row);
    } else {
        // Same NoUpdate for the same reason: clearing the CURRENT row must
        // not clear a selection the user built for an export.
        m_tree->setCurrentItem(nullptr, 0, QItemSelectionModel::NoUpdate);
    }
    m_syncing = false;

    const bool nothingPlaying = archiveFileIdx < 0 || clipIdx < 0;
    if (shown || nothingPlaying) {
        m_playNote.clear();
    } else {
        const QString name = catalogClipName(archiveFileIdx, clipIdx);
        m_playNote =
            QStringLiteral(" · playing %1, which this list is not showing")
                .arg(name.isEmpty() ? QStringLiteral("a clip") : name);
    }
    paintCount();
    // Bounded by not repeating itself: flicking down a clip list is one
    // showCurrent per clip and this would otherwise be a line per keypress.
    // Only the TRANSITIONS are logged, which are the events worth having.
    if (archiveFileIdx != m_loggedArchive || clipIdx != m_loggedClip
        || shown != m_loggedShown) {
        if (nothingPlaying)
            qInfo("anims: nothing is playing — panel selection cleared");
        else if (shown)
            qInfo("anims: panel shows the playing clip (archive %d, clip %d)",
                  archiveFileIdx, clipIdx);
        else
            qInfo("anims: panel CANNOT show the playing clip (archive %d, clip "
                  "%d) — scope '%s', filter '%s', archive row %s, clip row %s",
                  archiveFileIdx, clipIdx, qUtf8Printable(scope()),
                  qUtf8Printable(m_filter ? m_filter->text() : QString()),
                  top ? (top->isHidden() ? "hidden" : "shown") : "absent",
                  row ? (row->isHidden() ? "hidden" : "shown") : "absent");
        m_loggedArchive = archiveFileIdx;
        m_loggedClip = clipIdx;
        m_loggedShown = shown;
    }
    m_playShown = shown;
    refreshButtons();
    return shown;
}

QVector<QPair<int, int>> AnimationsPanel::selectedClips() const
{
    QVector<QPair<int, int>> out;
    QSet<QPair<int, int>> seen;
    const auto add = [&](QTreeWidgetItem* row) {
        const QPair<int, int> key{row->data(0, kArchiveRole).toInt(),
                                  row->data(0, kClipRole).toInt()};
        if (key.second < 0 || seen.contains(key)) return;
        seen.insert(key);
        out.append(key);
    };
    // Tree order, not selection order: an export of nine clips should come out
    // in the order the list showed them, whichever way round they were picked.
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = m_tree->topLevelItem(i);
        if (top->isHidden()) continue;
        const bool wholeArchive = top->isSelected();
        for (int k = 0; k < top->childCount(); ++k) {
            QTreeWidgetItem* row = top->child(k);
            // HIDDEN rows are not in the selection even when their archive is:
            // the filter is what the user can see, and exporting rows a filter
            // is hiding is the one thing a filter promises not to do.
            if (row->isHidden()) continue;
            if (wholeArchive || row->isSelected()) add(row);
        }
    }
    return out;
}

void AnimationsPanel::setFilter(const QString& text)
{
    if (m_filter) m_filter->setText(text);
}

int AnimationsPanel::selectVisible()
{
    if (!m_built) return 0;
    m_syncing = true;   // selecting rows must not load 400 clips one by one
    m_tree->clearSelection();
    int n = 0;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = m_tree->topLevelItem(i);
        if (top->isHidden()) continue;
        for (int k = 0; k < top->childCount(); ++k) {
            QTreeWidgetItem* row = top->child(k);
            if (row->isHidden()) continue;
            row->setSelected(true);
            ++n;
        }
    }
    m_syncing = false;
    refreshButtons();
    return n;
}

QString AnimationsPanel::selectionSummary() const
{
    const int n = selectedClips().size();
    if (n == 0) return QStringLiteral("no clips selected");
    return QStringLiteral("%1 clip%2 selected")
        .arg(n)
        .arg(n == 1 ? QString() : QStringLiteral("s"));
}

void AnimationsPanel::refreshButtons()
{
    const bool any = !selectedClips().isEmpty();
    m_exportOne->setEnabled(any);
    m_exportEach->setEnabled(any);
}
