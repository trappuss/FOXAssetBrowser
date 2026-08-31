#include "view/AttachmentsPanel.h"

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QHeaderView>
#include <QSet>
#include <QMenu>

#include "util/MenuText.h"
#include <QTreeWidget>

#include "util/TableCopy.h"
#include <QVBoxLayout>

#include "fox/FmdlFile.h"
#include "fox/FoxHash.h"
#include "index/ArchiveIndex.h"
#include "util/ExportActions.h"
#include "view/PanelBox.h"

namespace fox {

namespace {

// The role each row plays, so the click handler does not have to re-derive it
// from what columns happen to be filled in.
enum RowKind { RowTable = 1, RowGroup, RowAttach, RowSubs };
constexpr int kKindRole = Qt::UserRole + 1;
constexpr int kHashRole = Qt::UserRole + 2;
constexpr int kFileRole = Qt::UserRole + 3;

// The directory a path lives in, forward-slashed, with no trailing slash.
QString dirOf(const QString& path)
{
    const int cut = path.lastIndexOf(QLatin1Char('/'));
    return cut < 0 ? QString() : path.left(cut);
}

}  // namespace

AttachmentsPanel::AttachmentsPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderHidden(true);
    m_tree->setIndentation(12);
    m_tree->setUniformRowHeights(true);
    m_tree->setTextElideMode(Qt::ElideMiddle);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    v->addWidget(m_tree);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    connect(m_tree, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem* it, int col) {
                if (col != 0 || !it) return;
                if (it->data(0, kKindRole).toInt() != RowGroup) return;
                Q_EMIT meshGroupToggled(it->data(0, kHashRole).toUInt(),
                                        it->checkState(0) == Qt::Checked);
            });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* it, int) {
                if (!it || it->data(0, kKindRole).toInt() != RowAttach) return;
                const int fi = it->data(0, kFileRole).toInt();
                if (fi >= 0) Q_EMIT openModelRequested(fi);
            });
    // §12: every detail table gets Copy / Copy all and Ctrl+C. install()
    // alone, NOT installWithMenu — this tree builds its own menu below, and
    // the helper would install a second one over it. The two entries are
    // appended to that menu instead.
    tablecopy::install(m_tree);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& at) {
                QTreeWidgetItem* it = m_tree->itemAt(at);
                if (!it) return;
                QMenu m(this);
                const int kind = it->data(0, kKindRole).toInt();
                if (kind == RowAttach) {
                    const int fi = it->data(0, kFileRole).toInt();
                    QAction* a = m.addAction(
                        QStringLiteral("Open %1").arg(it->text(0)), this,
                        [this, fi] { Q_EMIT openModelRequested(fi); });
                    a->setEnabled(fi >= 0);
                }
                const QString name = it->text(0);
                m.addAction(MenuText::withCopyValue(MenuText::kCopyName, name), this,
                            [name] {
                                QGuiApplication::clipboard()->setText(name);
                            });
                // The two shared entries, last, so this table can be got out
                // of and not only read.
                if (!m.isEmpty()) m.addSeparator();
                tablecopy::addMenuActions(&m, m_tree);
                m.exec(m_tree->viewport()->mapToGlobal(at));
            });

    clearModel();
}

void AttachmentsPanel::clearModel()
{
    m_tree->clear();
    m_tables = m_attachments = m_groupRows = 0;
    m_summary.clear();
    auto* it = new QTreeWidgetItem(m_tree);
    it->setFirstColumnSpanned(true);
    it->setText(0, QStringLiteral("No model loaded."));
    it->setFlags(Qt::ItemIsEnabled);
    it->setForeground(0, QColor(0x8a, 0x8a, 0x92));
    Q_EMIT summaryChanged();
}

int AttachmentsPanel::modelFileFor(quint64 hash) const
{
    const ArchiveIndex& ix = ArchiveIndex::instance();
    const IndexedFile* f = ix.findByHash(hash);
    if (!f) return -1;
    const auto& files = ix.files();
    for (int i = 0; i < files.size(); ++i)
        if (&files[i] == f) return i;
    return -1;
}

