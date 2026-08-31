#include "view/FileInfoPanel.h"

#include <QBrush>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QTreeWidget>

#include "util/TableCopy.h"
#include <QVBoxLayout>

#include "fox/FmdlFile.h"
#include "index/ArchiveIndex.h"
#include "index/TextureUsers.h"

namespace fox {

namespace {

QString humanSize(quint64 bytes)
{
    return QLocale().formattedDataSize(qint64(bytes));
}

// One "label: value" row, or nothing at all when the value is empty.
//
// Nothing at all, deliberately. A field with no answer used to be drawn as
// "Archive: —" and a panel of those reads as a broken parse rather than as a
// file that simply has no archive because it is a loose mount. The rows that
// ARE there are the facts this install actually has.
void addRow(QString& out, const QString& label, const QString& value)
{
    if (value.isEmpty()) return;
    out += QStringLiteral("<tr><td style='padding-right:10px;color:palette(mid)'>%1</td>"
                          "<td>%2</td></tr>")
               .arg(label.toHtmlEscaped(), value.toHtmlEscaped());
}


// A texture reference's file index, or -1.
//
// findByHash and NOT fileIndexForPath(t.path): a FmdlTextureRef's `path` is
// the resolved NAME and does not carry the extension the index keys on, so the
// path lookup returned -1 for every row whose name HAD resolved — the panel
// showed a real texture name and refused to open it. The hash is what
// ModelLoader itself hands to findByHash to fetch the pixels, so it is the
// lookup that is known to work on this data.
static int fileIndexForTexture(const fox::ArchiveIndex& ix, quint64 pathHash)
{
    const fox::IndexedFile* f = ix.findByHash(pathHash);
    if (!f) return -1;
    const auto& files = ix.files();
    const qsizetype i = f - files.constData();
    return (i >= 0 && i < files.size()) ? int(i) : -1;
}

}  // namespace

FileInfoPanel::FileInfoPanel(QWidget* parent) : QWidget(parent)
{
    // ── FILE INFO ────────────────────────────────────────────────────────
    m_infoSection = new QWidget(this);
    auto* infoLay = new QVBoxLayout(m_infoSection);
    infoLay->setContentsMargins(6, 4, 6, 6);
    m_info = new QLabel(m_infoSection);
    m_info->setTextFormat(Qt::RichText);
    m_info->setWordWrap(true);
    // Selectable: the hash and the path are the two things anyone reading this
    // panel wants to paste somewhere else, and a label you cannot select is a
    // fact you have to retype by hand off the screen.
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_info->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    infoLay->addWidget(m_info);
    infoLay->addStretch(1);

    // ── ASSOCIATED ───────────────────────────────────────────────────────
    m_assocSection = new QWidget(this);
    auto* assocLay = new QVBoxLayout(m_assocSection);
    assocLay->setContentsMargins(6, 4, 6, 6);
    m_assocNote = new QLabel(m_assocSection);
    m_assocNote->setWordWrap(true);
    m_assocNote->setForegroundRole(QPalette::Mid);
    assocLay->addWidget(m_assocNote);
    m_assoc = new QTreeWidget(m_assocSection);
    m_assoc->setColumnCount(2);
    m_assoc->setHeaderLabels({QStringLiteral("Asset"), QStringLiteral("Role")});
    m_assoc->setRootIsDecorated(false);
    m_assoc->header()->setStretchLastSection(false);
    // The ASSET column gets the room and the ROLE column takes what it needs
    // from what is left, not the other way round. Sized from the header rather
    // than from the content: "SpecularMap_Tex_LIN" is longer than the column
    // is wide, and letting the role size to its contents left the asset column
    // showing "0x1569e…" — every row identical and none of them readable. The
    // screenshot is how that was found; the count in the header said 61 and
    // looked fine.
    m_assoc->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_assoc->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_assoc->header()->resizeSection(1, 110);
    // Elide from the RIGHT, which is Qt's default and is right for both
    // columns here — and eliding from the left was tried first and is wrong
    // for both, which only the screenshot showed. A role reads
    // "SpecularMap_Tex_LIN" and its PREFIX is what distinguishes it, so
    // "…ap_Tex_LIN" is unreadable while "SpecularMap_Te…" is not; and the
    // assets are file names and hashes whose leading characters differ, so the
    // same holds. The full text is on every row as a tooltip regardless.
    assocLay->addWidget(m_assoc, 1);
    // §12: this list had no way to get its contents out at all.
    tablecopy::installWithMenu(m_assoc);

    connect(m_assoc, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* it, int) {
                // -1 and 0 mean different things and an unset property reads
                // as 0 (convention 12), so this tests VALIDITY rather than
                // comparing against a sentinel — otherwise a row with no
                // target would navigate to file 0.
                const QVariant v = it->data(0, Qt::UserRole);
                if (v.isValid()) Q_EMIT assetActivated(v.toInt());
            });

