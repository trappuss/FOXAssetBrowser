// BulkExtractorTab.cpp — see BulkExtractorTab.h.
//
// Performance shape: matches are grouped by CONTAINER before the workers start,
// so an FPK with 200 matched children is decompressed once, not 200 times.
// Workers are plain std::threads over an atomic work cursor; every QarFile read
// opens its own handle, so no cross-thread file state exists. The sibling reads
// INSIDE texture assembly are covered separately, by the run-scoped blob cache
// (extract::blobcache) this run opens for its own duration.
#include "tabs/BulkExtractorTab.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMutex>
#include <QMutexLocker>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <thread>
#include <vector>

#include "app/Config.h"
#include "app/ExportNotifier.h"
#include "audio/WemFile.h"
#include "export/BulkLedger.h"
#include "export/ExportOptions.h"
#include "fox/FoxHash.h"
#include "fox/FpkFile.h"
#include "fox/PftxsFile.h"
#include "fox/SbpFile.h"
#include "index/ArchiveIndex.h"
#include "index/ModelTags.h"
#include "util/ExportLayout.h"
#include "util/Extract.h"
#include "util/SearchBox.h"
#include "util/SearchQuery.h"

using fox::ArchiveIndex;
using fox::IndexedFile;

namespace {

// One unit of work: either one top-level file, or one container with a batch
// of its children.
struct WorkUnit {
    QVector<int> fileIdxs;     // indices into ArchiveIndex::files()
    bool container = false;    // true → all fileIdxs share one container entry
};

// Everything a worker needs that is not an atomic, in one object, so the lambda
// captures one shared_ptr rather than eleven values — and so adding a run
// option cannot leave one worker configured differently from another.
struct RunConfig {
    QString outRoot;
    QString layout;
    QHash<QString, int> layoutRank;
    bool assemble = true;
    bool convertWem = true;
    bool onlyNew = true;
};

bool writeOut(const QString& dir, const QString& rel, const QByteArray& data,
              QString* error)
{
    const QString full = QDir(dir).filePath(rel);
    if (!QDir().mkpath(QFileInfo(full).absolutePath())) {
        if (error) *error = QStringLiteral("could not create the folder");
        return false;
    }
    QSaveFile out(full);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error) *error = out.errorString();
        return false;
    }
    out.write(data);
    if (!out.commit()) {
        if (error) *error = out.errorString();
        return false;
    }
    return true;
}

// Write, optionally converting .wem → playable .wav (PCM re-wrap, or vgmstream
// for packed Wwise codecs). Falls back to the raw .wem when conversion fails so
// nothing is silently dropped.
bool writeMaybeAudio(const QString& dir, QString rel, const QByteArray& data,
                     bool convertWem, QString* error)
{
    if (convertWem && rel.endsWith(QLatin1String(".wem"))) {
        const audio::WemInfo wi = audio::parseWem(data);
        QByteArray wav;
        if (wi.isPcm()) wav = audio::wemToWav(data, wi);
        else if (wi.riff) wav = audio::convertWithVgmstream(data);
        if (!wav.isEmpty()) {
            rel.chop(4);
            rel += QStringLiteral(".wav");
            return writeOut(dir, rel, wav, error);
        }
    }
    return writeOut(dir, rel, data, error);
}

// Human name for a file in the console and the failure list. The path when it
// has one, its hash when it does not — the same thing the browser lists it as.
QString labelFor(const IndexedFile& f)
{
    return f.named ? f.path
                   : QStringLiteral("<%1>").arg(f.hash, 16, 16, QLatin1Char('0'));
}

QString humanTime(qint64 ms)
{
    if (ms < 0) return QStringLiteral("—");
    const qint64 s = ms / 1000;
    if (s < 60) return QStringLiteral("%1s").arg(s);
    if (s < 3600) return QStringLiteral("%1m %2s").arg(s / 60).arg(s % 60);
    return QStringLiteral("%1h %2m").arg(s / 3600).arg((s % 3600) / 60);
}

// ── Factory presets (§8) ───────────────────────────────────────────────────
// The reason the query language has `|`. Each of these is a UNION of naming
// families that no AND-only syntax can express, and each was checked against
// the tag vocabulary rather than guessed: they use #tags where a tag exists and
// name fragments only where one does not.
struct Preset { const char* label; const char* query; const char* ext; };
const Preset kPresets[] = {
    {"—", "", ""},
    {"Every model", "#chara|#weapon|#item", "fmdl"},
    {"Every texture", "", "ftex"},
    {"MGO avatar customization", "avm_|avf_", "fmdl"},
    {"MGO avatar textures", "avm_|avf_", "ftex"},
    {"Player characters (TPP)", "#tpp #chara sna|qui|ocelot|miller", "fmdl"},
    {"Weapons and attachments", "#weapon", "fmdl"},
    {"Animation (skeletons and clips)", "", "mtar"},
    {"Language tables", "", "lng2"},
    {"Audio", "", "wem"},
};

}  // namespace

