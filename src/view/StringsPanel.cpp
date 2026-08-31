// StringsPanel.cpp — see StringsPanel.h.
#include "view/StringsPanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QSignalBlocker>
#include <QMenu>

#include "util/MenuText.h"
#include <QPushButton>
#include <QSaveFile>
#include <QTextStream>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "util/SearchBox.h"

#include "app/Config.h"
#include "app/ExportNotifier.h"
#include "fox/FoxHash.h"
#include "fox/LangFile.h"
#include "index/ArchiveIndex.h"
#include "index/MgoGearConfig.h"
#include "index/NameCatalog.h"

using fox::ArchiveIndex;
using fox::IndexedFile;

namespace {

// "mgo_gear.eng.lng2" → "eng". The extension is everything after the FIRST
// dot in Fox's own hashing, and that is where the language lives.
QString languageOf(const QString& path)
{
    const QString base = path.section(QLatin1Char('/'), -1);
    const QStringList bits = base.split(QLatin1Char('.'));
    return bits.size() >= 3 ? bits[bits.size() - 2] : QString();
}

QString stemOf(const QString& path)
{
    return path.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
}

constexpr int kAllTables = -1;

}  // namespace

StringsPanel::StringsPanel(QWidget* parent) : QWidget(parent)
{
    auto* right = this;
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);

    // The table list, as one row rather than a pane. The Files tree beside
    // this panel already IS a list of every file, language tables included, so
    // a second full-height list of the same things would be the duplication
    // the Models tab just lost. What the list was FOR — "which tables does
    // this install have, and let me step through them" — is exactly a combo.
    auto* tableBar = new QHBoxLayout();
    tableBar->addWidget(new QLabel(QStringLiteral("Table"), right));
    m_tables = new QComboBox(right);
    m_tables->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_tables->setMinimumWidth(220);
    m_tables->setToolTip(QStringLiteral(
        "Every .lng2 language table in the indexed archives. One file per "
        "language per category — tpp_weapon.eng.lng2, mgo_gear.eng.lng2 — so a "
        "single install carries dozens.\n\n"
        "Choosing one here selects it in the file tree too."));
    tableBar->addWidget(m_tables, 1);
    rv->addLayout(tableBar);

    auto* bar = new QHBoxLayout();
    m_filter = new QLineEdit(right);
    m_filter->setPlaceholderText(QStringLiteral("Filter text, label or key…"));
    // Esc clears, the down arrow recalls the last ten searches, and a
    // committed search is remembered — the same four behaviours in every
    // search box in the application, from one place (template §4/§15).
    fox::searchbox::attach(m_filter, QStringLiteral("strings/filterHistory"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setToolTip(QStringLiteral(
        "Matches the string itself, the label when this build knows one, and "
        "the key in hex — so a hash copied out of a log finds its string."));
    bar->addWidget(m_filter, 1);
    m_allTables = new QCheckBox(QStringLiteral("All tables"), right);
    m_allTables->setToolTip(QStringLiteral(
        "Search every table at once. This is how you find WHICH table holds a "
        "string, which is the question that matters when a name is missing."));
    bar->addWidget(m_allTables);
    m_exportBtn = new QPushButton(QStringLiteral("Export TSV…"), right);
    m_exportBtn->setToolTip(QStringLiteral(
        "Exactly the rows on screen, filter included — key, label, text and "
        "the table each came from."));
    bar->addWidget(m_exportBtn);
    rv->addLayout(bar);

    m_rows = new QTreeWidget(right);
    m_rows->setColumnCount(4);
    m_rows->setHeaderLabels({QStringLiteral("Key"), QStringLiteral("Label"),
                             QStringLiteral("Text"), QStringLiteral("Table")});
    m_rows->setRootIsDecorated(false);
    m_rows->setUniformRowHeights(true);
    m_rows->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_rows->setContextMenuPolicy(Qt::CustomContextMenu);
    m_rows->header()->setStretchLastSection(false);
    m_rows->setColumnWidth(0, 92);
    m_rows->setColumnWidth(1, 190);
    m_rows->setColumnWidth(2, 520);
    rv->addWidget(m_rows, 1);

    m_status = new QLabel(right);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rv->addWidget(m_status);

    connect(m_tables, &QComboBox::currentIndexChanged, this, [this](int row) {
        if (row < 0 || row >= m_tableFiles.size()) return;
        showTable(m_tableFiles[row]);
        // Not while the owner is the one driving the combo, or selecting a
        // table in the tree would bounce straight back out as a request to
        // select it again.
        if (!m_selecting) emit tableChosen(m_tableFiles[row]);
    });
    connect(m_filter, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    connect(m_allTables, &QCheckBox::toggled, this, [this](bool) {
        const int row = m_tables->currentIndex();
        showTable(m_allTables->isChecked()
                      ? kAllTables
                      : (row >= 0 && row < m_tableFiles.size()
                             ? m_tableFiles[row]
                             : kAllTables));
    });
    connect(m_exportBtn, &QPushButton::clicked, this, [this] { exportTsv(); });
    connect(m_rows, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                QTreeWidgetItem* it = m_rows->itemAt(pos);
                if (!it) return;
                QMenu menu(this);
                menu.addAction(MenuText::kCopyText, this, [it] {
                    QApplication::clipboard()->setText(it->text(2));
                });
                menu.addAction(MenuText::kCopyKey, this, [it] {
                    QApplication::clipboard()->setText(it->text(0));
                });
                menu.addAction(MenuText::kCopyRow, this, [it] {
                    QApplication::clipboard()->setText(
                        QStringLiteral("%1\t%2\t%3\t%4")
                            .arg(it->text(0), it->text(1), it->text(2),
                                 it->text(3)));
                });
                menu.exec(m_rows->viewport()->mapToGlobal(pos));
            });

    rescan();
}

bool StringsPanel::showTableFile(int fileIdx)
{
    const int row = m_tableFiles.indexOf(fileIdx);
    if (row < 0) return false;
    m_selecting = true;
    if (m_allTables->isChecked()) m_allTables->setChecked(false);
    if (m_tables->currentIndex() != row) m_tables->setCurrentIndex(row);
    else showTable(fileIdx);   // same row, but the panel may be showing "all"
    m_selecting = false;
    return true;
}

int StringsPanel::currentTableFile() const
{
    if (m_allTables && m_allTables->isChecked()) return -1;
    const int row = m_tables ? m_tables->currentIndex() : -1;
    return row >= 0 && row < m_tableFiles.size() ? m_tableFiles[row] : -1;
}

void StringsPanel::populateExportMenu(QMenu* menu)
{
    if (!menu) return;
    QAction* a = menu->addAction(
        m_shown == m_rows->topLevelItemCount()
            ? QStringLiteral("Export %1 string(s) as TSV…").arg(m_shown)
            : QStringLiteral("Export the %1 filtered string(s) as TSV…")
                  .arg(m_shown),
        this, [this] { exportTsv(); });
    a->setEnabled(m_shown > 0);
}

void StringsPanel::rescan()
{
    m_parsed.clear();
    m_allRows.clear();
    m_labels.clear();
    m_labelsBuilt = false;
    refreshTables();
}

void StringsPanel::refreshTables()
{
    QSignalBlocker block(m_tables);
    m_tables->clear();
    m_tableFiles.clear();
    const auto& files = ArchiveIndex::instance().files();
    // Sorted by path so the list reads as the archives are laid out, and so
    // two runs over the same install produce the same order.
    QVector<int> found;
    for (int i = 0; i < files.size(); ++i)
        if (ArchiveIndex::extensionOf(files[i]).endsWith(QLatin1String("lng2")))
            found.append(i);
    std::sort(found.begin(), found.end(), [&files](int a, int b) {
        return files[a].path < files[b].path;
    });

    for (const int fi : found) {
        const QString lang = languageOf(files[fi].path);
        m_tables->addItem(lang.isEmpty()
                              ? stemOf(files[fi].path)
                              : QStringLiteral("%1  ·  %2")
                                    .arg(stemOf(files[fi].path), lang),
                          fi);
        m_tables->setItemData(m_tables->count() - 1, files[fi].path,
                              Qt::ToolTipRole);
        m_tableFiles.append(fi);
    }
    if (found.isEmpty()) {
        m_status->setText(QStringLiteral(
            "No .lng2 language table in the indexed archives. The tables live "
            "in the GAME's own chunks (master/chunk0, a_chunk7 and friends) — "
            "a folder of extracted models does not carry them, and without "
            "them every item shows its id instead of its name."));
        m_rows->clear();
        m_shown = 0;
        return;
    }
    m_tables->setCurrentIndex(0);
    showTable(m_tableFiles[0]);   // the blocker above swallowed the signal
}

const QHash<quint32, QString>& StringsPanel::knownLabels()
{
    if (m_labelsBuilt) return m_labels;
    m_labelsBuilt = true;
    const auto add = [this](const QString& label) {
        if (label.isEmpty()) return;
        const quint32 h = quint32(
            fox::hashFileNameLegacy(label, /*removeExtension=*/false) & 0xFFFFFFFFu);
        if (!m_labels.contains(h)) m_labels.insert(h, label);
    };
    // The two label sets this build actually holds. Neither is the whole
    // vocabulary — nothing is, because StrCode32 cannot be inverted — so a key
    // with no label here means "not known", never "not a label".
    for (const QString& label : fox::NameCatalog::instance().knownLabels())
        add(label);
    const fox::MgoGearConfig& gc = fox::MgoGearConfig::instance();
    for (int f = 0; f < 2; ++f)
        for (const fox::MgoGearCategory& cat : gc.categories(f == 1))
            for (const fox::MgoGearItem& it : cat.items) add(it.nameTag);
    return m_labels;
}

const QVector<StringsPanel::Row>& StringsPanel::rowsFor(int fileIdx)
{
    static const QVector<Row> empty;
    if (fileIdx == kAllTables) {
        if (m_allRows.isEmpty())
            for (const int fi : m_tableFiles) m_allRows += rowsFor(fi);
        return m_allRows;
    }
    auto it = m_parsed.constFind(fileIdx);
    if (it != m_parsed.constEnd()) return *it;

    const auto& files = ArchiveIndex::instance().files();
    if (fileIdx < 0 || fileIdx >= files.size()) return empty;
    QVector<Row> rows;
    fox::LangFile lg;
    const QByteArray raw = ArchiveIndex::instance().readFile(files[fileIdx]);
    if (!raw.isEmpty() && lg.parse(raw)) {
        const QHash<quint32, QString>& labels = knownLabels();
        rows.reserve(lg.strings().size());
        for (auto s = lg.strings().constBegin(); s != lg.strings().constEnd(); ++s)
            rows.append(Row{s.key(), labels.value(s.key()), s.value(), fileIdx});
        // A hash table has no order at all, so a stable one has to be imposed:
        // known labels first and alphabetical, then the rest by key. Without
        // this the same table lists in a different order every run and nothing
        // can be compared against anything.
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.label.isEmpty() != b.label.isEmpty()) return !a.label.isEmpty();
            if (!a.label.isEmpty() && a.label != b.label) return a.label < b.label;
            return a.key < b.key;
        });
    }
    return *m_parsed.insert(fileIdx, rows);
}

