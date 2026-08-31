#include "app/MainWindow.h"

#include <algorithm>

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QLocale>
#include <QMessageBox>
#include <QScreen>
#include <QStatusBar>
#include <QToolButton>
#include <QDateTime>
#include <QThread>
#include <QTimer>
#include <QTabWidget>
#include <QSet>
#include <QTextStream>
#include <QUrl>

#include "app/AppLog.h"
#include "app/AppPaths.h"
#include <QLineEdit>

#include "app/Config.h"
#include "app/ExportNotifier.h"
#include <QElapsedTimer>
#include "app/Hotkeys.h"
#include "util/ExportActions.h"
#include "app/StatusLine.h"
#include "app/SettingsDialog.h"
#include "util/Extract.h"
#include "util/ModFolder.h"
#include "app/MgsvMetaDialog.h"
#include "util/ModPackage.h"
#include "fox/FtexFile.h"
#include "fox/FtexWriter.h"
#include "anim/RigBind.h"
#include "index/AnimCatalog.h"
#include "index/ArchiveIndex.h"
#include "gl/GLModelWidget.h"
#include "app/LogConsole.h"
#include "export/ExportOptions.h"
#include "export/ViewCapture.h"
#include "view/ViewportBar.h"
#include <QMouseEvent>
#include "view/ViewportGizmo.h"
#include "view/ViewportPanel.h"
#include "gl/ThumbnailRenderer.h"
#include "index/ModelTags.h"
#include "index/TextureUsers.h"
#include "index/EquipCatalog.h"
#include "fox/DfrmFile.h"
#include "fox/FmdlFile.h"
#include "fox/FovaFile.h"
#include "fox/FoxMaterial.h"
#include "fox/FoxHash.h"
#include "index/CharacterCatalog.h"
#include "index/IconCatalog.h"
#include "index/LayerColors.h"
#include "index/MaterialPresets.h"
#include "index/MgoGearConfig.h"
#include "fox/GrxlaFile.h"
#include "index/MechaCatalog.h"
#include "preview/ModelLoader.h"
#include "index/AvatarPresets.h"
#include "index/AvatarTextures.h"
#include "index/PlayerCatalog.h"
#include "index/WeaponCatalog.h"
#include "tabs/BulkExtractorTab.h"
#include "tabs/CustomizeTab.h"
#include "tabs/FilesTab.h"
#include "tabs/ModelsTab.h"
#include "view/StringsPanel.h"
#include "tabs/TexturesTab.h"

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("FOX Asset Browser"));
    resize(1280, 800);

    m_tabs = new QTabWidget(this);
    m_filesTab = new FilesTab(m_tabs);
    m_texturesTab = new TexturesTab(m_tabs);
    m_modelsTab = new ModelsTab(m_tabs);
    m_customizeTab = new CustomizeTab(m_tabs);
    m_bulkTab = new BulkExtractorTab(m_tabs);
    m_tabs->addTab(m_filesTab, QStringLiteral("Files"));
    m_tabs->addTab(m_texturesTab, QStringLiteral("Textures"));
    // Beside Textures rather than under Files: the two are the same kind of
    // thing — decoded content the archives carry, browsed for its own sake —
    // and the string tables are what turn an id into the name the game shows.
    // The per-game checkboxes are drawn in two tabs and set ONE global filter.
    // Without this the other tab keeps showing the old ticks and then silently
    // filters its list to match the new state on the next keystroke. One
    // direction only now: the Models tab's game switches became tags in its own
    // Filter popup and are LOCAL to that list, so it no longer writes the
    // app-wide filter and has nothing to announce.
    connect(m_texturesTab, &TexturesTab::gameFilterChanged, m_modelsTab,
            &ModelsTab::syncGameFilter);
    // ASSOCIATED MODELS → the Models tab (§7). The texture panel knows the
    // model's path and nothing about tabs; the window owns the tabs and knows
    // nothing about textures. This is the one line that joins them.
    connect(m_texturesTab, &TexturesTab::openModelRequested, this,
            [this](const QString& path) {
                if (!m_modelsTab) return;
                m_tabs->setCurrentWidget(m_modelsTab);
                // This REPLACES the Models tab's filter, which is what makes
                // the jump work — but it must not end up in the user's saved
                // search history, which is theirs and is offered back on the
                // down arrow. selectModel goes through the quiet setter.
                const QString stem = path.section(QLatin1Char('/'), -1);
                if (m_modelsTab->selectModel(stem))
                    // SAID OUT LOUD: the jump replaces whatever filter the
                    // Models tab was showing, and a search box that changed
                    // under you without a word is the kind of thing people
                    // report as the tool losing their work.
                    m_statusLabel->setText(
                        QStringLiteral("Models filtered to \"%1\" — clear the "
                                       "search box to go back.")
                            .arg(stem));
                else
                    m_statusLabel->setText(
                        QStringLiteral("%1 is not in the model list — it may be "
                                       "filtered out, or its game switched off.")
                            .arg(stem));
            });
    connect(m_texturesTab, &TexturesTab::unnamedModelActivated, this,
            [this](quint64 hash) {
                m_statusLabel->setText(
                    QStringLiteral("That model has no name in any loaded "
                                   "dictionary — it is only known as %1. "
                                   "Search the Models tab for that hash.")
                        .arg(hash, 16, 16, QLatin1Char('0')));
            });
    m_tabs->addTab(m_modelsTab, QStringLiteral("Models"));
    m_tabs->addTab(m_customizeTab, QStringLiteral("Customize"));
    m_tabs->addTab(m_bulkTab, QStringLiteral("Bulk Extract"));
    setCentralWidget(m_tabs);
    connect(m_tabs, &QTabWidget::currentChanged, this,
            [this](int) { syncTabStatus(); });

    m_statusLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel, 1);

    // ── The per-tab status line (template §15) ──────────────────────────
    // Each viewport tab used to carry its own QLabel under the model — two
    // permanent lines of window height for a message that matters for about
    // four seconds after a load. This is the same message, in the bar the
    // window already has for exactly this kind of thing.
    //
    // Filtered by SOURCE: a report is shown only while the widget that made it
    // is inside the current tab, so switching tabs cannot leave the Models
    // tab's last load sitting under the Customize tab.
    m_tabStatusLabel = new QLabel(this);
    m_tabStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_tabStatusLabel->setTextFormat(Qt::PlainText);
    statusBar()->addWidget(m_tabStatusLabel, 3);
    connect(&fox::StatusLine::instance(), &fox::StatusLine::reported, this,
            [this](QWidget* source, const QString& text) {
                m_tabStatusSource = source;
                m_tabStatusText = text;
                syncTabStatus();
            });

    // ── One export report for the whole application (template §9) ───────
    // Every export path calls ExportNotifier::notify(); this is the only
    // thing listening. Before it, the four tabs reported an export four
    // different ways — a status line here, a label under the builder, a
    // silent success in the preview pane, nothing at all from the shared
    // context-menu actions — and none of them told you where the file went.
    //
    // A status-bar message with a persistent "Show in folder" button rather
    // than a modal: an export is a thing that SUCCEEDED, and a dialog for
    // that makes the fifteenth one in a row a chore. The button replaces
    // itself on each export and hides when the message ages out, so it can
    // never open the folder of an export two exports ago.
    m_exportShowBtn = new QToolButton(this);
    m_exportShowBtn->setText(QStringLiteral("Show in folder"));
    m_exportShowBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_exportShowBtn->setAutoRaise(true);
    m_exportShowBtn->hide();
    statusBar()->addPermanentWidget(m_exportShowBtn);
    connect(m_exportShowBtn, &QToolButton::clicked, this, [this] {
        if (!m_exportFolder.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_exportFolder));
    });
    connect(&fox::ExportNotifier::instance(), &fox::ExportNotifier::exported,
            this, [this](const QString& text, const QString& folder) {
                m_exportFolder = folder;
                m_exportShowBtn->setVisible(!folder.isEmpty());
                statusBar()->showMessage(text, 15000);
                qInfo("export: %s", qUtf8Printable(text));
            });
    // showMessage's timeout hides the text but knows nothing about the
    // button beside it, which would otherwise stay offering a folder whose
    // message is long gone.
    connect(statusBar(), &QStatusBar::messageChanged, this,
            [this](const QString& m) {
                if (m.isEmpty() && m_exportShowBtn) m_exportShowBtn->hide();
            });

    auto& index = fox::ArchiveIndex::instance();
    connect(&index, &fox::ArchiveIndex::progress, this,
            [this](const QString& m) { m_statusLabel->setText(m); });
    connect(&index, &fox::ArchiveIndex::readyChanged, this, [this, &index](bool ready) {
        if (ready) {
            // Built here rather than lazily in the tab, so a rescan can never
            // leave a combo holding swatch hashes from the previous index.
            fox::LayerColorCatalog::instance().build();
            qInfo("layer: %s",
                  qUtf8Printable(fox::LayerColorCatalog::instance().note()));
            fox::MaterialPresetTable::instance().build();
            qInfo("fmtt: %s",
                  qUtf8Printable(fox::MaterialPresetTable::instance().note()));
            // Counts a person reads: grouped digits, and singulars that are
            // singular. "1 archives — 3510 files" was the line every session
            // started at.
            const QLocale loc;
            // Grouped digits and singulars that are singular — for EVERY
            // count on the line. Fixing only the first one left "1 archive —
            // 1 files", which is the same sentence half-corrected.
            const auto count = [&loc](qlonglong n, const char* one,
                                      const char* many) {
                return QStringLiteral("%1 %2").arg(
                    loc.toString(n),
                    QLatin1String(n == 1 ? one : many));
            };
            m_statusLabel->setText(
                QStringLiteral("%1 — %2 (%3, %4%5)")
                    .arg(count(index.archives().size(), "archive", "archives"),
                         count(index.files().size(), "file", "files"),
                         QStringLiteral("%1 named")
                             .arg(loc.toString(qlonglong(index.namedCount()))),
                         count(index.dictionaryEntries(), "dictionary entry",
                               "dictionary entries"),
                         index.deepScanned()
                             ? QStringLiteral(", containers indexed")
                             : QString()));
        }
        // The texture→model map is keyed on file CONTENT, not on file indices,
        // but it was built from the previous index and a rebuild can have added
        // or removed models — so it is dropped and swept again, in the
        // background, like every other derived catalogue here.
        fox::TextureUsers::instance().reset();
        m_filesTab->onIndexReady(ready);
        m_texturesTab->onIndexReady(ready);
        // The string tables are FILES in the index like anything else, so the
        // list of them is stale the moment the index is replaced — and a stale
        // one reads as "this install has no text", which is the one conclusion
        // this tab exists to make reliable.

        m_modelsTab->onIndexReady(ready);
        m_customizeTab->onIndexReady(ready);
        m_bulkTab->onIndexReady(ready);
        if (ready && m_shotPending) takeDevShot();
    });

    buildMenus();

    // Installing or reverting a replacement has to end in a rescan, and what a
    // rescan IS lives here — the bulk-extract guard and the texture-cache drop
    // are part of it. The action sites ask through this hook rather than each
    // calling ArchiveIndex::rebuild themselves, which would be three copies of
    // a rescan that could each forget a different half of it.
    modfolder::setChangedHook([this] { startRebuild(); });

    if (Config::gameDirs().isEmpty()) {
        m_statusLabel->setText(
            QStringLiteral("No game folder configured — File → Set game folder…"));
    } else {
        startRebuild();
    }
}

void MainWindow::buildMenus()
{
    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(QStringLiteral("Set &game folder…"), this,
                    &MainWindow::chooseGameFolder);
    file->addAction(QStringLiteral("&Settings…"),
                    QKeySequence(Qt::CTRL | Qt::Key_Comma), this,
                    [this] { openSettings(QString()); });

    // ── File ▸ Index (docs/CONTEXT_MENUS.md §6) ─────────────────────────
    // A SUBMENU rather than one action, because each index reports its own
    // state and can be rebuilt on its own. "Rescan archives" was the whole of
    // it, which told you nothing about the three indexes that hang off the
    // archive scan and take longer than it does.
    m_indexMenu = file->addMenu(QStringLiteral("&Index"));
    // Qt hides per-action tooltips in menus by default, so without this the
    // explanatory text below is simply never shown. §6 lists this as a bug
    // that was actually shipped.
    m_indexMenu->setToolTipsVisible(true);
    connect(m_indexMenu, &QMenu::aboutToShow, this,
            &MainWindow::populateIndexMenu);

    // Ctrl+L, kept from where this used to live in Help: everything this tool
    // measures it says in the log, and the binding people already know should
    // not change because the item moved menus.
    file->addAction(QStringLiteral("Toggle &console window"),
                    QKeySequence(Qt::CTRL | Qt::Key_L), this,
                    [this] { toggleLogConsole(); });
    file->addSeparator();
    file->addAction(QStringLiteral("E&xit"), this, &QWidget::close);

    // Export: rebuilt on every open so it always reflects the current tab and
    // selection (the contextual export vocabulary lives in each tab +
    // util/ExportActions — one implementation everywhere).
    m_exportMenu = menuBar()->addMenu(QStringLiteral("&Export"));
    connect(m_exportMenu, &QMenu::aboutToShow, this,
            &MainWindow::populateExportMenu);

    applyHotkeys();
    // The viewport's right-click "Export settings…" opens Settings ▸ Export,
    // because that is where those options live now (template §10). Installed
    // here because export/ must not reach into app/ — see ViewCapture.h.
    fox::setExportSettingsOpener(
        [this] { openSettings(QStringLiteral("Export")); });

    QMenu* help = menuBar()->addMenu(
        // NO MNEMONIC. "&Help" claims Alt+H, and Alt+H is the viewport's
        // SHOW-EVERY-PART key — measured: Qt resolves the pair as an ambiguous
        // shortcut overload and fires NEITHER, so the menu did not open and
        // nothing was revealed. Help is one click away and is also reachable
        // with Alt then the arrow keys; the viewport key is the one with no
        // alternative. (It was the isolate key when this was written; the two
        // swapped to match Blender, and the collision followed Alt+H.)
        QStringLiteral("Help"));
    // The log console lives in FILE now, as §6's "Toggle console window". It
    // was here as well for one build, which is two menu items for one window —
    // and they did not even agree: this one only ever showed it, so pressing it
    // with the console already up did nothing at all. Ctrl+L still reaches it;
    // the binding moved with the action.
    // ── §6's post-patch triage set ──────────────────────────────────────
    // This menu is what someone opens after a game update, when something that
    // worked yesterday does not. Every entry below answers "what broke?" or
    // "how do I tell someone what broke?".
    {
        // TWO shortcuts, one action. setShortcuts() takes a list; F1 is the
        // convention and "?" is what people actually press.
        QAction* keys = help->addAction(QStringLiteral("&Shortcuts…"), this,
                                        [this] { showShortcutSheet(); });
        keys->setShortcuts({QKeySequence(Qt::Key_F1),
                            QKeySequence(Qt::SHIFT | Qt::Key_Slash)});
        keys->setToolTip(QStringLiteral(
            "The viewport's keys, which are otherwise invisible."));
    }
    help->addSeparator();
    {
        QAction* audit = help->addAction(QStringLiteral("&Health check…"), this,
                                         [this] { runHealthCheck(); });
        audit->setToolTip(QStringLiteral(
            "Walk every model in the configured folders and report whether this "
            "install can actually draw it. Minutes on a full install; the first "
            "thing to run after a game patch."));
    }
    help->addSeparator();
    // Copy AND Export, deliberately both: pasting a log into a bug report or a
    // chat is the common case, and writing a file only to attach it is pure
    // friction. Each reports WHAT it produced.
    help->addAction(QStringLiteral("&Copy log to clipboard"), this, [this] {
        const QStringList lines = AppLog::tail();
        const QString text = lines.join(QLatin1Char('\n'));
        QApplication::clipboard()->setText(text);
        showStatus(QStringLiteral("Log copied to clipboard — %1 lines, %2")
                      .arg(QLocale().toString(lines.size()),
                           QLocale().formattedDataSize(text.size())));
    });
    help->addAction(QStringLiteral("&Export log…"), this,
                    [this] { exportLog(); });
    help->addAction(QStringLiteral("Copy &diagnostic info"), this,
                    [this] { copyDiagnosticInfo(); });
    help->addSeparator();
    help->addAction(QStringLiteral("Open &data folder"), this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(AppPaths::dataDir()));
    });
    help->addAction(QStringLiteral("Open log &folder"), this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(AppPaths::dataDir()));
    });
    help->addAction(QStringLiteral("&About"), this, [this] {
        QMessageBox::about(
            this, QStringLiteral("FOX Asset Browser"),
            QStringLiteral(
                "<b>FOX Asset Browser</b> v%1<br>"
                "Native C++/Qt browser and bulk extractor for Fox Engine archives "
                "(MGSV: The Phantom Pain, Ground Zeroes, Metal Gear Survive).<br><br>"
                "Format references: GzsTool (Atvaark), FtexTool (Atvaark), "
                "mgsv-lookup-strings dictionaries (kapuragu et al.).<br>"
                "Log: %2")
                .arg(QApplication::applicationVersion(), AppLog::filePath()));
    });
}

// The settings dialog, and everything that has to be re-read after it. A
// method rather than a lambda in the menu because the hotkey registry opens
// the same thing, and two copies of "what to re-read on accept" is how one of
// them comes to be missing a line.
void MainWindow::openSettings(const QString& startTab)
{
    // What the INDEX is built from, before and after. A rescan re-reads every
    // configured folder and renumbers the file table under every open tab —
    // seconds on a real install — so it has to be a consequence of changing
    // one of these three, not of pressing OK. The viewport's right-click
    // "Export settings…" opens this same dialog, and made changing the GIF
    // size cost a full re-scan.
    const QStringList wasDirs = Config::gameDirs();
    const QString wasDict = Config::dictDir();
    const bool wasDeep = Config::deepScan();

    SettingsDialog dlg(this);
    if (!startTab.isEmpty()) dlg.showTab(startTab);
    if (dlg.exec() != QDialog::Accepted) return;
    // The PBR switches are read from each viewport itself, which is
    // only initialised from the setting once at construction. Without this,
    // changing the setting did nothing for the rest of the session — the
    // rescan below does not touch it.
    m_modelsTab->syncPbrFromSettings();
    m_customizeTab->syncPbrFromSettings();
    // A rebinding takes effect now, not at the next launch.
    applyHotkeys();
    if (Config::gameDirs() != wasDirs || Config::dictDir() != wasDict
        || Config::deepScan() != wasDeep)
        startRebuild();
}

// ── The rebindable shortcuts (template §11) ─────────────────────────────────
// ONE table (app/Hotkeys.h) read here and written by Settings ▸ Hotkeys, so
// adding a shortcut is adding a row there and nothing else. Re-runnable: the
// settings dialog calls this on accept, so a rebinding takes effect without a
// restart. The QActions are owned by the window and kept in m_hotkeyActions so
// a re-apply replaces them rather than stacking a second set on top — two
// QActions with the same sequence give Qt an ambiguous shortcut and NEITHER
// fires, which reads as the hotkey having stopped working.

// --variantcensus: the naming vocabulary "Variants ▸" rests on, counted
// against the real archives.
//
// The menu groups assets by folder + extension + variantStemOf(). If that
// grouping is right, the census shows many small groups whose members differ
// only in a variant token. If it is wrong it shows either one enormous group
// (the stem is stripping too much) or none at all (too little), and both are
// visible in the first ten lines of the output.
void MainWindow::writeVariantCensus(const QString& tsvPath)
{
    const auto& files = fox::ArchiveIndex::instance().files();
    QHash<QString, QVector<int>> groups;
    for (int i = 0; i < files.size(); ++i) {
        if (files[i].path.isEmpty()) continue;
        const QString key = files[i].path.section(QLatin1Char('/'), 0, -2)
            + QLatin1Char('\x1f') + fox::ArchiveIndex::extensionOf(files[i])
            + QLatin1Char('\x1f') + exportactions::variantStemOf(files[i].path);
        groups[key].append(i);
    }
    QVector<QPair<int, QString>> bySize;
    bySize.reserve(groups.size());
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it)
        bySize.append({int(it.value().size()), it.key()});
    // Sorted, and the tie broken on the KEY: a QHash yields its keys in a
    // different order per run, and a census that differs between two runs of
    // one build cannot be used as evidence of anything (convention 11).
    std::sort(bySize.begin(), bySize.end(),
              [](const QPair<int, QString>& a, const QPair<int, QString>& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second < b.second;
              });

    QFile out(tsvPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning("variantcensus: cannot write %s", qUtf8Printable(tsvPath));
        return;
    }
    QTextStream ts(&out);
    ts << "members\tstem\text\tfolder\tnames\n";
    int singles = 0, grouped = 0, biggest = 0;
    for (const auto& g : bySize) {
        const QVector<int>& idxs = groups[g.second];
        if (idxs.size() == 1) { ++singles; continue; }
        ++grouped;
        biggest = qMax(biggest, int(idxs.size()));
        QStringList names;
        for (int i : idxs) {
            QString n = files[i].path.section(QLatin1Char('/'), -1);
            const int d = n.indexOf(QLatin1Char('.'));
            if (d > 0) n.truncate(d);
            names << n;
        }
        names.sort();
        const QStringList key = g.second.split(QLatin1Char('\x1f'));
        ts << idxs.size() << '\t' << (key.size() > 2 ? key[2] : QString())
           << '\t' << (key.size() > 1 ? key[1] : QString())
           << '\t' << key.value(0) << '\t' << names.join(QLatin1Char(' ')) << '\n';
    }
    qInfo("variantcensus: %d group(s) with 2+ members, %d singleton(s), "
          "largest group %d — %s",
          grouped, singles, biggest, qUtf8Printable(tsvPath));
}

// The asset-health audit, as a METHOD rather than a block inside the devshot
// path. Two callers now — `--healthaudit <tsv>` and Help â¸ Health check… —
// and §6 wants the second one, because "walk every model and tell me what this
// install can no longer draw" is the first question after a game patch and it
// should not require a command line.
int MainWindow::writeHealthAudit(const QString& tsvPath)
{
    const fox::ArchiveIndex& index = fox::ArchiveIndex::instance();
    const auto& files = index.files();
    struct Row { QString path; QString state; QString detail; };
    QVector<Row> rows;
    QHash<QString, int> tally;
    for (int i = 0; i < files.size(); ++i) {
        const fox::IndexedFile& f = files[i];
        if (!f.path.endsWith(QLatin1String(".fmdl"), Qt::CaseInsensitive))
            continue;
        Row r;
        r.path = f.path;
        const QByteArray data = index.readFile(f);
        fox::FmdlFile m;
        if (data.isEmpty()) {
            r.state = QStringLiteral("no-data");
            r.detail = QStringLiteral("the entry read back empty");
        } else if (!m.parse(data)) {
            r.state = QStringLiteral("unparsed");
            r.detail = m.errorString();
        } else {
            int tris = 0;
            for (const fox::FmdlMesh& mm : m.meshes())
                tris += mm.triangles.size() / 3;
            if (m.meshes().isEmpty() || tris == 0) {
                r.state = QStringLiteral("no-geometry");
                r.detail = QStringLiteral("%1 mesh(es), %2 bone(s)")
                               .arg(m.meshes().size())
                               .arg(m.bones().size());
            } else {
                // How many of its texture references point at a file this
                // install actually carries. Resolution, not decoding — a
                // reference that resolves can still fail to decode, and
                // that is a different report from this one.
                int want = 0, got = 0;
                for (const fox::FmdlMaterialInstance& mi : m.materials()) {
                    for (const fox::FmdlTextureRef& t : mi.textures) {
                        ++want;
                        if (t.pathHash && index.findByHash(t.pathHash)) ++got;
                    }
                }
                if (want == 0) {
                    r.state = QStringLiteral("no-textures");
                    r.detail = QStringLiteral("%1 tri, declares none")
                                   .arg(tris);
                } else if (got == 0) {
                    r.state = QStringLiteral("textures-missing");
                    r.detail = QStringLiteral("%1 tri, 0 of %2 resolve")
                                   .arg(tris).arg(want);
                } else if (got < want) {
                    r.state = QStringLiteral("textures-partial");
                    r.detail = QStringLiteral("%1 tri, %2 of %3 resolve")
                                   .arg(tris).arg(got).arg(want);
                } else {
                    r.state = QStringLiteral("ok");
                    r.detail = QStringLiteral("%1 tri, %2 texture(s)")
                                   .arg(tris).arg(want);
                }
            }
        }
        tally[r.state]++;
        rows.append(r);
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.path < b.path; });

    // THE DIFF, against whatever the file already holds. Read BEFORE the
    // write, obviously, and tolerant of a missing or half-written previous
    // run — the first audit has nothing to compare against and must say so
    // rather than reporting every model as "newly working".
    QHash<QString, QString> before;
    bool hadPrevious = false;
    {
        QFile prev(tsvPath);
        if (prev.open(QIODevice::ReadOnly | QIODevice::Text)) {
            hadPrevious = true;
            QTextStream in(&prev);
            while (!in.atEnd()) {
                const QStringList c =
                    in.readLine().split(QLatin1Char('\t'));
                if (c.size() >= 2 && c[0] != QLatin1String("path"))
                    before.insert(c[0], c[1]);
            }
        }
    }

    QFile out(tsvPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning("devshot: cannot write %s", qUtf8Printable(tsvPath));
    } else {
        QTextStream ts(&out);
        ts << "path\tstate\tdetail\n";
        for (const Row& r : rows)
            ts << r.path << '\t' << r.state << '\t' << r.detail << '\n';
        out.close();
    }

    QStringList states = tally.keys();
    std::sort(states.begin(), states.end());
    QStringList summary;
    for (const QString& k : states)
        summary << QStringLiteral("%1 %2").arg(tally[k]).arg(k);
    qInfo("audit: %d model(s) — %s", int(rows.size()),
          qUtf8Printable(summary.join(QStringLiteral(", "))));
    qInfo("audit: written to %s", qUtf8Printable(tsvPath));

    if (!hadPrevious) {
        qInfo("audit: no previous run to compare against — this one is the "
              "baseline. Run it again after a patch or a mod install.");
    } else {
        int broke = 0, fixed = 0, added = 0, gone = 0;
        QSet<QString> seen;
        for (const Row& r : rows) {
            seen.insert(r.path);
            const auto was = before.constFind(r.path);
            if (was == before.constEnd()) { ++added; continue; }
            if (*was == r.state) continue;
            const bool wasOk = (*was == QLatin1String("ok"));
            const bool isOk = (r.state == QLatin1String("ok"));
            if (wasOk && !isOk) {
                ++broke;
                qWarning("audit: NEWLY BROKEN  %s  (%s -> %s)",
                         qUtf8Printable(r.path), qUtf8Printable(*was),
                         qUtf8Printable(r.state));
            } else if (!wasOk && isOk) {
                ++fixed;
                qInfo("audit: newly working  %s  (%s -> ok)",
                      qUtf8Printable(r.path), qUtf8Printable(*was));
            } else {
                // A change that is neither — "textures-partial" to
                // "textures-missing" is a real regression and would be
                // invisible if only ok/not-ok were compared.
                qInfo("audit: changed        %s  (%s -> %s)",
                      qUtf8Printable(r.path), qUtf8Printable(*was),
                      qUtf8Printable(r.state));
            }
        }
        for (auto it = before.constBegin(); it != before.constEnd(); ++it)
            if (!seen.contains(it.key())) ++gone;
        qInfo("audit: vs the previous run — %d newly broken, %d newly "
              "working, %d new model(s), %d no longer present",
              broke, fixed, added, gone);
    }
    return int(rows.size());
}