    showFile(-1);
}

void FileInfoPanel::showFile(int fileIdx)
{
    m_fileIdx = fileIdx;
    rebuildInfo();
    rebuildAssociated();
}

void FileInfoPanel::rebuildInfo()
{
    const auto& files = ArchiveIndex::instance().files();
    if (m_fileIdx < 0 || m_fileIdx >= files.size()) {
        m_info->setText(QStringLiteral("<i>No file selected.</i>"));
        return;
    }
    const IndexedFile& f = files[m_fileIdx];
    const ArchiveIndex& index = ArchiveIndex::instance();

    QString name = f.path.section(QLatin1Char('/'), -1);
    if (name.isEmpty())
        name = QStringLiteral("0x%1").arg(f.hash, 16, 16, QLatin1Char('0'));

    QString rows;
    addRow(rows, QStringLiteral("Name"), name);
    addRow(rows, QStringLiteral("Folder"), f.path.section(QLatin1Char('/'), 0, -2));
    addRow(rows, QStringLiteral("Type"), ArchiveIndex::extensionOf(f));
    addRow(rows, QStringLiteral("Size"), humanSize(f.size));
    // Both schemes are in play in one install, so the label says which one
    // this hash is — a GZ hash pasted into a TPP lookup silently finds nothing.
    addRow(rows, f.gz ? QStringLiteral("Hash (GZ)") : QStringLiteral("Hash"),
           QStringLiteral("0x%1").arg(f.hash, 16, 16, QLatin1Char('0')));

    const auto& archives = index.archives();
    if (f.archiveId >= 0 && f.archiveId < archives.size()) {
        const IndexedArchive& a = archives[f.archiveId];
        addRow(rows, QStringLiteral("Archive"), a.shortName);
        addRow(rows, QStringLiteral("Game"),
               QString::fromLatin1(gameLongName(index.gameOf(f))));
    }
    if (f.childIdx >= 0)
        addRow(rows, QStringLiteral("Inside"),
               QStringLiteral("a container (entry %1)").arg(f.childIdx));
    if (!f.named)
        addRow(rows, QStringLiteral("Name"),
               QStringLiteral("not in the dictionary — shown by hash"));

    // The one that is worth a sentence rather than a word. A shadowed copy is
    // not a duplicate to be ignored: it is the copy the game would NOT load,
    // which is exactly what someone chasing "why does my mod not show up" is
    // looking at.
    QString warn;
    if (f.shadowed)
        warn = QStringLiteral(
            "<p style='color:#c07000'>Another archive with a higher mount "
            "priority carries this same hash, so <b>this is not the copy the "
            "game loads</b>. Mod installs do this deliberately.</p>");

    m_info->setText(QStringLiteral("<table>%1</table>%2").arg(rows, warn));
}