void StringsPanel::showTable(int fileIdx)
{
    const QVector<Row>& rows = rowsFor(m_allTables->isChecked() ? kAllTables
                                                                : fileIdx);
    const auto& files = ArchiveIndex::instance().files();
    m_rows->setUpdatesEnabled(false);
    m_rows->clear();
    QList<QTreeWidgetItem*> items;
    items.reserve(rows.size());
    for (const Row& r : rows) {
        auto* it = new QTreeWidgetItem();
        it->setText(0, QStringLiteral("%1").arg(r.key, 8, 16, QLatin1Char('0')));
        it->setText(1, r.label);
        it->setText(2, r.text);
        it->setText(3, r.fileIdx >= 0 && r.fileIdx < files.size()
                           ? stemOf(files[r.fileIdx].path)
                           : QString());
        items.append(it);
    }
    m_rows->addTopLevelItems(items);
    m_rows->setUpdatesEnabled(true);
    applyFilter();
}

void StringsPanel::applyFilter()
{
    const QString needle = m_filter->text().trimmed().toLower();
    int shown = 0;
    for (int i = 0; i < m_rows->topLevelItemCount(); ++i) {
        QTreeWidgetItem* it = m_rows->topLevelItem(i);
        const bool vis = needle.isEmpty()
            || it->text(2).toLower().contains(needle)
            || it->text(1).toLower().contains(needle)
            || it->text(0).contains(needle);
        it->setHidden(!vis);
        if (vis) ++shown;
    }
    m_shown = shown;
    const int total = m_rows->topLevelItemCount();
    const int named = [this] {
        int n = 0;
        for (int i = 0; i < m_rows->topLevelItemCount(); ++i)
            if (!m_rows->topLevelItem(i)->text(1).isEmpty()) ++n;
        return n;
    }();
    m_status->setText(
        QStringLiteral("%1 of %2 string(s) · %3 with a label this build knows "
                       "(the key is a StrCode32 hash and cannot be turned back "
                       "into a label)")
            .arg(shown)
            .arg(total)
            .arg(named));
}

