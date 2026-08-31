// TextureInfoPanel.cpp — see TextureInfoPanel.h.
#include "view/TextureInfoPanel.h"

#include "util/TableCopy.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QBrush>
#include <QMenu>

#include "util/MenuText.h"
#include <QTreeWidget>
#include <QVBoxLayout>

#include "fox/FtexFile.h"
#include "index/ArchiveIndex.h"
#include "fox/FoxHash.h"
#include "index/ModelTags.h"
#include "index/TextureUsers.h"

namespace fox {

namespace {

// The model's full path, kept off the display role so the column can elide the
// name without the jump-to-Models handler losing the thing it needs.
constexpr int kPathRole = Qt::UserRole + 1;

// Is "<stem>.<n>.ftexs" mounted anywhere? Asked by hash, which is how the
// assembler finds the same file — so this says exactly what the decode will
// find rather than guessing from the folder the .ftex happens to sit in.
bool haveStream(const QString& ftexPath, int n)
{
    QString p = ftexPath;
    if (p.endsWith(QLatin1String(".ftex"), Qt::CaseInsensitive)) p.chop(5);
    const QString want = QStringLiteral("%1.%2.ftexs").arg(p).arg(n);
    return ArchiveIndex::instance().findByHash(
               hashFileNameWithExtension(want)) != nullptr;
}

QString sizeText(qint64 bytes)
{
    const QLocale loc;
    if (bytes < 1024) return QStringLiteral("%1 bytes").arg(bytes);
    const double kib = bytes / 1024.0;
    if (kib < 1024)
        return QStringLiteral("%1 KiB (%2 bytes)")
            .arg(kib, 0, 'f', 2)
            .arg(loc.toString(bytes));
    return QStringLiteral("%1 MiB (%2 bytes)")
        .arg(kib / 1024.0, 0, 'f', 2)
        .arg(loc.toString(bytes));
}

QString formatName(int pixelFormatType)
{
    switch (pixelFormatType) {
    case 0: return QStringLiteral("A8R8G8B8");
    case 1: return QStringLiteral("L8");
    case 2: return QStringLiteral("DXT1 (BC1)");
    case 4: return QStringLiteral("DXT5 (BC3)");
    default: return QStringLiteral("unknown (%1)").arg(pixelFormatType);
    }
}

// A section heading that reads like the rest of the application's panels.
QLabel* heading(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

}  // namespace

// THE THREE SECTIONS ARE SEPARATE WIDGETS NOW (template §6). They were three
// headings and three widgets stacked inside one column; §6 asks for panels —
// each with its own header, its own ▲▼✕, its own size, and a switch on the
// icon strip — and a heading in a QVBoxLayout is none of those.
//
// This class stays: it owns the DATA (showTexture, the users sweep, the mip
// walk) and hands out three ready-made widgets for the column to place. It is
// itself never shown; the sections are parented to it so that lifetime is
// still one object's problem, and NPanel reparents them as it adds them.
TextureInfoPanel::TextureInfoPanel(QWidget* parent) : QWidget(parent)
{
    hide();   // a controller, not a visible panel

    m_infoSection = new QWidget(this);
    auto* v = new QVBoxLayout(m_infoSection);
    v->setContentsMargins(6, 4, 6, 4);
    v->setSpacing(4);

    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_info->setText(QStringLiteral("Select a texture."));
    v->addWidget(m_info);

    // The section HEADINGS are gone: PanelBox draws the title, and a bold
    // label inside a panel whose header already says MIP LEVELS is the title
    // twice. m_mipsHead/m_usersHead went with them.
    m_mipsSection = new QWidget(this);
    auto* mv = new QVBoxLayout(m_mipsSection);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(0);
    m_mips = new QTreeWidget(this);
    m_mips->setRootIsDecorated(false);
    m_mips->setUniformRowHeights(true);
    m_mips->setAlternatingRowColors(true);
    // "In" rather than "Lives in", and the ftexs NUMBER rather than the whole
    // file name: the panel is a narrow column, and a stretch column between two
    // fitted ones got about four characters — enough to render every row as
    // "a…". The full name is on the row's tooltip, where a long string belongs.
    m_mips->setHeaderLabels({QStringLiteral("Mip"), QStringLiteral("Size"),
                             QStringLiteral("In"), QStringLiteral("Bytes")});
    m_mips->header()->setStretchLastSection(true);
    m_mips->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_mips->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_mips->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_mips->setContextMenuPolicy(Qt::CustomContextMenu);
    m_mips->setToolTip(QStringLiteral(
        "A Fox texture is a mip chain, and the big mips do not live in the "
        ".ftex — they are in <name>.N.ftexs files beside it. This says which "
        "mip is in which, so a texture that decodes small on a partial install "
        "explains itself instead of just looking wrong."));
    connect(m_mips, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                QTreeWidgetItem* it = m_mips->itemAt(pos);
                if (!it || m_fileIdx < 0) return;
                QMenu menu(this);
                const int mip = it->data(0, Qt::UserRole).toInt();
                QAction* a = menu.addAction(
                    QStringLiteral("Export mip %1 as .png…").arg(mip));
                menu.addSeparator();
                tablecopy::addMenuActions(&menu, m_mips);
                if (menu.exec(m_mips->viewport()->mapToGlobal(pos)) == a)
                    Q_EMIT exportMipRequested(m_fileIdx, mip);
            });
    tablecopy::install(m_mips);
    mv->addWidget(m_mips, 1);
    m_mipsSection->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    m_usersSection = new QWidget(this);
    auto* uv = new QVBoxLayout(m_usersSection);
    uv->setContentsMargins(0, 0, 0, 0);
    uv->setSpacing(2);
    m_users = new QTreeWidget(this);
    m_users->setRootIsDecorated(true);
    m_users->setUniformRowHeights(true);
    m_users->setAlternatingRowColors(true);
    // ONE column. A Fox identity is a 16-digit hash, and a column wide enough
    // to hold one leaves about twelve characters for the name it identifies —
    // so the hash is on the tooltip and in the right-click menu, where a long
    // string that is copied rather than read belongs.
    m_users->setHeaderLabels({QStringLiteral("Asset / role")});
    m_users->header()->setStretchLastSection(true);
    m_users->setToolTip(QStringLiteral(
        "Every model whose materials name this texture, and the slot each one "
        "puts it in. Double-click a model to open it in the Models tab."));
    m_usersNote = new QLabel(this);
    m_usersNote->setWordWrap(true);
    // TOP-aligned. A QLabel's default is AlignVCenter, and when the tree below
    // it is hidden — which is exactly when this note is shown — the label is
    // handed the whole panel and centres its text in it, so the explanation
    // for an empty panel floated in the middle of one with a hundred pixels of
    // nothing above it.
    m_usersNote->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_usersNote->hide();
    connect(m_users, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* it, int) {
                if (!it) return;
                // Only the MODEL rows carry a hash; a role row activates its
                // parent, which is what double-clicking a child means to
                // anyone who is not thinking about the tree structure.
                QTreeWidgetItem* model = it->parent() ? it->parent() : it;
                const quint64 h = model->data(0, Qt::UserRole).toULongLong();
                // The PATH from the item's data, never its display text: the
                // text is what fits in the column and is a display decision.
                if (h) Q_EMIT modelActivated(h, model->data(0, kPathRole).toString());
            });
    // The same actions the rest of the application offers on an asset, from
    // the same shape of menu (§12).
    m_users->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_users, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                QTreeWidgetItem* it = m_users->itemAt(pos);
                if (!it) return;
                QTreeWidgetItem* model = it->parent() ? it->parent() : it;
                const quint64 h = model->data(0, Qt::UserRole).toULongLong();
                const QString path = model->data(0, kPathRole).toString();
                QMenu menu(this);
                QAction* open = menu.addAction(QStringLiteral("Open in Models"));
                open->setEnabled(!path.isEmpty());
                QAction* copyPath = menu.addAction(MenuText::kCopyPath);
                copyPath->setEnabled(!path.isEmpty());
                QAction* copyHash = menu.addAction(MenuText::kCopyHash);
                menu.addSeparator();
                tablecopy::addMenuActions(&menu, m_users);
                QAction* chosen = menu.exec(m_users->viewport()->mapToGlobal(pos));
                if (chosen == open && h) Q_EMIT modelActivated(h, path);
                else if (chosen == copyPath)
                    QGuiApplication::clipboard()->setText(path);
                else if (chosen == copyHash)
                    QGuiApplication::clipboard()->setText(
                        QStringLiteral("%1").arg(h, 16, 16, QLatin1Char('0')));
            });
    uv->addWidget(m_usersNote);
    tablecopy::install(m_users);
    uv->addWidget(m_users, 1);
    m_usersSection->setSizePolicy(QSizePolicy::Preferred,
                                  QSizePolicy::Expanding);

    // The sweep runs in the background; when it lands, whatever is on screen
    // fills in without the user having to reselect it.
    connect(&TextureUsers::instance(), &TextureUsers::finished, this,
            [this](bool) { refreshUsers(); });
    connect(&TextureUsers::instance(), &TextureUsers::progress, this,
            [this](int, int) {
                if (!TextureUsers::instance().ready()) refreshUsers();
            });

    showTexture(-1);
}