// One place the menu handlers report to. They were writing to the status bar
// through a helper this class does not have; it does now, and it is the same
// label every index message already uses.
void MainWindow::showStatus(const QString& text)
{
    if (m_statusLabel) m_statusLabel->setText(text);
    qInfo("%s", qUtf8Printable(text));
}

// ── Help ▸ the triage handlers (docs/CONTEXT_MENUS.md §6) ─────────────────

void MainWindow::toggleLogConsole()
{
    if (!m_logConsole) m_logConsole = new fox::LogConsole(this);
    if (m_logConsole->isVisible()) m_logConsole->hide();
    else m_logConsole->showConsole();
}

// The viewport's keys, which are otherwise invisible. Built from the SAME
// registry the bindings come from (app/Hotkeys.h), so a rebinding shows up here
// without anyone remembering to edit a second list — a cheat sheet that can go
// stale is worse than none, because it is believed.
void MainWindow::showShortcutSheet()
{
    QString html = QStringLiteral("<table cellpadding='4'>");
    for (const Hotkeys::Def& d : Hotkeys::defs()) {
        const QKeySequence seq = Hotkeys::seq(d.key, d.def);
        html += QStringLiteral(
                    "<tr><td><b>%1</b></td><td>%2</td></tr>"
                    "<tr><td></td><td style='color:gray'>%3</td></tr>")
                    .arg(seq.isEmpty() ? QStringLiteral("—")
                                       : seq.toString(QKeySequence::NativeText),
                         d.label.toHtmlEscaped(), d.hint.toHtmlEscaped());
    }
    html += QStringLiteral("</table>");
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Shortcuts"));
    box.setTextFormat(Qt::RichText);
    box.setText(html);
    box.exec();
}