BulkExtractorTab::BulkExtractorTab(QWidget* parent) : QWidget(parent)
{
    m_pending = std::make_shared<QStringList>();
    m_pendingMx = std::make_shared<QMutex>();

    auto* layout = new QVBoxLayout(this);

    // ── What to run over ────────────────────────────────────────────────
    auto* whatBox = new QGroupBox(QStringLiteral("What to extract"), this);
    auto* grid = new QGridLayout(whatBox);
    int row = 0;

    grid->addWidget(new QLabel(QStringLiteral("Filter:")), row, 0);
    m_query = new QLineEdit(whatBox);
    m_query->setPlaceholderText(QStringLiteral(
        "Path contains… (space = AND, -term excludes, \"quoted\" is literal, "
        "a|b matches either, #tag filters by category)"));
    // The same four behaviours as every other search box (template §4/§15).
    fox::searchbox::attach(m_query, QStringLiteral("bulk/searchHistory"));
    m_query->setClearButtonEnabled(true);
    grid->addWidget(m_query, row, 1, 1, 3);

    // Preset and Extension share a row in a BOX, not in grid cells. Given
    // cells they inherit the grid's column widths, which are sized by the
    // filter row spanning all of them — so a three-character extension combo
    // came out as wide as the search box. A box packs each control at the size
    // it asked for and puts the slack at the end, where slack belongs.
    ++row;
    grid->addWidget(new QLabel(QStringLiteral("Preset:")), row, 0);
    auto* pickRow = new QHBoxLayout();
    m_preset = new QComboBox(whatBox);
    for (const Preset& p : kPresets)
        m_preset->addItem(QString::fromUtf8(p.label));
    m_preset->setToolTip(QStringLiteral(
        "Ready-made filters for the families this install actually ships. They "
        "fill the filter in — they do not start anything, so you can see the "
        "match count and edit the query before running it."));
    pickRow->addWidget(m_preset);
    pickRow->addSpacing(16);
    pickRow->addWidget(new QLabel(QStringLiteral("Extension:"), whatBox));
    m_extension = new QComboBox(whatBox);
    m_extension->setEditable(true);
    m_extension->addItems({QString(), QStringLiteral("ftex"), QStringLiteral("fmdl"),
                           QStringLiteral("mtar"), QStringLiteral("gani"),
                           QStringLiteral("frig"), QStringLiteral("fova"),
                           QStringLiteral("fv2"), QStringLiteral("lua"),
                           QStringLiteral("fox2"), QStringLiteral("fpk"),
                           QStringLiteral("fpkd"), QStringLiteral("pftxs"),
                           QStringLiteral("lng2"), QStringLiteral("wem")});
    m_extension->setMinimumContentsLength(8);
    pickRow->addWidget(m_extension);
    pickRow->addStretch(1);
    grid->addLayout(pickRow, row, 1, 1, 3);

    ++row;
    auto* checkRow = new QHBoxLayout();
    m_namedOnly = new QCheckBox(QStringLiteral("Resolved names only"), whatBox);
    m_namedOnly->setToolTip(QStringLiteral(
        "Skip entries no dictionary has a name for. They still extract — under "
        "unresolved\\<hash>.<ext> — so this is about whether you want them."));
    checkRow->addWidget(m_namedOnly);
    checkRow->addSpacing(16);
    m_includeContainers =
        new QCheckBox(QStringLiteral("Include container contents"), whatBox);
    m_includeContainers->setChecked(true);
    checkRow->addWidget(m_includeContainers);
    checkRow->addStretch(1);
    grid->addLayout(checkRow, row, 1, 1, 3);

    ++row;
    m_matchLabel = new QLabel(whatBox);
    grid->addWidget(m_matchLabel, row, 1, 1, 3);
    grid->setColumnStretch(3, 1);
    layout->addWidget(whatBox);

    // ── The queue ───────────────────────────────────────────────────────
    auto* queueBox = new QGroupBox(QStringLiteral("Queue"), this);
    queueBox->setToolTip(QStringLiteral(
        "Build a set by hand, a filter at a time. The queue survives filter "
        "changes and restarts, so \"these forty models\" can be assembled over "
        "several searches and then run in one go."));
    auto* qgrid = new QGridLayout(queueBox);
    m_queueAdd = new QPushButton(QStringLiteral("Add matches"), queueBox);
    m_queueRemove = new QPushButton(QStringLiteral("Remove matches"), queueBox);
    m_queueClear = new QPushButton(QStringLiteral("Clear"), queueBox);
    m_queueLabel = new QLabel(queueBox);
    qgrid->addWidget(m_queueAdd, 0, 0);
    qgrid->addWidget(m_queueRemove, 0, 1);
    qgrid->addWidget(m_queueClear, 0, 2);
    qgrid->addWidget(m_queueLabel, 0, 3);
    qgrid->setColumnStretch(3, 1);
    layout->addWidget(queueBox);

    // ── How to run it ───────────────────────────────────────────────────
    auto* howBox = new QGroupBox(QStringLiteral("How to run it"), this);
    auto* hgrid = new QGridLayout(howBox);
    row = 0;

    hgrid->addWidget(new QLabel(QStringLiteral("Run over:")), row, 0);
    auto* srcRow = new QHBoxLayout();
    m_source = new QComboBox(howBox);
    m_source->addItem(QStringLiteral("Everything the filter matches"),
                      QStringLiteral("filter"));
    m_source->addItem(QStringLiteral("The queue"), QStringLiteral("queue"));
    srcRow->addWidget(m_source);
    srcRow->addSpacing(16);
    srcRow->addWidget(new QLabel(QStringLiteral("Already exported:"), howBox));
    m_existing = new QComboBox(howBox);
    m_existing->addItem(QStringLiteral("Skip (only new)"), true);
    m_existing->addItem(QStringLiteral("Write again (overwrite)"), false);
    m_existing->setToolTip(QStringLiteral(
        "\"Only new\" reads _bulk_manifest.json in the output folder, which "
        "this tool writes as it goes. The ledger belongs to the FOLDER, not to "
        "your settings, so pointing a fresh install at the same folder picks up "
        "where the last run stopped."));
    srcRow->addWidget(m_existing);
    srcRow->addSpacing(16);
    srcRow->addWidget(new QLabel(QStringLiteral("Workers:"), howBox));
    m_workers = new QSpinBox(howBox);
    m_workers->setRange(0, 64);
    m_workers->setSpecialValueText(
        QStringLiteral("Auto (%1)").arg(qMax(2, QThread::idealThreadCount() - 1)));
    m_workers->setToolTip(QStringLiteral(
        "How many files are read and written at once. Auto leaves one core for "
        "the interface. More is not always faster: past a point every worker is "
        "waiting on the same disk."));
    srcRow->addWidget(m_workers);
    srcRow->addStretch(1);
    hgrid->addLayout(srcRow, row, 1);

    ++row;
    hgrid->addWidget(new QLabel(QStringLiteral("Output:")), row, 0);
    auto* outRow = new QHBoxLayout();
    m_outputDir = new QLineEdit(Config::exportDir(), howBox);
    m_browseBtn = new QPushButton(QStringLiteral("Browse…"), howBox);
    outRow->addWidget(m_outputDir, 1);
    outRow->addWidget(m_browseBtn);
    hgrid->addLayout(outRow, row, 1);

    ++row;
    {
        auto* note = new QLabel(howBox);
        note->setWordWrap(true);
        note->setText(QStringLiteral(
            "What a .ftex or a .wem turns into, what the files are called and "
            "which folders they go into are in <b>Settings ▸ Export</b>, shared "
            "with every other way this tool writes a file."));
        hgrid->addWidget(note, row, 0, 1, 2);
    }
    layout->addWidget(howBox);

    // ── Run controls ────────────────────────────────────────────────────
    auto* buttons = new QGridLayout();
    m_startBtn = new QPushButton(QStringLiteral("Extract"), this);
    m_pauseBtn = new QPushButton(QStringLiteral("Pause"), this);
    m_cancelBtn = new QPushButton(QStringLiteral("Cancel"), this);
    m_pauseBtn->setEnabled(false);
    m_cancelBtn->setEnabled(false);
    buttons->addWidget(m_startBtn, 0, 0);
    buttons->addWidget(m_pauseBtn, 0, 1);
    buttons->addWidget(m_cancelBtn, 0, 2);
    buttons->setColumnStretch(3, 1);
    layout->addLayout(buttons);

    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(true);
    layout->addWidget(m_progress);
    m_status = new QLabel(this);
    layout->addWidget(m_status);

    m_console = new QPlainTextEdit(this);
    m_console->setReadOnly(true);
    // Bounded, permanently. An unbounded console over a 200,000-file run is a
    // memory leak with a scrollbar.
    m_console->setMaximumBlockCount(2000);
    m_console->setPlaceholderText(
        QStringLiteral("The run's log appears here, and every failure appears "
                       "in _bulk_failed.txt beside the output."));
    layout->addWidget(m_console, 1);

    m_poll = new QTimer(this);
    m_poll->setInterval(150);
    connect(m_poll, &QTimer::timeout, this, &BulkExtractorTab::pollProgress);

    auto* debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(200);
    connect(m_query, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
    connect(m_extension, &QComboBox::currentTextChanged, debounce,
            qOverload<>(&QTimer::start));
    connect(m_namedOnly, &QCheckBox::toggled, debounce, qOverload<>(&QTimer::start));
    connect(m_includeContainers, &QCheckBox::toggled, debounce,
            qOverload<>(&QTimer::start));
    connect(debounce, &QTimer::timeout, this, &BulkExtractorTab::updateMatchCount);
    connect(m_source, &QComboBox::currentIndexChanged, this,
            [this](int) { updateMatchCount(); });

    connect(m_preset, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i <= 0 || i >= int(sizeof(kPresets) / sizeof(kPresets[0]))) return;
        m_query->setText(QString::fromUtf8(kPresets[i].query));
        m_extension->setCurrentText(QString::fromUtf8(kPresets[i].ext));
        updateMatchCount();
    });
    connect(m_browseBtn, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose output folder"), m_outputDir->text());
        if (!dir.isEmpty()) m_outputDir->setText(dir);
    });
    connect(m_startBtn, &QPushButton::clicked, this, &BulkExtractorTab::startExtraction);
    connect(m_pauseBtn, &QPushButton::clicked, this, &BulkExtractorTab::togglePause);
    connect(m_cancelBtn, &QPushButton::clicked, this, &BulkExtractorTab::cancelExtraction);
    connect(m_queueAdd, &QPushButton::clicked, this, &BulkExtractorTab::queueAddMatches);
    connect(m_queueRemove, &QPushButton::clicked, this,
            &BulkExtractorTab::queueRemoveMatches);
    connect(m_queueClear, &QPushButton::clicked, this, &BulkExtractorTab::queueClear);

    loadQueue();
    updateQueueLabel();
}