void TextureInfoPanel::showTexture(int fileIdx)
{
    m_fileIdx = fileIdx;
    m_mips->clear();
    const ArchiveIndex& index = ArchiveIndex::instance();
    if (fileIdx < 0 || fileIdx >= index.files().size()) {
        m_info->setText(QStringLiteral("Select a texture."));
        // The HEADINGS too. They carry counts, and a count left over from the
        // previous selection ("MIP LEVELS (10 — 2 not mounted)") sitting above
        // an empty tree is a statement about nothing.
        Q_EMIT mipsTitleChanged(QStringLiteral("MIP LEVELS"));
        Q_EMIT usersTitleChanged(QStringLiteral("ASSOCIATED MODELS"));
        setUsersMessage(QString());
        return;
    }
    const IndexedFile& f = index.files()[fileIdx];

    // The .ftex ENTRY, which is the 64-byte header plus its mip table and
    // nothing else — the pixel data for the big mips is in the .ftexs files
    // beside it. So selecting a texture costs one small read, never the full
    // assembly of its streams; the preview does that separately.
    FtexFile ftex;
    const QByteArray head = index.readFile(f);
    const bool parsed = ftex.parse(head);

    const QString name = f.named ? f.path.section(QLatin1Char('/'), -1)
                                 : QStringLiteral("%1.ftex")
                                       .arg(f.hash, 16, 16, QLatin1Char('0'));
    QStringList lines;
    lines << QStringLiteral("<b>%1</b>").arg(name.toHtmlEscaped());
    if (f.named) lines << f.path.section(QLatin1Char('/'), 0, -2).toHtmlEscaped();
    lines << QStringLiteral("Hash: %1").arg(f.hash, 16, 16, QLatin1Char('0'));
    // NAMED, because "224 bytes" beside "2048 × 2048" reads as a contradiction
    // until you know that the big mips are in the .ftexs files next to it.
    lines << QStringLiteral(".ftex entry: %1").arg(sizeText(f.size));
    if (parsed) {
        lines << QStringLiteral("Format: %1").arg(formatName(ftex.pixelFormatType()));
        lines << QStringLiteral("Size: %1 × %2")
                     .arg(ftex.width())
                     .arg(ftex.height());
        lines << QStringLiteral("Mips: %1  ·  stream files: %2")
                     .arg(ftex.mipCount())
                     .arg(ftex.ftexsFileCount());
        if (ftex.depth() > 1)
            lines << QStringLiteral("Faces / depth: %1").arg(ftex.depth());
    } else {
        lines << QStringLiteral("<i>Header unreadable: %1</i>")
                     .arg(ftex.errorString().toHtmlEscaped());
    }
    // NO "Tags:" line. ModelTags is the MODEL vocabulary and its tagsOf()
    // documents itself as empty for anything that is not a model, so a texture
    // would have shown an empty line for ever. What a texture is FOR is the
    // ASSOCIATED MODELS section below, which is the honest answer to the same
    // question.
    if (f.shadowed)
        lines << QStringLiteral("<i>Shadowed — another archive carries this "
                                "hash at a higher mount priority, so the game "
                                "loads that copy, not this one.</i>");
    m_info->setText(lines.join(QStringLiteral("<br>")));

    // ── Mip levels ──────────────────────────────────────────────────────
    int missing = 0;
    if (parsed) {
        for (const FtexMipInfo& m : ftex.mipInfos()) {
            // The dimensions come from the mip's own INDEX, never from its
            // position in the table. mipInfos() is in FILE order — grouped by
            // ftexs file number, small inline tail first — which is exactly why
            // the assembler re-keys it into a QMap<index,…> before writing the
            // DDS. Walking it with a halving counter printed "Mip 6 —
            // 2048×2048" on every texture that has a stream file at all.
            const int w = qMax(1, int(ftex.width()) >> m.index);
            const int h = qMax(1, int(ftex.height()) >> m.index);
            auto* row = new QTreeWidgetItem(m_mips);
            row->setText(0, QString::number(m.index));
            row->setText(1, QStringLiteral("%1×%2").arg(w).arg(h));
            const QString stem = name.section(QLatin1Char('.'), 0, 0);
            const QString where =
                m.ftexsFileNumber == 0
                    ? QStringLiteral(".ftex")
                    : QStringLiteral(".%1").arg(m.ftexsFileNumber);
            row->setText(2, where);
            row->setToolTip(2, m.ftexsFileNumber == 0
                                   ? QStringLiteral("Inline, in %1.ftex").arg(stem)
                                   : QStringLiteral("Streamed from %1.%2.ftexs")
                                         .arg(stem)
                                         .arg(m.ftexsFileNumber));
            row->setText(3, QStringLiteral("%1 → %2")
                                .arg(m.size)
                                .arg(m.decompressedSize));
            row->setData(0, Qt::UserRole, int(m.index));
            // A mip whose stream file is NOT MOUNTED is the thing this table
            // exists to make visible, so it is marked rather than left to be
            // inferred from a blurry preview. Checked by hash, the same way
            // the assembler finds the siblings it needs.
            if (m.ftexsFileNumber > 0 && f.named && !haveStream(f.path, m.ftexsFileNumber)) {
                row->setText(2, row->text(2) + QStringLiteral("  (missing)"));
                for (int c = 0; c < 4; ++c)
                    row->setForeground(c, QBrush(QColor(200, 120, 110)));
                row->setToolTip(2, QStringLiteral(
                    "%1.%2.ftexs is not in any mounted archive, so this mip "
                    "cannot be decoded and the preview is showing a smaller "
                    "one.").arg(stem).arg(m.ftexsFileNumber));
                ++missing;
            }
        }
    }
    Q_EMIT mipsTitleChanged(
        missing == 0
            ? QStringLiteral("MIP LEVELS (%1)").arg(m_mips->topLevelItemCount())
            : QStringLiteral("MIP LEVELS (%1 — %2 not mounted)")
                  .arg(m_mips->topLevelItemCount())
                  .arg(missing));

    refreshUsers();
}