void StringsPanel::exportTsv()
{
    const QString out = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export strings"),
        QDir(Config::exportDir()).filePath(QStringLiteral("strings.tsv")),
        QStringLiteral("Tab-separated values (*.tsv)"));
    if (out.isEmpty()) return;
    Config::setExportDir(QFileInfo(out).absolutePath());
    QSaveFile f(out);
    if (!f.open(QIODevice::WriteOnly)) {
        m_status->setText(QStringLiteral("Could not write %1").arg(out));
        return;
    }
    QTextStream ts(&f);
    ts << "key\tlabel\ttext\ttable\n";
    int n = 0;
    for (int i = 0; i < m_rows->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* it = m_rows->topLevelItem(i);
        if (it->isHidden()) continue;   // the filter is part of the request
        // A tab or a newline inside a string would break the file it is being
        // written into, so both become spaces. The strings themselves are menu
        // text and do carry newlines.
        QString text = it->text(2);
        text.replace(QLatin1Char('\t'), QLatin1Char(' '));
        text.replace(QLatin1Char('\n'), QLatin1Char(' '));
        text.replace(QLatin1Char('\r'), QLatin1Char(' '));
        ts << it->text(0) << '\t' << it->text(1) << '\t' << text << '\t'
           << it->text(3) << '\n';
        ++n;
    }
    ts.flush();
    const bool ok = f.commit();
    if (ok)
        fox::ExportNotifier::instance().notify(
            QStringLiteral("Exported %1 string(s) as TSV").arg(n),
            QFileInfo(out).absolutePath());
    m_status->setText(ok
                          ? QStringLiteral("Exported %1 row(s) to %2").arg(n).arg(out)
                          : QStringLiteral("Could not write %1").arg(out));
}