void MainWindow::runHealthCheck()
{
    if (!fox::ArchiveIndex::instance().ready()) {
        showStatus(QStringLiteral("Health check: the index is not ready yet."));
        return;
    }
    const QString dir = AppPaths::subDir(QStringLiteral("audit"));
    const QString path = QDir(dir).filePath(QStringLiteral("asset_health.tsv"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const int rows = writeHealthAudit(path);
    QApplication::restoreOverrideCursor();
    // Says where the file is AND what to do with it — the status line is where
    // you tell people what the thing they just made is for.
    showStatus(QStringLiteral("Health check: %1 model(s) audited → %2")
                  .arg(QLocale().toString(rows), path));
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::exportLog()
{
    const QString suggested =
        QStringLiteral("foxab_log_%1.txt")
            .arg(QDateTime::currentDateTime().toString(
                QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export log"),
        QDir(Config::exportDir().isEmpty() ? AppPaths::dataDir()
                                           : Config::exportDir())
            .filePath(suggested),
        QStringLiteral("Text (*.txt)"));
    if (path.isEmpty()) return;
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        showStatus(QStringLiteral("Could not write %1").arg(path));
        return;
    }
    const QStringList lines = AppLog::tail();
    QTextStream(&out) << lines.join(QLatin1Char('\n')) << '\n';
    out.close();
    showStatus(QStringLiteral("Log written — %1 lines. Attach it to a bug report.")
                  .arg(QLocale().toString(lines.size())));
}

// Everything a bug report needs and nobody thinks to include, in one paste.
void MainWindow::copyDiagnosticInfo()
{
    const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
    QStringList out;
    out << QStringLiteral("FOXAssetBrowser %1  (Qt %2)")
               .arg(QCoreApplication::applicationVersion(),
                    QLatin1String(qVersion()))
        << QStringLiteral("OS: %1  (%2)")
               .arg(QSysInfo::prettyProductName(),
                    QSysInfo::currentCpuArchitecture())
        << QStringLiteral("Data folder: %1").arg(AppPaths::dataDir())
        << QStringLiteral("Dictionaries: %1").arg(Config::dictDir())
        << QStringLiteral("Game folders:");
    const QStringList dirs = Config::gameDirs();
    if (dirs.isEmpty()) out << QStringLiteral("  (none configured)");
    for (const QString& d : dirs) out << QStringLiteral("  %1").arg(d);
    out << QStringLiteral("Index: %1  — %2 archive(s), %3 file(s), %4 named, "
                          "deep scan %5")
               .arg(ix.ready() ? QStringLiteral("ready")
                               : QStringLiteral("NOT READY"))
               .arg(ix.archives().size())
               .arg(ix.files().size())
               .arg(ix.namedCount())
               .arg(ix.deepScanned() ? QStringLiteral("on") : QStringLiteral("off"));
    const fox::TextureUsers& tu = fox::TextureUsers::instance();
    out << QStringLiteral("Texture→model sweep: %1 (%2/%3)")
               .arg(tu.ready() ? QStringLiteral("ready")
                               : QStringLiteral("not ready"))
               .arg(tu.done())
               .arg(tu.total());
    // The GPU string — one of the first things a rendering bug report needs and
    // the last thing anyone remembers to include. Read from the log rather than
    // from the widget: GLModelWidget already prints it at initialisation, and
    // adding an accessor for a string it has already published would be a
    // second copy of the same fact.
    for (const QString& line : AppLog::tail())
        if (line.contains(QLatin1String("gl: "))
            && line.contains(QLatin1String("renderer"), Qt::CaseInsensitive)) {
            out << QStringLiteral("GPU: %1").arg(line.section(QLatin1Char(' '), 2));
            break;
        }
    QApplication::clipboard()->setText(out.join(QLatin1Char('\n')));
    showStatus(QStringLiteral("Diagnostic info copied — paste it into the report."));
}

// File ▸ Index, rebuilt on every open. Each row says what that index is doing
// RIGHT NOW; a static label would be a lie within a second of the menu opening.
void MainWindow::populateIndexMenu()
{
    m_indexMenu->clear();
    m_indexMenu->setToolTipsVisible(true);
    const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
    const bool haveGame = !Config::gameDirs().isEmpty();

    QAction* all = m_indexMenu->addAction(
        QStringLiteral("Index &all"), this, [this] { startRebuild(); });
    all->setToolTip(QStringLiteral(
        "Start everything not already built or running. Anything already done "
        "is left alone."));
    // GUARD on there being anything to index. Building against no game folder
    // produces a DEGRADED index — empty, cached, and marked ready — and every
    // later build then skips it as done, permanently. Refuse with a status
    // line instead of doing it.
    all->setEnabled(haveGame);

    QAction* re = m_indexMenu->addAction(
        QStringLiteral("&Re-index everything…"), this, [this] {
            if (QMessageBox::question(
                    this, QStringLiteral("Re-index everything"),
                    QStringLiteral(
                        "Drop every cache and rebuild from scratch?\n\n"
                        "Measured on a full install: about three minutes."))
                != QMessageBox::Yes)
                return;
            for (const QString& f :
                 QDir(AppPaths::cacheDir())
                     .entryList({QStringLiteral("*.bin")}, QDir::Files))
                QFile::remove(QDir(AppPaths::cacheDir()).filePath(f));
            startRebuild();
        });
    re->setToolTip(QStringLiteral(
        "Drop every cache and rebuild from scratch — minutes of work. Use after "
        "a game patch. Your settings are not touched."));
    re->setEnabled(haveGame);

    m_indexMenu->addSeparator();
    if (!haveGame) {
        QAction* none = m_indexMenu->addAction(
            QStringLiteral("No game folder configured — File ▸ Set game folder…"));
        none->setEnabled(false);
        return;
    }
    const auto row = [this](const QString& name, const QString& state) {
        QAction* a = m_indexMenu->addAction(
            QStringLiteral("%1  —  %2").arg(name, state));
        a->setEnabled(false);
        return a;
    };
    row(QStringLiteral("Archives"),
        ix.ready() ? QStringLiteral("done — %1 archive(s), %2 file(s)")
                         .arg(ix.archives().size())
                         .arg(QLocale().toString(qint64(ix.files().size())))
                   : QStringLiteral("running"));
    row(QStringLiteral("Containers"),
        !Config::deepScan() ? QStringLiteral("off (Settings ▸ Indexing)")
                            : ix.deepScanned() ? QStringLiteral("done")
                                               : QStringLiteral("not started"));
    const fox::TextureUsers& tu = fox::TextureUsers::instance();
    row(QStringLiteral("Texture → model"),
        tu.ready()
            ? QStringLiteral("done — %1 model(s)").arg(QLocale().toString(tu.total()))
            : tu.state() == fox::TextureUsers::State::Building
                  ? QStringLiteral("running %1%")
                        .arg(tu.total() ? tu.done() * 100 / tu.total() : 0)
                  : tu.state() == fox::TextureUsers::State::Failed
                        ? QStringLiteral("failed — see the log")
                        : QStringLiteral("not started"));
}


// --menudump: the whole menu bar, as text.
//
// A menu is its own top-level window, so a screenshot of the application does
// not contain one — the same reason --filemenu logs its entries rather than
// relying on the grab. §6 is a SHAPE (three menus, these items, these
// separators, these mnemonics, these enabled states) and a shape is exactly
// what a dump can be checked against and a picture cannot.
QString MainWindow::dumpMenuBar()
{
    QStringList out;
    const std::function<void(QMenu*, int)> walk = [&](QMenu* m, int depth) {
        // aboutToShow is what fills the Export and Index menus; without firing
        // it the dump would show them empty, which is exactly the failure the
        // menus themselves have when the signal is not connected.
        Q_EMIT m->aboutToShow();
        for (QAction* a : m->actions()) {
            const QString pad = QString(depth * 2, QLatin1Char(' '));
            if (a->isSeparator()) { out << pad + QStringLiteral("---"); continue; }
            QString line = pad + a->text();
            if (!a->shortcut().isEmpty())
                line += QStringLiteral("  [%1]")
                            .arg(a->shortcut().toString(QKeySequence::NativeText));
            if (!a->isEnabled()) line += QStringLiteral("  (disabled)");
            if (!a->isVisible()) line += QStringLiteral("  (hidden)");
            out << line;
            if (a->menu()) walk(a->menu(), depth + 1);
        }
    };
    for (QAction* top : menuBar()->actions()) {
        out << QStringLiteral("=== %1").arg(top->text());
        if (top->menu()) walk(top->menu(), 1);
    }
    return out.join(QLatin1Char('\n'));
}

void MainWindow::applyHotkeys()
{
    qDeleteAll(m_hotkeyActions);
    m_hotkeyActions.clear();

    // The VIEWPORT's rows live on the viewports, not here: they are
    // widget-scoped, so the focused viewport answers and the other does not.
    // Rebuilt from the same table at the same moment, so "rebind and it takes
    // effect without a restart" is true for all of them or none.
    for (GLModelWidget* v : findChildren<GLModelWidget*>())
        v->installViewportShortcuts();

    const auto add = [this](const QString& key, auto&& fn) {
        for (const Hotkeys::Def& d : Hotkeys::defs()) {
            if (d.key != key) continue;
            const QKeySequence seq = Hotkeys::seq(d.key, d.def);
            if (seq.isEmpty()) return;   // deliberately unbound
            auto* a = new QAction(d.label, this);
            a->setShortcut(seq);
            a->setShortcutContext(Qt::ApplicationShortcut);
            connect(a, &QAction::triggered, this, fn);
            addAction(a);
            m_hotkeyActions.append(a);
            return;
        }
    };

    // Every one of these routes to something that already exists — the point
    // of the registry is the BINDING, not a second implementation of the
    // action. An entry whose target the current tab does not offer does
    // nothing, which is the honest answer to "export the selection" on a tab
    // with no selection.
    // Undo is Customize's, and only while Customize is the tab in front: an
    // application-scoped Ctrl+Z that stepped the wardrobe back while the user
    // was on Textures would be a misfire, not a feature.
    add(QStringLiteral("hotkeys/customizeUndo"), [this] {
        if (m_customizeTab && m_tabs && m_tabs->currentWidget() == m_customizeTab)
            m_customizeTab->undoScene();
    });
    add(QStringLiteral("hotkeys/customizeRedo"), [this] {
        if (m_customizeTab && m_tabs && m_tabs->currentWidget() == m_customizeTab)
            m_customizeTab->redoScene();
    });
    add(QStringLiteral("hotkeys/settings"), [this] { openSettings(QString()); });
    add(QStringLiteral("hotkeys/rescan"), [this] { startRebuild(); });
    add(QStringLiteral("hotkeys/focusSearch"), [this] { focusCurrentSearch(); });
    add(QStringLiteral("hotkeys/exportSelection"),
        [this] { triggerExportAction(Hotkeys::Role::exportSelection()); });
    add(QStringLiteral("hotkeys/exportAnimations"),
        [this] { triggerExportAction(Hotkeys::Role::exportAnimations()); });
    add(QStringLiteral("hotkeys/saveImage"),
        [this] { triggerExportAction(Hotkeys::Role::saveImage()); });
    add(QStringLiteral("hotkeys/turntable"),
        [this] { triggerExportAction(Hotkeys::Role::turntable()); });
    // N is the viewport panel's own key: the viewport is the only thing that
    // knows whether it has the focus, so it reads the binding itself. Listed
    // in the registry so it appears in the editor and can be rebound.
}

// Ctrl+F: the search box of whatever tab is in front. The window is the only
// thing that knows which that is, which is why this lives here and not in
// util/SearchBox.h with the other three behaviours.
void MainWindow::focusCurrentSearch()
{
    QWidget* cur = m_tabs ? m_tabs->currentWidget() : nullptr;
    if (!cur) return;
    // The FIRST visible QLineEdit that is a search box, by object name. Every
    // tab's box is named when it is built; a tab with no search box does
    // nothing rather than stealing the focus into some other field.
    for (QLineEdit* e : cur->findChildren<QLineEdit*>()) {
        if (e->objectName() != QLatin1String("foxabSearchBox")) continue;
        if (!e->isVisible()) continue;
        e->setFocus(Qt::ShortcutFocusReason);
        e->selectAll();
        return;
    }
}

// Fire one of the Export menu's entries without opening it. The menu is
// rebuilt per tab from each tab's own populateExportMenu(), so the actions
// exist only while it is being shown — this builds it into a throwaway menu,
// finds the entry by ROLE and triggers it. Going through the menu rather than
// calling the tabs directly is deliberate: it is the same code path the mouse
// takes, so a hotkey can never do something the menu cannot.
//
// By role and NOT by matching the label. Text matching was tried and was wrong
// twice in one build: "export" found "Export settings…" before it found any
// export (so Ctrl+E opened the settings dialog on a tab with nothing loaded,
// because every real export action was disabled), and "save image" matched
// nothing at all because the action is called "Save viewport image…".
void MainWindow::triggerExportAction(const QString& role)
{
    QMenu menu(this);
    QWidget* cur = m_tabs ? m_tabs->currentWidget() : nullptr;
    if (cur == m_filesTab) m_filesTab->populateExportMenu(&menu);
    else if (cur == m_texturesTab) m_texturesTab->populateExportMenu(&menu);
    else if (cur == m_modelsTab) m_modelsTab->populateExportMenu(&menu);
    else if (cur == m_customizeTab) m_customizeTab->populateExportMenu(&menu);
    else return;

    QAction* hit = Hotkeys::Role::find(menu.actions(), role);
    if (!hit) {
        // Said out loud. A hotkey that silently does nothing is
        // indistinguishable from a hotkey that is not bound, and this tab
        // genuinely may not offer this action.
        statusBar()->showMessage(
            QStringLiteral("Nothing on this tab to do that with."), 4000);
        return;
    }
    if (!hit->isEnabled()) {
        statusBar()->showMessage(
            QStringLiteral("%1 — not available right now.")
                .arg(hit->text().remove(QLatin1Char('&'))),
            4000);
        return;
    }
    hit->trigger();
}

// Show the last per-tab report only while the widget that made it is inside
// the tab now on screen. A pointer walk rather than a stored tab index: a tab
// is a tree of widgets and the reporter is somewhere down it, and asking the
// parent chain is the one test that cannot go stale when a panel is reparented.
void MainWindow::syncTabStatus()
{
    if (!m_tabStatusLabel) return;
    QWidget* cur = m_tabs ? m_tabs->currentWidget() : nullptr;
    bool mine = m_tabStatusSource.isNull();   // null source = from nowhere
    for (QWidget* w = m_tabStatusSource; w && !mine; w = w->parentWidget())
        if (w == cur) mine = true;
    m_tabStatusLabel->setText(mine ? m_tabStatusText : QString());
}

void MainWindow::populateExportMenu()
{
    m_exportMenu->clear();
    // clear() deletes actions but not child QMenus (the Bulk submenu) — tidy
    // them so repeated opens don't accumulate orphans.
    qDeleteAll(m_exportMenu->findChildren<QMenu*>(QString(),
                                                  Qt::FindDirectChildrenOnly));
    if (!fox::ArchiveIndex::instance().ready()) {
        m_exportMenu->addAction(QStringLiteral("(index not ready)"))
            ->setEnabled(false);
        return;
    }

    // ── contextual section: what the CURRENT tab is looking at ──────────────
    QWidget* cur = m_tabs->currentWidget();
    if (cur == m_filesTab) m_filesTab->populateExportMenu(m_exportMenu);
    else if (cur == m_texturesTab) m_texturesTab->populateExportMenu(m_exportMenu);
    else if (cur == m_modelsTab) m_modelsTab->populateExportMenu(m_exportMenu);
    else if (cur == m_customizeTab) m_customizeTab->populateExportMenu(m_exportMenu);

    else if (cur == m_bulkTab)
        m_exportMenu->addAction(QStringLiteral("Extract matches"))->setEnabled(false);

    // ── bulk presets: jump to Bulk Extract prefilled ────────────────────────
    // BEFORE "Export settings…", which §6 puts last: it is the way out of this
    // menu into the dialog, and an item that leaves the menu belongs at the
    // bottom of it. The capture actions put it there themselves, so it is
    // lifted out and re-added at the end below.
    for (QAction* a : m_exportMenu->actions())
        if (a->text().startsWith(QLatin1String("Export settings")))
            m_exportMenu->removeAction(a);
    m_exportMenu->addSeparator();
    QMenu* bulk = m_exportMenu->addMenu(QStringLiteral("Bulk export"));
    const auto preset = [this](const QString& query, const QString& ext) {
        m_bulkTab->applyPreset(query, ext);
        m_tabs->setCurrentWidget(m_bulkTab);
    };
    bulk->addAction(QStringLiteral("All textures → DDS…"), this,
                    [preset] { preset(QString(), QStringLiteral("ftex")); });
    bulk->addAction(QStringLiteral("All models (.fmdl)…"), this,
                    [preset] { preset(QString(), QStringLiteral("fmdl")); });
    bulk->addAction(QStringLiteral("All audio → WAV…"), this,
                    [preset] { preset(QString(), QStringLiteral("wem")); });
    bulk->addAction(QStringLiteral("All animations (.mtar)…"), this,
                    [preset] { preset(QString(), QStringLiteral("mtar")); });
    bulk->addAction(QStringLiteral("All scripts (.lua)…"), this,
                    [preset] { preset(QString(), QStringLiteral("lua")); });
    bulk->addSeparator();
    bulk->addAction(QStringLiteral("Everything…"), this,
                    [preset] { preset(QString(), QString()); });

    // ── the mod folder, as one file ─────────────────────────────────────────
    // OMITTED, not disabled, when no mod folder is configured — the same rule
    // the row menu follows for the replace entries (§7). Someone who has never
    // set one up is not being told about a feature by a grey line; someone who
    // has is one click from shipping what they made.
    if (!modfolder::dir().isEmpty()) {
        m_exportMenu->addSeparator();
        const int n = modfolder::list().size();
        QAction* pkg = m_exportMenu->addAction(
            n > 0 ? QStringLiteral("Package mod folder (%1 file(s))…").arg(n)
                  : QStringLiteral("Package mod folder…"),
            this, &MainWindow::exportModPackage);
        // The count is in the label rather than behind the click, so the one
        // question this action raises — "is my replacement actually in there"
        // — is answered before the file dialog rather than after it.
        pkg->setEnabled(n > 0);
        if (n == 0)
            pkg->setToolTip(QStringLiteral(
                "The mod folder holds no replacements yet. Replace an asset "
                "from any row menu first."));
        // The two packages are two different deliverables, not a format
        // choice: the plain one is a folder anyone can unzip, and this one is
        // something a mod manager installs. Both are offered, and the label
        // says which is which rather than making someone find out.
        QAction* mgsv = m_exportMenu->addAction(
            QStringLiteral("Package as a SnakeBite mod (.mgsv)…"), this,
            &MainWindow::exportMgsvPackage);
        mgsv->setEnabled(n > 0);
    }

    // LAST (§6). Someone who has just noticed the export options are wrong is
    // standing in this menu, not hunting through File — so it belongs here,
    // and it belongs at the bottom because it leaves the menu.
    m_exportMenu->addSeparator();
    m_exportMenu->addAction(QStringLiteral("Export &settings…"), this,
                            [this] { openSettings(QStringLiteral("Export")); });
}

void MainWindow::exportModPackage()
{
    const QString suggested =
        QStringLiteral("foxab_mod_%1.zip")
            .arg(QDateTime::currentDateTime().toString(
                QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Package mod folder"),
        QDir(Config::exportDir().isEmpty() ? AppPaths::dataDir()
                                           : Config::exportDir())
            .filePath(suggested),
        QStringLiteral("Zip archive (*.zip)"));
    if (path.isEmpty()) return;

    const modpackage::Result r = modpackage::write(path);
    if (!r.error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Package mod folder"),
                             r.error);
        showStatus(r.error);
        return;
    }
    // The container count is said out loud rather than left in the manifest.
    // It is the difference between an archive somebody can unzip over a mod
    // folder and one an installer would have to re-pack into .fpk containers,
    // and that is worth knowing at the moment the file is written rather than
    // when a loader refuses it.
    QString msg = QStringLiteral("Packaged %1 replacement(s).").arg(r.files);
    if (r.inContainer > 0)
        msg += QStringLiteral(" %1 of them replace a file the game keeps "
                              "INSIDE a container (.fpk/.pftxs) — manifest.tsv "
                              "records which.").arg(r.inContainer);
    if (r.notInIndex > 0)
        msg += QStringLiteral(" %1 override nothing in this install.")
                   .arg(r.notInIndex);
    showStatus(msg);
    qInfo("modpackage: %d asset(s), %d in a container, %d not in the index "
          "-> %s", r.files, r.inContainer, r.notInIndex, qUtf8Printable(path));
}

// --mgsvmeta "k=v;…", parsed once. Two callers — the package flag and the
// dialog grab — and a second spelling of a key list is a second set of keys.
modpackage::MgsvMeta MainWindow::shotMgsvMeta(const QString& defaultName) const
{
    modpackage::MgsvMeta meta;
    meta.name = defaultName;
    for (const QString& kv :
         m_shot.mgsvMeta.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString k = kv.section(QLatin1Char('='), 0, 0).trimmed().toLower();
        const QString v = kv.section(QLatin1Char('='), 1);
        if (k == QLatin1String("name")) meta.name = v;
        else if (k == QLatin1String("version")) meta.version = v;
        else if (k == QLatin1String("author")) meta.author = v;
        else if (k == QLatin1String("website")) meta.website = v;
        else if (k == QLatin1String("description")) meta.description = v;
        else if (k == QLatin1String("mgsversion")) meta.mgsVersion = v;
        else if (k == QLatin1String("sbversion")) meta.sbVersion = v;
        else qWarning("mgsvmeta: unknown key '%s'", qUtf8Printable(k));
    }
    return meta;
}

void MainWindow::exportMgsvPackage()
{
    const QString folderName = QDir(modfolder::dir()).dirName();
    MgsvMetaDialog dlg(folderName, modfolder::list().size(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    const modpackage::MgsvMeta meta = dlg.meta();

    QString suggested = meta.name;
    // A mod name is free text and a file name is not, so the characters a
    // file system refuses are replaced rather than handed to the save dialog
    // to fail on.
    suggested.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")),
                      QStringLiteral("_"));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Package as a SnakeBite mod"),
        QDir(Config::exportDir().isEmpty() ? AppPaths::dataDir()
                                           : Config::exportDir())
            .filePath(suggested + QStringLiteral(".mgsv")),
        QStringLiteral("SnakeBite mod (*.mgsv)"));
    if (path.isEmpty()) return;

    const modpackage::Result r = modpackage::writeMgsv(path, meta);
    if (!r.error.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("Package as a SnakeBite mod"),
                             r.error);
        showStatus(r.blocked.isEmpty()
                       ? r.error
                       : QStringLiteral("%1 replacement(s) live inside a "
                                        "container — see the message.")
                             .arg(r.blocked.size()));
        return;
    }
    showStatus(QStringLiteral("Wrote %1 — %2 file(s), installable with "
                              "SnakeBite 0.8 or newer.")
                   .arg(QFileInfo(path).fileName())
                   .arg(r.files));
    qInfo("mgsvpackage: %d QarEntry(s) -> %s", r.files, qUtf8Printable(path));
}

void MainWindow::chooseGameFolder()
{
    const QStringList current = Config::gameDirs();
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose the game install folder (scanned recursively)"),
        current.isEmpty() ? QString() : current.first());
    if (dir.isEmpty()) return;
    Config::setGameDirs({dir});
    startRebuild();
}

void MainWindow::startRebuild()
{
    const QStringList dirs = Config::gameDirs();
    if (dirs.isEmpty()) return;
    if (m_bulkTab && m_bulkTab->extracting()) {
        // Worker threads hold references into the current index — swapping the
        // file tables under them is a use-after-free. Finish or cancel first.
        QMessageBox::information(
            this, QStringLiteral("Rescan"),
            QStringLiteral("A bulk extraction is running — cancel it or let it "
                           "finish before rescanning the archives."));
        return;
    }
    m_statusLabel->setText(QStringLiteral("Scanning…"));
    // Decoded textures are keyed on a file's position in the index, and the
    // rebuilt index re-numbers everything. The cache notices that by itself
    // once the new list is installed; dropping it here just hands the memory
    // back at the moment the user asked for a fresh look rather than holding
    // up to a quarter of a gigabyte of images from an install being replaced.
    extract::clearTextureCache();
    fox::ArchiveIndex::instance().rebuild(dirs, Config::dictDir(), Config::deepScan());
}

void MainWindow::scheduleDevShot(const DevShot& shot)
{
    m_shot = shot;
    m_shotPending = true;
    if (fox::ArchiveIndex::instance().ready()) takeDevShot();
}

bool MainWindow::DevShot::moreOutputPending(QString DevShot::* except) const
{
    for (QString DevShot::* f : {&DevShot::outPng, &DevShot::capturePng,
                                 &DevShot::captureGif,
                                 &DevShot::captureAnimGif, &DevShot::exportParts,
                                 &DevShot::gearDump, &DevShot::iconDump,
                                 &DevShot::lightDump})
        if (f != except && !(this->*f).isEmpty()) return true;
    return false;
}

void MainWindow::takeDevShot()
{
    m_shotPending = false;   // one shot per schedule — a later rescan must not
                             // re-grab (or quit) mid-session
    if (!m_shot.dumpTex.isEmpty()) {
        // Which textures do these models actually reference? The dictionary
        // names only a fraction of them — 9 of the Survive avatar's — but the
        // MODEL carries the path hash of every map it wants, so it can say
        // exactly what to pull without a single path being known.
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        const QString needle = m_shot.dumpTex.section(QLatin1Char('='), 0, 0);
        const QString out = m_shot.dumpTex.section(QLatin1Char('='), 1);
        QFile f(out);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning("devshot: cannot write %s", qUtf8Printable(out));
            if (!m_shot.stay) QApplication::quit();
            return;
        }
        QTextStream ts(&f);
        QSet<quint64> seen;
        int models = 0, refs = 0;
        // Every streamed mip lives under its own extension code, so ask for the
        // header and the first few streams of each.
        // The header plus one stream is a usable texture at browser
        // resolution; the higher-numbered streams hold the 2K/4K mips and are
        // most of the bytes.
        const char* const kExts[] = {"ftex", "1.ftexs"};
        for (const fox::IndexedFile& e : ix.files()) {
            if (!e.path.endsWith(QLatin1String(".fmdl"), Qt::CaseInsensitive)) continue;
            if (!e.path.contains(needle, Qt::CaseInsensitive)) continue;
            fox::FmdlFile m;
            if (!m.parse(ix.readFile(e))) continue;
            ++models;
            for (const fox::FmdlMaterialInstance& mi : m.materials())
                for (const fox::FmdlTextureRef& t : mi.textures) {
                    if (t.pathHash == 0) continue;
                    ++refs;
                    for (const char* ext : kExts) {
                        const quint64 code =
                            (fox::hashExtension(QLatin1String(ext)) << 51)
                            | (t.pathHash & fox::kPathMask);
                        if (seen.contains(code)) continue;
                        seen.insert(code);
                        ts << code << '\n';
                    }
                }
        }
        f.close();
        qInfo("devshot: texdump — %d model(s), %d reference(s), %lld code(s) -> %s",
              models, refs, qint64(seen.size()), qUtf8Printable(out));
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    // ── The light dump ───────────────────────────────────────────────────
    // WHAT THE GAMES' OWN LIGHTING ACTUALLY IS. Fox keeps it in four places —
    // .grxla light arrays (point and spot lights, and probe volumes), .grxoc
    // occluders, .lpsh light probes as spherical harmonics, .atsh the sky's
    // bounce, .pcsp a precomputed scattering table — and every one of them
    // lives inside an FPK, which is why no extract on hand has ever held one.
    //
    // This is the measuring device, not the feature. It lists every one the
    // index can see, per game, and for the two array formats it walks the
    // documented entry chain and reports whether the walk reached the
    // terminator. "N of N complete" over a real install is what settles the
    // layout; until then the per-game rigs in ViewEnvironment are authored and
    // say so. Nothing here touches the renderer.
    if (!m_shot.lightDump.isEmpty()) {
        QString dumpPath = m_shot.lightDump.section(QLatin1Char('='), 0, 0);
        QFile out(dumpPath);
        if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&out);
            ts << "path\tgame\text\tbytes\tparsed\tentries\n";
            const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
            static const char* const kExt[] = {".grxla", ".grxoc", ".lpsh",
                                               ".atsh", ".pcsp"};
            // A CAP on how many arrays are actually opened. Each one lives
            // inside an FPK and readFile() decompresses the whole container to
            // reach it, so a full TPP install — the wiki counts 8,880 .grxla —
            // would re-inflate the same packs thousands of times on the GUI
            // thread. The listing is complete either way; only the WALK is
            // capped, and the summary says how many it walked out of how many
            // it could have. "--lightdump out.tsv=2000" raises it.
            int cap = 400;
            if (m_shot.lightDump.contains(QLatin1Char('='))) {
                const int n =
                    m_shot.lightDump.section(QLatin1Char('='), 1).toInt();
                if (n > 0) cap = n;
            }
            int rows = 0, walked = 0, complete = 0, badHeader = 0, walkable = 0;
            QHash<QString, int> byExt, byType;
            for (const fox::IndexedFile& f : ix.files()) {
                QString ext;
                for (const char* e : kExt)
                    if (f.path.endsWith(QLatin1String(e), Qt::CaseInsensitive)) {
                        ext = QLatin1String(e).mid(1);
                        break;
                    }
                if (ext.isEmpty()) continue;
                ++rows;
                ++byExt[ext];
                QString parsed = QStringLiteral("-"), detail;
                if (ext == QLatin1String("grxla")
                    || ext == QLatin1String("grxoc")) {
                    ++walkable;
                    if (walked >= cap) {
                        parsed = QStringLiteral("not walked (cap)");
                    } else {
                        fox::GrxlaFile g;
                        const QByteArray d = ix.readFile(f);
                        if (d.isEmpty()) {
                            parsed = QStringLiteral("UNREADABLE");
                        } else if (!g.parse(d)) {
                            parsed = QStringLiteral("NOT-FGX");
                        } else {
                            ++walked;
                            if (!g.headerOk()) ++badHeader;
                            if (g.complete() && g.headerOk()) ++complete;
                            parsed = (g.complete() && g.headerOk())
                                ? QStringLiteral("complete")
                                : QStringLiteral("INCOMPLETE: ") + g.error();
                            detail = g.describe();
                            for (const fox::GrxEntry& e : g.entries())
                                ++byType[e.type];
                        }
                    }
                }
                ts << f.path << '\t' << fox::gameShortName(ix.gameOf(f))
                   << '\t' << ext << '\t' << f.size << '\t' << parsed
                   << '\t' << detail << '\n';
            }
            out.close();
            QStringList extBits;
            QStringList keys = byExt.keys();
            keys.sort();
            for (const QString& k : keys)
                extBits << QStringLiteral("%1 %2").arg(byExt.value(k)).arg(k);
            QStringList typeBits;
            QStringList tkeys = byType.keys();
            tkeys.sort();
            for (const QString& k : tkeys)
                typeBits << QStringLiteral("%1x%2").arg(k).arg(byType.value(k));
            qInfo("devshot: lightdump %s — %d file(s): %s",
                  qUtf8Printable(dumpPath), rows,
                  qUtf8Printable(extBits.join(QLatin1String(", "))));
            if (walked > 0)
                qInfo("devshot: lightdump — %d of %d walked array(s) parsed "
                      "clean (%d of %d opened, %d with a bad header)%s%s",
                      complete, walked, walked, walkable, badHeader,
                      typeBits.isEmpty() ? "" : "; entries: ",
                      qUtf8Printable(typeBits.join(QLatin1Char(' '))));
            else if (rows == 0)
                qInfo("devshot: lightdump — no lighting data in the configured "
                      "folders (it lives inside FPKs; mount the game's own "
                      "chunks, not an extract)");
        } else {
            qWarning("devshot: cannot write %s", qUtf8Printable(m_shot.lightDump));
        }
        if (!m_shot.stay
            && !m_shot.moreOutputPending(&DevShot::lightDump)) {
            QApplication::quit();
            return;
        }
    }

    // ── The icon dump ────────────────────────────────────────────────────
    // Every gear item and every colour, with the icon the game names for it
    // and whether this install can actually decode that icon. Two different
    // failures look identical on screen — "the data names no icon" and "the
    // icon is named but the texture is not in the configured folders" — and
    // this is the only thing that tells them apart. The DECODE is done here,
    // not just the lookup, because a path that resolves and a texture that
    // decodes are also two different things.
    if (!m_shot.iconDump.isEmpty()) {
        QFile out(m_shot.iconDump);
        if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&out);
            ts << "kind\tsubject\tslot\tid\tname\ticon\tdecoded\n";
            fox::IconCatalog& ic = fox::IconCatalog::instance();
            int rows = 0, named = 0, decoded = 0;
            for (const fox::PlayerSubject& sub :
                 fox::PlayerCatalog::instance().subjects())
                for (const fox::PlayerSlot& sl : sub.slotList)
                    for (const fox::CatalogPart& p : sl.parts) {
                        if (p.gearId.isEmpty()) continue;
                        const bool has = !p.gearIcon.isEmpty();
                        const bool ok =
                            has && !ic.swatchForPath(p.gearIcon, 32).isNull();
                        if (has) ++named;
                        if (ok) ++decoded;
                        ts << "gear\t" << sub.name << '\t' << sl.label << '\t'
                           << p.gearId << '\t' << p.displayName << '\t'
                           << (has ? p.gearIcon : QStringLiteral("-")) << '\t'
                           << (ok ? "yes" : "NO") << '\n';
                        ++rows;
                    }
            // EVERY defined colour, in id order — not only the ones some
            // item's palette reaches. 367 are defined and 337 referenced, and
            // a question about one of the other 30 has to get an answer rather
            // than a missing row.
            const fox::MgoGearConfig& gc = fox::MgoGearConfig::instance();
            int cRows = 0, cDecoded = 0;
            QList<QString> colourIds = gc.colourIds();
            std::sort(colourIds.begin(), colourIds.end());
            for (const QString& cid : colourIds) {
                const QString sw = gc.colourSwatch(cid);
                const bool ok =
                    !sw.isEmpty() && !ic.swatchForPath(sw, 32).isNull();
                if (ok) ++cDecoded;
                ts << "colour\t-\t-\t" << cid << '\t'
                   << (gc.colourType(cid).isEmpty() ? QStringLiteral("-")
                                                    : gc.colourType(cid))
                   << '\t' << (sw.isEmpty() ? QStringLiteral("-") : sw)
                   << '\t' << (ok ? "yes" : "NO") << '\n';
                ++cRows;
            }
            out.close();
            qInfo("devshot: icondump %s — gear %d row(s), %d named, %d decoded; "
                  "colours %d row(s), %d decoded",
                  qUtf8Printable(m_shot.iconDump), rows, named, decoded, cRows,
                  cDecoded);
        } else {
            qWarning("devshot: cannot write %s", qUtf8Printable(m_shot.iconDump));
        }
        if (!m_shot.stay
            && !m_shot.moreOutputPending(&DevShot::iconDump)) {
            QApplication::quit();
            return;
        }
    }

    // ── The gear dump ────────────────────────────────────────────────────
    // What the game calls each item, what this tool resolved it to, and the
    // bone that model roots at. A report of "the beret sits wrong" is only
    // actionable once those three are on one line: the name is what the player
    // sees, the stem is what a render can be checked against, and the root bone
    // is what decides where it lands.
    if (!m_shot.gearDump.isEmpty()) {
        QFile out(m_shot.gearDump);
        if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&out);
            ts << "subject\tslot\tid\tname\tmodel\trootBone\trootInWearer\n";
            const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
            int rows = 0;
            for (const fox::PlayerSubject& sub :
                 fox::PlayerCatalog::instance().subjects()) {
                // The wearer's own bones, for the "does this part align
                // itself" column — the whole question behind a mispositioned
                // hat.
                QSet<quint32> wearer;
                if (sub.baseFileIdx >= 0 && sub.baseFileIdx < ix.files().size()) {
                    fox::FmdlFile base;
                    if (base.parse(ix.readFile(ix.files()[sub.baseFileIdx])))
                        for (const fox::FmdlBone& b : base.bones())
                            wearer.insert(b.hash32);
                }
                for (const fox::PlayerSlot& sl : sub.slotList)
                    for (const fox::CatalogPart& p : sl.parts) {
                        QString rootName;
                        QString inWearer = QStringLiteral("?");
                        if (p.modelFileIdx >= 0
                            && p.modelFileIdx < ix.files().size()) {
                            fox::FmdlFile m;
                            if (m.parse(ix.readFile(ix.files()[p.modelFileIdx])))
                                for (const fox::FmdlBone& b : m.bones())
                                    if (b.parentIndex < 0) {
                                        rootName = b.name;
                                        inWearer = wearer.isEmpty()
                                            ? QStringLiteral("?")
                                            : (wearer.contains(b.hash32)
                                                   ? QStringLiteral("yes")
                                                   : QStringLiteral("NO"));
                                        break;
                                    }
                        }
                        ts << sub.name << '\t' << sl.label << '\t'
                           << (p.gearId.isEmpty() ? QStringLiteral("-")
                                                  : p.gearId)
                           << '\t' << p.displayName << '\t' << p.id << '\t'
                           << (rootName.isEmpty() ? QStringLiteral("-")
                                                  : rootName)
                           << '\t' << inWearer << '\n';
                        ++rows;
                    }
            }
            qInfo("devshot: geardump %s — %d row(s)", qUtf8Printable(m_shot.gearDump),
                  rows);
        } else {
            qWarning("devshot: cannot write %s", qUtf8Printable(m_shot.gearDump));
        }
        // Quit only when nothing ELSE was asked for. --capture, --turngif and
        // --exportparts all arm the harness on their own now, and each of them
        // does its work in the settle-window timer further down: posting quit()
        // here without a return let that timer never fire, so
        // "--geardump a.tsv --capture b.png" wrote the tsv and silently never
        // wrote the png. --icondump is in the same set, and the two dumps do
        // compose: they read different tables and neither disturbs the other.
        if (!m_shot.stay
            && !m_shot.moreOutputPending(&DevShot::gearDump)) {
            QApplication::quit();
            return;
        }
    }

    if (!m_shot.dumpBones.isEmpty()) {
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        for (const QString& needle :
             m_shot.dumpBones.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            int found = -1;
            for (int i = 0; i < ix.files().size(); ++i)
                if (ix.files()[i].path.contains(needle, Qt::CaseInsensitive)
                    && ix.files()[i].path.endsWith(QLatin1String(".fmdl"),
                                                   Qt::CaseInsensitive)) {
                    found = i;
                    break;
                }
            if (found < 0) { qWarning("bones: no model matches '%s'", qUtf8Printable(needle)); continue; }
            fox::FmdlFile m;
            if (!m.parse(ix.readFile(ix.files()[found]))) {
                qWarning("bones: %s did not parse", qUtf8Printable(needle));
                continue;
            }
            // Bounds too: a part authored in BONE-LOCAL space sits around the
            // origin, one authored in character space sits at its wearing
            // height. That distinction is what decides whether a part needs
            // re-anchoring, and it is invisible in the bone list alone.
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            qint64 verts = 0;
            for (const fox::FmdlMesh& mesh : m.meshes())
                for (int v = 0; v + 2 < mesh.positions.size(); v += 3) {
                    ++verts;
                    for (int a = 0; a < 3; ++a) {
                        lo[a] = qMin(lo[a], mesh.positions[v + a]);
                        hi[a] = qMax(hi[a], mesh.positions[v + a]);
                    }
                }
            qInfo("bones: %s — %lld bone(s), %lld vert(s), bounds "
                  "[%.3f %.3f %.3f] .. [%.3f %.3f %.3f]",
                  qUtf8Printable(needle), qint64(m.bones().size()), verts,
                  lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
            // The bone NAMES, which is what decides whether an animation
            // drives this model at all: a clip addresses bones by StrCode32 of
            // the name, so a part whose bones no track names stays in bind
            // pose while everything around it moves.
            for (int b = 0; b < m.bones().size() && b < 64; ++b)
                qInfo("   bone %3d: %-30s parent=%d  rest [%.3f %.3f %.3f]", b,
                      qUtf8Printable(m.bones()[b].name),
                      int(m.bones()[b].parentIndex),
                      m.bones()[b].worldPos[0], m.bones()[b].worldPos[1],
                      m.bones()[b].worldPos[2]);
            if (m.bones().size() > 64)
                qInfo("   … %lld more bone(s)", qint64(m.bones().size() - 64));
            for (int mi = 0; mi < m.materials().size(); ++mi) {
                const fox::FmdlMaterialInstance& mat = m.materials()[mi];
                QStringList refs;
                for (const fox::FmdlTextureRef& t : mat.textures)
                    refs << QStringLiteral("%1=%2")
                                .arg(t.role,
                                     t.path.isEmpty()
                                         ? QStringLiteral("0x%1").arg(t.pathHash, 0, 16)
                                         : t.path);
                qInfo("   mat %d: name=%s shader=%s | %s", mi,
                      qUtf8Printable(mat.name), qUtf8Printable(mat.shader),
                      qUtf8Printable(refs.join(QStringLiteral("  "))));
            }
            for (int g = 0; g < m.meshGroups().size(); ++g)
                qInfo("   group %d: %-28s parent=%d visible=%d", g,
                      qUtf8Printable(m.meshGroups()[g].name),
                      int(m.meshGroups()[g].parentIndex),
                      m.meshGroups()[g].visible ? 1 : 0);
            for (int mi = 0; mi < m.meshes().size(); ++mi) {
                const fox::FmdlMesh& me = m.meshes()[mi];
                QStringList usages;
                for (quint8 u : me.vertexUsages) usages << QString::number(u);
                float mlo[3] = {1e30f, 1e30f, 1e30f}, mhi[3] = {-1e30f, -1e30f, -1e30f};
                for (int v = 0; v + 2 < me.positions.size(); v += 3)
                    for (int a = 0; a < 3; ++a) {
                        mlo[a] = qMin(mlo[a], me.positions[v + a]);
                        mhi[a] = qMax(mhi[a], me.positions[v + a]);
                    }
                qInfo("   mesh %d: group=%d material=%d %lld vert(s) "
                      "Y %.3f..%.3f  usages [%s]", mi, me.meshGroupIndex,
                      me.materialInstanceIndex,
                      qint64(me.positions.size() / 3), mlo[1], mhi[1],
                      qUtf8Printable(usages.join(QLatin1Char(' '))));
            }
            for (const fox::FmdlBone& b : m.bones())
                qInfo("   %08x %-30s parent=%d  local %.3f %.3f %.3f  world "
                      "%.3f %.3f %.3f",
                      b.hash32, qUtf8Printable(b.name), int(b.parentIndex),
                      b.localPos[0], b.localPos[1], b.localPos[2],
                      b.worldPos[0], b.worldPos[1], b.worldPos[2]);
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpDfrm.isEmpty()) {
        // What a head's .dfrm actually contains. See DfrmFile.h — the short
        // version is that it is topology, not shape, and this is here so the
        // next look at avatar morphs starts from that rather than from the
        // file's name.
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        int shown = 0;
        for (const fox::IndexedFile& f : ix.files()) {
            if (!f.path.endsWith(QLatin1String(".dfrm"), Qt::CaseInsensitive)
                || !f.path.contains(m_shot.dumpDfrm, Qt::CaseInsensitive))
                continue;
            fox::DfrmFile df;
            const QByteArray blob = ix.readFile(f);
            if (!df.parse(blob)) {
                qWarning("dfrm: %s — %s", qUtf8Printable(f.path),
                         qUtf8Printable(df.errorString()));
                continue;
            }
            qInfo("dfrm: %s — %s", qUtf8Printable(f.path), qUtf8Printable(df.summary()));
            for (int i = 0; i < df.meshes().size(); ++i)
                qInfo("   mesh %2d: %u vert(s) from weld[%u]", i,
                      df.meshes()[i].vertexCount, df.meshes()[i].firstWeld);
            for (int w = 0; w < qMin(4, df.weldedCount()); ++w) {
                QStringList bits;
                for (const fox::DfrmFan& fn : df.fansFor(w))
                    bits << QStringLiteral("%1/%2").arg(fn.mesh).arg(fn.triangle);
                qInfo("   welded %d: fan %s", w, qUtf8Printable(bits.join(QLatin1Char(' '))));
            }
            for (int g = 0; g < qMin(4, df.groupCount()); ++g) {
                QStringList bits;
                for (quint32 m : df.group(g)) bits << QString::number(m);
                qInfo("   group %d: %s", g, qUtf8Printable(bits.join(QLatin1Char(' '))));
            }
            if (++shown >= 3) break;
        }
        if (!shown) qWarning("dfrm: nothing matches '%s'",
                             qUtf8Printable(m_shot.dumpDfrm));
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpFile.isEmpty()) {
        const QString needle = m_shot.dumpFile.section(QLatin1Char('='), 0, 0);
        const QString out = m_shot.dumpFile.section(QLatin1Char('='), 1);
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        int found = -1;
        for (int i = 0; i < ix.files().size(); ++i)
            if (ix.files()[i].path.contains(needle, Qt::CaseInsensitive)) { found = i; break; }
        if (found < 0) {
            qWarning("devshot: no indexed file matches '%s'", qUtf8Printable(needle));
        } else {
            const QByteArray d = ix.readFile(ix.files()[found]);
            QFile f(out);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(d);
                f.close();
                qInfo("devshot: %s -> %s (%lld bytes)",
                      qUtf8Printable(ix.files()[found].path), qUtf8Printable(out),
                      qint64(d.size()));
            } else {
                qWarning("devshot: cannot write %s", qUtf8Printable(out));
            }
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpParams.isEmpty()) {
        const QString needle = m_shot.dumpParams.section(QLatin1Char('='), 0, 0);
        const QString outTsv = m_shot.dumpParams.section(QLatin1Char('='), 1);
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        QFile f(outTsv);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning("devshot: cannot write %s", qUtf8Printable(outTsv));
            if (!m_shot.stay) QApplication::quit();
            return;
        }
        QTextStream ts(&f);
        // The parameter's HASH travels with its name. The name is only there
        // because the dictionary resolved it; the hash is what the renderer
        // actually matches on, and a new texrole constant cannot be written
        // without it.
        ts << "model\tmat\tshader\tmt\tuv1\tparam\thash\tx\ty\tz\tw\n";
        int models = 0, rows = 0, matsWithParams = 0, mats = 0;
        for (const fox::IndexedFile& e : ix.files()) {
            if (!e.path.endsWith(QLatin1String(".fmdl"), Qt::CaseInsensitive))
                continue;
            if (!needle.isEmpty() && !e.path.contains(needle, Qt::CaseInsensitive))
                continue;
            fox::FmdlFile m;
            if (!m.parse(ix.readFile(e))) continue;
            ++models;
            const QString stem = e.path.section('/', -1);
            // Which materials are used by a mesh that declares a SECOND UV
            // set (vertex-attribute usage 9). That is the question a UV-repeat
            // parameter turns on: a repeat of 100 cannot be scaling a fitted
            // atlas laid out in uv0, but it can scale a tiling detail sampled
            // from uv1.
            QSet<int> matHasUv1;
            for (const fox::FmdlMesh& me : m.meshes()) {
                if (me.materialInstanceIndex < 0) continue;
                for (quint8 u : me.vertexUsages)
                    if (u == 9) { matHasUv1.insert(me.materialInstanceIndex); break; }
            }
            for (int mi = 0; mi < m.materials().size(); ++mi) {
                const fox::FmdlMaterialInstance& inst = m.materials()[mi];
                ++mats;
                if (!inst.params.isEmpty()) ++matsWithParams;
                const fox::MaterialModel mm = fox::classifyShader(inst.shader);
                for (const fox::FmdlMaterialParam& p : inst.params) {
                    ts << stem << '\t' << mi << '\t' << inst.shader << '\t'
                       << mm.materialTypes << '\t'
                       << (matHasUv1.contains(mi) ? 1 : 0) << '\t'
                       << p.name << '\t'
                       << QString::number(p.nameHash32, 16) << '\t'
                       << p.value[0] << '\t' << p.value[1] << '\t'
                       << p.value[2] << '\t' << p.value[3] << '\n';
                    ++rows;
                }
            }
        }
        f.close();
        qInfo("devshot: paramdump '%s' -> %s (%d model(s), %d material(s), "
              "%d with parameters, %d row(s))",
              qUtf8Printable(needle), qUtf8Printable(outTsv), models, mats,
              matsWithParams, rows);
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpAnimBind.isEmpty()) {
        const QString needle = m_shot.dumpAnimBind.section(QLatin1Char('='), 0, 0);
        const QString outTsv = m_shot.dumpAnimBind.section(QLatin1Char('='), 1);
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        int modelIdx = -1;
        for (int i = 0; i < ix.files().size(); ++i) {
            if (fox::ArchiveIndex::extensionOf(ix.files()[i])
                != QLatin1String("fmdl"))
                continue;
            if (!ix.files()[i].path.contains(needle, Qt::CaseInsensitive))
                continue;
            modelIdx = i;
            break;
        }
        fox::FmdlFile model;
        if (modelIdx < 0
            || !model.parse(ix.readFile(ix.files()[modelIdx]))) {
            qWarning("devshot: animbind — no model matches '%s'",
                     qUtf8Printable(needle));
            if (!m_shot.stay) QApplication::quit();
            return;
        }
        QFile f(outTsv);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning("devshot: cannot write %s", qUtf8Printable(outTsv));
            if (!m_shot.stay) QApplication::quit();
            return;
        }
        const QVector<quint32> bones = fox::modelBoneHashes(model);
        fox::FrigFile frig;
        QString via;
        const bool haveFrig =
            rigbind::loadFrigFor(ix.files()[modelIdx].path, &frig, &via);
        QTextStream ts(&f);
        // `rigNames` is the column that matters for the open question: how
        // many of the archive's rig-unit names the MODEL'S RIG also names.
        // `score` is what the panel filters on today and never looks at those
        // names at all — see animBindNameOverlap. Both are written so one run
        // on a real install shows whether they agree.
        ts << "game\tgroup\tmtar\tv2\tclip0\tunits\tclips\tshared\tscore"
              "\trigNames\trigFrac\n";
        const fox::AnimCatalog& cat = fox::AnimCatalog::instance();
        // The distribution, bucketed, is the reason this probe exists: a
        // threshold is only defensible if the two populations do not overlap.
        int buckets[10] = {};
        int over = 0;
        int bestOverlap = -1, withRig = 0;
        QString bestOverlapStem;
        for (int ai : cat.order()) {
            const fox::AnimArchive& a = cat.archives()[ai];
            int shared = 0;
            const float sc = fox::animBindScore(a, bones,
                                                haveFrig ? &frig : nullptr,
                                                &shared);
            buckets[qBound(0, int(sc * 10.0f), 9)]++;
            if (sc >= fox::animBindThreshold()) ++over;
            ts << a.game << '\t' << a.group << '\t' << a.stem << '\t'
               << (a.v2 ? 1 : 0) << '\t' << (a.unitsFromClip0 ? 1 : 0) << '\t'
               << a.unitHashes.size() << '\t' << a.clips.size() << '\t'
               << shared << '\t'
               << QString::number(double(sc), 'f', 3) << '\t';
            const int ov = fox::animBindNameOverlap(a, haveFrig ? &frig : nullptr);
            ts << ov << '\t'
               << (ov < 0 || a.unitHashes.isEmpty()
                       ? QStringLiteral("-")
                       : QString::number(double(ov) / a.unitHashes.size(),
                                         'f', 3))
               << '\n';
            if (ov > bestOverlap) { bestOverlap = ov; bestOverlapStem = a.stem; }
            if (ov >= 0) ++withRig;
        }
        f.close();
        QStringList hist;
        for (int b = 0; b < 10; ++b)
            hist << QStringLiteral("%1-%2:%3")
                        .arg(b / 10.0, 0, 'f', 1)
                        .arg((b + 1) / 10.0, 0, 'f', 1)
                        .arg(buckets[b]);
        qInfo("devshot: animbind '%s' (%d bones, rig %s) -> %s | %d archive(s), "
              "%d at/over %.2f | %s",
              qUtf8Printable(ix.files()[modelIdx].path), int(bones.size()),
              haveFrig ? qUtf8Printable(via) : "none",
              qUtf8Printable(outTsv), int(cat.archives().size()), over,
              double(fox::animBindThreshold()), qUtf8Printable(hist.join(' ')));
        // The diagnostic, said out loud: the score above never consults which
        // bones an archive animates, so this is the number that would.
        qInfo("devshot: animbind rig-name overlap — %d archive(s) comparable, "
              "best %d unit name(s) shared with the model's rig (%s)",
              withRig, bestOverlap,
              bestOverlapStem.isEmpty() ? "none"
                                        : qUtf8Printable(bestOverlapStem));
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpAnims.isEmpty()) {
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        QFile f(m_shot.dumpAnims);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning("devshot: cannot write %s", qUtf8Printable(m_shot.dumpAnims));
            if (!m_shot.stay) QApplication::quit();
            return;
        }
        QTextStream ts(&f);
        ts << "game\tdir\tmtar\tv2\tclip\tname\tnamed\tbytes\tframes\ttracks"
              "\tcategory\tlabel\n";
        int archives = 0, failed = 0, clips = 0, named = 0, decoded = 0;
        for (const fox::IndexedFile& e : ix.files()) {
            if (fox::ArchiveIndex::extensionOf(e) != QLatin1String("mtar"))
                continue;
            fox::MtarFile m;
            const QByteArray d = ix.readFile(e);
            if (d.isEmpty() || !m.parse(d)) { ++failed; continue; }
            ++archives;
            // "/Assets/tpp/motion/mtar/player2/Receiver/x.mtar" splits into the
            // GAME and the path under motion/mtar, which is the shelf the file
            // sits on and the only grouping the data states outright.
            const QStringList seg = e.path.split(QLatin1Char('/'),
                                                 Qt::SkipEmptyParts);
            const QString game = seg.size() > 1 ? seg[1] : QString();
            int cut = e.path.indexOf(QLatin1String("/motion/mtar/"),
                                     0, Qt::CaseInsensitive);
            QString dir = cut >= 0 ? e.path.mid(cut + 13) : e.path;
            dir = dir.section(QLatin1Char('/'), 0, -2);
            const QString stem = e.path.section(QLatin1Char('/'), -1)
                                     .section(QLatin1Char('.'), 0, 0);
            for (int c = 0; c < m.clips().size(); ++c) {
                const fox::MtarClip& cl = m.clips()[c];
                ++clips;
                const bool hasName = !cl.name.isEmpty();
                if (hasName) ++named;
                // The frame count costs a decode, which is the expensive part
                // of this probe — but "how long is it" is exactly what a
                // grouping needs in order to tell a one-frame pose from a
                // walk cycle, so it is worth paying once.
                const fox::GaniAnim a = m.decodeClip(c);
                if (a.valid()) ++decoded;
                ts << game << '\t' << dir << '\t' << stem << '\t'
                   << (m.isV2() ? 1 : 0) << '\t' << c << '\t'
                   << (hasName ? cl.name
                               : QStringLiteral("0x%1").arg(cl.hash, 0, 16))
                   << '\t' << (hasName ? 1 : 0) << '\t' << cl.size << '\t'
                   << (a.valid() ? a.frameCount : 0) << '\t'
                   << (a.valid() ? int(a.tracks.size()) : 0) << '\t'
                   << fox::animCategoryName(
                          fox::animCategoryFor(cl.name, stem + '/' + dir))
                   << '\t' << fox::animLabelFor(cl.name) << '\n';
            }
        }
        f.close();
        qInfo("devshot: animdump -> %s | %d archive(s) parsed, %d failed, "
              "%d clip(s), %d named, %d decoded",
              qUtf8Printable(m_shot.dumpAnims), archives, failed, clips, named,
              decoded);
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    // ── The asset-health audit (§13) ────────────────────────────────────
    // Walks every model, says whether the tool can actually DRAW it, and diffs
    // against the previous run. The diff is the feature: a patch or a mod
    // install silently changes what resolves, and without a before-and-after
    // that shows up weeks later as "this used to work".
    //
    // Deliberately cheap per model — it parses the FMDL and RESOLVES its
    // texture references against the index, and does not decode a single
    // texture or upload anything. A full install is tens of thousands of
    // models; an audit that rendered each one would be an overnight job and
    // would never be run.
    if (!m_shot.healthAudit.isEmpty()) {
        writeHealthAudit(m_shot.healthAudit);
        // The QUIT belongs to the shot, not to the audit — Help ▸ Health check
        // runs the same code and must not close the application when it is
        // done. Lifting the audit into a method is what separated them.
        if (!m_shot.stay) QApplication::quit();
        return;
    }

    if (!m_shot.dumpFovaBind.isEmpty()) {
        // Which models does a .fv2 actually MATCH, judged by its contents?
        //
        // The wiki is explicit that a table names no target and is bound by
        // filename. But a table's material-instance hashes are StrCode32 of
        // the TARGET model's material names, and its mesh-group hashes are
        // StrCode32 of the target's group names — so overlap with a model's
        // own hashes is evidence the filename convention cannot give. This
        // probe measures how far that evidence goes before any of it is wired
        // into a menu.
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        QFile f(m_shot.dumpFovaBind);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning("devshot: cannot write %s", qUtf8Printable(m_shot.dumpFovaBind));
            if (!m_shot.stay) QApplication::quit();
            return;
        }
        // Every model's own material-instance and mesh-group hashes, once.
        struct ModelKeys {
            QString stem;
            QSet<quint32> mats;
            QSet<quint32> groups;
        };
        QVector<ModelKeys> models;
        QHash<quint64, int> modelByHash;   // PathCode64 -> index into models
        int parsedModels = 0;
        for (int i = 0; i < ix.files().size(); ++i) {
            const fox::IndexedFile& e = ix.files()[i];
            if (!e.path.endsWith(QLatin1String(".fmdl"), Qt::CaseInsensitive))
                continue;
            fox::FmdlFile m;
            if (!m.parse(ix.readFile(e))) continue;
            ++parsedModels;
            ModelKeys k;
            k.stem = e.path.section(QLatin1Char('/'), -1)
                         .section(QLatin1Char('.'), 0, 0);
            // The parser already carries the StrCode32 of each material
            // instance name — the same hash a .fv2 substitution names.
            for (const fox::FmdlMaterialInstance& mi : m.materials())
                k.mats.insert(mi.nameHash32);
            for (const fox::FmdlMeshGroup& g : m.meshGroups())
                k.groups.insert(g.nameHash32);
            models.append(k);
            if (e.hash) modelByHash.insert(e.hash, models.size() - 1);
        }

        QTextStream ts(&f);
        ts << "table\tsubs\thide\tshow\tattach\tattachedStems\t"
              "matHits\tgroupHits\tbestStem\tbestScore\tties\n";
        int tables = 0, byMat = 0, byGroup = 0, byAttach = 0, unique = 0, none = 0;
        for (int i = 0; i < ix.files().size(); ++i) {
            const fox::IndexedFile& e = ix.files()[i];
            if (!e.path.endsWith(QLatin1String(".fv2"), Qt::CaseInsensitive))
                continue;
            fox::FovaFile fv;
            if (!fv.parse(ix.readFile(e))) continue;
            ++tables;
            QSet<quint32> wantMats, wantGroups;
            for (const fox::FovaSubstitution& sb : fv.substitutions())
                wantMats.insert(sb.materialHash32);
            for (quint32 h : fv.hiddenMeshGroups()) wantGroups.insert(h);
            for (quint32 h : fv.shownMeshGroups()) wantGroups.insert(h);

            QStringList attached;
            for (const fox::FovaAttachment& at : fv.attachments()) {
                if (at.modelIndex < 0 || at.modelIndex >= fv.files().size())
                    continue;
                const auto mi = modelByHash.constFind(fv.files()[at.modelIndex]);
                if (mi != modelByHash.constEnd())
                    attached << models[*mi].stem;
            }

            // Score every model by how much of the table's own hash set it
            // owns. A material hit is worth more than a group hit: group names
            // like "MESH_head" repeat across the whole cast, material instance
            // names much less so.
            int bestScore = 0, ties = 0, matHits = 0, groupHits = 0;
            QString best;
            for (const ModelKeys& k : models) {
                int mh = 0, gh = 0;
                for (quint32 h : wantMats) if (k.mats.contains(h)) ++mh;
                for (quint32 h : wantGroups) if (k.groups.contains(h)) ++gh;
                const int score = mh * 4 + gh;
                if (score == 0) continue;
                if (score > bestScore) {
                    bestScore = score; best = k.stem; ties = 1;
                    matHits = mh; groupHits = gh;
                } else if (score == bestScore) {
                    ++ties;
                }
            }
            // Counted from the TABLE's own hash sets, not from the winning
            // model's hit counts. matHits/groupHits are only assigned inside
            // the "beat the best" branch, so a table whose hashes match no
            // model at all scores 0, is skipped, and would have been counted
            // as carrying no hashes — which is the exact population this probe
            // exists to find.
            if (!wantMats.isEmpty()) ++byMat;
            if (!wantGroups.isEmpty()) ++byGroup;
            if (!attached.isEmpty()) ++byAttach;
            if (bestScore > 0 && ties == 1) ++unique;
            if (bestScore == 0 && attached.isEmpty()) ++none;

            ts << e.path.section(QLatin1Char('/'), -1) << '\t'
               << fv.substitutions().size() << '\t'
               << fv.hiddenMeshGroups().size() << '\t'
               << fv.shownMeshGroups().size() << '\t'
               << fv.attachments().size() << '\t'
               << attached.join(QLatin1Char(' ')) << '\t'
               << matHits << '\t' << groupHits << '\t'
               << best << '\t' << bestScore << '\t' << ties << '\n';
        }
        f.close();
        qInfo("devshot: fovabind -> %s | %d model(s) parsed, %d table(s): "
              "%d carry material hashes, %d carry group hashes, %d attach a "
              "model that is in this install, %d score a UNIQUE best model, "
              "%d match nothing at all",
              qUtf8Printable(m_shot.dumpFovaBind), parsedModels, tables,
              byMat, byGroup, byAttach, unique, none);
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpFovaCensus.isEmpty()) {
        const QString needle = m_shot.dumpFovaCensus.section(QLatin1Char('='), 0, 0);
        const QString outTsv = m_shot.dumpFovaCensus.section(QLatin1Char('='), 1);
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        QFile f(outTsv);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning("devshot: cannot write %s", qUtf8Printable(outTsv));
            if (!m_shot.stay) QApplication::quit();
            return;
        }
        QTextStream ts(&f);
        ts << "fova\tmatHash\troleHash\tfileHash\tfilePath\n";
        int tables = 0, rows = 0;
        for (int i = 0; i < ix.files().size(); ++i) {
            const fox::IndexedFile& e = ix.files()[i];
            if (!e.path.endsWith(QLatin1String(".fv2"), Qt::CaseInsensitive)) continue;
            if (!needle.isEmpty() && !e.path.contains(needle, Qt::CaseInsensitive))
                continue;
            fox::FovaFile fv;
            if (!fv.parse(ix.readFile(e))) continue;
            ++tables;
            const QString stem = e.path.section('/', -1);
            for (const fox::FovaSubstitution& sub : fv.substitutions()) {
                if (sub.textureIndex < 0 || sub.textureIndex >= fv.files().size())
                    continue;
                const quint64 h = fv.files()[sub.textureIndex];
                const fox::IndexedFile* t = ix.findByHash(h);
                ts << stem << '\t' << QString::number(sub.materialHash32, 16) << '\t'
                   << QString::number(sub.roleHash32, 16) << '\t'
                   << QString::number(h, 16) << '\t'
                   << (t ? t->path : QString()) << '\n';
                ++rows;
            }
        }
        f.close();
        qInfo("devshot: fovacensus '%s' -> %s (%d table(s), %d substitution(s))",
              qUtf8Printable(needle), qUtf8Printable(outTsv), tables, rows);
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpPbr.isEmpty()) {
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        int models = 0;
        qint64 mats = 0, srm = 0, trm = 0, lay = 0, lmk = 0, nrm = 0, base = 0;
        for (int i = 0; i < ix.files().size(); ++i) {
            const fox::IndexedFile& e = ix.files()[i];
            if (!e.path.endsWith(QLatin1String(".fmdl"), Qt::CaseInsensitive))
                continue;
            if (!e.path.contains(m_shot.dumpPbr, Qt::CaseInsensitive)) continue;
            const modelload::LoadedModel lm =
                modelload::load(i, modelload::PbrMode::Full);
            if (!lm.ok) continue;
            ++models;
            base += lm.texturesFound;
            nrm += lm.normalMapsFound;
            QString detail;
            for (int mi = 0; mi < lm.pbr.size(); ++mi) {
                const GLPbrMaterial& p = lm.pbr[mi];
                ++mats;
                if (!p.material.isNull()) ++srm;
                if (!p.translucent.isNull()) ++trm;
                if (!p.layer.isNull()) ++lay;
                if (!p.layerMask.isNull()) ++lmk;
                detail += QStringLiteral(" [%1:%2%3%4%5%6%7]")
                              .arg(mi)
                              .arg(p.material.isNull() ? QString() : QStringLiteral("srm "))
                              .arg(p.translucent.isNull() ? QString() : QStringLiteral("trm "))
                              .arg(p.layer.isNull() ? QString() : QStringLiteral("lay "))
                              .arg(p.layerMask.isNull() ? QString() : QStringLiteral("lmk "))
                              .arg(p.skin ? QStringLiteral("skin ") : QString())
                              .arg(p.layerMul ? QStringLiteral("mul") :
                                   (p.layerBlend ? QStringLiteral("bl") : QString()));
            }
            qInfo("pbr: %-38s %d base, %d nrm, %d map(s)%s",
                  qUtf8Printable(e.path.section('/', -1)), lm.texturesFound,
                  lm.normalMapsFound, lm.pbrMapsFound, qUtf8Printable(detail));
        }
        qInfo("devshot: pbrdump '%s' — %d model(s), %lld material(s): %lld base, "
              "%lld normal, %lld srm, %lld trm, %lld layer, %lld layermask",
              qUtf8Printable(m_shot.dumpPbr), models, mats, base, nrm, srm, trm,
              lay, lmk);
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpColors.isEmpty()) {
        fox::LayerColorCatalog& lc = fox::LayerColorCatalog::instance();
        lc.build();
        qInfo("layer: %s", qUtf8Printable(lc.note()));
        const auto show = [&](const char* caption,
                              const QVector<fox::LayerSwatch>& set) {
            qInfo("%s (%lld):", caption, qint64(set.size()));
            for (const fox::LayerSwatch& sw : set) {
                const QColor c = lc.colorOf(sw);
                qInfo("   %-12s %-4s  %s  %s", qUtf8Printable(sw.family),
                      qUtf8Printable(sw.id),
                      c.isValid() ? qUtf8Printable(c.name().toUpper()) : "-------",
                      qUtf8Printable(QString::number(sw.pathHash, 16)));
            }
        };
        show("solid colours", lc.solids());
        show("camo patterns", lc.patterns());

        // What the palette would actually paint. "Colourable" is the shader's
        // own answer, not a guess from which textures happen to be bound.
        const QString needle =
            m_shot.dumpColors == QLatin1String("*") ? QString() : m_shot.dumpColors;
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        int models = 0, colourable = 0, white = 0;
        for (const fox::IndexedFile& e : ix.files()) {
            if (!e.path.endsWith(QLatin1String(".fmdl"), Qt::CaseInsensitive))
                continue;
            if (!needle.isEmpty() && !e.path.contains(needle, Qt::CaseInsensitive))
                continue;
            fox::FmdlFile m;
            if (!m.parse(ix.readFile(e))) continue;
            ++models;
            const int n = modelload::colourableMaterialCount(m);
            if (n == 0) continue;
            colourable += n;
            // How many of those ship with the white placeholder in the layer
            // slot — the ones a colour visibly changes.
            int w = 0;
            for (const fox::FmdlMaterialInstance& mat : m.materials()) {
                if (!fox::classifyShader(mat.shader).colourable()) continue;
                for (const fox::FmdlTextureRef& r : mat.textures) {
                    if (r.roleHash32 != fox::texrole::kLayer) continue;
                    if (r.path.contains(QLatin1String("cm_flat_white"))) ++w;
                    break;
                }
            }
            white += w;
            qInfo("   %-40s %d colourable material(s), %d white",
                  qUtf8Printable(e.path.section('/', -1)), n, w);
        }
        qInfo("devshot: colordump '%s' — %d model(s), %d colourable material(s), "
              "%d shipping the white placeholder",
              qUtf8Printable(m_shot.dumpColors), models, colourable, white);
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpMatCensus.isEmpty()) {
        const QString needle = m_shot.dumpMatCensus.section(QLatin1Char('='), 0, 0);
        const QString outTsv = m_shot.dumpMatCensus.section(QLatin1Char('='), 1);
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        QFile f(outTsv);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning("devshot: cannot write %s", qUtf8Printable(outTsv));
            if (!m_shot.stay) QApplication::quit();
            return;
        }
        QTextStream ts(&f);
        ts << "model\tmat\tmatName\tshader\tkind\tfeatures\trole\troleHash"
              "\ttexHash\ttexPath\n";
        int models = 0, rows = 0, failed = 0;
        for (int i = 0; i < ix.files().size(); ++i) {
            const fox::IndexedFile& e = ix.files()[i];
            if (!e.path.endsWith(QLatin1String(".fmdl"), Qt::CaseInsensitive))
                continue;
            if (!needle.isEmpty() && !e.path.contains(needle, Qt::CaseInsensitive))
                continue;
            fox::FmdlFile m;
            if (!m.parse(ix.readFile(e))) { ++failed; continue; }
            ++models;
            const QString stem = e.path.section('/', -1);
            for (int mi = 0; mi < m.materials().size(); ++mi) {
                const fox::FmdlMaterialInstance& mat = m.materials()[mi];
                const fox::MaterialModel mm = fox::classifyShader(mat.shader);
                static const char* kKind[] = {"Blin", "Skin", "Cloth", "Hair",
                                              "Eye", "Glass", "Constant",
                                              "MetalicBacteria", "Other"};
                QStringList feat;
                if (mm.layerMul) feat << QStringLiteral("layerMul");
                if (mm.layerBlend) feat << QStringLiteral("layerBlend");
                if (mm.dirty) feat << QStringLiteral("dirty");
                if (mm.subNormal) feat << QStringLiteral("subNormal");
                if (mm.tension) feat << QStringLiteral("tension");
                if (mm.incidence) feat << QStringLiteral("incidence");
                if (mm.forward) feat << QStringLiteral("forward");
                if (mm.alphaCutout) feat << QStringLiteral("cutout");
                if (mm.materialTypes > 1)
                    feat << QStringLiteral("%1MT").arg(mm.materialTypes);
                const QString kindName =
                    QString::fromLatin1(kKind[int(mm.kind)]);
                const QString featName = feat.join(QLatin1Char(','));
                for (const fox::FmdlTextureRef& t : mat.textures) {
                    ts << stem << '\t' << mi << '\t' << mat.name << '\t'
                       << mat.shader << '\t' << kindName << '\t' << featName
                       << '\t' << t.role << '\t'
                       << QString::number(t.roleHash32, 16) << '\t'
                       << QString::number(t.pathHash, 16) << '\t' << t.path << '\n';
                    ++rows;
                }
            }
        }
        f.close();
        qInfo("devshot: matcensus '%s' -> %s (%d model(s), %d row(s), %d unparsed)",
              qUtf8Printable(needle), qUtf8Printable(outTsv), models, rows, failed);
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpFova.isEmpty()) {
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        int seen = 0;
        for (int i = 0; i < ix.files().size(); ++i) {
            const fox::IndexedFile& e = ix.files()[i];
            if (!e.path.endsWith(QLatin1String(".fv2"), Qt::CaseInsensitive))
                continue;
            if (!e.path.contains(m_shot.dumpFova, Qt::CaseInsensitive)) continue;
            fox::FovaFile fv;
            if (!fv.parse(ix.readFile(e))) continue;
            ++seen;
            qInfo("fova: %s — %s", qUtf8Printable(e.path.section('/', -1)),
                  qUtf8Printable(fv.describe()));
            for (quint32 h : fv.hiddenMeshGroups()) qInfo("   hide  %08x", h);
            for (quint32 h : fv.shownMeshGroups()) qInfo("   show  %08x", h);
            int lost = 0;
            QVector<int> idx;
            const QVector<modelload::LoadedModel> got =
                modelload::fovaAttachedModels(fv, &lost, &idx);
            for (int k = 0; k < got.size(); ++k)
                qInfo("   attach %s (%lld bone(s), %lld material(s))",
                      qUtf8Printable(ix.files()[idx[k]].path.section('/', -1)),
                      qint64(got[k].model.bones().size()),
                      qint64(got[k].textures.size()));
            if (lost)
                qInfo("   %d attachment(s) skipped (connect-point form, or not "
                      "in this install)", lost);
        }
        qInfo("devshot: fovadump '%s' — %d table(s)", qUtf8Printable(m_shot.dumpFova),
              seen);
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpTree.isEmpty()) {
        const QString needle = m_shot.dumpTree.section(QLatin1Char('='), 0, 0);
        const QString outDir = m_shot.dumpTree.section(QLatin1Char('='), 1);
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        int written = 0, failed = 0;
        for (int i = 0; i < ix.files().size(); ++i) {
            const fox::IndexedFile& e = ix.files()[i];
            if (!e.path.contains(needle, Qt::CaseInsensitive)) continue;
            // The indexed path is absolute inside the game's virtual tree, so
            // it is joined RELATIVE to outDir — otherwise QDir would resolve
            // the leading slash and scatter files across the real filesystem.
            QString rel = e.path;
            while (rel.startsWith(QLatin1Char('/'))) rel.remove(0, 1);
            const QString dest = outDir + QLatin1Char('/') + rel;
            QDir().mkpath(QFileInfo(dest).absolutePath());
            QFile f(dest);
            if (!f.open(QIODevice::WriteOnly)) { ++failed; continue; }
            f.write(ix.readFile(e));
            f.close();
            ++written;
        }
        qInfo("devshot: dumptree '%s' -> %s (%d written, %d failed)",
              qUtf8Printable(needle), qUtf8Printable(outDir), written, failed);
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpPlayers.isEmpty()) {
        QFile f(m_shot.dumpPlayers);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            out << "subject\tgame\tgender\tslot\tparts\tsample\n";
            const fox::BuilderSource bs =
                fox::PlayerCatalog::instance().builderSource();
            for (const fox::CatalogSubject& cs : bs.subjects)
                out << cs.id << '\t' << cs.groupName << '\t' << "-" << '\t'
                    << "(variants)" << '\t' << cs.variants.size() << '\t'
                    << (cs.variants.isEmpty() ? QString() : cs.variants.first().id)
                    << '\n';
            const fox::PlayerCatalog& pc = fox::PlayerCatalog::instance();
            for (const fox::PlayerSubject& s : pc.subjects())
                for (const fox::PlayerSlot& sl : s.slotList) {
                    QStringList sample;
                    for (int i = 0; i < sl.parts.size() && i < 5; ++i)
                        sample << sl.parts[i].id;
                    out << s.name << '\t' << fox::gameShortName(s.game) << '\t'
                        << fox::genderName(s.gender) << '\t' << sl.label << '\t'
                        << sl.parts.size() << '\t'
                        << sample.join(QLatin1Char(' ')) << '\n';
                }
            f.close();
            qInfo("devshot: player dump -> %s | %s", qUtf8Printable(m_shot.dumpPlayers),
                  qUtf8Printable(pc.describe()));
        } else {
            qWarning("devshot: cannot write %s", qUtf8Printable(m_shot.dumpPlayers));
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (m_shot.dumpPng.startsWith(QLatin1Char('*'))) {
        // Batch form: --pngdump "*<substring>=<dir>" decodes every indexed
        // texture whose path contains <substring> and writes it as a PNG named
        // after the asset stem. One index build instead of one per texture,
        // which is the difference between looking at an icon set and not.
        const QString needle = m_shot.dumpPng.mid(1).section(QLatin1Char('='), 0, 0);
        const QString outDir = m_shot.dumpPng.section(QLatin1Char('='), 1);
        if (outDir.isEmpty()) {
            qWarning("devshot: --pngdump batch needs \"*<substring>=<dir>\"");
            if (!m_shot.stay) QApplication::quit();
            return;
        }
        QDir().mkpath(outDir);
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        int n = 0, bad = 0;
        QSet<QString> seen;
        for (const fox::IndexedFile& f : ix.files()) {
            if (!f.path.contains(needle, Qt::CaseInsensitive)) continue;
            // .ftex only: the decoder resolves a texture by hashing
            // <stem>.ftex, so a .dds entry either fails or, worse, silently
            // decodes an unrelated sibling and writes it under the .dds name.
            if (!f.path.endsWith(QLatin1String(".ftex"), Qt::CaseInsensitive))
                continue;
            QString stem = f.path.section(QLatin1Char('/'), -1);
            stem.truncate(stem.lastIndexOf(QLatin1Char('.')));
            if (seen.contains(stem)) continue;
            seen.insert(stem);
            QString base = f.path;
            base.truncate(base.lastIndexOf(QLatin1Char('.')));
            const QPixmap pm = fox::IconCatalog::instance().swatchForPath(base, 512);
            if (pm.isNull() || !pm.save(outDir + QLatin1Char('/') + stem + QLatin1String(".png")))
                ++bad;
            else
                ++n;
        }
        qInfo("devshot: png batch '%s' -> %s (%d written, %d failed)",
              qUtf8Printable(needle), qUtf8Printable(outDir), n, bad);
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpPng.isEmpty()) {
        const QString path = m_shot.dumpPng.section(QLatin1Char('='), 0, 0);
        const QString out = m_shot.dumpPng.section(QLatin1Char('='), 1);
        const QPixmap pm = fox::IconCatalog::instance().swatchForPath(path, 1024);
        if (pm.isNull())
            qWarning("devshot: %s did not decode", qUtf8Printable(path));
        else if (!pm.save(out))
            qWarning("devshot: cannot write %s", qUtf8Printable(out));
        else
            qInfo("devshot: %s -> %s (%dx%d)", qUtf8Printable(path), qUtf8Printable(out),
                  pm.width(), pm.height());
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpMaterials.isEmpty()) {
        // The single most useful thing to know about a character model: for
        // every material, what it resolved to, what came back, and what colour
        // it is. A white eyebrow and a missing eyebrow look identical in a
        // screenshot and are completely different bugs.
        const QString needle = m_shot.dumpMaterials.section(QLatin1Char('='), 0, 0);
        const QString outDir = m_shot.dumpMaterials.section(QLatin1Char('='), 1);
        QDir().mkpath(outDir);
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        int fileIdx = -1;
        for (int i = 0; i < ix.files().size(); ++i)
            if (ix.files()[i].path.contains(needle, Qt::CaseInsensitive)
                && ix.files()[i].path.endsWith(QLatin1String(".fmdl"),
                                               Qt::CaseInsensitive)) {
                fileIdx = i;
                break;
            }
        if (fileIdx < 0) {
            qWarning("devshot: no model matches '%s'", qUtf8Printable(needle));
        } else {
            const modelload::LoadedModel lm = modelload::load(fileIdx);
            QFile tsv(outDir + QStringLiteral("/materials.tsv"));
            if (tsv.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream ts(&tsv);
                ts << "material\tname\tshader\trole\tresolved\thash\tsize\t"
                      "meanRGBA\n";
                for (int mi = 0; mi < lm.model.materials().size(); ++mi) {
                    const fox::FmdlMaterialInstance& mat = lm.model.materials()[mi];
                    const QImage img = mi < lm.textures.size() ? lm.textures[mi]
                                                               : QImage();
                    QString mean = QStringLiteral("-");
                    if (!img.isNull()) {
                        const QImage a = img.convertToFormat(QImage::Format_RGBA8888);
                        qint64 r = 0, g = 0, b = 0, al = 0, np = 0;
                        for (int y = 0; y < a.height(); y += 4) {
                            const uchar* row = a.constScanLine(y);
                            for (int x = 0; x < a.width(); x += 4) {
                                r += row[x * 4]; g += row[x * 4 + 1];
                                b += row[x * 4 + 2]; al += row[x * 4 + 3];
                                ++np;
                            }
                        }
                        if (np)
                            mean = QStringLiteral("%1,%2,%3,%4")
                                       .arg(r / np).arg(g / np).arg(b / np).arg(al / np);
                        img.save(outDir + QStringLiteral("/mat%1_base.png")
                                              .arg(mi, 2, 10, QLatin1Char('0')));
                    }
                    for (const fox::FmdlTextureRef& t : mat.textures)
                        ts << mi << '\t' << mat.name << '\t' << mat.shader << '\t'
                           << t.role << '\t'
                           << (t.path.isEmpty() ? QStringLiteral("(unnamed)") : t.path)
                           << '\t' << QString::number(t.pathHash, 16) << '\t'
                           << (img.isNull() ? QStringLiteral("-")
                                            : QStringLiteral("%1x%2")
                                                  .arg(img.width()).arg(img.height()))
                           << '\t' << mean << '\n';
                }
                tsv.close();
            }
            qInfo("devshot: material dump -> %s (%s, %lld material(s))",
                  qUtf8Printable(outDir), qUtf8Printable(ix.files()[fileIdx].path),
                  qint64(lm.model.materials().size()));
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpAvatarTex.isEmpty()) {
        const fox::AvatarTextures& at = fox::AvatarTextures::instance();
        QFile f(m_shot.dumpAvatarTex);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << "set\tindex\tpath\n";
            for (int s : at.skins())
                ts << "skin\t" << s << '\t' << at.facePath(0, s) << '\n';
            for (int w : at.wrinkles())
                ts << "wrinkle\t" << w << '\t' << at.facePath(w, 0) << '\n';
            // Clamp rather than hard-code a colour: an install with fewer
            // than three hair colours would otherwise dump every beard row
            // empty and read as "no beards".
            const int bcol = qMin(2, at.hairColours().size() - 1);
            for (int i = 0; i < at.beardShapes().size(); ++i) {
                ts << "beard\t" << at.beardShapes()[i] << '\t'
                   << at.beardPath(i, bcol) << '\n';
                ts << "beardskin\t" << at.beardShapes()[i] << '\t'
                   << at.beardSkinPath(i, bcol) << '\n';
            }
            // Both genders, side by side: the whole point of the body set is
            // that there are two of them and the wrong one is invisible until
            // someone notices the male survivor has the women's skin.
            for (int i = 0; i < 5; ++i) {
                ts << "body-women\t" << i << '\t' << at.bodyPath(i, false) << '\n';
                ts << "body-men\t" << i << '\t' << at.bodyPath(i, true) << '\n';
            }
            for (int c : at.irisColours())
                ts << "iris\t"
                   << QString::fromLatin1(fox::AvatarTextures::irisName(c))
                   << '\t' << at.irisPath(c, at.irisShades().value(0, 0))
                   << '\n';
            for (int i = 0; i < at.browShapes().size(); ++i)
                ts << "brow\t" << at.browShapes()[i] << '\t'
                   << at.browPath(i, 0) << '\n';
            // Both hair families per style, side by side. hair0 is the card
            // atlas the mesh wears and hair1 is the hairline painted on the
            // face; they are different art and confusing them is invisible in
            // any single row, so the dump prints them together.
            for (const char* stem : {"avf_hair_a0_v0_cov", "avf_hair_b0_v0_cov",
                                     "avm_hair_a0_v0_cov", "avm_hair_b0_v0_cov"})
                for (int i = 0; i < at.hairColours().size(); ++i) {
                    const QString st = QString::fromLatin1(stem);
                    ts << "hair-mesh\t" << st << '/' << at.hairColours()[i]
                       << '\t' << at.hairPath(st, i, 0) << '\n';
                    ts << "hair-line\t" << st << '/' << at.hairColours()[i]
                       << '\t' << at.hairSkinPath(st, i) << '\n';
                }
            for (int fam = 0; fam < 3; ++fam)
                for (int id : at.decoIds(fam))
                    ts << fox::AvatarTextures::decoName(fam) << '\t' << id
                       << '\t' << at.decoPath(fam, id, 0) << '\n';
            f.close();
            qInfo("devshot: avatar texture dump -> %s | %s",
                  qUtf8Printable(m_shot.dumpAvatarTex), qUtf8Printable(at.note()));
        } else {
            qWarning("devshot: cannot write %s", qUtf8Printable(m_shot.dumpAvatarTex));
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpThumbs.isEmpty()) {
        // Render every matching model through the same offscreen renderer the
        // grid uses, so what lands on disk is exactly what the browser draws.
        const QString needle = m_shot.dumpThumbs.section(QLatin1Char('='), 0, 0);
        const QString dir = m_shot.dumpThumbs.section(QLatin1Char('='), 1);
        QDir().mkpath(dir);
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        auto* want = new QHash<int, QString>;
        for (int i = 0; i < ix.files().size(); ++i) {
            const fox::IndexedFile& f = ix.files()[i];
            if (fox::ArchiveIndex::extensionOf(f) != QLatin1String("fmdl")) continue;
            if (!needle.isEmpty() && !f.path.contains(needle, Qt::CaseInsensitive))
                continue;
            want->insert(i, f.path.section(QLatin1Char('/'), -1)
                                 .section(QLatin1Char('.'), 0, 0));
        }
        qInfo("devshot: thumbdump '%s' -> %s (%lld model(s))", qUtf8Printable(needle),
              qUtf8Printable(dir), qint64(want->size()));
        auto* left = new int(want->size());
        fox::ThumbnailRenderer& tr = fox::ThumbnailRenderer::instance();
        tr.prewarm();
        connect(&tr, &fox::ThumbnailRenderer::ready, this,
                [this, want, left, dir](int fileIdx, int size) {
                    if (!want->contains(fileIdx)) return;
                    const QPixmap pm =
                        fox::ThumbnailRenderer::instance().cached(fileIdx, size);
                    if (!pm.isNull())
                        pm.save(dir + QLatin1Char('/') + want->value(fileIdx)
                                + QStringLiteral(".png"));
                    if (--(*left) <= 0) {
                        qInfo("devshot: thumbdump complete");
                        if (!m_shot.stay) QApplication::quit();
                    }
                });
        for (auto it = want->constBegin(); it != want->constEnd(); ++it)
            tr.request(it.key(), 192);
        // A model that fails to render never fires `ready`, so do not wait for
        // it for ever.
        QTimer::singleShot(900000, this, [this] {
            qWarning("devshot: thumbdump timed out");
            if (!m_shot.stay) QApplication::quit();
        });
        return;
    }
    if (m_shot.cacheCheck) {
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        // A file whose bytes actually come out of a CONTAINER. A loose mount
        // has none — its entries are files on disk, there is nothing to
        // inflate and nothing to cache — so on a loose-only tree this reports
        // that rather than failing a check it could not run.
        const fox::IndexedFile* pick = nullptr;
        for (const fox::IndexedFile& c : ix.files()) {
            if (c.archiveId < 0 || c.archiveId >= ix.archives().size()) continue;
            if (ix.archives()[c.archiveId].kind == fox::ArchiveKind::Loose)
                continue;
            pick = &c;
            break;
        }
        if (ix.files().isEmpty()) {
            qWarning("cachecheck: nothing indexed");
        } else if (!pick) {
            qInfo("cachecheck: SKIPPED — this mount has no archive-backed "
                  "entry, so there is no container read to cache");
        } else {
            const fox::IndexedFile& f = *pick;
            // OUTSIDE a scope: every read goes to the archive, and the cache
            // must record nothing at all.
            const QByteArray a1 =
                extract::blobcache::readEntry(f.archiveId, f.entryIdx);
            const QByteArray a2 =
                extract::blobcache::readEntry(f.archiveId, f.entryIdx);
            const extract::blobcache::Stats off = extract::blobcache::stats();
            qInfo("cachecheck: outside a scope — %lld hit(s), %lld miss(es), "
                  "%d entr(ies), %lld byte(s) held",
                  off.hits, off.misses, off.entries, off.bytes);
            // INSIDE: the first read misses and is kept, the second hits, and
            // the bytes are the same object either way.
            bool same = false;
            extract::blobcache::Stats on{};
            {
                extract::blobcache::Scope scope(64LL * 1024 * 1024);
                const QByteArray b1 =
                    extract::blobcache::readEntry(f.archiveId, f.entryIdx);
                const QByteArray b2 =
                    extract::blobcache::readEntry(f.archiveId, f.entryIdx);
                same = b1 == b2 && b1 == a1;
                on = extract::blobcache::stats();
            }
            const extract::blobcache::Stats after = extract::blobcache::stats();
            qInfo("cachecheck: inside a scope — %lld hit(s), %lld miss(es), "
                  "%d entr(ies), %lld byte(s) held; bytes identical: %s",
                  on.hits, on.misses, on.entries, on.bytes,
                  same ? "yes" : "NO");
            qInfo("cachecheck: after the scope — %d entr(ies), %lld byte(s) "
                  "held (both must be 0)",
                  after.entries, after.bytes);
            qInfo("cachecheck: %s",
                  (off.hits == 0 && off.entries == 0 && on.hits == 1
                   && on.misses == 1 && on.entries == 1 && same
                   && after.entries == 0 && after.bytes == 0)
                      ? "PASS" : "FAIL");
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    // ── Bulk Extract, headless (§8) ─────────────────────────────────────
    // Placed with the other data probes rather than with the screenshots,
    // because it produces files and a log line and never grabs a pixel.
    if (!m_shot.bulkOut.isEmpty() || !m_shot.bulkQueueAdd.isEmpty()) {
        if (!m_bulkTab) {
            qWarning("devshot: no bulk tab");
            if (!m_shot.stay) QApplication::quit();
            return;
        }
        // AN OMITTED FILTER IS NOT AN EMPTY ONE. An empty query has no text
        // terms and no tags, so it matches every file in the index — which
        // makes `--bulkout D:\\probe` with a forgotten `--bulk` extract the
        // whole install, and `--bulkqueueadd` alone union 200,000 hashes into
        // the user's saved queue and write them all into the portable INI.
        // Neither is recoverable and neither was asked for, so saying "all" has
        // to be deliberate. (The earlier note that "a typo in a filter cannot
        // start a run" was right about typos and silent about omissions.)
        if (m_shot.bulkQuery.isEmpty() && m_shot.bulkExt.isEmpty()
            && !m_shot.bulkUseQueue && !m_shot.bulkAll) {
            qWarning("devshot: --bulkout/--bulkqueueadd with no --bulk, "
                     "--bulkext or --bulkqueue would take the WHOLE index. "
                     "Add --bulkall if that is what you meant.");
            if (!m_shot.stay) QApplication::quit();
            return;
        }
        m_bulkTab->configureRun(m_shot.bulkQuery, m_shot.bulkExt, m_shot.bulkOut,
                                m_shot.bulkWorkers, m_shot.bulkOverwrite,
                                m_shot.bulkUseQueue);
        if (!m_shot.bulkQueueAdd.isEmpty()) {
            m_bulkTab->addMatchesToQueue();
            qInfo("devshot: queue updated");
            if (!m_shot.stay) QApplication::quit();
            return;
        }
        connect(m_bulkTab, &BulkExtractorTab::runFinished, this,
                [this](int written, int failed, int skipped) {
                    qInfo("devshot: bulk finished — %d written, %d failed, "
                          "%d skipped",
                          written, failed, skipped);
                    if (!m_shot.stay) QApplication::quit();
                });
        if (m_shot.bulkPauseMs > 0)
            QTimer::singleShot(m_shot.bulkPauseMs, this, [this] {
                m_bulkTab->pauseForShot(true);
                QTimer::singleShot(1000, this,
                                   [this] { m_bulkTab->pauseForShot(false); });
            });
        if (m_shot.bulkCancelMs > 0)
            QTimer::singleShot(m_shot.bulkCancelMs, this,
                               [this] { m_bulkTab->cancelForShot(); });
        m_bulkTab->startRun();
        // A run over a filter that matches nothing never starts and so never
        // finishes; without this the harness would hang looking like a slow
        // extraction rather than an empty one.
        if (!m_bulkTab->extracting()) {
            qWarning("devshot: bulk matched nothing — nothing to run");
            if (!m_shot.stay) QApplication::quit();
        }
        return;
    }
    if (!m_shot.dumpTags.isEmpty()) {
        const fox::ModelTags& mt = fox::ModelTags::instance();
        QFile f(m_shot.dumpTags);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << "category\ttag\tlabel\tcount\n";
            int tags = 0;
            for (const fox::TagCategory& c : mt.categories())
                for (const fox::TagInfo& t : c.tags) {
                    ts << c.id << '\t' << t.tag << '\t' << t.label << '\t'
                       << t.count << '\n';
                    ++tags;
                }
            f.close();
            qInfo("devshot: tag dump -> %s (%lld categor(ies), %d tag(s), "
                  "%d model(s))",
                  qUtf8Printable(m_shot.dumpTags), qint64(mt.categories().size()),
                  tags, mt.modelCount());
        } else {
            qWarning("devshot: cannot write %s", qUtf8Printable(m_shot.dumpTags));
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpAvatar.isEmpty()) {
        const fox::AvatarPresets& ap = fox::AvatarPresets::instance();
        QFile f(m_shot.dumpAvatar);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << "sex\tpreset\thead\thair\thairColour\tbrow\tbeard\t"
                  "beardVar\teyes\teyeColR\teyeColL\tskin\tfeature\ticon\n";
            for (fox::AvatarPresets::Sex sex : {fox::AvatarPresets::Sex::Women,
                                                fox::AvatarPresets::Sex::Men})
            for (const fox::AvatarPreset& p : ap.presets(sex))
                ts << (sex == fox::AvatarPresets::Sex::Men ? "m" : "f") << '\t'
                   << (p.index + 1) << '\t'
                   << fox::AvatarPresets::headStemFor(p.faceType, sex) << '\t'
                   << (p.hairMesh < 0
                           ? QStringLiteral("(bald)")
                           : fox::AvatarPresets::hairStemFor(p.hairMesh, sex))
                   << '\t' << p.hairColour << '\t' << p.browShape << '\t'
                   << p.beard << '\t' << p.beardVariant << '\t'
                   << p.eyeSet << '\t'
                   << QStringLiteral("%1/%2").arg(p.eyeColourR).arg(p.eyeShadeR)
                   << '\t'
                   << QStringLiteral("%1/%2").arg(p.eyeColourL).arg(p.eyeShadeL)
                   << '\t' << p.skinColour << '\t'
                   << (p.decoType < 0
                           ? QStringLiteral("-")
                           : QStringLiteral("%1.%2").arg(p.decoType).arg(p.decoId))
                   << '\t' << fox::AvatarPresets::iconPathFor(p.index, sex)
                   << '\n';
            f.close();
            qInfo("devshot: avatar dump -> %s | %s", qUtf8Printable(m_shot.dumpAvatar),
                  qUtf8Printable(ap.note()));
        } else {
            qWarning("devshot: cannot write %s", qUtf8Printable(m_shot.dumpAvatar));
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpHash.isEmpty() && m_shot.dumpHash.startsWith(QLatin1String("EXT="))) {
        // Extension-code table: the 13-bit code an archive key carries in its
        // top bits, for every extension the hasher knows. An external extractor
        // needs this to give a pulled entry its real file name.
        const QString out = m_shot.dumpHash.mid(4);
        QFile f(out);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            for (const QString& e : fox::HashResolver::instance().allExtensions())
                ts << QString::number(fox::hashExtension(e), 16) << '\t'
                   << e << '\n';
            f.close();
            qInfo("devshot: extension table -> %s", qUtf8Printable(out));
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpHash.isEmpty()) {
        const QString needle = m_shot.dumpHash.section(QLatin1Char('='), 0, 0);
        const QString out = m_shot.dumpHash.section(QLatin1Char('='), 1);
        QFile f(out);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            int n = 0;
            for (const QString& name :
                 fox::HashResolver::instance().allNames()) {
                if (!needle.isEmpty() && !name.contains(needle, Qt::CaseInsensitive))
                    continue;
                ts << QString::number(fox::hashFileName(name) & fox::kPathMask, 16)
                   << '\t' << name << '\n';
                ++n;
            }
            f.close();
            qInfo("devshot: hash dump -> %s (%d name(s) matching '%s')",
                  qUtf8Printable(out), n, qUtf8Printable(needle));
        } else {
            qWarning("devshot: cannot write %s", qUtf8Printable(out));
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpFiles.isEmpty()) {
        QFile f(m_shot.dumpFiles);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            out << "path\tsize\tnamed\tgz\tshadowed\tarchive\n";
            const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
            for (const fox::IndexedFile& e : ix.files())
                out << e.path << '\t' << e.size << '\t' << (e.named ? 1 : 0)
                    << '\t' << (e.gz ? 1 : 0) << '\t' << (e.shadowed ? 1 : 0)
                    << '\t'
                    << (e.archiveId >= 0 && e.archiveId < ix.archives().size()
                            ? ix.archives()[e.archiveId].shortName
                            : QString())
                    << '\n';
            f.close();
            qInfo("devshot: file dump -> %s (%lld files)", qUtf8Printable(m_shot.dumpFiles),
                  qint64(ix.files().size()));
        } else {
            qWarning("devshot: cannot write %s", qUtf8Printable(m_shot.dumpFiles));
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpSwatch.isEmpty() || !m_shot.dumpCompat.isEmpty()
        || !m_shot.dumpEquip.isEmpty() || !m_shot.dumpCamo.isEmpty()) {
        if (!m_shot.outPng.isEmpty())
            qInfo("devshot: a data dump was requested — writing the dump and "
                  "skipping the screenshot");
    }
    if (!m_shot.dumpSwatch.isEmpty()) {
        // Data probe: what the customize screen's colour icons actually are.
        QDir().mkpath(m_shot.dumpSwatch);
        const auto& files = fox::ArchiveIndex::instance().files();
        int n = 0, ok = 0;
        QFile list(m_shot.dumpSwatch + QStringLiteral("/paths.txt"));
        if (!list.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning("devshot: cannot write %s/paths.txt",
                     qUtf8Printable(m_shot.dumpSwatch));
            if (!m_shot.stay) QApplication::quit();
            return;
        }
        QTextStream lo(&list);
        for (const fox::IndexedFile& f : files) {
            if (!f.path.contains(QLatin1String("/ui/texture/Customize/"),
                                 Qt::CaseInsensitive)
                && !f.path.contains(QLatin1String("/common_source/layer/"),
                                    Qt::CaseInsensitive)
                && !(f.path.contains(QLatin1String("/chara/"), Qt::CaseInsensitive)
                     && f.path.contains(QLatin1String("_bsm."), Qt::CaseInsensitive)))
                continue;
            if (!f.path.endsWith(QLatin1String(".ftex"), Qt::CaseInsensitive)) continue;
            ++n;
            const QString base = f.path.left(f.path.size() - 5);
            lo << f.path << '\n';
            const QPixmap pm =
                fox::IconCatalog::instance().swatchForPath(base, 64);
            if (pm.isNull()) continue;
            ++ok;
            pm.save(m_shot.dumpSwatch + QLatin1Char('/')
                    + base.section(QLatin1Char('/'), -1) + QStringLiteral(".png"));
        }
        list.close();
        // …and every appearance variation each catalogue offers, so the two
        // categories the customize screen splits Color into can be decided
        // from the game's own names rather than by eye.
        QFile vf(m_shot.dumpSwatch + QStringLiteral("/variations.tsv"));
        if (vf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream vo(&vf);
            vo << "category\tsubject\tvariant\tname\tpath\n";
            const fox::BuilderSource srcs[] = {
                fox::WeaponCatalog::instance().builderSource(),
                fox::CharacterCatalog::instance().builderSource(),
                fox::MechaCatalog::instance().builderSource(),
            };
            const char* const tags[] = {"weapon", "character", "vehicle"};
            for (int c = 0; c < 3; ++c)
                for (const fox::CatalogSubject& subj : srcs[c].subjects)
                    for (const fox::CatalogPart& r : subj.variants)
                        for (const fox::CatalogVariation& v :
                             srcs[c].variationsFor(r.displayName))
                            vo << tags[c] << '\t' << subj.id << '\t'
                               << r.displayName << '\t' << v.name << '\t'
                               << v.path << '\n';
            vf.close();
        }
        // …and what each of those tables actually SUBSTITUTES, so the claim
        // that weapon camo_cNN is chip camo4_cNN is checked against the table
        // rather than assumed from the numbering.
        QFile sf(m_shot.dumpSwatch + QStringLiteral("/substitutions.tsv"));
        if (sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream so(&sf);
            so << "category\tname\tfova\tsubstituted\n";
            const fox::BuilderSource srcs2[] = {
                fox::WeaponCatalog::instance().builderSource(),
                fox::CharacterCatalog::instance().builderSource(),
                fox::MechaCatalog::instance().builderSource(),
            };
            const char* const tags2[] = {"weapon", "character", "vehicle"};
            const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
            QSet<int> seen;
            for (int c = 0; c < 3; ++c)
                for (const fox::CatalogSubject& subj : srcs2[c].subjects)
                    for (const fox::CatalogPart& r : subj.variants)
                        for (const fox::CatalogVariation& v :
                             srcs2[c].variationsFor(r.displayName)) {
                            if (v.fileIdx < 0 || v.fileIdx >= ix.files().size()) continue;
                            if (seen.contains(v.fileIdx)) continue;
                            seen.insert(v.fileIdx);
                            fox::FovaFile fova;
                            const QByteArray d = ix.readFile(ix.files()[v.fileIdx]);
                            if (d.isEmpty() || !fova.parse(d)) continue;
                            for (quint64 t : fova.textures()) {
                                QString path;
                                if (!fox::HashResolver::instance().tryResolve(t, &path))
                                    continue;
                                so << tags2[c] << '\t' << v.name << '\t'
                                   << v.path.section(QLatin1Char('/'), -1) << '\t'
                                   << path << '\n';
                            }
                        }
            sf.close();
        }
        qInfo("devshot: swatch dump — %d Customize texture(s), %d decoded -> %s",
              n, ok, qUtf8Printable(m_shot.dumpSwatch));
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpCompat.isEmpty()) {
        // Data probe: for every receiver the weapon catalogue carries, how many
        // parts each slot would offer with the compatibility switch ON versus
        // OFF. This is the exact pair CustomizeTab::refreshSlotItems() computes
        // — an EMPTY allowed-set there means "no rule", and the list is left
        // whole, which is what a filter that looks like it does nothing is.
        QFile f(m_shot.dumpCompat);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            const fox::EquipCatalog& eq = fox::EquipCatalog::instance();
            const fox::BuilderSource src = fox::WeaponCatalog::instance().builderSource();
            const QString hostSlot = src.slotNames.value(0);
            QStringList slotList;
            for (const QString& slot : src.slotNames) {
                if (slot == hostSlot) continue;
                slotList << slot;
                const QString second = slot + QLatin1Char('2');
                if (fox::EquipCatalog::baseSlot(second) == slot
                    && eq.slotIsUsed(second))
                    slotList << second;
            }
            out << "receiver\tknown\thasBuilds\tbarrel\tslot\ttotal\tallowed\tunusable\n";
            for (const fox::CatalogSubject& subj : src.subjects) {
                for (const fox::CatalogPart& r : subj.variants) {
                    const QString rc = r.displayName;
                    // Twice: with no barrel fitted (how a weapon first opens)
                    // and with the barrel the game's own build gives it.
                    QStringList barrels{QString()};
                    for (const fox::WeaponPreset& p : eq.presets())
                        if (p.stemFor(hostSlot) == rc) {
                            const QString b = p.stemFor(QStringLiteral("barrel"));
                            if (!b.isEmpty() && !barrels.contains(b)) barrels << b;
                            break;
                        }
                    for (const QString& ba : barrels) {
                        for (const QString& slot : slotList) {
                            const int total =
                                src.partsFor(fox::EquipCatalog::baseSlot(slot)).size();
                            if (total == 0) continue;
                            const QSet<QString> allowed =
                                eq.compatibleStems(slot, rc, ba);
                            const bool unusable = eq.hasBuildsFor(rc)
                                && !eq.slotEverUsedOn(rc, slot);
                            out << rc << '\t' << (eq.knowsReceiver(rc) ? 1 : 0)
                                << '\t' << (eq.hasBuildsFor(rc) ? 1 : 0) << '\t'
                                << (ba.isEmpty() ? QStringLiteral("-") : ba) << '\t'
                                << slot << '\t' << total << '\t'
                                << (allowed.isEmpty() ? -1 : int(allowed.size()))
                                << '\t' << (unusable ? 1 : 0) << '\n';
                        }
                    }
                }
            }
            f.close();
            qInfo("devshot: compat dump -> %s", qUtf8Printable(m_shot.dumpCompat));
            QFile g(m_shot.dumpCompat + QStringLiteral(".inclusion.tsv"));
            if (g.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream go(&g);
                go << eq.dumpInclusion();
                g.close();
            }
        } else {
            qWarning("devshot: cannot write %s", qUtf8Printable(m_shot.dumpCompat));
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.assetMenu.isEmpty()) {
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        int fi = ix.fileIndexForPath(m_shot.assetMenu);
        if (fi < 0)   // …or the first file whose path contains it
            for (int i = 0; i < ix.files().size() && fi < 0; ++i)
                if (ix.files()[i].path.contains(m_shot.assetMenu,
                                                Qt::CaseInsensitive))
                    fi = i;
        if (fi < 0) {
            qWarning("assetmenu: '%s' matched nothing",
                     qUtf8Printable(m_shot.assetMenu));
        } else {
            auto* menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            exportactions::addFileActions(menu, fi, this);
            QStringList labels;
            for (QAction* a : menu->actions()) {
                if (a->isSeparator()) { labels << QStringLiteral("---"); continue; }
                QString l = a->text();
                l.remove(QLatin1Char('&'));
                if (a->menu())
                    l += QStringLiteral(" [%1 item(s)]").arg(a->menu()->actions().size());
                if (!a->isEnabled()) l += QStringLiteral(" (disabled)");
                labels << l;
            }
            qInfo("assetmenu: %s -> %s", qUtf8Printable(ix.files()[fi].path),
                  qUtf8Printable(labels.join(QStringLiteral(" | "))));
            menu->close();
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.texDds.isEmpty()) {
        const QString asset = m_shot.texDds.section(QLatin1Char('='), 0, 0);
        const QString outPath = m_shot.texDds.section(QLatin1Char('='), 1);
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        const int fi = ix.fileIndexForPath(asset);
        QString err;
        int missing = 0;
        QByteArray dds;
        if (fi < 0) err = QStringLiteral("'%1' is not in the index").arg(asset);
        else dds = extract::assembleFtexToDds(ix.files()[fi], &err, &missing);
        if (!dds.isEmpty()) {
            QFile f(outPath);
            if (f.open(QIODevice::WriteOnly)) { f.write(dds); f.close(); }
            else err = QStringLiteral("could not write %1").arg(outPath);
        }
        qInfo("texdds: %s -> %s — %s%s", qUtf8Printable(asset),
              qUtf8Printable(outPath),
              dds.isEmpty() ? qUtf8Printable(err)
                            : qUtf8Printable(QStringLiteral("%1 bytes").arg(dds.size())),
              missing > 0 ? "  (some mips are in .ftexs streams this install "
                            "does not have — the DDS starts at the largest one "
                            "that IS present)" : "");
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.modReplaceTex.isEmpty()) {
        const QString asset = m_shot.modReplaceTex.section(QLatin1Char('='), 0, 0);
        const QString ddsPath = m_shot.modReplaceTex.section(QLatin1Char('='), 1);
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        const int fi = ix.fileIndexForPath(asset);
        QString err;
        int wrote = 0;
        if (fi < 0) {
            err = QStringLiteral("'%1' is not in the index").arg(asset);
        } else {
            const QByteArray original = ix.readFile(ix.files()[fi]);
            QFile in(ddsPath);
            QByteArray dds;
            if (in.open(QIODevice::ReadOnly)) { dds = in.readAll(); in.close(); }
            if (original.isEmpty()) err = QStringLiteral("could not read the original");
            else if (dds.isEmpty()) err = QStringLiteral("could not read %1").arg(ddsPath);
            else {
                const fox::FtexWriteResult w = fox::writeFtexLike(original, dds);
                if (!w.ok()) {
                    err = w.error;
                } else {
                    const QString stem = asset.left(asset.size() - 5);
                    QVector<QPair<QString, QByteArray>> set;
                    set.append({asset, w.ftex});
                    for (auto it = w.ftexs.constBegin(); it != w.ftexs.constEnd(); ++it)
                        set.append({stem + QStringLiteral(".%1.ftexs").arg(it.key()),
                                    it.value()});
                    wrote = set.size();
                    err = modfolder::putSet(set);
                }
            }
        }
        qInfo("mod: replace texture %s <- %s — %s (%d file(s))",
              qUtf8Printable(asset), qUtf8Printable(ddsPath),
              err.isEmpty() ? "ok" : qUtf8Printable(err), err.isEmpty() ? wrote : 0);
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.ftexRoundTrip.isEmpty()) {
        // ── THE ACCEPTANCE TEST FOR TEXTURE REPLACEMENT ──────────────────
        // Assemble a shipped texture to DDS, write it back through the writer,
        // assemble THAT, and compare. The two DDS files must be identical —
        // not similar, identical — because the second one went through every
        // decision the writer makes: the chunk split, the compress-or-store
        // test, the offsets, the mip-to-ftexs assignment.
        //
        // Doing it over the WHOLE corpus rather than a sample is the point. A
        // writer that is right about 2,700 mips and wrong about the 4x4 one at
        // the end of the chain looks perfect on any single texture somebody
        // thought to try.
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        QFile f(m_shot.ftexRoundTrip);
        const bool haveFile = f.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream out(&f);
        if (haveFile)
            out << "asset\tformat\tmips\tftexs\torigBytes\tnewBytes\tddsBytes"
                   "\tverdict\n";
        int tried = 0, same = 0, differ = 0, refused = 0, unreadable = 0;
        // ── THE PER-MIP PASS ─────────────────────────────────────────────
        // A whole-FILE round trip needs every .N.ftexs stream present, and on
        // a gear extract most textures keep their two largest mips in streams
        // the pull does not include — four of 283 here. Four is not a corpus.
        // Every mip that IS readable, on the other hand, can be re-chunked and
        // read straight back, and that exercises the entire writer: the
        // 16,384-byte split, the compress-or-store test, the index records and
        // their relative offsets. 
        int mips = 0, mipSame = 0, mipDiff = 0;
        int mipCountMatch = 0, mipCountDiff = 0;
        QStringList mipFailures;
        for (int i = 0; i < ix.files().size(); ++i) {
            const fox::IndexedFile& e = ix.files()[i];
            if (!extract::isFtex(e)) continue;
            const QByteArray rawFtex = ix.readFile(e);
            fox::FtexFile ff;
            if (rawFtex.isEmpty() || !ff.parse(rawFtex)) continue;
            QHash<int, QByteArray> src;
            src.insert(0, rawFtex);
            for (const fox::FtexMipInfo& mi : ff.mipInfos()) {
                if (mi.chunkCount <= 0) continue;
                if (!src.contains(mi.ftexsFileNumber)) {
                    // The stream files by their own path, which on a loose
                    // mount is how they are indexed: "<stem>.<N>.ftexs".
                    const QString sp =
                        e.path.left(e.path.size() - 5)
                        + QStringLiteral(".%1.ftexs").arg(mi.ftexsFileNumber);
                    const int si = ix.fileIndexForPath(sp);
                    src.insert(mi.ftexsFileNumber,
                               si >= 0 ? ix.readFile(ix.files()[si]) : QByteArray());
                }
                const QByteArray& s0 = src[mi.ftexsFileNumber];
                if (s0.isEmpty()) continue;
                bool ok = false;
                const QByteArray got =
                    fox::FtexFile::readChunkedMip(s0, mi.offset, mi.chunkCount, &ok);
                if (!ok || got.size() != mi.decompressedSize) continue;
                ++mips;
                int cc = 0;
                const QByteArray packed = fox::chunkMip(got, &cc);
                // …and the FIDELITY check the corpus can give that a
                // self-consistency check cannot: does our chunk count match
                // the one the game shipped for this exact mip? A wrong chunk
                // size would still round-trip through our own reader, so
                // without this the constant would be untested.
                if (cc == mi.chunkCount) ++mipCountMatch; else ++mipCountDiff;
                bool ok2 = false;
                const QByteArray back =
                    fox::FtexFile::readChunkedMip(packed, 0, cc, &ok2);
                if (ok2 && back == got) {
                    ++mipSame;
                } else {
                    ++mipDiff;
                    if (mipFailures.size() < 8)
                        mipFailures << QStringLiteral("%1 mip %2")
                                           .arg(e.path).arg(int(mi.index));
                }
            }
        }
        qInfo("ftexroundtrip: %d mip(s) re-chunked and read back — %d "
              "IDENTICAL, %d different", mips, mipSame, mipDiff);
        qInfo("ftexroundtrip: chunk COUNT vs the shipped file — %d match, "
              "%d differ (this is what tests the 16,384-byte constant; a wrong "
              "size still round-trips through our own reader)",
              mipCountMatch, mipCountDiff);
        for (const QString& p : mipFailures)
            qWarning("ftexroundtrip:   MIP DIFFERENT  %s", qUtf8Printable(p));

        QStringList firstFailures;
        for (int i = 0; i < ix.files().size(); ++i) {
            const fox::IndexedFile& e = ix.files()[i];
            if (!extract::isFtex(e)) continue;
            QString err;
            int missing = 0;
            const QByteArray dds = extract::assembleFtexToDds(e, &err, &missing);
            const QByteArray raw = ix.readFile(e);
            if (dds.isEmpty() || raw.isEmpty() || missing > 0) {
                ++unreadable;
                continue;
            }
            ++tried;
            const fox::FtexWriteResult w = fox::writeFtexLike(raw, dds);
            fox::FtexFile back;
            QString verdict;
            qint64 newBytes = w.ftex.size();
            for (auto it = w.ftexs.constBegin(); it != w.ftexs.constEnd(); ++it)
                newBytes += it.value().size();
            if (!w.ok()) {
                verdict = QStringLiteral("REFUSED — ") + w.error;
                ++refused;
            } else if (!back.parse(w.ftex)) {
                verdict = QStringLiteral("REFUSED — the written .ftex does not "
                                         "parse: ") + back.errorString();
                ++refused;
            } else {
                const QByteArray again = back.assembleDds(
                    w.ftex, [&w](int n) { return w.ftexs.value(n); });
                if (again == dds) { verdict = QStringLiteral("identical"); ++same; }
                else {
                    verdict = QStringLiteral("DIFFERENT — %1 bytes out, %2 back")
                                  .arg(dds.size()).arg(again.size());
                    ++differ;
                    if (firstFailures.size() < 8) firstFailures << e.path;
                }
            }
            fox::FtexFile of;
            of.parse(raw);
            if (haveFile)
                out << e.path << '\t' << of.describe().section(QLatin1Char(' '), 0, 0)
                    << '\t' << int(of.mipCount()) << '\t'
                    << int(of.ftexsFileCount()) << '\t' << raw.size() << '\t'
                    << newBytes << '\t' << dds.size() << '\t' << verdict << '\n';
        }
        if (haveFile) f.close();
        qInfo("ftexroundtrip: %d texture(s) round-tripped — %d IDENTICAL, "
              "%d different, %d refused (%d could not be assembled at all and "
              "were skipped) -> %s",
              tried, same, differ, refused, unreadable,
              qUtf8Printable(m_shot.ftexRoundTrip));
        for (const QString& p : firstFailures)
            qWarning("ftexroundtrip:   DIFFERENT  %s", qUtf8Printable(p));
        if (tried > 0 && same == tried)
            qInfo("ftexroundtrip: every texture in this install re-encodes to "
                  "the same image it came from");
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpMod.isEmpty()) {
        // Data probe: what is installed, and — the part that matters — whether
        // each replacement is the copy the index HANDS OUT. A mod folder full
        // of files that override nothing looks identical from outside to a mod
        // folder that works, and the difference is one lookup.
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        const QStringList installed = modfolder::list();
        QFile f(m_shot.dumpMod);
        int winning = 0, notFound = 0, shadowedOut = 0;
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            out << "asset\tbytes\tresolvedArchive\tpriority\tverdict\n";
            for (const QString& asset : installed) {
                const int fi = ix.fileIndexForPath(asset);
                QString arch, verdict;
                int prio = -1;
                if (fi < 0 || fi >= ix.files().size()) {
                    verdict = QStringLiteral("NOT IN THE INDEX — this "
                                             "replacement overrides nothing");
                    ++notFound;
                } else {
                    const fox::IndexedFile& e = ix.files()[fi];
                    if (e.archiveId >= 0 && e.archiveId < ix.archives().size()) {
                        arch = ix.archives()[e.archiveId].shortName;
                        prio = ix.archives()[e.archiveId].priority;
                    }
                    if (ix.archives().value(e.archiveId).kind
                        == fox::ArchiveKind::Loose
                        && prio >= 1100) {
                        verdict = QStringLiteral("active");
                        ++winning;
                    } else {
                        verdict = QStringLiteral("NOT WINNING — the index "
                                                 "resolves this asset to "
                                                 "another mount");
                        ++shadowedOut;
                    }
                }
                out << asset << '\t'
                    << QFileInfo(modfolder::pathFor(asset)).size() << '\t'
                    << arch << '\t' << prio << '\t' << verdict << '\n';
            }
            f.close();
        }
        qInfo("mod: folder %s", modfolder::dir().isEmpty()
                  ? "(none configured)"
                  : qUtf8Printable(modfolder::dir()));
        qInfo("mod: %lld replacement(s) — %d active, %d not in the index, "
              "%d not winning -> %s", qint64(installed.size()), winning,
              notFound, shadowedOut, qUtf8Printable(m_shot.dumpMod));
        for (const QString& asset : installed)
            qInfo("mod:   %s", qUtf8Printable(asset));
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.modPackage.isEmpty()) {
        const modpackage::Result r = modpackage::write(m_shot.modPackage);
        if (!r.error.isEmpty()) {
            qWarning("modpackage: %s", qUtf8Printable(r.error));
        } else {
            qInfo("modpackage: %d asset(s), %lld byte(s) -> %s", r.files,
                  r.bytes, qUtf8Printable(m_shot.modPackage));
            qInfo("modpackage: game copy lives inside a container for %d of "
                  "%d; %d not in this install's index", r.inContainer,
                  r.files, r.notInIndex);
            qInfo("modpackage: archive is %lld byte(s) on disk",
                  QFileInfo(m_shot.modPackage).size());
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.mgsvPackage.isEmpty()) {
        const modpackage::MgsvMeta meta = shotMgsvMeta(
            QFileInfo(m_shot.mgsvPackage).completeBaseName());
        const modpackage::Result r =
            modpackage::writeMgsv(m_shot.mgsvPackage, meta);
        if (!r.error.isEmpty()) {
            qWarning("mgsvpackage: %s", qUtf8Printable(r.error));
        } else {
            qInfo("mgsvpackage: %d QarEntry(s), %lld byte(s) -> %s", r.files,
                  r.bytes, qUtf8Printable(m_shot.mgsvPackage));
            qInfo("mgsvpackage: archive is %lld byte(s) on disk",
                  QFileInfo(m_shot.mgsvPackage).size());
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (m_shot.texUserSweep) {
        // QUEUE 4d. The sweep runs on its own thread and publishes through
        // atomics, so this waits with a plain sleep rather than pumping the
        // event loop — the same reason the Textures tab's wait does. The cap
        // is thirty minutes rather than the tab's sixty seconds, because the
        // whole point is that nobody knows how long this takes and a wait
        // shorter than the answer would report the question as the answer.
        fox::TextureUsers& tu = fox::TextureUsers::instance();
        tu.build();
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        const qint64 until = t0 + 30LL * 60LL * 1000LL;
        int lastDone = -1;
        while (tu.state() == fox::TextureUsers::State::Building
               && QDateTime::currentMSecsSinceEpoch() < until) {
            QThread::msleep(250);
            // A progress line every 5,000 models, so a run that is going to
            // take minutes says so instead of looking hung.
            if (tu.done() / 5000 != lastDone / 5000) {
                lastDone = tu.done();
                qInfo("texusers: %d of %d model(s) swept, %lld ms so far",
                      tu.done(), tu.total(),
                      QDateTime::currentMSecsSinceEpoch() - t0);
            }
        }
        const qint64 ms = QDateTime::currentMSecsSinceEpoch() - t0;
        const bool ok = tu.state() == fox::TextureUsers::State::Ready;
        qInfo("texusers: %s after %lld ms — %d model(s), %d texture(s), "
              "%d model(s) the sweep could take nothing out of",
              ok ? "COMPLETE" : (tu.state() == fox::TextureUsers::State::Building
                                     ? "STILL RUNNING at the 30 min cap"
                                     : "FAILED"),
              ms, tu.modelCount(), tu.textureCount(), tu.opaqueModelCount());
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (m_shot.camoDefaultTest) {
        qInfo("devshot: camodefault — %s",
              qUtf8Printable(m_customizeTab->camoDefaultSelfTest()));
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpCamo.isEmpty()) {
        // Data probe: QUEUE 2's census. The Customize tab owns the list and
        // its section rule, so the tab is asked rather than the rule being
        // written a second time here.
        qInfo("devshot: camodump — %s",
              qUtf8Printable(m_customizeTab->camoDumpReport(m_shot.dumpCamo)));
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    if (!m_shot.dumpEquip.isEmpty()) {
        // Data probe: the game's own development list, one build per line, so
        // a design question ("how many tiers does the WU S.PISTOL have?") is
        // answered by measuring rather than by assuming.
        QFile f(m_shot.dumpEquip);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            out << "name\tcategory\tgrade\tdevId\tprereq\twpId\tparts\n";
            for (const fox::WeaponPreset& p : fox::EquipCatalog::instance().presets()) {
                QStringList parts;
                for (const auto& sp : p.parts)
                    parts << sp.first + QLatin1Char('=') + sp.second;
                out << p.name << '\t' << p.category << '\t' << p.grade << '\t'
                    << p.devId << '\t' << p.prereqDevId << '\t' << p.wpId << '\t'
                    << parts.join(QLatin1Char(' ')) << '\n';
            }
            f.close();
            qInfo("devshot: equip dump -> %s (%lld builds)",
                  qUtf8Printable(m_shot.dumpEquip),
                  qint64(fox::EquipCatalog::instance().presets().size()));
        } else {
            qWarning("devshot: cannot write %s", qUtf8Printable(m_shot.dumpEquip));
        }
        if (!m_shot.stay) QApplication::quit();
        return;
    }
    // BEFORE any builder branch. The colour is folded in as each part loads,
    // so selecting it afterwards would need a second full rebuild — and
    // selecting it inside only ONE branch (which is what this used to do) made
    // --gearcolor silently do nothing alongside --preset and --userpreset.
    // A saved preset that names its own gearcolor= still wins, because
    // loadWeaponPreset() applies the spec's value after this runs.
    // An INDEX-WIDE dump: nothing to do with the Customize page, so it does not
    // sit inside the builder branch. It did, briefly, and simply never ran.
    if (!m_shot.variantCensus.isEmpty()) writeVariantCensus(m_shot.variantCensus);
    if (!m_shot.gearColor.isEmpty())
        qInfo("devshot: gear colour '%s' %s", qUtf8Printable(m_shot.gearColor),
              m_customizeTab->selectGearColor(m_shot.gearColor)
                  ? "selected" : "NOT IN THIS INSTALL");
    // FIRST of everything a shot does, because every branch below acts on
    // "the tab in front" and this is what decides which tab that is. It sat
    // after them, so --tab Textures --filterpopup opened the MODELS popup:
    // the chain ran while Files was still frontmost, and the tab switched
    // afterwards. The screenshot was of a real popup on a real tab, just not
    // the pair that was asked for.
    if (!m_shot.tab.isEmpty() && m_tabs) {
        int found = -1;
        for (int i = 0; i < m_tabs->count(); ++i)
            if (m_tabs->tabText(i).compare(m_shot.tab, Qt::CaseInsensitive) == 0)
                found = i;
        if (found >= 0) {
            m_tabs->setCurrentIndex(found);
            for (int i = 0; i < 6; ++i) QCoreApplication::processEvents();
            qInfo("devshot: tab -> %s", qUtf8Printable(m_tabs->tabText(found)));
        } else {
            QStringList have;
            for (int i = 0; i < m_tabs->count(); ++i) have << m_tabs->tabText(i);
            qWarning("devshot: no tab called '%s' — this window has: %s",
                     qUtf8Printable(m_shot.tab),
                     qUtf8Printable(have.join(QLatin1String(", "))));
        }
    }
    if (!m_shot.userPreset.isEmpty()) {
        m_tabs->setCurrentWidget(m_customizeTab);
        // A CHARACTER spec first when one was given, for the same reason the
        // preset branch below builds its weapon first: presets are stored per
        // category, so the page has to be on the right one before the preset
        // is looked up. Without this, --character with --userpreset loaded the
        // character and then searched the WEAPON preset group, and every saved
        // character preset was untestable from the harness.
        if (!m_shot.charSpec.isEmpty())
            qInfo("devshot: character — %s",
                  qUtf8Printable(m_customizeTab->buildFromSpec(2, m_shot.charSpec,
                                                           m_shot.camoFilter)));
        qInfo("devshot: %s",
              qUtf8Printable(m_customizeTab->applyUserPresetFromSpec(
                  m_shot.userPreset, m_shot.compatOnly)));
        // A saved preset carries its OWN colour — including "none", which it
        // applies by clearing. That is right for loading a preset and wrong
        // for a harness run that asked for a colour on the command line, so
        // the flag is re-applied here. Selecting a colour that is already
        // selected changes no index and fires no signal, so this costs
        // nothing when the preset did not clobber it.
        if (!m_shot.gearColor.isEmpty())
            m_customizeTab->selectGearColor(m_shot.gearColor);
    } else if (!m_shot.presetFilter.isEmpty() || m_shot.compatOnly) {
        m_tabs->setCurrentWidget(m_customizeTab);
        // A weapon spec first when one was given, so the preset lands on top of
        // a known starting state rather than on whatever the tab opened with.
        if (!m_shot.weaponSpec.isEmpty())
            qInfo("devshot: weapon — %s",
                  qUtf8Printable(m_customizeTab->buildFromSpec(1, m_shot.weaponSpec,
                                                           m_shot.camoFilter)));
        qInfo("devshot: %s",
              qUtf8Printable(m_customizeTab->applyPresetFromSpec(m_shot.presetFilter,
                                                             m_shot.compatOnly)));
        // The preset branch gets the same three as every other Customize
        // branch. Leaving them out here meant --preset --submeshes --hidemesh
        // did nothing AND logged nothing, which is the failure that takes
        // longest to notice in a harness.
        if (m_shot.showSubmeshTree || !m_shot.hideSubmesh.isEmpty())
            m_customizeTab->setSubmeshTreeVisible(true);
        if (m_shot.showDebugPanel) m_customizeTab->setDebugPanelVisible(true);
        if (!m_shot.matFilter.isEmpty())
            m_customizeTab->setMaterialFilter(m_shot.matFilter);
        if (!m_shot.hideSubmesh.isEmpty())
            qInfo("devshot: hide submesh '%s' %s", qUtf8Printable(m_shot.hideSubmesh),
                  m_customizeTab->hideSubmesh(m_shot.hideSubmesh) ? "ok" : "NOT FOUND");
        if (!m_shot.popupCombo.isEmpty()) {
            const bool ok = m_customizeTab->openBuilderPopup(m_shot.popupCombo);
            qInfo("devshot: popup %s", ok ? "opened" : "NOT FOUND");
            if (ok && !m_shot.popupType.isEmpty())
                m_customizeTab->typeIntoBuilderPopup(m_shot.popupType);
        }
    } else if (!m_shot.weaponSpec.isEmpty() || !m_shot.charSpec.isEmpty()
               || !m_shot.vehicleSpec.isEmpty()) {
        m_tabs->setCurrentWidget(m_customizeTab);
        int cat = 1;
        QString spec = m_shot.weaponSpec, what = QStringLiteral("weapon");
        if (spec.isEmpty() && !m_shot.charSpec.isEmpty()) {
            cat = 2; spec = m_shot.charSpec; what = QStringLiteral("character");
            // "other:dds" reaches the every-other-humanoid category, so the
            // divider below the player characters can be exercised too.
            if (spec.startsWith(QLatin1String("other:"))) {
                cat = 4;
                spec = spec.mid(6);
                what = QStringLiteral("other character");
            }
        } else if (spec.isEmpty()) {
            cat = 3; spec = m_shot.vehicleSpec; what = QStringLiteral("vehicle");
        }
        // BEFORE the build: the switch governs what the spec's own selections
        // do to each other, so setting it afterwards would test the state the
        // run was not asking for.
        if (m_shot.gearUnlocked >= 0)
            m_customizeTab->setGearUnlocked(m_shot.gearUnlocked == 1);
        const QString report =
            m_customizeTab->buildFromSpec(cat, spec, m_shot.camoFilter);
        qInfo("devshot: %s — %s", qUtf8Printable(what), qUtf8Printable(report));
        if (m_shot.pbrOverride >= 0)
            m_customizeTab->setPbrShading(m_shot.pbrOverride == 1);
        if (m_shot.showSubmeshTree || !m_shot.hideSubmesh.isEmpty())
            m_customizeTab->setSubmeshTreeVisible(true);
        if (!m_shot.hideSubmesh.isEmpty())
            qInfo("devshot: hide submesh '%s' %s", qUtf8Printable(m_shot.hideSubmesh),
                  m_customizeTab->hideSubmesh(m_shot.hideSubmesh) ? "ok" : "NOT FOUND");
        // …and the animation, which until now only reached Customize through
        // the --parts branch: "--character X --mtar Y --clip Z" built the
        // character and then left it in bind pose with the combos saying "No
        // animation", which reads as a posing bug rather than a missing line.
        if (!m_shot.mtarFilter.isEmpty()
            && !m_customizeTab->selectAnim(m_shot.mtarFilter, m_shot.clipFilter,
                                           m_shot.frame))
            qWarning("devshot: customize anim did not resolve");
        if (m_shot.showDebugPanel) m_customizeTab->setDebugPanelVisible(true);
        if (!m_shot.matFilter.isEmpty())
            m_customizeTab->setMaterialFilter(m_shot.matFilter);
        // AFTER the build and AFTER the clip: the sweep measures the POSED
        // position of every item, and at bind pose every one of them is right —
        // which is exactly why "it's fine in T-pose" was the reported symptom
        // for eight batches. It replaces the shot rather than accompanying it,
        // like every other data probe.
        if (!m_shot.restAlignSweep.isEmpty()) {
            qInfo("devshot: restalignsweep — %s",
                  qUtf8Printable(
                      m_customizeTab->restAlignSweepReport(m_shot.restAlignSweep)));
            if (!m_shot.stay) QApplication::quit();
            return;
        }
        // Saving is LAST in this branch, so the blob records the page as it
        // finally stands rather than mid-build. A preset is USER DATA with a
        // grammar of its own and there was no way to round-trip it before —
        // which is how two of its fields came to be written in a form that
        // only read back correctly by accident.
        if (!m_shot.savePreset.isEmpty())
            qInfo("devshot: %s",
                  qUtf8Printable(m_customizeTab->saveWeaponPresetAs(m_shot.savePreset)));
        // §15's undo stack, scripted. AFTER the build above, so the sequence
        // starts from a scene rather than from an empty page, and printed line
        // by line: the whole point is that the state after N undos can be
        // compared character-for-character with the state N steps ago, which a
        // screenshot cannot show and a person clicking cannot check.
        if (!m_shot.undoSeq.isEmpty())
            for (const QString& line :
                 m_customizeTab->undoSeqReport(m_shot.undoSeq)
                     .split(QLatin1Char('\n')))
                qInfo("undoseq: %s", qUtf8Printable(line));
        if (!m_shot.popupCombo.isEmpty()) {
            const bool ok = m_customizeTab->openBuilderPopup(m_shot.popupCombo);
            qInfo("devshot: popup %s", ok ? "opened" : "NOT FOUND");
            if (ok && !m_shot.popupType.isEmpty()) {
                m_customizeTab->typeIntoBuilderPopup(m_shot.popupType);
                qInfo("devshot: typed '%s' into the popup",
                      qUtf8Printable(m_shot.popupType));
            }
            if (ok && m_shot.hoverSet)
                qInfo("devshot: hover row %d %s", m_shot.hoverRow,
                      m_customizeTab->hoverBuilderPopupRow(m_shot.hoverRow)
                          ? "sent" : "NOT SENT");
        }
    } else if (!m_shot.partsFilter.isEmpty()) {
        m_tabs->setCurrentWidget(m_customizeTab);
        // Same two before the build as the builder branch below, so a --parts
        // shot is not the one arrangement where these flags quietly do
        // nothing.
        if (m_shot.pbrOverride >= 0)
            m_customizeTab->setPbrShading(m_shot.pbrOverride == 1);
        if (m_shot.showSubmeshTree || !m_shot.hideSubmesh.isEmpty())
            m_customizeTab->setSubmeshTreeVisible(true);
        if (m_shot.showDebugPanel) m_customizeTab->setDebugPanelVisible(true);
        if (!m_shot.matFilter.isEmpty())
            m_customizeTab->setMaterialFilter(m_shot.matFilter);
        const int n = m_customizeTab->equipParts(
            m_shot.partsFilter.split(QLatin1Char(','), Qt::SkipEmptyParts));
        qInfo("devshot: equipped %d parts", n);
        // AFTER the parts are equipped: the tree has no rows until the scene
        // exists, so unchecking one before that silently found nothing.
        if (!m_shot.hideSubmesh.isEmpty())
            qInfo("devshot: hide submesh '%s' %s", qUtf8Printable(m_shot.hideSubmesh),
                  m_customizeTab->hideSubmesh(m_shot.hideSubmesh) ? "ok" : "NOT FOUND");
        if (!m_shot.mtarFilter.isEmpty()
            && !m_customizeTab->selectAnim(m_shot.mtarFilter, m_shot.clipFilter,
                                           m_shot.frame))
            qWarning("devshot: customize anim did not resolve");
        if (!m_shot.attachCnp.isEmpty() && n >= 2) {
            if (m_customizeTab->attachPartTo(1, 0, m_shot.attachCnp))
                qInfo("devshot: attached part 1 at %s",
                      qUtf8Printable(m_shot.attachCnp));
            else
                qWarning("devshot: attach at '%s' failed",
                         qUtf8Printable(m_shot.attachCnp));
        }
    } else if (m_shot.strings) {
        // The Strings tab is the Files tab's strings PANEL now, so the
        // harness goes in the way a person does: open Files, select a
        // language table, then drive the panel that appears.
        m_tabs->setCurrentWidget(m_filesTab);
        StringsPanel* sp = m_filesTab->stringsPanel();
        if (!m_filesTab->showFirstStringTable())
            qWarning("devshot: no .lng2 language table in this index");
        const int n = sp ? sp->applyDevFilter(m_shot.stringFilter,
                                              m_shot.stringsAll)
                         : 0;
        qInfo("devshot: strings '%s'%s -> %d row(s)",
              qUtf8Printable(m_shot.stringFilter),
              m_shot.stringsAll ? " (all tables)" : "", n);
    } else if ((!m_shot.bulkQuery.isEmpty() || !m_shot.bulkExt.isEmpty())
               && m_bulkTab) {
        // --bulk / --bulkext WITHOUT --bulkout: show the tab configured, do
        // not run it. This is how the tab gets photographed, and keeping the
        // "run" verb on --bulkout alone means a filter typo can never start an
        // extraction.
        m_tabs->setCurrentWidget(m_bulkTab);
        m_bulkTab->configureRun(m_shot.bulkQuery, m_shot.bulkExt, QString(),
                                m_shot.bulkWorkers, m_shot.bulkOverwrite,
                                m_shot.bulkUseQueue);
        qInfo("devshot: bulk tab configured");
    } else if (!m_shot.texSearch.isEmpty() || !m_shot.texUsed.isEmpty()
               || !m_shot.texFormat.isEmpty() || !m_shot.texUserTag.isEmpty()
               || !m_shot.texChannel.isEmpty()) {
        m_tabs->setCurrentWidget(m_texturesTab);
        m_texturesTab->setGridForShot(m_shot.grid, m_shot.gridIcon);
        m_texturesTab->setSearchText(m_shot.texSearch);
        // The used/orphan and used-by-tag filters need the sweep. Waiting here
        // rather than measuring whatever the half-built map says is the whole
        // point: an "orphans" count taken mid-sweep is a number that means
        // nothing and looks like a number that means something.
        if (!m_shot.texUsed.isEmpty() || !m_shot.texUserTag.isEmpty()) {
            // A PLAIN sleep, not processEvents. The sweep runs on its own
            // thread and publishes through an atomic, so nothing here needs the
            // event loop — and pumping it from inside the devshot, which is
            // itself called from a signal handler, is a re-entrancy hazard for
            // no gain.
            // Bounded, and it ends when the sweep ENDS — including when it
            // ends badly. A sweep abandoned by a rescan sets Failed; without
            // that this loop spun its whole timeout and then measured a filter
            // against a map that was never going to arrive.
            const qint64 until = QDateTime::currentMSecsSinceEpoch() + 60000;
            while (!m_texturesTab->usersDone()
                   && QDateTime::currentMSecsSinceEpoch() < until)
                QThread::msleep(25);
            if (!m_texturesTab->usersReady()) {
                qWarning("devshot: the texture→model sweep did not finish — "
                         "the filters that need it are not applied");
                m_shot.texUsed.clear();
                m_shot.texUserTag.clear();
            }
        }
        if (!m_shot.texUsed.isEmpty() && !m_texturesTab->setUsedFilter(m_shot.texUsed))
            qWarning("devshot: --texused '%s' is not all|used|orphans",
                     qUtf8Printable(m_shot.texUsed));
        if (!m_shot.texUserTag.isEmpty()
            && !m_texturesTab->setUserTagFilter(m_shot.texUserTag))
            qWarning("devshot: --texusertag '%s' is not a model tag",
                     qUtf8Printable(m_shot.texUserTag));
        if (!m_shot.texFormat.isEmpty()
            && !m_texturesTab->setFormatFilter(m_shot.texFormat))
            qWarning("devshot: --texformat '%s' was not applied — either it is "
                     "not a known format, or the list is too long to read "
                     "headers for (narrow it with --texsearch)",
                     qUtf8Printable(m_shot.texFormat));
        if (!m_shot.texChannel.isEmpty()
            && !m_texturesTab->setChannel(m_shot.texChannel))
            qWarning("devshot: --texchannel '%s' is not a channel",
                     qUtf8Printable(m_shot.texChannel));
        qInfo("devshot: textures '%s' -> %d match(es)",
              qUtf8Printable(m_shot.texSearch), m_texturesTab->matchCount());
    } else if ((m_shot.grid || m_shot.filterPopup) && m_texturesTab
               && m_tabs->currentWidget() == m_texturesTab) {
        // --filterpopup follows --tab, like --npanel does. It was Models-only,
        // so the Textures funnel could not be photographed at all.
        if (m_shot.filterPopup)
            qInfo("devshot: filter popup %s",
                  m_texturesTab->openFilterPopupForShot() ? "opened" : "NO POPUP");
    } else if (m_shot.grid || m_shot.filterPopup) {
        if (m_shot.tab.isEmpty()) m_tabs->setCurrentWidget(m_modelsTab);
        if (!m_shot.searchFilter.isEmpty()) m_modelsTab->setSearchText(m_shot.searchFilter);
        if (m_shot.grid) m_modelsTab->showGrid(true, m_shot.gridIcon);
        qInfo("devshot: models list -> %d match(es)", m_modelsTab->listCount());
        if (m_shot.filterPopup)
            qInfo("devshot: filter popup %s",
                  m_modelsTab->openFilterForShot() ? "opened" : "NOT OPENED");
        if (m_shot.hoverSet)
            qInfo("devshot: grid hover %d %s", m_shot.hoverRow,
                  m_modelsTab->hoverGridCell(m_shot.hoverRow) ? "sent"
                                                              : "NOT SENT");
    } else if (!m_shot.modelFilter.isEmpty()) {
        m_tabs->setCurrentWidget(m_modelsTab);
        if (!m_modelsTab->selectModel(m_shot.modelFilter))
            qWarning("devshot: no model matches '%s'",
                     qUtf8Printable(m_shot.modelFilter));
        if (m_shot.pbrOverride >= 0)
            m_modelsTab->setPbrShading(m_shot.pbrOverride == 1);
        // --hidemesh implies the panel: the tree has no rows while it is
        // closed, so without this the hide reported "NOT FOUND" — which reads
        // as "no such submesh" rather than "there was nothing to search".
        if (m_shot.showSubmeshTree || !m_shot.hideSubmesh.isEmpty())
            m_modelsTab->setSubmeshTreeVisible(true);
        if (!m_shot.hideSubmesh.isEmpty())
            qInfo("devshot: hide submesh '%s' %s", qUtf8Printable(m_shot.hideSubmesh),
                  m_modelsTab->hideSubmesh(m_shot.hideSubmesh) ? "ok" : "NOT FOUND");
        if (m_shot.showDebugPanel) m_modelsTab->setDebugPanelVisible(true);
        if (!m_shot.matFilter.isEmpty())
            m_modelsTab->setMaterialFilter(m_shot.matFilter);
        if (!m_shot.mtarFilter.isEmpty()) {
            // Model loads are synchronous; pose immediately.
            if (!m_modelsTab->selectAnim(m_shot.mtarFilter, m_shot.clipFilter,
                                         m_shot.frame))
                qWarning("devshot: anim '%s'/'%s' did not resolve",
                         qUtf8Printable(m_shot.mtarFilter),
                         qUtf8Printable(m_shot.clipFilter));
        }
        // …and LAST, so the popup sits over a scene that has finished
        // building rather than over one still loading.
        if (!m_shot.popupCombo.isEmpty())
            qInfo("devshot: anim popup '%s' %s", qUtf8Printable(m_shot.popupCombo),
                  m_modelsTab->openAnimPopup(m_shot.popupCombo) ? "opened"
                                                                : "NOT FOUND");
    } else if (!m_shot.searchFilter.isEmpty()) {
        // The Textures tab has its own --texsearch and this is the FILES
        // search; a second spelling of "search the textures" was written here
        // and removed again the moment --texsearch was found. What stays is
        // only that --tab wins: a run that named a tab must not be switched
        // away from it.
        if (m_shot.tab.isEmpty()) m_tabs->setCurrentWidget(m_filesTab);
        m_filesTab->setSearchText(m_shot.searchFilter);
        // Wait out the search debounce AND the selection that follows it, or
        // the screenshot photographs the empty state — the one state that
        // needed no photograph. 700 ms covers the 180 ms debounce and the
        // 400 ms selection with room to spare.
        {
            QElapsedTimer w;
            w.start();
            while (w.elapsed() < 700) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
                QThread::msleep(5);
            }
        }
        qInfo("devshot: files search -> %s",
              m_filesTab->selectFirstResult() ? "first result selected"
                                              : "no result");
    }
    // Forced .fv2, applied to whatever the branches above fitted — after ALL
    // of them, so one line covers the weapon, character, vehicle and preset
    // paths instead of four copies drifting apart. It has to run before the
    // export below, which is what it exists to feed.
    if (!m_shot.fovaForce.isEmpty() && m_customizeTab)
        qInfo("devshot: fova '%s' applied to %d part(s)",
              qUtf8Printable(m_shot.fovaForce),
              m_customizeTab->applyFovaByName(m_shot.fovaForce));

    // ── The display mode and the N-panel column (§4, §6) ────────────────
    // AFTER the tab branches, because both act on the Models tab and the
    // branch above may have just loaded a model into it — an outliner built
    // before the load has no parts row to grow.
    if (!m_shot.display.isEmpty() && m_modelsTab) {
        m_tabs->setCurrentWidget(m_modelsTab);
        // --viewmode takes the Models tab, so a --search alongside it is about
        // THIS list. Without this the search went to the Files tab (its branch
        // above) and the models list was photographed unfiltered — which is
        // also the only way to see the filter chips, since an unfiltered list
        // has none.
        if (!m_shot.searchFilter.isEmpty())
            m_modelsTab->setSearchText(m_shot.searchFilter);
        if (!m_modelsTab->setDisplayModeForShot(m_shot.display))
            qWarning("devshot: --viewmode '%s' is not list|outliner|grid",
                     qUtf8Printable(m_shot.display));
        else
            qInfo("devshot: display '%s' -> %s",
                  qUtf8Printable(m_shot.display),
                  qUtf8Printable(m_modelsTab->displaySummary()));
    }
    if (m_shot.rowZoomSet && m_modelsTab) {
        m_tabs->setCurrentWidget(m_modelsTab);
        m_modelsTab->setRowZoomForShot(m_shot.rowZoom);
    }
    if (!m_shot.animSort.isEmpty() && m_modelsTab) {
        m_tabs->setCurrentWidget(m_modelsTab);
        qInfo("devshot: animations sort '%s' -> %s",
              qUtf8Printable(m_shot.animSort),
              qUtf8Printable(m_modelsTab->setAnimSortForShot(m_shot.animSort)));
    }
    // LAST of the tab-level actions, so the menu sits over a page that has
    // finished building — the same reason the animation popup goes last.
    //
    // It briefly moved ABOVE the chain, carried along when --tab was lifted
    // out of this spot, and every menu it then described was built before a
    // model had loaded: "no file selected", from a flag that had worked the
    // batch before. The two blocks want opposite ends of this function —
    // --tab decides which tab, so it goes first; --filemenu photographs what
    // the tab ended up holding, so it goes last.
    if (m_shot.selectRows > 0 && m_modelsTab) {
        m_tabs->setCurrentWidget(m_modelsTab);
        qInfo("devshot: selected %d row(s)",
              m_modelsTab->selectRowsForShot(m_shot.selectRows));
    }
    if (!m_shot.partMenu.isEmpty()) {
        GLModelWidget* v = nullptr;
        for (GLModelWidget* w : findChildren<GLModelWidget*>())
            if (w->isVisible()) { v = w; break; }
        if (!v) {
            qWarning("devshot: --partmenu — no visible viewport");
        } else {
            const QStringList xy =
                m_shot.partMenu.split(QLatin1Char(','), Qt::SkipEmptyParts);
            // Fractions of the viewport when both are <= 1, pixels otherwise —
            // the same convention --pickat uses, decided once for the pair.
            const double fx = xy.value(0).toDouble(), fy = xy.value(1).toDouble();
            const QPoint p(int(fx <= 1.0 ? fx * v->width() : fx),
                           int(fy <= 1.0 ? fy * v->height() : fy));
            fox::setPartMenuDumpOnly(true);
            Q_EMIT v->customContextMenuRequested(p);
            fox::setPartMenuDumpOnly(false);
        }
    }
    if (m_shot.menuDump)
        for (const QString& line : dumpMenuBar().split(QLatin1Char('\n')))
            qInfo("menubar: %s", qUtf8Printable(line));
    if (m_shot.fileMenu && m_modelsTab) {
        m_tabs->setCurrentWidget(m_modelsTab);
        for (int i = 0; i < 6; ++i) QCoreApplication::processEvents();
        qInfo("devshot: file menu -> %s",
              qUtf8Printable(m_modelsTab->openFileMenuForShot()));
    }
    if (!m_shot.npanel.isEmpty()) {
        // WHICHEVER TAB IS ON SCREEN. It targeted the Models tab by name, and
        // then the Customize tab grew a column of its own — a flag that always
        // switched away from the tab the run had just built was a flag that
        // could not photograph it.
        QWidget* cur = m_tabs ? m_tabs->currentWidget() : nullptr;
        if (cur == m_customizeTab && m_customizeTab) {
            qInfo("devshot: n-panel -> %s",
                  qUtf8Printable(m_customizeTab->setPanelsForShot(m_shot.npanel)));
        } else if (cur == m_texturesTab && m_texturesTab) {
            qInfo("devshot: n-panel -> %s",
                  qUtf8Printable(m_texturesTab->setPanelsForShot(m_shot.npanel)));
        } else if (cur == m_filesTab && m_filesTab) {
            qInfo("devshot: n-panel -> %s",
                  qUtf8Printable(m_filesTab->setPanelsForShot(m_shot.npanel)));
        } else if (m_modelsTab) {
            m_tabs->setCurrentWidget(m_modelsTab);
            qInfo("devshot: n-panel -> %s",
                  qUtf8Printable(m_modelsTab->setPanelsForShot(m_shot.npanel)));
        }
    }

    // ── The viewport rig ────────────────────────────────────────────────
    // Applied to every GLModelWidget in the window and to every N-panel, after
    // the tab branches above have loaded whatever they load: a rig set before
    // the scene arrives is a rig on an empty viewport.
    {
        const auto views = findChildren<GLModelWidget*>();
        for (GLModelWidget* v : views) {
            if (!m_shot.viewEnv.isEmpty()) {
                if (m_shot.viewEnv == fox::ViewEnvironment::autoId()) {
                    v->setEnvironmentAuto(true);
                    qInfo("devshot: environment auto -> %s",
                          qUtf8Printable(v->environment().name));
                } else if (const fox::ViewEnvironment* e =
                               fox::ViewEnvironment::find(m_shot.viewEnv)) {
                    v->setEnvironment(*e);
                } else {
                    qWarning("devshot: no such environment '%s'",
                             qUtf8Printable(m_shot.viewEnv));
                }
            }
            if (!m_shot.viewDebug.isEmpty()) {
                bool got = false;
                for (fox::DebugView d : fox::debugViews())
                    if (m_shot.viewDebug.compare(
                            QLatin1String(fox::debugViewName(d)),
                            Qt::CaseInsensitive) == 0) {
                        v->setDebugView(d);
                        got = true;
                        break;
                    }
                if (!got)
                    qWarning("devshot: no such debug view '%s'",
                             qUtf8Printable(m_shot.viewDebug));
            }
            if (!m_shot.viewLight.isEmpty()) {
                const QStringList xy =
                    m_shot.viewLight.split(QLatin1Char(','), Qt::SkipEmptyParts);
                if (xy.size() == 2)
                    v->setKeyAngles(xy[0].toFloat(), xy[1].toFloat());
                else
                    qWarning("devshot: --light wants 'az,el'");
            }
            if (m_shot.viewKey >= 0.0f) v->setKeyIntensity(m_shot.viewKey);
            if (m_shot.viewAmbient >= 0.0f)
                v->setAmbientIntensity(m_shot.viewAmbient);
            if (m_shot.viewExposure >= 0.0f)
                v->setExposure(m_shot.viewExposure);
            if (m_shot.viewTurn != 0.0f) v->setTurntable(true, m_shot.viewTurn);
        }
        // ── Template §5: the viewport's keyboard, headlessly ────────────
        // Applied to the VISIBLE viewport, like everything else in this block.
        if (!m_shot.pickAt.isEmpty() || !m_shot.viewKeys.isEmpty()
            || !m_shot.rightDrag.isEmpty() || !m_shot.gizmo.isEmpty()
            || !m_shot.selSeq.isEmpty() || m_shot.overlayMenu
            || m_shot.channelScrollSet || m_shot.viewHelp) {
            GLModelWidget* v = nullptr;
            for (GLModelWidget* w : findChildren<GLModelWidget*>())
                if (w->isVisible()) { v = w; break; }
            if (!v) {
                qWarning("devshot: no visible viewport for --pickat/--viewkeys");
            } else {
                // The selection rules, scripted. Fractions of the viewport
                // when both numbers are <= 1, pixels otherwise — the same
                // convention --pickat uses, decided once for the pair.
                if (!m_shot.selSeq.isEmpty()) {
                    const QStringList steps = m_shot.selSeq.split(
                        QLatin1Char(';'), Qt::SkipEmptyParts);
                    for (const QString& step : steps) {
                        const int at = step.indexOf(QLatin1Char('@'));
                        if (at < 0) {
                            qWarning("devshot: --selseq step '%s' wants "
                                     "gesture@x,y", qUtf8Printable(step));
                            continue;
                        }
                        const QString g = step.left(at).trimmed().toLower();
                        const QStringList xy = step.mid(at + 1).split(
                            QLatin1Char(','), Qt::SkipEmptyParts);
                        if (xy.size() != 2) {
                            qWarning("devshot: --selseq step '%s' wants x,y",
                                     qUtf8Printable(step));
                            continue;
                        }
                        const double fx = xy[0].toDouble();
                        const double fy = xy[1].toDouble();
                        const bool frac = fx <= 1.0 && fy <= 1.0;
                        const QPoint pt(int(frac ? fx * v->width() : fx),
                                        int(frac ? fy * v->height() : fy));
                        Qt::KeyboardModifiers mods = Qt::NoModifier;
                        if (g == QLatin1String("ctrl")) mods = Qt::ControlModifier;
                        else if (g == QLatin1String("shift")) mods = Qt::ShiftModifier;
                        else if (g != QLatin1String("pick")) {
                            qWarning("devshot: --selseq '%s' is not "
                                     "pick|ctrl|shift", qUtf8Printable(g));
                            continue;
                        }
                        v->testPickGesture(pt, mods);
                        qInfo("devshot: selseq %s @ (%d,%d) -> %s",
                              qUtf8Printable(g), pt.x(), pt.y(),
                              qUtf8Printable(v->selectionForShot()));
                    }
                }
                if (m_shot.channelScrollSet) {
                    fox::ViewportBar* bar = fox::viewportBarFor(v);
                    qInfo("devshot: channel wheel %+d -> %s",
                          m_shot.channelScroll,
                          bar ? qUtf8Printable(bar->scrollChannelForShot(
                                    m_shot.channelScroll))
                              : "NO BAR");
                }
                // The bar's overlay list, popped non-modally so it lands in
                // the grab. LAST of the viewport steps, so it sits over a
                // scene that has finished changing.
                if (m_shot.overlayMenu) {
                    fox::ViewportBar* bar = fox::viewportBarFor(v);
                    qInfo("devshot: overlay menu -> %s",
                          bar ? qUtf8Printable(bar->openOverlayMenuForShot())
                              : "NO BAR");
                }
                // The axis gizmo (§5, and the user's own list). Snapping the
                // camera and switching projection are both things a
                // screenshot can only show AFTER the fact, so the numbers go
                // in the log as well.
                if (!m_shot.gizmo.isEmpty()) {
                    auto* giz = v->findChild<fox::ViewportGizmo*>(
                        QString(), Qt::FindDirectChildrenOnly);
                    if (!giz) {
                        qWarning("devshot: this viewport has no gizmo");
                    } else if (m_shot.gizmo.compare(QLatin1String("ortho"),
                                                    Qt::CaseInsensitive) == 0) {
                        // Through the BUTTON, not straight at the viewport:
                        // the button is the control the user has, and a
                        // harness that bypassed it would pass while the
                        // button did nothing.
                        // Through the RING, and through the real hit test —
                        // a harness that toggled the viewport directly would
                        // pass while the control did nothing.
                        const QPoint at = giz->ringPointForShot();
                        const int under = giz->hitTest(at);
                        QMouseEvent press(QEvent::MouseButtonPress, QPointF(at),
                                          QPointF(at), Qt::LeftButton,
                                          Qt::LeftButton, Qt::NoModifier);
                        QCoreApplication::sendEvent(giz, &press);
                        // …and leave the pointer ON it, so the grab shows the
                        // ring the click went through rather than a gizmo at
                        // rest with no ring at all.
                        QMouseEvent hover(QEvent::MouseMove, QPointF(at),
                                          QPointF(at), Qt::NoButton,
                                          Qt::NoButton, Qt::NoModifier);
                        QCoreApplication::sendEvent(giz, &hover);
                        qInfo("devshot: gizmo ring at %d,%d hit-tests as %d "
                              "(-1 = ring) — projection is now %s",
                              at.x(), at.y(), under,
                              v->orthographic() ? "orthographic"
                                                : "perspective");
                    } else if (m_shot.gizmo.startsWith(
                                   QLatin1String("hover:"))) {
                        // Hover the ball's OWN centre, so the test lands on
                        // the same point the paint draws — a hover that
                        // guessed the position would be testing its own guess.
                        const int n = m_shot.gizmo.mid(6).toInt();
                        const QPoint at = giz->ballPos(n);
                        QMouseEvent mv(QEvent::MouseMove, QPointF(at),
                                       QPointF(at), Qt::NoButton, Qt::NoButton,
                                       Qt::NoModifier);
                        QCoreApplication::sendEvent(giz, &mv);
                        qInfo("devshot: gizmo — hovered ball %d at (%d,%d); "
                              "hit test says %d, lit axis %d",
                              n, at.x(), at.y(), giz->hitTest(at),
                              giz->hovered());
                    } else {
                        static const char* const kNames[] = {"x",  "y",  "z",
                                                             "-x", "-y", "-z"};
                        int axis = -1;
                        for (int i = 0; i < 6; ++i)
                            if (m_shot.gizmo.compare(QLatin1String(kNames[i]),
                                                     Qt::CaseInsensitive) == 0)
                                axis = i;
                        if (axis < 0) {
                            qWarning("devshot: --gizmo '%s'?",
                                     qUtf8Printable(m_shot.gizmo));
                        } else {
                            giz->activate(axis);
                            qInfo("devshot: gizmo %s -> yaw %.1f pitch %.1f, "
                                  "%s",
                                  kNames[axis], v->cameraYaw(),
                                  v->cameraPitch(),
                                  v->orthographic() ? "orthographic"
                                                    : "perspective");
                        }
                    }
                }
                // Blender's rule, measured rather than looked at: right-DRAG
                // pans and must not raise the context menu; right-CLICK still
                // does. A screenshot cannot show either.
                if (!m_shot.rightDrag.isEmpty()) {
                    const QStringList n = m_shot.rightDrag.split(
                        QLatin1Char(','), Qt::SkipEmptyParts);
                    if (n.size() != 4) {
                        qWarning("devshot: --rightdrag wants x,y,dx,dy");
                    } else {
                        const QPoint from(n[0].toInt(), n[1].toInt());
                        const QPoint to(from.x() + n[2].toInt(),
                                        from.y() + n[3].toInt());
                        const char* what = "?";
                        switch (v->testRightDrag(from, to)) {
                            case GLModelWidget::RightDragResult::Click:
                                what = "read as a CLICK — the menu is allowed";
                                break;
                            case GLModelWidget::RightDragResult::DragSwallowed:
                                what = "read as a DRAG — the menu was "
                                       "swallowed, the camera pans";
                                break;
                            case GLModelWidget::RightDragResult::DragLeaked:
                                what = "read as a DRAG but the menu was NOT "
                                       "swallowed — this is the bug";
                                break;
                        }
                        qInfo("devshot: right-drag %d,%d -> %d,%d — %s",
                              from.x(), from.y(), to.x(), to.y(), what);
                    }
                }
                if (!m_shot.pickAt.isEmpty()) {
                    const QStringList xy =
                        m_shot.pickAt.split(QLatin1Char(','), Qt::SkipEmptyParts);
                    if (xy.size() != 2) {
                        qWarning("devshot: --pickat wants x,y");
                    } else {
                        // FRACTIONS of the viewport when BOTH are <= 1, pixels
                        // otherwise — decided once for the pair, not per axis.
                        // Per axis, "0.5,200" silently meant half-fraction and
                        // half-pixel, and the log printed only the converted
                        // result, so the substitution was invisible.
                        const double fx = xy[0].toDouble();
                        const double fy = xy[1].toDouble();
                        const bool frac = fx <= 1.0 && fy <= 1.0;
                        const QPoint at(int(frac ? fx * v->width() : fx),
                                        int(frac ? fy * v->height() : fy));
                        const int id = v->pickMeshAt(at);
                        v->setPickedMesh(id);
                        Q_EMIT v->meshPicked(id);
                        qInfo("devshot: pick at (%d,%d) -> submesh %d", at.x(),
                              at.y(), id);
                    }
                }
                const QStringList keys = m_shot.viewKeys.split(
                    QLatin1Char(','), Qt::SkipEmptyParts);
                for (const QString& k : keys) {
                    const QString key = k.trimmed().toLower();
                    // The names are the KEYS, and they now mean what those
                    // keys are bound to: Alt+H reveals and Shift+H isolates,
                    // Blender's arrangement. They were the other way round
                    // here and stayed that way for one run after the bindings
                    // moved, which is how "alth -> 14 hidden" got logged for a
                    // key whose job is to unhide everything.
                    if (key == QLatin1String("h")) v->hidePicked();
                    else if (key == QLatin1String("alth")) v->unhideAll();
                    else if (key == QLatin1String("shifth")) v->isolatePicked();
                    else if (key == QLatin1String("frame")) v->frameMesh(v->pickedMesh());
                    else if (key == QLatin1String("full")) v->setViewportFullscreen(true);
                    else if (key == QLatin1String("help")) v->setShowHelp(true);
                    else if (key == QLatin1String("reset")) v->resetCamera();
                    else { qWarning("devshot: --viewkeys '%s'?", qUtf8Printable(key)); continue; }
                    qInfo("devshot: viewkey %s -> %d hidden submesh(es)",
                          qUtf8Printable(key), int(v->hiddenMeshes().size()));
                }
                // RE-PICK the same point after the keys. "Hiding it changed
                // what is under the cursor" is the only cheap proof that the
                // pick is depth-correct and that the hide reached the GPU —
                // both of which a screenshot can be satisfied about and wrong.
                if (!m_shot.pickAt.isEmpty() && !keys.isEmpty()) {
                    const QStringList xy =
                        m_shot.pickAt.split(QLatin1Char(','), Qt::SkipEmptyParts);
                    if (xy.size() == 2) {
                        const double fx = xy[0].toDouble(), fy = xy[1].toDouble();
                        const bool frac = fx <= 1.0 && fy <= 1.0;
                        const QPoint at(int(frac ? fx * v->width() : fx),
                                        int(frac ? fy * v->height() : fy));
                        qInfo("devshot: same point after the keys -> submesh %d",
                              v->pickMeshAt(at));
                    }
                }
                if (m_shot.viewHelp) v->setShowHelp(true);
            }
        }
        // ── Template §5: the shading mode, the channel, the overlays ────
        // Applied to the VISIBLE viewport only, the same rule --viewpanel
        // follows, and through the same gate a person's click goes through —
        // a harness that reached past the gate would photograph a state the
        // application cannot actually be put into.
        if (!m_shot.shading.isEmpty()) {
            static const char* const kNames[] = {"wireframe", "flat", "shaded",
                                                 "rendered"};
            int want = -1;
            for (int i = 0; i < 4; ++i)
                if (m_shot.shading.compare(QLatin1String(kNames[i]),
                                           Qt::CaseInsensitive) == 0)
                    want = i;
            if (want < 0)
                qWarning("devshot: no such shading mode '%s'",
                         qUtf8Printable(m_shot.shading));
            else
                for (GLModelWidget* v : findChildren<GLModelWidget*>())
                    if (v->isVisible()) v->setShadingMode(ShadingMode(want));
        }
        if (!m_shot.channel.isEmpty()) {
            fox::DebugView want = fox::DebugView::Off;
            bool found = false;
            for (const fox::DebugView d : fox::debugViews())
                if (m_shot.channel.compare(QLatin1String(fox::debugViewName(d)),
                                           Qt::CaseInsensitive) == 0) {
                    want = d;
                    found = true;
                }
            if (!found)
                qWarning("devshot: no such channel '%s'",
                         qUtf8Printable(m_shot.channel));
            else
                for (GLModelWidget* v : findChildren<GLModelWidget*>())
                    if (v->isVisible()) v->setDebugView(want);
        }
        if (!m_shot.overlays.isEmpty()) {
            static const char* const kKeys[] = {"stats", "grid", "axes",
                                                "skeleton", "bonenames",
                                                "connectpoints"};
            const bool all = m_shot.overlays.compare(QLatin1String("all"),
                                                     Qt::CaseInsensitive) == 0;
            const bool none = m_shot.overlays.compare(QLatin1String("none"),
                                                      Qt::CaseInsensitive) == 0;
            const QStringList want = m_shot.overlays.split(QLatin1Char(','),
                                                           Qt::SkipEmptyParts);
            for (GLModelWidget* v : findChildren<GLModelWidget*>()) {
                if (!v->isVisible()) continue;
                fox::ViewportBar* bar = fox::viewportBarFor(v);
                if (!bar) continue;
                // "none" CLOSES THE GATE and leaves the remembered set alone,
                // which is what the gate means everywhere else. Writing false
                // into all six as well would have made "--overlays none" a
                // clear, so a later "master on" would restore nothing.
                if (none) {
                    bar->setOverlaysMaster(false);
                    continue;
                }
                bar->setOverlaysMaster(true);
                for (const char* k : kKeys)
                    bar->setOverlay(QLatin1String(k),
                                    all || want.contains(QLatin1String(k),
                                                         Qt::CaseInsensitive));
            }
            qInfo("devshot: overlays '%s'", qUtf8Printable(m_shot.overlays));
        }
        if (!m_shot.popover.isEmpty()) {
            bool opened = false;
            for (GLModelWidget* v : findChildren<GLModelWidget*>()) {
                if (!v->isVisible()) continue;
                if (fox::ViewportBar* bar = fox::viewportBarFor(v))
                    opened = bar->openPopover(m_shot.popover) || opened;
            }
            qInfo("devshot: popover '%s' %s", qUtf8Printable(m_shot.popover),
                  opened ? "opened" : "NOT OPENED");
        }
        if (!m_shot.viewPanel.isEmpty()) {
            const int page = fox::ViewportPanel::pageIndexFor(m_shot.viewPanel);
            if (page < 0) {
                qWarning("devshot: no such view panel page '%s'",
                         qUtf8Printable(m_shot.viewPanel));
            } else {
                // Only the VISIBLE one: opening the card on all three would
                // put a panel over a tab nobody is grabbing, and QWidget::
                // isVisible() on a hidden tab's child is exactly that test.
                for (fox::ViewportPanel* p : findChildren<fox::ViewportPanel*>())
                    if (p->parentWidget() && p->parentWidget()->isVisible())
                        p->showPage(page);
            }
        }
    }

    // The settings dialog, MODELESS for the grab: exec() would block the
    // devshot's own timer and the screenshot would never be taken.
    if (!m_shot.settingsTab.isEmpty()) {
        auto* dlg = new SettingsDialog(this);
        dlg->showTab(m_shot.settingsTab);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
        qInfo("devshot: settings on '%s'", qUtf8Printable(m_shot.settingsTab));
    }

    // Same reason as the settings dialog above: modeless, or exec() would
    // block the devshot timer and no grab would ever happen.
    if (m_shot.mgsvDialog) {
        const QString folderName = QDir(modfolder::dir()).dirName();
        auto* dlg = new MgsvMetaDialog(folderName,
                                       modfolder::list().size(), this);
        if (!m_shot.mgsvMeta.isEmpty()) dlg->setMeta(shotMgsvMeta(folderName));
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
        qInfo("devshot: mgsv metadata dialog");
    }

    if (m_shot.logConsole) {
        if (!m_logConsole) m_logConsole = new fox::LogConsole(this);
        m_logConsole->showConsole();
    }

    const QString path = m_shot.outPng;
    const bool stay = m_shot.stay;
    // A hover preview is re-sent late, after everything else has settled: the
    // first send happens while the grid is still laying out and thumbnails are
    // still landing, and any of that can deliver a Leave that takes the popup
    // away again. Re-sending costs nothing and makes the harness reliable.
    if (m_shot.hoverSet && m_shot.settleMs > 1200) {
        const int row = m_shot.hoverRow;
        const bool grid = m_shot.grid;
        QTimer::singleShot(m_shot.settleMs - 900, this, [this, row, grid] {
            if (grid) m_modelsTab->hoverGridCell(row);
            else m_customizeTab->hoverBuilderPopupRow(row);
        });
    }
    QTimer::singleShot(m_shot.settleMs, this, [this, path, stay] {
        // ── Capture and batch export ────────────────────────────────────
        // INSIDE the settle window, beside --export and before the grab: the
        // tab branches above kick off work that finishes on the event loop
        // (a rebuild, a thumbnail landing), and running these straight after
        // them captured the scene one repaint too early.
        {
            GLModelWidget* view = nullptr;
            for (GLModelWidget* v : findChildren<GLModelWidget*>())
                if (v->isVisible()) { view = v; break; }
            if (!m_shot.capturePng.isEmpty()) {
                QString err;
                if (!fox::captureStill(view, m_shot.capturePng, &err))
                    qWarning("devshot: capture failed — %s", qUtf8Printable(err));
            }
            if (!m_shot.captureGif.isEmpty()) {
                fox::CaptureOptions co = fox::loadCaptureOptions();
                if (m_shot.captureFrames > 0) co.frames = m_shot.captureFrames;
                // Never the user's saved "also write the PNGs" in a harness
                // run: a headless capture that sprays a numbered sequence into
                // the output folder because of a setting made months ago is a
                // surprise, and there is no flag to turn it back off.
                co.alsoFrames = false;
                QString err;
                if (!fox::captureTurntable(view, m_shot.captureGif, co, &err))
                    qWarning("devshot: turntable failed — %s", qUtf8Printable(err));
            }
            if (!m_shot.captureAnimGif.isEmpty()) {
                const auto& provider =
                    view ? view->animationFrameProvider()
                         : GLModelWidget::AnimFrameProvider();
                if (!provider) {
                    qWarning("devshot: --animgif — this viewport has NO "
                             "animation frame provider (load a model and a "
                             "clip: --mtar/--clip)");
                } else {
                    fox::CaptureOptions co = fox::loadCaptureOptions();
                    if (m_shot.captureFrames > 0) co.frames = m_shot.captureFrames;
                    co.alsoFrames = false;
                    QVector<QImage> frames = provider(co.frames);
                    QString err;
                    if (!fox::encodeGif(view, frames, m_shot.captureAnimGif, co,
                                        &err))
                        qWarning("devshot: animgif failed — %s", qUtf8Printable(err));
                    else
                        qInfo("devshot: animgif -> %s (%d frame(s))",
                              qUtf8Printable(m_shot.captureAnimGif),
                              int(frames.size()));
                }
            }
        }
        if (!m_shot.exportParts.isEmpty()) {
            if (!m_customizeTab || m_customizeTab->exportPartsTo(
                                       m_shot.exportParts) == 0)
                qWarning("devshot: --exportparts wrote nothing — is there a "
                         "Customize scene? (--character / --weapon / --parts)");
        }
        if (!m_shot.exportSlot.isEmpty()) {
            const QString slot = m_shot.exportSlot.section(QLatin1Char('='), 0, 0);
            const QString out = m_shot.exportSlot.section(QLatin1Char('='), 1);
            const QString err = m_customizeTab
                                    ? m_customizeTab->exportSlotTo(slot, out)
                                    : QStringLiteral("no Customize tab");
            qInfo("devshot: exportslot '%s' -> %s — %s", qUtf8Printable(slot),
                  qUtf8Printable(out),
                  err.isEmpty() ? "ok" : qUtf8Printable(err));
        }
        if (!m_shot.exportVariations.isEmpty()) {
            QString err;
            const int n = m_customizeTab
                              ? m_customizeTab->exportVariationsTo(
                                    m_shot.exportVariations, &err)
                              : 0;
            qInfo("devshot: exportvariations -> %s — %d file(s)%s%s",
                  qUtf8Printable(m_shot.exportVariations), n,
                  err.isEmpty() ? "" : " — ", qUtf8Printable(err));
        }
        if (!m_shot.slotMenu.isEmpty() && m_customizeTab)
            qInfo("slotmenu: %s : %s", qUtf8Printable(m_shot.slotMenu),
                  qUtf8Printable(m_customizeTab->slotMenuDump(m_shot.slotMenu)));
        if (!m_shot.exportGlb.isEmpty()) {
            // Customize runs export through the scene path; Models otherwise.
            if (!m_shot.partsFilter.isEmpty() || !m_shot.weaponSpec.isEmpty()
                || !m_shot.charSpec.isEmpty() || !m_shot.presetFilter.isEmpty()
                || !m_shot.vehicleSpec.isEmpty())
                m_customizeTab->exportSceneTo(m_shot.exportGlb);
            else
                m_modelsTab->exportTo(m_shot.exportGlb);
        }
        // BEFORE the export, so --animpanel can be what chooses the clips.
        // The SCOPE goes first of all: it decides which rows exist, and a
        // filter applied to the wrong set of rows selects the wrong clips.
        if (!m_shot.animScope.isEmpty() && m_modelsTab)
            qInfo("devshot: animscope %s",
                  qUtf8Printable(m_modelsTab->setAnimScopeForShot(m_shot.animScope)));
        if (m_shot.animPanel && m_modelsTab)
            m_modelsTab->showAnimationsPanel(m_shot.animPanelFilter);
        if (!m_shot.exportAnim.isEmpty()) {
            // Which clips: "all" for the whole archive, otherwise a comma list
            // of substrings or indices, and the CURRENTLY SELECTED clip when
            // nothing is named — which is what "export the thing I am looking
            // at" means from the UI, and what --clip already chose here.
            // The PANEL's selection when it is the thing that was opened —
            // otherwise the run would show one set of clips on screen and
            // export another, which is the exact confusion the panel exists
            // to remove.
            QVector<QPair<int, int>> sel;
            if (m_shot.animPanel && m_modelsTab) sel = m_modelsTab->panelSelection();
            if (sel.isEmpty() && m_modelsTab)
                sel = m_modelsTab->clipsMatching(m_shot.animClips);
            QString err;
            if (!m_modelsTab
                || !m_modelsTab->exportAnimatedTo(m_shot.exportAnim, sel, &err))
                qWarning("devshot: --exportanim wrote nothing — %s",
                         qUtf8Printable(err.isEmpty()
                                        ? QStringLiteral("is a model and a "
                                                         ".mtar loaded? "
                                                         "(--model / --mtar)")
                                        : err));
        }
        if (!m_shot.exportAnimDir.isEmpty() && m_modelsTab) {
            QVector<QPair<int, int>> sel;
            if (m_shot.animPanel) sel = m_modelsTab->panelSelection();
            if (sel.isEmpty()) sel = m_modelsTab->clipsMatching(m_shot.animClips);
            int written = 0;
            QString firstError;
            for (const QPair<int, int>& one : sel) {
                QString err;
                const QString path =
                    QDir(m_shot.exportAnimDir)
                        .filePath(QStringLiteral("clip_%1_%2.glb")
                                      .arg(one.first)
                                      .arg(one.second));
                if (m_modelsTab->exportAnimatedTo(path, {one}, &err)) ++written;
                else if (firstError.isEmpty()) firstError = err;
            }
            qInfo("devshot: --exportanimdir wrote %d of %lld clip(s)%s%s",
                  written, static_cast<long long>(sel.size()),
                  firstError.isEmpty() ? "" : " — first failure: ",
                  qUtf8Printable(firstError));
        }
        if (!m_shot.exportSceneAnim.isEmpty() && m_customizeTab) {
            const QVector<QPair<int, int>> sel =
                m_customizeTab->clipsMatching(m_shot.animClips);
            QString err;
            if (m_customizeTab->exportSceneAnimatedTo(m_shot.exportSceneAnim,
                                                      sel, &err) == 0)
                qWarning("devshot: --exportsceneanim wrote nothing — %s",
                         qUtf8Printable(err.isEmpty()
                                        ? QStringLiteral("is a scene built and "
                                                         "a clip loaded? "
                                                         "(--character / --mtar)")
                                        : err));
        }
        if (!m_shot.stringDump.isEmpty() && m_filesTab)
            if (StringsPanel* sp = m_filesTab->stringsPanel())
                sp->dumpAll(m_shot.stringDump);
        if (m_shot.exportMenu) {
            // Verification: build the contextual menu, log every action, and
            // pop it up so the follow-up screen grab shows it.
            populateExportMenu();
            const auto dump = [](const QList<QAction*>& actions, int depth,
                                 auto&& dumpRef) -> void {
                for (const QAction* a : actions) {
                    if (a->isSeparator()) continue;
                    const QString line = QString(depth * 2, QLatin1Char(' '))
                        + a->text()
                        + (a->isEnabled() ? QString()
                                          : QStringLiteral(" [disabled]"));
                    qInfo("exportmenu: %s", qUtf8Printable(line));
                    if (a->menu())
                        dumpRef(a->menu()->actions(), depth + 1, dumpRef);
                }
            };
            dump(m_exportMenu->actions(), 0, dump);
            m_exportMenu->popup(mapToGlobal(QPoint(40, 30)));
        }
        // A dropped-open combo popup and an open menu are both separate
        // top-level windows, so grab() of the main widget misses them.
        // The hover preview is a third one — a Qt::ToolTip window that sits
        // outside the widget entirely.
        // …and the log console is a fourth: a non-modal top-level window,
        // which QWidget::grab() on the main window cannot see.
        // Anything that draws OUTSIDE this window has to be grabbed off the
        // screen instead of off the widget: a popup menu, a combo's drop-down,
        // a hover card, the log console — and the settings dialog, which is a
        // top-level window of its own and came out of grab() as a photograph
        // of the tab behind it.
        if (m_shot.npanelSizes) {
            fox::NPanel* col = nullptr;
            QWidget* cur = m_tabs ? m_tabs->currentWidget() : nullptr;
            if (cur) col = cur->findChild<fox::NPanel*>();
            qInfo("devshot: n-panel sizes -> %s",
                  col ? qUtf8Printable(col->probeSizesForShot())
                      : "this tab has no panel column");
        }
        // ── LAST, immediately before the grab ────────────────────────────
        // The outliner probe expands rows under the LOADED model, and almost
        // everything else in this sequence rebuilds that model's subtree —
        // --rowzoom did, which collapsed the categories the probe had just
        // opened and produced a screenshot of a closed tree beside a log
        // saying 335 clip rows were reachable. A probe whose whole job is to
        // set up what the picture shows belongs at the end of the setup, not
        // in the middle of it.
        if (!m_shot.outlinerDump.isEmpty() && m_modelsTab)
            qInfo("devshot: outliner dump -> %s",
                  qUtf8Printable(m_modelsTab->outlinerDumpForShot(m_shot.outlinerDump)));
        if (!m_shot.outlinerProbe.isEmpty() && m_modelsTab)
            qInfo("devshot: outliner '%s' -> %s",
                  qUtf8Printable(m_shot.outlinerProbe),
                  qUtf8Printable(
                      m_modelsTab->outlinerProbeForShot(m_shot.outlinerProbe)));

        const bool grabScreen = m_shot.exportMenu
            || !m_shot.popupCombo.isEmpty() || m_shot.hoverSet
            || m_shot.filterPopup || m_shot.logConsole
            || !m_shot.settingsTab.isEmpty() || m_shot.mgsvDialog;
        const auto doGrab = [this, path, stay, grabScreen] {
            // No --shot: a run armed by --capture / --turngif / --exportparts
            // alone has already written what it came for, and grabbing a
            // window to save it nowhere costs a screen capture and a
            // "QFSFileEngine::open: No file name specified" in the log.
            if (path.isEmpty()) {
                if (!stay) QApplication::quit();
                return;
            }
            const QPixmap pm =
                grabScreen && screen() ? screen()->grabWindow(0) : grab();
            // Where the camera ended up. Cheap, always on, and the only way a
            // headless run can prove the turntable actually turned.
            for (GLModelWidget* v : findChildren<GLModelWidget*>())
                if (v->isVisible()) {
                    qInfo("devshot: camera yaw %.1f pitch %.1f distance %.3f%s",
                          double(v->cameraYaw()), double(v->cameraPitch()),
                          double(v->cameraDistance()),
                          v->turntable() ? " (turntable running)" : "");
                    break;
                }
            if (pm.save(path))
                qInfo("devshot: saved %s (%dx%d)", qUtf8Printable(path), pm.width(),
                      pm.height());
            else
                qWarning("devshot: FAILED to save %s", qUtf8Printable(path));
            if (!stay) QApplication::quit();
        };
        if (grabScreen)
            QTimer::singleShot(500, this, doGrab);   // let the popup paint
        else
            doGrab();
    });
}