void FileInfoPanel::rebuildAssociated()
{
    m_assoc->clear();
    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();
    if (m_fileIdx < 0 || m_fileIdx >= files.size()) {
        m_assocNote->setText(QString());
        Q_EMIT associatedTitleChanged(QStringLiteral("ASSOCIATED"));
        return;
    }
    const IndexedFile& f = files[m_fileIdx];
    const QString ext = ArchiveIndex::extensionOf(f);

    const auto add = [this](const QString& asset, const QString& role,
                            int target) {
        auto* it = new QTreeWidgetItem(m_assoc);
        it->setText(0, asset);
        it->setText(1, role);
        // The column is narrow by design and both of these elide. The full
        // text on the row costs nothing and is the difference between a panel
        // you can read and one you can only look at.
        it->setToolTip(0, asset);
        it->setToolTip(1, role);
        if (target >= 0) it->setData(0, Qt::UserRole, target);
        else it->setForeground(0, QBrush(Qt::gray));
    };

    int n = 0;
    if (ext == QLatin1String("fmdl")) {
        FmdlFile m;
        if (m.parse(index.readFile(f))) {
            // ONE ROW PER TEXTURE, not one per material that mentions it.
            // A character head has twelve materials over fifteen meshes and
            // they share their maps, so the raw walk produced sixty-one rows
            // of which about seven were distinct — the same four names
            // repeating down the panel with nothing to tell them apart. The
            // roles are collected onto the row instead, which is the fact
            // worth keeping ("this texture is the base map AND the translucent
            // map").
            QVector<QPair<quint64, QString>> order;   // (hash, display)
            QHash<quint64, QStringList> roles;
            for (const FmdlMaterialInstance& mat : m.materials())
                for (const FmdlTextureRef& t : mat.textures) {
                    if (!roles.contains(t.pathHash)) {
                        // An unresolved reference is REPORTED, not skipped.
                        // "This model wants a texture this install cannot
                        // name" is the answer someone is most often here for.
                        order.append(
                            {t.pathHash,
                             t.path.isEmpty()
                                 ? QStringLiteral("0x%1")
                                       .arg(t.pathHash, 16, 16, QLatin1Char('0'))
                                 : t.path.section(QLatin1Char('/'), -1)});
                        roles.insert(t.pathHash, {});
                    }
                    QStringList& r = roles[t.pathHash];
                    if (!r.contains(t.role)) r.append(t.role);
                }
            for (const auto& e : order) {
                QStringList r = roles.value(e.first);
                r.sort();   // a QHash-ordered role list would differ per run
                add(e.second, r.join(QLatin1String(", ")),
                    fileIndexForTexture(index, e.first));
                ++n;
            }
            m_assocNote->setText(
                n ? QStringLiteral("Textures this model asks for. Grey rows are "
                                     "names this install cannot resolve.")
                  : QStringLiteral("This model declares no textures."));
        } else {
            m_assocNote->setText(QStringLiteral("This model would not parse."));
        }
    } else if (ext == QLatin1String("ftex")) {
        const TextureUsers& users = TextureUsers::instance();
        if (!users.ready()) {
            // Distinct from "nothing uses it". An unfinished sweep answering
            // "no users" is the confidently wrong answer, and a panel is
            // exactly where it would be believed — the cold sweep on a full
            // install runs for minutes.
            m_assocNote->setText(QStringLiteral(
                "The texture→model sweep has not finished yet, so this is not "
                "an answer either way."));
        } else {
            for (const TextureUse& u : users.usesOf(f.hash)) {
                add(u.modelPath.isEmpty()
                        ? QStringLiteral("0x%1").arg(u.modelHash, 16, 16,
                                                     QLatin1Char('0'))
                        : u.modelPath.section(QLatin1Char('/'), -1),
                    u.role,
                    u.modelPath.isEmpty()
                        ? -1
                        : index.fileIndexForPath(u.modelPath));
                ++n;
            }
            m_assocNote->setText(
                n ? QStringLiteral("Models this texture is on.")
                  : QStringLiteral("Nothing in this install references it."));
        }
    } else {
        m_assocNote->setText(QStringLiteral(
            "Nothing is indexed for this kind of file yet — the model→material"
            "→texture map covers .fmdl and .ftex."));
    }

    Q_EMIT associatedTitleChanged(
        n ? QStringLiteral("ASSOCIATED (%1)").arg(n)
          : QStringLiteral("ASSOCIATED"));
}

}  // namespace fox