int StringsPanel::applyDevFilter(const QString& filter, bool allTables)
{
    if (allTables != m_allTables->isChecked()) m_allTables->setChecked(allTables);
    m_filter->setText(filter);
    qInfo("strings: %d table(s), %s", int(m_tableFiles.size()),
          qUtf8Printable(m_status->text().simplified()));
    return m_shown;
}

int StringsPanel::dumpAll(const QString& tsvPath)
{
    QSaveFile f(tsvPath);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning("strings: cannot write %s", qUtf8Printable(tsvPath));
        return 0;
    }
    QTextStream ts(&f);
    ts << "key\tlabel\ttext\ttable\tpath\n";
    const auto& files = ArchiveIndex::instance().files();
    int n = 0;
    for (const int fi : m_tableFiles) {
        for (const Row& r : rowsFor(fi)) {
            QString text = r.text;
            text.replace(QLatin1Char('\t'), QLatin1Char(' '));
            text.replace(QLatin1Char('\n'), QLatin1Char(' '));
            text.replace(QLatin1Char('\r'), QLatin1Char(' '));
            ts << QStringLiteral("%1").arg(r.key, 8, 16, QLatin1Char('0')) << '\t'
               << r.label << '\t' << text << '\t' << stemOf(files[fi].path)
               << '\t' << files[fi].path << '\n';
            ++n;
        }
    }
    ts.flush();
    f.commit();
    qInfo("strings: dumped %d string(s) from %d table(s) to %s", n,
          int(m_tableFiles.size()), qUtf8Printable(tsvPath));
    return n;
}