QVector<AttachmentsPanel::Candidate>
AttachmentsPanel::findTables(int fileIdx, const FmdlFile* model) const
{
    QVector<Candidate> out;
    if (!model) return out;
    ArchiveIndex& ix = ArchiveIndex::instance();
    const auto& files = ix.files();
    if (fileIdx < 0 || fileIdx >= files.size()) return out;

    // The hashes THIS model declares. A table belongs to it when it names one.
    QSet<quint32> groups;
    for (const FmdlMeshGroup& g : model->meshGroups())
        if (g.nameHash32) groups.insert(g.nameHash32);
    QSet<quint32> mats;
    for (const FmdlMaterialInstance& mi : model->materials())
        if (mi.nameHash32) mats.insert(mi.nameHash32);

    // ── The neighbourhood ────────────────────────────────────────────────
    // The model's own directory, a sibling "fova" directory — and the game's
    // OWN fova tree, which is a different place entirely.
    //
    // MEASURED: /Assets/mgo/weapon/onw/Scenes/ar00_owep0_def.fmdl has three
    // tables — default/ar00_owep0_def.fv2 and asr/ar00_owep0_def_{cam,clv}.fv2
    // — and this panel reported "No variation tables beside this model."
    // `--fovabind` confirmed the default table shares a material hash with the
    // model, so it passes the hash test below; the DIRECTORY test was what
    // excluded it. MGO does not put a pack beside its model: it keeps one tree
    // at /Assets/<game>/fova/<kind>/, which the sibling-directory rule cannot
    // reach and which holds every table MGO ships.
    //
    // Widening the directories is safe because the hash test is what decides:
    // a table still has to name a mesh group or a material this model declares
    // (or bring an attachment). What the extra directories change is which
    // tables get ASKED.
    const QString home = dirOf(files[fileIdx].path);
    const QString parent = dirOf(home);
    const QString fova = parent.isEmpty() ? QString()
                                          : parent + QStringLiteral("/fova");
    // "/Assets/mgo/fova" from "/Assets/mgo/weapon/onw/Scenes/x.fmdl": the
    // first two path components plus fova. Not a search of the whole index —
    // one subtree, named by the asset root the model itself sits under.
    QString gameFova;
    {
        const QString p = files[fileIdx].path;
        const QStringList parts = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        // parts[0] = "Assets", parts[1] = the game ("mgo", "tpp", "ssd").
        if (parts.size() > 2)
            gameFova = QStringLiteral("/%1/%2/fova").arg(parts[0], parts[1]);
    }

    // The model's VARIANT STEM, for the naming join MGO's own data states: one
    // pack per item, named for the model it dresses. This is a binding the
    // FILES state — the same class of evidence as the hash — and it is what
    // breaks the tie when a material hash is shared by thirty weapons, which
    // --fovabind measured that it is (ar00's table ties with thirty models on
    // hashes alone).
    //
    // The variant stem and not the full stem, and that distinction was
    // measured: gl02_owep0_def.fmdl is dressed by gl02_owep0_def.fv2 AND by
    // gl02_owep0_m01/m02/m03/m68.fv2 — the DLC gold, silver and copper packs.
    // Those share "gl02_owep0" with the model and NOT "gl02_owep0_def", so
    // matching on the full stem found one table of five. It is the same stem
    // function the Variants ▸ menu uses (§12), not a second copy of the same
    // vocabulary — two spellings of "what counts as a variant of this" is
    // exactly the defect this project keeps finding.
    QString stemFull = files[fileIdx].path.section(QLatin1Char('/'), -1);
    const int dotAt = stemFull.indexOf(QLatin1Char('.'));
    if (dotAt > 0) stemFull.truncate(dotAt);
    const QString stemVariant = exportactions::variantStemOf(files[fileIdx].path);

    for (int i = 0; i < files.size(); ++i) {
        const IndexedFile& c = files[i];
        if (!c.path.endsWith(QLatin1String(".fv2"), Qt::CaseInsensitive)) continue;
        const QString d = dirOf(c.path);
        const bool nearby =
            d == home || (!fova.isEmpty() && d == fova)
            || (!gameFova.isEmpty() && (d == gameFova
                                        || d.startsWith(gameFova
                                                        + QLatin1Char('/'))));
        if (!nearby) continue;
        // Inside the game's fova TREE the naming join applies: a table there
        // is this model's when it is named for it. Without this, a table
        // belonging to another weapon that happens to share one material name
        // hash would be listed as this model's — and --fovabind measured that
        // ar00's table ties with thirty other models on hashes alone, so that
        // is not a hypothetical.
        // TWO bindings, unioned, because the game uses both and each misses
        // what the other catches — measured on this tree:
        //   ar00_owep0_def.fmdl  ->  ar00_owep0_def{,_cam,_clv}.fv2
        //        the table carries the model's FULL stem plus a variation
        //        suffix, so the variant-stem test alone found 1 of 3
        //   gl02_owep0_def.fmdl  ->  gl02_owep0_{def,m01,m02,m03,m68}.fv2
        //        the DLC colour packs share only the VARIANT stem, so the
        //        full-stem test alone found 1 of 5
        QString cstemFull = c.path.section(QLatin1Char('/'), -1);
        const int cdot = cstemFull.indexOf(QLatin1Char('.'));
        if (cdot > 0) cstemFull.truncate(cdot);
        const bool namedForThisModel =
            (!stemFull.isEmpty()
             && (cstemFull == stemFull
                 || cstemFull.startsWith(stemFull + QLatin1Char('_'))))
            || (!stemVariant.isEmpty()
                && exportactions::variantStemOf(c.path) == stemVariant);
        if (d != home && (fova.isEmpty() || d != fova) && !namedForThisModel)
            continue;
        const QByteArray data = ix.readFile(c);
        if (data.isEmpty()) continue;
        Candidate cand;
        cand.fileIdx = i;
        cand.path = c.path;
        if (!cand.table.parse(data)) continue;
        int matched = 0;
        for (quint32 h : cand.table.hiddenMeshGroups())
            if (groups.contains(h)) ++matched;
        for (quint32 h : cand.table.shownMeshGroups())
            if (groups.contains(h)) ++matched;
        for (const FovaSubstitution& s : cand.table.substitutions())
            if (mats.contains(s.materialHash32)) ++matched;
        cand.matchedRows = matched;
        // A table with attachments and no matched row is still shown: the
        // attachment IS the association — a hat table addresses the hat, not
        // the head — and dropping it would hide exactly the case this panel
        // exists for. Everything else has to name something this model has.
        if (matched == 0 && cand.table.attachments().isEmpty()) continue;
        out.append(std::move(cand));
    }
    return out;
}