// A message goes in a WRAPPING LABEL, not in a tree row. A row cannot wrap and
// cannot be read at this column width — "No model in this install us…" is a
// sentence that has been turned into a defect.
void TextureInfoPanel::setUsersMessage(const QString& text)
{
    m_users->clear();
    m_usersNote->setText(text);
    m_usersNote->setVisible(!text.isEmpty());
    m_users->setVisible(text.isEmpty());
}

void TextureInfoPanel::refreshUsers()
{
    TextureUsers& tu = TextureUsers::instance();
    const ArchiveIndex& index = ArchiveIndex::instance();
    if (m_fileIdx < 0 || m_fileIdx >= index.files().size()) {
        Q_EMIT usersTitleChanged(QStringLiteral("ASSOCIATED MODELS"));
        setUsersMessage(QString());
        return;
    }
    if (!tu.ready()) {
        Q_EMIT usersTitleChanged(QStringLiteral("ASSOCIATED MODELS"));
        const int total = tu.total();
        setUsersMessage(
            total > 0
                ? QStringLiteral("Reading the models… %1 of %2")
                      .arg(tu.done())
                      .arg(total)
                : QStringLiteral("Reading the models…"));
        return;
    }

    const IndexedFile& f = index.files()[m_fileIdx];
    const QVector<TextureUse> uses = tu.usesOf(f.hash);
    setUsersMessage(QString());
    m_users->clear();
    if (uses.isEmpty()) {
        Q_EMIT usersTitleChanged(QStringLiteral("ASSOCIATED MODELS (0)"));
        // An ORPHAN, and worth saying so plainly: it is a real category — UI
        // art, an effect map, or something the install does not ship the model
        // for — and "nothing here" reads as a failure rather than an answer.
        // The CAVEAT, when there is one. Ground Zeroes models store their
        // textures by name and leave the path hash at zero, so they parse fine
        // and tell this map nothing — and "no model uses it" would then be a
        // confident wrong answer for every GZ texture in the install.
        const int opaque = tu.opaqueModelCount();
        setUsersMessage(
            opaque == 0
                ? QStringLiteral("No model in this install uses it "
                                 "(%1 model(s) read).")
                      .arg(tu.modelCount())
                : QStringLiteral("No model in this install uses it — but %1 of "
                                 "the %2 models read name their textures by "
                                 "text rather than by hash (Ground Zeroes "
                                 "does this), so they could not be searched.")
                      .arg(opaque)
                      .arg(tu.modelCount()));
        return;
    }

    // Group by model, so one model with six slots is one row with six children
    // rather than six top-level rows saying the same name.
    QHash<quint64, QTreeWidgetItem*> byModel;
    int models = 0;
    for (const TextureUse& u : uses) {
        QTreeWidgetItem* parent = byModel.value(u.modelHash);
        if (!parent) {
            parent = new QTreeWidgetItem(m_users);
            parent->setText(0, u.modelPath.isEmpty()
                                   ? QStringLiteral("%1.fmdl").arg(
                                         u.modelHash, 16, 16, QLatin1Char('0'))
                                   : u.modelPath.section(QLatin1Char('/'), -1));
            parent->setToolTip(
                0, QStringLiteral("%1\nhash %2")
                       .arg(u.modelPath.isEmpty() ? QStringLiteral("(unnamed)")
                                                  : u.modelPath)
                       .arg(u.modelHash, 16, 16, QLatin1Char('0')));
            parent->setData(0, Qt::UserRole, QVariant::fromValue(u.modelHash));
            parent->setData(0, kPathRole, u.modelPath);
            parent->setExpanded(true);
            byModel.insert(u.modelHash, parent);
            ++models;
        }
        auto* child = new QTreeWidgetItem(parent);
        child->setText(0, QStringLiteral("%1  —  %2").arg(u.role, u.material));
        child->setToolTip(0, u.shader);
    }
    Q_EMIT usersTitleChanged(QStringLiteral("ASSOCIATED MODELS (%1 model(s) / %2 slot(s))")
                             .arg(models)
                             .arg(uses.size()));
}

}  // namespace fox