BulkExtractorTab::~BulkExtractorTab()
{
    const bool wasRunning = m_running.load();
    m_cancel.store(true);
    m_paused.store(false);   // a paused worker never sees the cancel otherwise
    // Detached workers check m_cancel and also m_running; give them a moment.
    while (m_threadsLeft.load() > 0) QThread::msleep(10);
    // The workers are gone, so nothing is still recording. Write what the run
    // got through — those files really are on disk.
    if (wasRunning) finishLedgers(false);
}

void BulkExtractorTab::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape && m_running.load()) {
        cancelExtraction();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

void BulkExtractorTab::onIndexReady(bool ready)
{
    if (!ready) return;
    updateMatchCount();
    updateQueueLabel();
}

void BulkExtractorTab::configureRun(const QString& query, const QString& ext,
                                    const QString& outDir, int workers,
                                    bool overwrite, bool useQueue)
{
    m_query->setText(query);
    m_extension->setCurrentText(ext);
    if (!outDir.isEmpty()) m_outputDir->setText(outDir);
    m_harness = true;
    m_workers->setValue(qBound(0, workers, m_workers->maximum()));
    const int ex = m_existing->findData(!overwrite);
    if (ex >= 0) m_existing->setCurrentIndex(ex);
    const int src = m_source->findData(useQueue ? QStringLiteral("queue")
                                                : QStringLiteral("filter"));
    if (src >= 0) m_source->setCurrentIndex(src);
    updateMatchCount();
}

void BulkExtractorTab::startRun() { startExtraction(); }

void BulkExtractorTab::cancelForShot() { cancelExtraction(); }

void BulkExtractorTab::pauseForShot(bool on)
{
    if (m_paused.load() != on) togglePause();
}

void BulkExtractorTab::addMatchesToQueue() { queueAddMatches(); }

void BulkExtractorTab::applyPreset(const QString& query, const QString& ext)
{
    m_query->setText(query);
    m_extension->setCurrentText(ext);
    updateMatchCount();
}

QVector<int> BulkExtractorTab::matchedFiles() const
{
    QVector<int> out;
    const ArchiveIndex& index = ArchiveIndex::instance();
    if (!index.ready()) return out;

    // THE SHARED MATCHER (template §4). This used to split on spaces and run a
    // bare contains() per term — its own fourth parser, which understood none
    // of the language the other three do: no -exclude, no quotes, no #tags, no
    // a|b. Filtering "avm -cov" in the Models tab and typing the same thing
    // here produced two different sets, which is precisely the failure the
    // one-matcher rule is written against, and it was live in this tab.
    const searchq::Query q(m_query->text());
    const QString wantExt = m_extension->currentText().trimmed().toLower();
    const bool namedOnly = m_namedOnly->isChecked();
    const bool includeContainers = m_includeContainers->isChecked();
    const bool skipFtexs = m_assembleFtex;

    const auto& files = index.files();
    for (int i = 0; i < files.size(); ++i) {
        const IndexedFile& f = files[i];
        if (namedOnly && !f.named) continue;
        if (!includeContainers && f.childIdx >= 0) continue;
        const QString ext = ArchiveIndex::extensionOf(f);
        if (!wantExt.isEmpty() && ext != wantExt) continue;
        if (skipFtexs && wantExt.isEmpty() && ext.endsWith(QLatin1String(".ftexs")))
            continue;
        // TAGS INCLUDED. Without them "#chara" here parsed to a query with no
        // text terms, matched every file in the index, and offered to extract
        // the whole install.
        if (!fox::queryMatchesFile(q, i, f)) continue;
        out.append(i);
    }
    return out;
}

QVector<int> BulkExtractorTab::runSet() const
{
    if (m_source->currentData().toString() != QLatin1String("queue"))
        return matchedFiles();
    // Resolve the queue's hashes against the CURRENT index. A hash the index no
    // longer holds (a mod removed, a folder unmounted) is dropped from the run
    // and left in the queue, so unmounting a folder does not silently empty a
    // list the user spent time building.
    QVector<int> out;
    const ArchiveIndex& index = ArchiveIndex::instance();
    if (!index.ready() || m_queue.isEmpty()) return out;
    // ONE row per hash. A hash sits at several indices routinely — an asset
    // that ships in both chunk0 and texture0, and every mod override, which
    // the index marks `shadowed`. Taking all of them queued three files and
    // ran six, inflated the total and the progress bar, printed "3 file(s) —
    // -3 not in the current index", and had two workers racing to commit the
    // same output path. The winning (non-shadowed) row is the one the rest of
    // the tool would show, so it is the one extracted.
    QSet<quint64> taken;
    const auto& files = index.files();
    for (int i = 0; i < files.size(); ++i) {
        if (!m_queue.contains(files[i].hash)) continue;
        if (files[i].shadowed) continue;
        if (taken.contains(files[i].hash)) continue;
        taken.insert(files[i].hash);
        out.append(i);
    }
    // A queued hash whose only copy is shadowed is still a file the user asked
    // for; take it rather than dropping it silently.
    for (int i = 0; i < files.size(); ++i) {
        if (!m_queue.contains(files[i].hash)) continue;
        if (taken.contains(files[i].hash)) continue;
        taken.insert(files[i].hash);
        out.append(i);
    }
    std::sort(out.begin(), out.end());
    return out;
}

BulkExtractorTab::Counts BulkExtractorTab::counts() const
{
    Counts c;
    c.matches = int(matchedFiles().size());
    c.queuePresent = m_queue.isEmpty() ? 0 : int(runSet().size());
    c.run = m_source->currentData().toString() == QLatin1String("queue")
                ? c.queuePresent
                : c.matches;
    return c;
}

void BulkExtractorTab::updateMatchCount()
{
    const bool useQueue =
        m_source->currentData().toString() == QLatin1String("queue");
    const Counts c = counts();
    m_matchLabel->setText(
        useQueue
            ? QStringLiteral("%1 file(s) match the filter — the run will use the "
                             "queue's %2")
                  .arg(c.matches)
                  .arg(c.run)
            : QStringLiteral("%1 file(s) match").arg(c.matches));
    m_startBtn->setEnabled(c.run > 0 && !m_running.load());
    updateQueueLabel(c);
}

// ── The queue ──────────────────────────────────────────────────────────────

void BulkExtractorTab::loadQueue()
{
    m_queue.clear();
    const QStringList hex =
        QSettings().value(QStringLiteral("bulk/queue")).toStringList();
    for (const QString& h : hex) {
        bool ok = false;
        const quint64 v = h.toULongLong(&ok, 16);
        if (ok) m_queue.insert(v);
    }
}

void BulkExtractorTab::saveQueue() const
{
    // NOT guarded by m_harness. The queue is only ever written by the three
    // explicit commands — Add, Remove, Clear — and the harness reaches those
    // only through --bulkqueueadd, which is a request to change the queue.
    // Suppressing it there made the flag silently do nothing. What the harness
    // must not touch is the INCIDENTAL setting a run rewrites as a side
    // effect, which is the export folder, and that guard is in startExtraction.
    QStringList hex;
    hex.reserve(m_queue.size());
    for (const quint64 h : m_queue)
        hex.append(QStringLiteral("%1").arg(h, 16, 16, QLatin1Char('0')));
    // Sorted, so the INI does not churn on every add — a settings file whose
    // diff is different every launch is one nobody can keep in version control.
    hex.sort();
    QSettings().setValue(QStringLiteral("bulk/queue"), hex);
}

void BulkExtractorTab::queueAddMatches()
{
    const ArchiveIndex& index = ArchiveIndex::instance();
    const QVector<int> m = matchedFiles();
    const int before = m_queue.size();
    for (const int i : m) m_queue.insert(index.files()[i].hash);
    saveQueue();
    updateQueueLabel();
    updateMatchCount();
    log(QStringLiteral("queue: added %1 (%2 were already in it)")
            .arg(m_queue.size() - before)
            .arg(m.size() - (m_queue.size() - before)));
}

void BulkExtractorTab::queueRemoveMatches()
{
    const ArchiveIndex& index = ArchiveIndex::instance();
    const int before = m_queue.size();
    for (const int i : matchedFiles()) m_queue.remove(index.files()[i].hash);
    saveQueue();
    updateQueueLabel();
    updateMatchCount();
    log(QStringLiteral("queue: removed %1").arg(before - m_queue.size()));
}

void BulkExtractorTab::queueClear()
{
    const int n = m_queue.size();
    m_queue.clear();
    saveQueue();
    updateQueueLabel();
    updateMatchCount();
    log(QStringLiteral("queue: cleared %1").arg(n));
}

void BulkExtractorTab::updateQueueLabel() { updateQueueLabel(counts()); }

void BulkExtractorTab::updateQueueLabel(const Counts& c)
{
    const int held = int(m_queue.size());
    const int present = c.queuePresent;
    if (held == 0) {
        m_queueLabel->setText(QStringLiteral("empty"));
    } else if (present == held) {
        m_queueLabel->setText(QStringLiteral("%1 file(s)").arg(held));
    } else {
        m_queueLabel->setText(
            QStringLiteral("%1 file(s) — %2 not in the current index")
                .arg(held)
                .arg(held - present));
    }
    m_queueRemove->setEnabled(held > 0);
    m_queueClear->setEnabled(held > 0);
}

// ── The run ────────────────────────────────────────────────────────────────

void BulkExtractorTab::log(const QString& line)
{
    if (m_console) m_console->appendPlainText(line);
}

void BulkExtractorTab::drainConsole()
{
    QStringList take;
    {
        QMutexLocker lock(m_pendingMx.get());
        take.swap(*m_pending);
    }
    for (const QString& l : take) m_console->appendPlainText(l);
}

void BulkExtractorTab::startExtraction()
{
    if (m_running.load()) return;
    const QString outRoot = m_outputDir->text().trimmed();
    if (outRoot.isEmpty()) {
        m_status->setText(QStringLiteral("Choose an output folder first."));
        return;
    }
    if (!QDir().mkpath(outRoot)) {
        m_status->setText(
            QStringLiteral("Could not create %1 — check the path and permissions.")
                .arg(QDir::toNativeSeparators(outRoot)));
        return;
    }
    // NOT in a harness run. "A harness run must not write the user's
    // settings" is stated for the export options and holds here too: a probe
    // invocation permanently repointed the remembered export folder.
    if (!m_harness) Config::setExportDir(outRoot);

    const QVector<int> matches = runSet();
    if (matches.isEmpty()) return;

    const fox::ExportOptions eo = fox::loadExportOptions();
    m_assembleFtex = eo.assembleFtex;
    auto cfg = std::make_shared<RunConfig>();
    cfg->outRoot = outRoot;
    cfg->layout = ExportLayout::mode();
    cfg->layoutRank = ExportLayout::needsTags(cfg->layout)
                          ? ExportLayout::rankOf(cfg->layout)
                          : QHash<QString, int>();
    cfg->assemble = eo.assembleFtex;
    cfg->convertWem = eo.convertWem;
    cfg->onlyNew = m_existing->currentData().toBool();

    m_manifest = std::make_shared<fox::BulkManifest>(outRoot);
    m_failures = std::make_shared<fox::BulkFailureLog>(outRoot);

    // Group container children by their (archiveId, entryIdx) so each container
    // decompresses once.
    const ArchiveIndex& index = ArchiveIndex::instance();
    QHash<quint64, int> unitOfContainer;
    auto units = std::make_shared<std::vector<WorkUnit>>();
    for (const int fi : matches) {
        const IndexedFile& f = index.files()[fi];
        if (f.childIdx < 0) {
            WorkUnit u;
            u.fileIdxs.append(fi);
            units->push_back(u);
        } else {
            const quint64 key =
                (static_cast<quint64>(f.archiveId) << 32) | static_cast<quint32>(f.entryIdx);
            auto it = unitOfContainer.find(key);
            if (it == unitOfContainer.end()) {
                WorkUnit u;
                u.container = true;
                u.fileIdxs.append(fi);
                unitOfContainer.insert(key, static_cast<int>(units->size()));
                units->push_back(u);
            } else {
                (*units)[it.value()].fileIdxs.append(fi);
            }
        }
    }

    m_running.store(true);
    m_cancel.store(false);
    m_paused.store(false);
    m_done.store(0);
    m_failed.store(0);
    m_skipped.store(0);
    m_degraded.store(0);
    m_total.store(matches.size());
    m_pausedMs = 0;
    m_pauseStart = -1;
    m_clock.start();
    m_progress->setRange(0, matches.size());
    m_progress->setValue(0);
    m_startBtn->setEnabled(false);
    m_pauseBtn->setEnabled(true);
    m_pauseBtn->setText(QStringLiteral("Pause"));
    m_cancelBtn->setEnabled(true);
    m_status->setText(QStringLiteral("Extracting…"));
    m_console->clear();
    m_poll->start();

    const int wanted = m_workers->value();
    const int threadCount =
        wanted > 0 ? wanted : qMax(2, QThread::idealThreadCount() - 1);
    m_threadsLeft.store(threadCount);
    auto cursor = std::make_shared<std::atomic<size_t>>(0);

    log(QStringLiteral("run: %1 file(s), %2 unit(s), %3 worker(s), %4, layout %5")
            .arg(matches.size())
            .arg(int(units->size()))
            .arg(threadCount)
            .arg(cfg->onlyNew ? QStringLiteral("only new") : QStringLiteral("overwrite"),
                 cfg->layout.isEmpty() ? QStringLiteral("flat") : cfg->layout));
    log(QStringLiteral("run: %1").arg(eo.describe()));

    // A tag layout over files the vocabulary does not cover files EVERYTHING
    // under _misc, and the option then looks like it did something it did not.
    // Measured before the run rather than discovered after it: the tag map
    // covers models, so a run over textures or audio under "by family" is one
    // folder with every file in it.
    if (ExportLayout::needsTags(cfg->layout)) {
        int tagged = 0;
        for (const int fi : matches)
            if (ExportLayout::tagFolderIn(cfg->layoutRank, fi)
                != QStringLiteral("_misc"))
                ++tagged;
        if (tagged == 0) {
            log(QStringLiteral(
                    "note: nothing in this run carries a \"%1\" tag, so the "
                    "grouping in Settings \u25b8 Export has nothing to group "
                    "by and every file lands in _misc. The tags describe "
                    "MODELS; a run over textures or audio is the usual reason.")
                    .arg(cfg->layout));
        } else if (tagged < matches.size()) {
            log(QStringLiteral("note: %1 of %2 file(s) carry no \"%3\" tag "
                               "and go to _misc")
                    .arg(int(matches.size()) - tagged)
                    .arg(int(matches.size()))
                    .arg(cfg->layout));
        }
    }
    qInfo("bulk: starting %d file(s) over %d worker(s)", int(matches.size()),
          threadCount);

    auto manifest = m_manifest;
    auto failures = m_failures;
    auto pending = m_pending;
    auto pendingMx = m_pendingMx;
    // The run-scoped decode cache (§8), owned by the TAB. A worker that owned
    // it released it as its lambda was destroyed, which happens AFTER the
    // m_threadsLeft decrement — so a rescan could begin with a scope still
    // open, and the summary could only ever report zero bytes held.
    m_cache = std::make_shared<extract::blobcache::Scope>();

    for (int t = 0; t < threadCount; ++t) {
        std::thread([this, units, cursor, cfg, manifest, failures, pending,
                     pendingMx] {
            const ArchiveIndex& idx = ArchiveIndex::instance();

            auto note = [pending, pendingMx](const QString& line) {
                QMutexLocker lock(pendingMx.get());
                // Bounded, because a run that fails on every file would
                // otherwise queue a million strings faster than the GUI drains
                // them. The console is a sample, not a transcript; the
                // transcript is _bulk_failed.txt.
                if (pending->size() < 400) pending->append(line);
            };
            // The output folder for one file, which is where the layout is
            // applied. Batch path: it applies (§8).
            auto dirFor = [cfg](int fileIdx, const IndexedFile& f) {
                const QString sub = ExportLayout::folderForFile(
                    cfg->layout, cfg->layoutRank, fileIdx, f);
                return sub.isEmpty() ? cfg->outRoot
                                     : QDir(cfg->outRoot).filePath(sub);
            };
            // Has this file already been written TO THE PLACE THIS RUN WOULD
            // PUT IT? The output path is what the ledger records, so a change of
            // layout or name template is new work rather than a silent skip.
            auto alreadyDone = [&](int fileIdx, const IndexedFile& f) {
                QString rel = extract::relativePathFor(f);
                if (cfg->assemble && extract::isFtex(f)) {
                    rel.chop(5);
                    rel += QStringLiteral(".dds");
                } else if (cfg->convertWem && rel.endsWith(QLatin1String(".wem"))) {
                    rel.chop(4);
                    rel += QStringLiteral(".wav");
                }
                const QString sub = ExportLayout::folderForFile(
                    cfg->layout, cfg->layoutRank, fileIdx, f);
                return manifest->has(f.hash,
                                     sub.isEmpty() ? rel
                                                   : sub + QLatin1Char('/') + rel);
            };
            // Everything that happens to one file once its bytes are in hand.
            auto writeOne = [&](int fileIdx, const IndexedFile& f,
                                const QByteArray& data) {
                QString rel = extract::relativePathFor(f);
                const QString dir = dirFor(fileIdx, f);
                QString err;
                bool ok = false;
                // Written, but not as the thing that was asked for. Counted,
                // logged with a reason, and deliberately NOT recorded in the
                // manifest, so a later run retries it.
                bool degraded = false;
                if (cfg->assemble && extract::isFtex(f)) {
                    QString aerr;
                    const QByteArray dds =
                        extract::assembleFtexToDds(f, &aerr, nullptr, false);
                    if (!dds.isEmpty()) {
                        rel.chop(5);   // ".ftex"
                        rel += QStringLiteral(".dds");
                        ok = writeOut(dir, rel, dds, &err);
                    } else if (!data.isEmpty()) {
                        // A texture whose streams are missing still has its own
                        // header worth keeping — write the raw .ftex rather
                        // than counting the file as a loss. But it is DEGRADED,
                        // and a degraded write must not go in the manifest:
                        // recorded as complete, "only new" would skip it for
                        // ever and the folder would stay stuck with header-only
                        // files even after the texture archive was mounted.
                        degraded = writeOut(dir, rel, data, &err);
                        if (degraded) {
                            const QString why = aerr.isEmpty()
                                ? QStringLiteral("mip streams not found")
                                : aerr;
                            note(QStringLiteral("raw .ftex (%1): %2")
                                     .arg(why, labelFor(f)));
                            failures->add(labelFor(f),
                                          QStringLiteral("wrote the raw .ftex "
                                                         "instead of a .dds: ")
                                              + why);
                        }
                    } else {
                        err = aerr.isEmpty() ? QStringLiteral("could not assemble")
                                             : aerr;
                    }
                } else if (!data.isEmpty() || f.size == 0) {
                    ok = writeMaybeAudio(dir, rel, data, cfg->convertWem, &err);
                } else {
                    err = QStringLiteral("read returned nothing");
                }
                if (degraded) {
                    m_degraded.fetch_add(1);
                    m_done.fetch_add(1);
                } else if (ok) {
                    // The key is the hash AND the path, so changing the folder
                    // layout or the name template correctly counts as new work.
                    manifest->record(f.hash, QDir(cfg->outRoot).relativeFilePath(
                                                 QDir(dir).filePath(rel)));
                    m_done.fetch_add(1);
                } else {
                    failures->add(labelFor(f),
                                  err.isEmpty() ? QStringLiteral("unknown") : err);
                    note(QStringLiteral("FAILED %1 — %2").arg(labelFor(f), err));
                    m_failed.fetch_add(1);
                }
            };

            while (!m_cancel.load()) {
                // PAUSE. Checked before taking work, so a paused run stops
                // between files rather than mid-write, and the sleep is short
                // enough that Resume and Cancel both feel immediate.
                while (m_paused.load() && !m_cancel.load()) QThread::msleep(50);
                if (m_cancel.load()) break;

                const size_t u = cursor->fetch_add(1);
                if (u >= units->size()) break;
                const WorkUnit& unit = (*units)[u];

                if (unit.container) {
                    // Decompress the container ONCE, then pull every matched
                    // child. Through the run cache, so a container that also
                    // holds textures is not inflated again by the assembler.
                    const IndexedFile& first = idx.files()[unit.fileIdxs.first()];
                    const QByteArray blob = extract::blobcache::readEntry(
                        first.archiveId, first.entryIdx);
                    fox::FpkFile fpk;
                    fox::PftxsFile pftxs;
                    const bool isPftxs = fox::PftxsFile::isPftxs(blob);
                    const bool isSbp = fox::SbpFile::isSbp(blob);
                    QVector<fox::SbpWem> sbpWems;
                    if (isSbp) sbpWems = fox::SbpFile::listWems(blob);
                    const bool parsed = !blob.isEmpty()
                        && (isSbp ? !sbpWems.isEmpty()
                                  : isPftxs ? pftxs.parse(blob)
                                            : fpk.parse(blob));
                    if (!parsed) {
                        for (const int fi : unit.fileIdxs) {
                            const IndexedFile& f = idx.files()[fi];
                            failures->add(labelFor(f),
                                          blob.isEmpty()
                                              ? QStringLiteral("container unreadable")
                                              : QStringLiteral("container unparsable"));
                        }
                        note(QStringLiteral("FAILED container holding %1 file(s)")
                                 .arg(unit.fileIdxs.size()));
                        m_failed.fetch_add(unit.fileIdxs.size());
                        continue;
                    }
                    for (const int fi : unit.fileIdxs) {
                        if (m_cancel.load()) break;
                        while (m_paused.load() && !m_cancel.load())
                            QThread::msleep(50);
                        // AGAIN, because the pause loop above exits when cancel
                        // is set — without this each worker wrote one more file
                        // after "Cancelling…" appeared.
                        if (m_cancel.load()) break;
                        const IndexedFile& f = idx.files()[fi];
                        if (cfg->onlyNew && alreadyDone(fi, f)) {
                            m_skipped.fetch_add(1);
                            continue;
                        }
                        QByteArray data;
                        if (isSbp) {
                            if (f.childIdx < sbpWems.size())
                                data = fox::SbpFile::readWem(blob, sbpWems[f.childIdx]);
                        } else if (isPftxs) {
                            int flat = 0;
                            for (const fox::PftxsGroup& g : pftxs.groups()) {
                                if (f.childIdx < flat + g.entries.size()) {
                                    data = fox::PftxsFile::readEntry(
                                        blob, g.entries[f.childIdx - flat]);
                                    break;
                                }
                                flat += g.entries.size();
                            }
                        } else if (f.childIdx < fpk.entries().size()) {
                            data = fox::FpkFile::readEntry(blob, fpk.entries()[f.childIdx]);
                        }
                        writeOne(fi, f, data);
                    }
                } else {
                    const int fi = unit.fileIdxs.first();
                    const IndexedFile& f = idx.files()[fi];
                    if (cfg->onlyNew && alreadyDone(fi, f)) {
                        m_skipped.fetch_add(1);
                        continue;
                    }
                    writeOne(fi, f, idx.readFile(f));
                }
            }
            m_threadsLeft.fetch_sub(1);
        }).detach();
    }
}

void BulkExtractorTab::cancelExtraction()
{
    m_cancel.store(true);
    // A paused worker is asleep in its own loop and would never see the cancel.
    // Releasing the pause first is what makes Cancel work WHILE paused, which
    // is exactly when someone reaches for it.
    if (m_paused.load()) {
        m_paused.store(false);
        if (m_pauseStart >= 0) {
            m_pausedMs += m_clock.elapsed() - m_pauseStart;
            m_pauseStart = -1;
        }
        m_pauseBtn->setText(QStringLiteral("Pause"));
    }
    m_pauseBtn->setEnabled(false);
    m_status->setText(QStringLiteral("Cancelling…"));
}

void BulkExtractorTab::togglePause()
{
    if (!m_running.load()) return;
    if (m_paused.load()) {
        m_paused.store(false);
        if (m_pauseStart >= 0) {
            // PAUSED TIME LEAVES THE ETA (§8). Without this the estimate is
            // computed against wall-clock time that no work happened in, so a
            // coffee break makes the tool claim the run got slower.
            m_pausedMs += m_clock.elapsed() - m_pauseStart;
            m_pauseStart = -1;
        }
        m_pauseBtn->setText(QStringLiteral("Pause"));
        log(QStringLiteral("run: resumed"));
    } else {
        m_paused.store(true);
        m_pauseStart = m_clock.elapsed();
        m_pauseBtn->setText(QStringLiteral("Resume"));
        log(QStringLiteral("run: paused"));
    }
}

// Everything that has to happen when a run stops, for ANY reason: the ledgers
// written, the decode cache released. Called from the poll timer's terminal
// block and from the destructor — because the timer needs one more 150 ms tick
// to notice the workers are gone, and closing the window does not give it one.
// Before this existed, closing the app 40% into a 50,000-file run threw away
// the ledger for every file already on disk, so "only new" had nothing to skip
// and the next run started from zero.
void BulkExtractorTab::finishLedgers(bool completed)
{
    if (m_manifest) {
        QString saveErr;
        if (!m_manifest->save(&saveErr))
            qWarning("bulk: could not write the manifest — %s",
                     qUtf8Printable(saveErr));
    }
    if (m_failures && !m_failures->flush(completed))
        qWarning("bulk: could not write _bulk_failed.txt");
    m_cache.reset();
}

void BulkExtractorTab::pollProgress()
{
    drainConsole();
    // CHECKPOINT. A run long enough to be worth resuming is long enough to be
    // interrupted, so the ledger is written as it goes rather than only at the
    // end. Every ~6 s, and only when something has actually been recorded.
    if (m_running.load() && m_manifest && m_manifest->dirty()
        && ++m_saveTick >= 40) {
        m_saveTick = 0;
        m_manifest->save();
        // Bounded: once per checkpoint interval, so a long run says in the log
        // that its ledger is being kept up to date rather than leaving "would
        // a crash here lose everything" unanswerable.
        qInfo("bulk: manifest checkpointed at %d of %d",
              m_done.load() + m_failed.load() + m_skipped.load(),
              m_total.load());
    }
    const int done = m_done.load() + m_failed.load() + m_skipped.load();
    m_progress->setValue(done);

    // Active time: the wall clock, less every completed pause AND the one in
    // progress. Reading the in-progress pause here rather than accumulating it
    // is what stops the estimate drifting while the run is stopped.
    qint64 paused = m_pausedMs;
    if (m_pauseStart >= 0) paused += m_clock.elapsed() - m_pauseStart;
    const qint64 active = qMax(qint64(0), m_clock.elapsed() - paused);

    if (m_threadsLeft.load() > 0 && !m_cancel.load()) {
        // SKIPS ARE NOT WORK. A skip is one hash lookup; a write is a
        // decompress and a disk write. Counting both in the rate made a
        // second "only new" run over 100,000 files report "about 0s left" for
        // the ten minutes of real extraction that followed the skips, and made
        // the progress bar jump to 99% in five seconds and then crawl.
        const int worked = m_done.load() + m_failed.load();
        QString eta = QStringLiteral("—");
        if (worked > 0 && active > 500 && !m_paused.load()) {
            const qint64 remaining =
                qMax(qint64(0), qint64(m_total.load() - done));
            eta = humanTime(remaining * active / worked);
        }
        // In a harness run the status LINE is the thing under test — the ETA
        // arithmetic has no other observable output — so it goes to the log,
        // every poll tick, because a harness run is short and a line that never
        // fires proves nothing.
        if (m_harness && ++m_statusTick >= 1) {
            m_statusTick = 0;
            qInfo("bulk-status: %d/%d done, %d worked, eta %s",
                  done, m_total.load(), worked, qUtf8Printable(eta));
        }
        m_status->setText(
            m_paused.load()
                ? QStringLiteral("Paused at %1 / %2 (%3 failed, %4 skipped)")
                      .arg(done).arg(m_total.load())
                      .arg(m_failed.load()).arg(m_skipped.load())
                : QStringLiteral("Extracting… %1 / %2 (%3 failed, %4 skipped) — "
                                 "%5 elapsed, about %6 left")
                      .arg(done).arg(m_total.load())
                      .arg(m_failed.load()).arg(m_skipped.load())
                      .arg(humanTime(active), eta));
        return;
    }
    if (m_threadsLeft.load() > 0) return;   // cancelled, workers still draining

    m_poll->stop();
    drainConsole();
    m_running.store(false);
    m_startBtn->setEnabled(true);
    m_pauseBtn->setEnabled(false);
    m_cancelBtn->setEnabled(false);

    // The cache's numbers must be read BEFORE the scope is released, which
    // finishLedgers does.
    const extract::blobcache::Stats cs = extract::blobcache::stats();
    const int failedCount = m_failures ? m_failures->count() : 0;
    finishLedgers(!m_cancel.load());
    if (cs.hits + cs.misses > 0)
        log(QStringLiteral("cache: %1 hit(s), %2 miss(es), %3 MB held")
                .arg(cs.hits).arg(cs.misses)
                .arg(cs.bytes / (1024.0 * 1024.0), 0, 'f', 1));

    const QString summary =
        m_cancel.load()
            ? QStringLiteral("Cancelled after %1 file(s): %2 written, %3 failed, "
                             "%4 skipped.")
                  .arg(done).arg(m_done.load()).arg(m_failed.load()).arg(m_skipped.load())
            : QStringLiteral("Done: %1 written, %2 failed, %3 skipped, in %4.")
                  .arg(m_done.load()).arg(m_failed.load()).arg(m_skipped.load())
                  .arg(humanTime(active));
    m_status->setText(summary);
    log(summary);
    if (failedCount > 0)
        log(QStringLiteral("%1 failure(s) listed in %2")
                .arg(failedCount)
                .arg(QDir::toNativeSeparators(m_failures->path())));

    // The ACTIVE time, not the wall clock: a run that was paused for two
    // minutes did not get slower, and the log is where that is checked.
    qInfo("bulk: %d written (%d degraded), %d failed, %d skipped in %lld ms "
          "active (%lld ms paused)%s",
          m_done.load(), m_degraded.load(), m_failed.load(), m_skipped.load(),
          active, paused, m_cancel.load() ? " (cancelled)" : "");
    if (cs.hits + cs.misses > 0)
        qInfo("bulk: cache %lld hit(s), %lld miss(es), %d entr(ies)", cs.hits,
              cs.misses, cs.entries);

    // Announced through the shared notifier, like every other export path, so
    // "what did that write and where" has one answer everywhere (§9).
    if (m_done.load() > 0)
        fox::ExportNotifier::instance().notify(
            summary, QDir(m_outputDir->text().trimmed()).absolutePath());

    updateQueueLabel();
    emit runFinished(m_done.load(), m_failed.load(), m_skipped.load());
}