void AttachmentsPanel::setModel(int fileIdx, const FmdlFile* model)
{
    if (!model) { clearModel(); return; }
    m_tree->clear();
    m_tables = m_attachments = m_groupRows = 0;

    QHash<quint32, QString> groupName;
    for (const FmdlMeshGroup& g : model->meshGroups())
        if (g.nameHash32) groupName.insert(g.nameHash32, g.name);

    const QVector<Candidate> cands = findTables(fileIdx, model);
    ArchiveIndex& ix = ArchiveIndex::instance();
    // Always on and bounded (convention 7): one line per model load saying
    // what was matched and from where. The panel reported "no tables" for a
    // model with three of them for as long as it has existed, and there was
    // nothing in any log to notice it by — the count only ever existed as a
    // widget nobody was looking at.
    {
        QStringList names;
        for (const Candidate& c : cands)
            names << c.path.section(QLatin1Char('/'), -1);
        names.sort();
        qInfo("fova: %s — %lld table(s)%s%s",
              qUtf8Printable(ArchiveIndex::instance()
                                 .files()[fileIdx]
                                 .path.section(QLatin1Char('/'), -1)),
              static_cast<long long>(cands.size()),
              names.isEmpty() ? "" : ": ",
              qUtf8Printable(names.mid(0, 8).join(QLatin1String(", "))));
    }

    // The signal is per row and this fills many rows at once; without the
    // block every checkbox we CREATE would report itself as a user toggle and
    // the viewport would be reshaped by simply opening the panel.
    const bool wasBlocked = m_tree->blockSignals(true);

    for (const Candidate& c : cands) {
        auto* top = new QTreeWidgetItem(m_tree);
        top->setText(0, c.path.section(QLatin1Char('/'), -1));
        top->setToolTip(0, c.path + QStringLiteral("\n\n") + c.table.describe());
        top->setData(0, kKindRole, RowTable);
        top->setData(0, kFileRole, c.fileIdx);
        top->setExpanded(true);
        ++m_tables;

        for (const FovaAttachment& a : c.table.attachments()) {
            const auto& fl = c.table.files();
            if (a.modelIndex < 0 || a.modelIndex >= fl.size()) continue;
            const quint64 h = fl[a.modelIndex];
            const int fi = modelFileFor(h);
            QString name;
            const IndexedFile* got = ix.findByHash(h);
            if (got) name = got->path.section(QLatin1Char('/'), -1);
            if (name.isEmpty())
                name = QStringLiteral("0x%1").arg(h, 16, 16, QLatin1Char('0'));
            auto* it = new QTreeWidgetItem(top);
            it->setText(0, name);
            it->setData(0, kKindRole, RowAttach);
            it->setData(0, kFileRole, fi);
            QString where = a.byConnectPoint
                ? QStringLiteral("connect point 0x%1")
                      .arg(a.connectPointHash32, 8, 16, QLatin1Char('0'))
                : QStringLiteral("host bones");
            it->setText(1, where);
            it->setToolTip(0, QStringLiteral(
                "An extra model this table brings with it, on %1.\n\n"
                "Double-click to open it in this tab. It is not drawn on top "
                "of the current model here: it carries its own skeleton, and "
                "posing one model's mesh on another's rig is a retarget — the "
                "Customize tab is where a whole appearance is assembled.")
                .arg(where));
            if (fi < 0)
                it->setForeground(0, QColor(0xc0, 0x7a, 0x5a));   // not installed
            ++m_attachments;
        }

        auto addGroups = [&](const QVector<quint32>& hashes, bool shown) {
            for (quint32 h : hashes) {
                auto nameIt = groupName.constFind(h);
                if (nameIt == groupName.constEnd()) continue;
                auto* it = new QTreeWidgetItem(top);
                it->setText(0, *nameIt);
                it->setText(1, shown ? QStringLiteral("shows")
                                     : QStringLiteral("hides"));
                it->setData(0, kKindRole, RowGroup);
                it->setData(0, kHashRole, h);
                it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
                // Ticked = the group is on screen. A HIDE row therefore starts
                // ticked (the table is not applied yet) and unticking it is
                // what applying the table would do.
                it->setCheckState(0, Qt::Checked);
                it->setToolTip(0, QStringLiteral(
                    "A mesh group of this model that the table %1.\n\nThe box "
                    "switches the group in the viewport, so this is the hat "
                    "on and off without leaving the tab.")
                    .arg(shown ? QStringLiteral("turns on")
                               : QStringLiteral("turns off")));
                ++m_groupRows;
            }
        };
        addGroups(c.table.hiddenMeshGroups(), false);
        addGroups(c.table.shownMeshGroups(), true);

        const int subs = c.table.substitutions().size();
        if (subs > 0) {
            auto* it = new QTreeWidgetItem(top);
            it->setFirstColumnSpanned(true);
            it->setText(0, QStringLiteral("%1 texture substitution%2")
                               .arg(subs).arg(subs == 1 ? QString()
                                                        : QStringLiteral("s")));
            it->setData(0, kKindRole, RowSubs);
            it->setFlags(Qt::ItemIsEnabled);
            it->setForeground(0, QColor(0x8a, 0x8a, 0x92));
            it->setToolTip(0, QStringLiteral(
                "This table also repaints materials. Applying a whole "
                "appearance — geometry and textures together — is the "
                "Customize tab; this panel is what the model itself carries."));
        }
    }

    m_tree->blockSignals(wasBlocked);

    if (m_tables == 0) {
        auto* it = new QTreeWidgetItem(m_tree);
        it->setFirstColumnSpanned(true);
        it->setText(0, QStringLiteral("No variation tables beside this model."));
        it->setFlags(Qt::ItemIsEnabled);
        it->setForeground(0, QColor(0x8a, 0x8a, 0x92));
        it->setToolTip(0, QStringLiteral(
            "A .fv2 FOVA table is matched to this model by HASH — its rows "
            "have to name a mesh group or material instance the model itself "
            "declares — and only tables in the model's own directory or the "
            "fova/ folder beside it are read. Nothing here means nothing in "
            "that neighbourhood addresses this model."));
    }

    QStringList bits;
    bits << QStringLiteral("%1 table%2").arg(m_tables)
                .arg(m_tables == 1 ? QString() : QStringLiteral("s"));
    if (m_attachments > 0)
        bits << QStringLiteral("%1 attached").arg(m_attachments);
    if (m_groupRows > 0)
        bits << QStringLiteral("%1 group%2").arg(m_groupRows)
                    .arg(m_groupRows == 1 ? QString() : QStringLiteral("s"));
    m_summary = m_tables == 0 ? QString() : bits.join(QStringLiteral(" · "));
    setProperty(kPanelWantH,
                (m_tables + m_attachments + m_groupRows) * 18 + 24);
    Q_EMIT summaryChanged();
}

}  // namespace fox
