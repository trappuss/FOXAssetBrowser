#include "app/SettingsDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "app/AppPaths.h"
#include "app/Config.h"
#include "app/Hotkeys.h"
#include "export/ExportOptions.h"
#include "view/TipBar.h"
#include "gl/ViewEnvironment.h"

namespace {

// The size of one cache on disk, for the Maintenance page. Reported rather
// than assumed: "clear the thumbnail cache" is a button nobody presses without
// knowing whether it is three megabytes or three hundred.
qint64 folderBytes(const QString& path, const QStringList& globs)
{
    qint64 total = 0;
    QDir d(path);
    if (!d.exists()) return 0;
    for (const QFileInfo& fi :
         d.entryInfoList(globs, QDir::Files | QDir::NoDotAndDotDot))
        total += fi.size();
    return total;
}

QString humanBytes(qint64 n)
{
    if (n <= 0) return QStringLiteral("empty");
    if (n < 1024) return QStringLiteral("%1 B").arg(n);
    if (n < 1024 * 1024) return QStringLiteral("%1 KB").arg(n / 1024);
    return QStringLiteral("%1 MB").arg(n / (1024 * 1024));
}

}  // namespace

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Settings"));
    resize(660, 620);
    auto* layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    // NEVER elide a tab label (§10). A scrollable bar the user can move is a
    // worse outcome than a wide dialog and a much better one than a tab
    // called "eneral".
    m_tabs->tabBar()->setElideMode(Qt::ElideNone);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->setUsesScrollButtons(true);
    layout->addWidget(m_tabs, 1);

    // ── General ─────────────────────────────────────────────────────────
    {
        auto* general = new QWidget;
        auto* gv = new QVBoxLayout(general);
    gv->addWidget(new QLabel(
        QStringLiteral("Game folders (scanned recursively for SQAR archives):")));
    m_dirs = new QListWidget(general);
    for (const QString& d : Config::gameDirs()) m_dirs->addItem(d);
    gv->addWidget(m_dirs, 1);

    auto* dirButtons = new QHBoxLayout();
    auto* add = new QPushButton(QStringLiteral("Add…"), general);
    auto* remove = new QPushButton(QStringLiteral("Remove"), general);
    dirButtons->addWidget(add);
    dirButtons->addWidget(remove);
    dirButtons->addStretch(1);
    gv->addLayout(dirButtons);
    connect(add, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Add game folder"));
        if (!dir.isEmpty()) m_dirs->addItem(dir);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        qDeleteAll(m_dirs->selectedItems());
    });

    auto* dictRow = new QHBoxLayout();
    dictRow->addWidget(new QLabel(QStringLiteral("Dictionary folder:")));
    m_dictDir = new QLineEdit(Config::dictDir(), general);
    dictRow->addWidget(m_dictDir, 1);
    auto* dictBrowse = new QPushButton(QStringLiteral("…"), general);
    dictRow->addWidget(dictBrowse);
    gv->addLayout(dictRow);
    connect(dictBrowse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Dictionary folder"), m_dictDir->text());
        if (!dir.isEmpty()) m_dictDir->setText(dir);
    });

    // ── The mod folder ───────────────────────────────────────────────────
    // Sits with the other folders because that is what it is. The note is
    // three lines and every one of them is load-bearing: WHERE the files go
    // (so a mod made elsewhere can be dropped in by hand), that the game's own
    // archives are never written, and that emptying the folder is the uninstall
    // — which is the sentence that makes the feature safe to try.
    auto* modRow = new QHBoxLayout();
    modRow->addWidget(new QLabel(QStringLiteral("Mod folder:")));
    m_modDir = new QLineEdit(Config::modDir(), general);
    m_modDir->setPlaceholderText(
        QStringLiteral("not set — right-click ▸ Replace is hidden until it is"));
    modRow->addWidget(m_modDir, 1);
    auto* modBrowse = new QPushButton(QStringLiteral("…"), general);
    modRow->addWidget(modBrowse);
    gv->addLayout(modRow);
    connect(modBrowse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Mod folder — replacement assets"),
            m_modDir->text());
        if (!dir.isEmpty()) m_modDir->setText(dir);
    });
    {
        auto* note = new QLabel(
            QStringLiteral(
                "Replacement assets, laid out as <folder>\\Assets\\… exactly the "
                "way this tool extracts them. The folder is mounted OVER the "
                "game's archives, so a file here is the copy the browser "
                "loads.\n"
                "The game's own .dat files are never written to. Deleting a "
                "file here — or the whole folder — puts everything back."),
            general);
        note->setWordWrap(true);
        QFont nf = note->font();
        nf.setPointSizeF(qMax(7.0, nf.pointSizeF() - 1.0));
        note->setFont(nf);
        note->setEnabled(false);
        gv->addWidget(note);
    }

    m_deepScan = new QCheckBox(
        QStringLiteral("Deep-scan containers (index files inside FPK / FPKD / PFTXS — "
                       "slower first scan, cached afterwards)"),
        this);
    m_deepScan->setChecked(Config::deepScan());
    gv->addWidget(m_deepScan);
        gv->addStretch(1);
        addPage(QStringLiteral("General"), general);
    }

    // ── Interface ───────────────────────────────────────────────────────
    {
        auto* iface = new QWidget;
        auto* iv = new QVBoxLayout(iface);
        auto* layoutBox =
            new QGroupBox(QStringLiteral("Startup && layout"), iface);
        auto* lb = new QVBoxLayout(layoutBox);
        m_rememberPanels = new QCheckBox(
            QStringLiteral("Remember the panel sizes I drag"), layoutBox);
        m_rememberPanels->setToolTip(QStringLiteral(
            "Every tab in this tool is a set of panes you can drag. On, the "
            "widths you set come back next time. Off, each tab opens at its "
            "designed default every launch — which is what you want if you "
            "use the tool for one thing and the default suits it."));
        m_rememberPanels->setChecked(
            QSettings()
                .value(QStringLiteral("interface/rememberPanels"), true)
                .toBool());
        lb->addWidget(m_rememberPanels);

        m_rememberViewport = new QCheckBox(
            QStringLiteral("Remember my viewport settings"), layoutBox);
        m_rememberViewport->setToolTip(QStringLiteral(
            "The 3D viewport's own state, across launches: the key light and "
            "whether it follows the camera, the environment, the exposure, the "
            "shading mode, the debug channel, the projection, the field of "
            "view, and which overlays are on.\n\nOFF by default, and that is "
            "a choice rather than caution — a tool that comes up in whatever "
            "state a one-off experiment left it in is a tool nobody can "
            "describe over a screenshot. On is what you want once you have "
            "tuned a rig you like.\n\nNothing is written while this is off, "
            "so turning it on saves what you have now rather than restoring "
            "something from before."));
        m_rememberViewport->setChecked(Config::rememberViewport());
        lb->addWidget(m_rememberViewport);

        // The way back from a dismissed tip. TipBar::resetAll() existed for
        // this the moment it was written, and a helper with no caller is a
        // promise the header makes and the application does not keep.
        auto* tips = new QPushButton(
            QStringLiteral("Show the viewport tips again"), layoutBox);
        tips->setToolTip(QStringLiteral(
            "The hint strip above each viewport has an \u2715 that hides it. "
            "This brings every one of them back."));
        connect(tips, &QPushButton::clicked, this, [tips] {
            fox::TipBar::resetAll();
            tips->setText(QStringLiteral("Tips will show again next time a tab "
                                         "is built"));
            tips->setEnabled(false);
        });
        lb->addWidget(tips);
        iv->addWidget(layoutBox);

        auto* diagBox = new QGroupBox(QStringLiteral("Diagnostics"), iface);
        auto* db = new QVBoxLayout(diagBox);
        auto* diagNote = new QLabel(diagBox);
        diagNote->setWordWrap(true);
        diagNote->setText(QStringLiteral(
            "Everything this tool measures it says in the log — the gear "
            "join's counts, which animation base it resolved, which materials "
            "a look retextured, which submeshes an export dropped. Help \342\226\270 "
            "Log console (Ctrl+L) shows it live.\n\n"
            "The deeper dumps are environment variables rather than switches "
            "here, because they are per-run and belong beside the run: "
            "FOXAB_DUMP_MATERIALS, FOXAB_NO_BORROW, FOXAB_NO_RESTALIGN, "
            "FOXAB_AVATAR_LUA, FOXAB_GEARCONFIG_LUA, FOXAB_NO_NEUTRAL_LOOK."));
        db->addWidget(diagNote);
        iv->addWidget(diagBox);

        // Textures ▸ alpha checkerboard. It was a toggle button above every
        // image, which made a display PREFERENCE look like a per-image action
        // and put it on the one bar that has now gone. It belongs here, where
        // the other "how things are drawn" switches are, and it persists.
        auto* texBox = new QGroupBox(QStringLiteral("Textures"), iface);
        auto* tv = new QVBoxLayout(texBox);
        auto* alphaBg = new QCheckBox(
            QStringLiteral("Checkerboard behind transparent pixels"), texBox);
        alphaBg->setChecked(Config::textureAlphaBg());
        alphaBg->setToolTip(QStringLiteral(
            "So transparent pixels look transparent instead of looking black. "
            "Off gives a flat ground, which is easier to read a mask against."));
        connect(alphaBg, &QCheckBox::toggled, this, [](bool on) {
            Config::setTextureAlphaBg(on);
        });
        tv->addWidget(alphaBg);
        iv->addWidget(texBox);

        iv->addStretch(1);
        addPage(QStringLiteral("Interface"), iface);
    }

    // ── Viewport ────────────────────────────────────────────────────────
    {
        auto* viewport = new QWidget;
        auto* vv = new QVBoxLayout(viewport);
    // ── Shading ──────────────────────────────────────────────────────────
    // Fox is a physically based renderer and a material carries up to seven
    // maps. With this on, a viewport decodes all of the ones it can use — the
    // SRM (occlusion, roughness, reflection), the translucency map and the
    // colour layer pair — and lights the surface with GGX. With it off it
    // decodes the base colour and normal map only, which is faster to load and
    // is what every viewport did before.
    //
    // Three switches and not one, because the three viewports are used
    // differently: the Files preview is a flick-through where load time is the
    // whole experience, and the Customize tab rebuilds a whole character on
    // every row change.
    vv->addWidget(new QLabel(QStringLiteral(
        "Full PBR shading (loads every material map — occlusion, roughness, "
        "translucency, colour layers; off loads base + normal only):")));
    m_pbrFiles = new QCheckBox(
        QStringLiteral("Files tab preview"), viewport);
    m_pbrFiles->setChecked(Config::pbrEnabled(Config::PbrView::Files));
    vv->addWidget(m_pbrFiles);
    m_pbrModels = new QCheckBox(QStringLiteral("Models tab"), viewport);
    m_pbrModels->setChecked(Config::pbrEnabled(Config::PbrView::Models));
    vv->addWidget(m_pbrModels);
    m_pbrCustomize = new QCheckBox(QStringLiteral("Customize tab"), viewport);
    m_pbrCustomize->setChecked(Config::pbrEnabled(Config::PbrView::Customize));
    vv->addWidget(m_pbrCustomize);

    // ── The viewport's starting state ────────────────────────────────────
    // Defaults for the NEXT viewport built, not a second set of live controls:
    // the N-panel (N in any viewport) edits the one in front of you, and two
    // controls for one thing that disagree is worse than one.
    vv->addWidget(new QLabel(QStringLiteral(
        "Viewport defaults — applied to viewports built from now on, which "
        "in practice means the next launch. Press N in a viewport to change "
        "the one on screen:")));
    auto* envRow = new QHBoxLayout();
    envRow->addWidget(new QLabel(QStringLiteral("Lighting environment:")));
    m_viewEnv = new QComboBox(viewport);
    m_viewEnv->addItem(QStringLiteral("Auto — follow the game"),
                       fox::ViewEnvironment::autoId());
    for (const fox::ViewEnvironment& e : fox::ViewEnvironment::presets())
        if (e.game == fox::GameId::Unknown) m_viewEnv->addItem(e.name, e.id);
    {
        bool sep = false;
        for (const fox::ViewEnvironment& e : fox::ViewEnvironment::presets())
            if (e.game != fox::GameId::Unknown) {
                if (!sep) {
                    m_viewEnv->insertSeparator(m_viewEnv->count());
                    sep = true;
                }
                m_viewEnv->addItem(e.name, e.id);
            }
    }
    {
        const int i = m_viewEnv->findData(Config::viewEnvironment());
        // An id this build does not know falls back to DEFAULT, not to index
        // 0 — index 0 is now Auto, and a dialog that shows Auto for a stale
        // setting turns every new viewport auto the moment someone presses OK
        // without touching the combo.
        const int fallback = qMax(
            0, m_viewEnv->findData(
                   fox::ViewEnvironment::presets().first().id));
        m_viewEnv->setCurrentIndex(i >= 0 ? i : fallback);
    }
    envRow->addWidget(m_viewEnv, 1);
    vv->addLayout(envRow);

    auto* expRow = new QHBoxLayout();
    expRow->addWidget(new QLabel(QStringLiteral("Exposure:")));
    m_viewExposure = new QDoubleSpinBox(viewport);
    // 0 is not "black" here — it is "whatever the environment says", which is
    // what the special-case minimum label spells out. Night carries 1.35 of
    // its own, and overriding that with a number chosen here would undo it.
    m_viewExposure->setRange(0.0, 4.0);
    m_viewExposure->setSingleStep(0.05);
    m_viewExposure->setDecimals(2);
    m_viewExposure->setSpecialValueText(
        QStringLiteral("the environment's own"));
    m_viewExposure->setValue(Config::viewExposure());
    expRow->addWidget(m_viewExposure, 1);
    vv->addLayout(expRow);

    m_viewPanel = new QCheckBox(
        QStringLiteral("Open the view panel on a new viewport"), viewport);
    m_viewPanel->setChecked(Config::viewPanelOpen());
    vv->addWidget(m_viewPanel);
        vv->addStretch(1);
        addPage(QStringLiteral("Viewport"), viewport);
    }

    // ── Export, in sub-tabs (§10) ───────────────────────────────────────
    {
        auto* host = new QWidget;
        auto* hv = new QVBoxLayout(host);
        hv->setContentsMargins(0, 0, 0, 0);
        auto* sub = new QTabWidget(host);
        sub->tabBar()->setElideMode(Qt::ElideNone);
        sub->tabBar()->setExpanding(false);
        m_export = std::make_shared<fox::ExportPages>(sub);
        sub->addTab(m_export->modelsPage(), QStringLiteral("Models"));
        sub->addTab(m_export->imagesPage(), QStringLiteral("Images && GIFs"));
        sub->addTab(m_export->namesPage(), QStringLiteral("Files && names"));
        hv->addWidget(sub);
        addPage(QStringLiteral("Export"), host);
    }

    // ── Hotkeys (§11) ───────────────────────────────────────────────────
    {
        auto* keys = new QWidget;
        auto* kv = new QVBoxLayout(keys);
        auto* note = new QLabel(keys);
        note->setWordWrap(true);
        note->setText(QStringLiteral(
            "Click a box and press the combination you want. Clear a box to "
            "unbind that action — an unbound action is a real choice, and two "
            "of these ship that way because they are slow and destructive of "
            "a camera angle you probably spent time on."));
        kv->addWidget(note);
        auto* form = new QFormLayout();
        for (const Hotkeys::Def& d : Hotkeys::defs()) {
            auto* edit = new QKeySequenceEdit(keys);
            edit->setKeySequence(Hotkeys::seq(d.key, d.def));
            edit->setToolTip(d.hint);
            auto* label = new QLabel(d.label, keys);
            label->setToolTip(d.hint);
            form->addRow(label, edit);
            m_hotkeys.append({d.key, edit});
        }
        kv->addLayout(form);
        auto* reset = new QPushButton(
            QStringLiteral("Put every shortcut back to its default"), keys);
        connect(reset, &QPushButton::clicked, this, [this] {
            const auto defs = Hotkeys::defs();
            for (auto& row : m_hotkeys)
                for (const Hotkeys::Def& d : defs)
                    if (d.key == row.first)
                        row.second->setKeySequence(
                            d.def.isEmpty() ? QKeySequence()
                                            : QKeySequence(d.def));
        });
        kv->addWidget(reset);
        // ── Duplicates ──────────────────────────────────────────────────
        // Two QActions carrying the same sequence make Qt call the shortcut
        // ambiguous and fire NEITHER — so a duplicate does not "win", it
        // silently breaks both keys. Said as it is typed rather than
        // discovered later by pressing one of them.
        m_hotkeyWarning = new QLabel(keys);
        m_hotkeyWarning->setWordWrap(true);
        m_hotkeyWarning->setStyleSheet(QStringLiteral("color:#b04040;"));
        m_hotkeyWarning->hide();
        kv->addWidget(m_hotkeyWarning);
        for (const auto& row : m_hotkeys)
            connect(row.second, &QKeySequenceEdit::keySequenceChanged, this,
                    &SettingsDialog::checkHotkeyClashes);
        checkHotkeyClashes();
        kv->addStretch(1);
        addPage(QStringLiteral("Hotkeys"), keys);
    }

    // ── Maintenance ─────────────────────────────────────────────────────
    {
        auto* maint = new QWidget;
        auto* mv = new QVBoxLayout(maint);
        auto* cacheBox = new QGroupBox(QStringLiteral("Caches && reset"), maint);
        auto* cb = new QVBoxLayout(cacheBox);
        auto* note = new QLabel(cacheBox);
        note->setWordWrap(true);
        note->setText(QStringLiteral(
            "Everything this tool writes lives in data\\ beside the "
            "executable — settings, caches, thumbnails, logs. Nothing goes to "
            "the registry and nothing goes to AppData, so moving the folder "
            "moves the tool with all of it and deleting the folder leaves "
            "nothing behind.\n\n"
            "A cache is safe to clear at any time: it is rebuilt on demand, "
            "and the only cost is the rebuild. Caches left behind by an older "
            "version of this tool are deleted for you at startup.\n\n"
            "The thumbnail grids are not listed because they are not files: "
            "model and texture icons live in memory and are rebuilt each "
            "launch."));
        cb->addWidget(note);

        // MEASURED, not assumed. The two files this build actually writes, under the names it
        // actually writes them. The first version of this page named the wrong
        // one: the row was LABELLED "the entry tables… six seconds without it"
        // and globbed fox_index_v*, which is the DEEP-SCAN container cache and
        // does not exist at all unless deep scan is on. With the default
        // settings the row read "empty", the button was permanently disabled,
        // and the multi-megabyte startup cache could neither be seen nor
        // cleared — "a button that clears nothing", which is what the note
        // above claims to have fixed.
        //
        // The LOG is deliberately not offered here. The application holds it
        // open for writing from startup to exit: on Windows the delete simply
        // fails, and on Linux it succeeds into an orphaned inode that keeps
        // growing and no longer appears in data\. Either way the button would
        // report a success it did not have. Help ▸ Log console is where the
        // log is dealt with.
        struct CacheDef { const char* label; const char* glob; const char* what; };
        static const CacheDef kCaches[] = {
            {"Archive entry tables", "fox_archives_v*.bin",
             "The per-entry headers of every archive. Reading them is the whole "
             "cost of starting up — measured on a 32-archive install: 273,094 "
             "of them, 18.9 seconds — and they cannot change until the archive "
             "file does. Clearing this costs one slow launch."},
            {"Container index", "fox_index_v*.bin",
             "What is inside the FPK / FPKD / PFTXS containers, when deep "
             "scanning is on, which it is by default. Measured on a real "
             "install: 1,111,128 entries, 88.6 seconds to rebuild — by a long "
             "way the most expensive thing to clear here."},
        };
        for (const CacheDef& c : kCaches) {
            auto* row = new QHBoxLayout();
            const QString glob = QString::fromLatin1(c.glob);
            const qint64 bytes = folderBytes(AppPaths::cacheDir(), {glob});
            auto* lbl = new QLabel(QStringLiteral("%1 — %2")
                                       .arg(QString::fromLatin1(c.label),
                                            humanBytes(bytes)),
                                   cacheBox);
            lbl->setToolTip(QString::fromLatin1(c.what));
            row->addWidget(lbl, 1);
            auto* btn = new QPushButton(QStringLiteral("Clear"), cacheBox);
            btn->setEnabled(bytes > 0);
            const QString label = QString::fromLatin1(c.label);
            connect(btn, &QPushButton::clicked, this, [glob, lbl, btn, label] {
                // The RESULT of each delete, not the attempt. A file the
                // application still holds open cannot be removed on Windows,
                // and reporting "empty" for it would be a button that lies
                // about the one thing it does.
                QDir d(AppPaths::dataDir());
                int gone = 0, kept = 0;
                for (const QString& f :
                     d.entryList({glob}, QDir::Files | QDir::NoDotAndDotDot))
                    (d.remove(f) ? gone : kept)++;
                const qint64 left = folderBytes(AppPaths::cacheDir(), {glob});
                lbl->setText(kept == 0
                                 ? QStringLiteral("%1 — empty").arg(label)
                                 : QStringLiteral("%1 — %2 (%3 file(s) in use)")
                                       .arg(label, humanBytes(left))
                                       .arg(kept));
                btn->setEnabled(left > 0);
                qInfo("settings: cleared %d cache file(s), %d in use", gone, kept);
            });
            row->addWidget(btn);
            cb->addLayout(row);
        }

        auto* resetAll =
            new QPushButton(QStringLiteral("Reset every setting…"), cacheBox);
        resetAll->setToolTip(QStringLiteral(
            "Back to a fresh install: every option, every remembered layout, "
            "every search history. The game folders go too, so you will be "
            "asked for them again."));
        connect(resetAll, &QPushButton::clicked, this, [this] {
            if (QMessageBox::question(
                    this, QStringLiteral("Reset every setting"),
                    QStringLiteral(
                        "Put every option, layout and history back to its "
                        "default? The game folders are cleared too.\n\nThis "
                        "cannot be undone."))
                != QMessageBox::Yes)
                return;
            QSettings s;
            s.clear();
            s.sync();
            QMessageBox::information(
                this, QStringLiteral("Reset"),
                QStringLiteral("Settings cleared. Restart the tool."));
            // CLOSE, and close by rejecting. Leaving the dialog open let the
            // OK the user pressed next write every widget on every page
            // straight back into the file that had just been cleared —
            // including the game folders the confirmation had promised were
            // gone. There is nothing left to accept.
            reject();
        });
        cb->addWidget(resetAll);
        mv->addWidget(cacheBox);

        // ── Settings profile (§10) ──────────────────────────────────────
        auto* profBox =
            new QGroupBox(QStringLiteral("Settings profile"), maint);
        auto* pb = new QHBoxLayout(profBox);
        auto* exportBtn =
            new QPushButton(QStringLiteral("Export settings…"), profBox);
        auto* importBtn =
            new QPushButton(QStringLiteral("Import settings…"), profBox);
        exportBtn->setToolTip(QStringLiteral(
            "Write this tool's whole INI to a file you can keep or copy to "
            "another machine."));
        importBtn->setToolTip(QStringLiteral(
            "Replace every setting with the contents of a file written by "
            "Export settings. Takes effect on the next launch."));
        connect(exportBtn, &QPushButton::clicked, this, [this] {
            const QString out = QFileDialog::getSaveFileName(
                this, QStringLiteral("Export settings"),
                QDir::homePath() + QStringLiteral("/FOXAssetBrowser.ini"),
                QStringLiteral("Settings (*.ini)"));
            if (out.isEmpty()) return;
            QSettings s;
            s.sync();
            QFile::remove(out);
            if (QFile::copy(s.fileName(), out))
                QMessageBox::information(this, QStringLiteral("Exported"), out);
            else
                QMessageBox::warning(this, QStringLiteral("Export failed"), out);
        });
        connect(importBtn, &QPushButton::clicked, this, [this] {
            const QString in = QFileDialog::getOpenFileName(
                this, QStringLiteral("Import settings"), QDir::homePath(),
                QStringLiteral("Settings (*.ini)"));
            if (in.isEmpty()) return;
            QSettings s;
            const QString target = s.fileName();
            s.sync();
            QFile::remove(target);
            if (QFile::copy(in, target))
                QMessageBox::information(
                    this, QStringLiteral("Imported"),
                    QStringLiteral("Settings replaced. Restart the tool."));
            else
                QMessageBox::warning(this, QStringLiteral("Import failed"), in);
        });
        pb->addWidget(exportBtn);
        pb->addWidget(importBtn);
        pb->addStretch(1);
        mv->addWidget(profBox);
        mv->addStretch(1);
        addPage(QStringLiteral("Maintenance"), maint);
    }

    // ── Information (§10) ───────────────────────────────────────────────
    // "Explaining in-tool beats a wiki nobody opens." Side-by-side, for the
    // options that LOOK interchangeable and are not — which in this tool is
    // two pairs that have each been asked about, plus the one fact about Fox
    // materials that everyone reads as a bug the first time.
    {
        auto* info = new QWidget;
        auto* iv = new QVBoxLayout(info);
        auto* text = new QTextBrowser(info);
        text->setOpenExternalLinks(false);
        text->setHtml(QStringLiteral(
            "<h3>Two things called &quot;PBR&quot;</h3>"
            "<table cellpadding='6' border='1' cellspacing='0' width='100%'>"
            "<tr><th align='left'>Settings &#9656; Viewport &#9656; Full PBR "
            "shading</th>"
            "<th align='left'>The Rendered shading ball</th></tr>"
            "<tr><td>Decides whether a viewport <b>loads</b> the material maps "
            "at all — the SRM, the translucency map, the colour-layer pair. "
            "Off, they are never read, which is faster and is what a "
            "flick-through wants.</td>"
            "<td>Decides whether the maps already in hand <b>light the "
            "surface</b>. No reload either way, so it is a direct A/B on "
            "identical geometry — except that choosing it with no maps loaded "
            "fetches them once.</td>"
            "</tr></table>"
            "<h3>Two ways to get a file out</h3>"
            "<table cellpadding='6' border='1' cellspacing='0' width='100%'>"
            "<tr><th align='left'>Extract</th>"
            "<th align='left'>Export</th></tr>"
            "<tr><td>The bytes as the game ships them — an .fmdl comes out an "
            ".fmdl. Keeps its shipped name, because the file is meant to go "
            "back into the game, so the File-name template deliberately does "
            "not touch it.</td>"
            "<td>Converted: a .glb, a .png, a .dds. Everything on the Export "
            "pages applies, and the name comes from the template.</td>"
            "</tr></table>"
            "<h3>What this engine's material maps are</h3>"
            "<p>Fox packs three things into the three channels of one "
            "<b>SRM</b> texture: <b>R = ambient occlusion, G = roughness, "
            "B = the reflection mask</b>. &quot;Specular&quot; and "
            "&quot;roughness&quot; are not two missing files; they are one "
            "file.</p>"
            "<p>There is <b>no metalness map</b>, and there never will be — F0 "
            "comes from the material's FMTT preset. A metalness channel that "
            "reads flat is the format working, not a decode that failed.</p>"
            "<h3>Where everything is kept</h3>"
            "<p>In <code>data\\</code> beside the executable: the INI, the "
            "index caches, the thumbnails and the log. No registry, no "
            "AppData. Move the folder and the tool moves with all of it; "
            "delete it and nothing is left behind.</p>"));
        iv->addWidget(text, 1);
        addPage(QStringLiteral("Information"), info);
    }

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    layout->addWidget(buttons);
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::checkHotkeyClashes()
{
    if (!m_hotkeyWarning) return;
    QHash<QString, QStringList> byKey;
    const auto defs = Hotkeys::defs();
    for (const auto& row : m_hotkeys) {
        const QString seq =
            row.second->keySequence().toString(QKeySequence::PortableText);
        if (seq.isEmpty()) continue;   // unbound is never a clash
        QString label = row.first;
        for (const Hotkeys::Def& d : defs)
            if (d.key == row.first) label = d.label;
        byKey[seq].append(label);
    }
    QStringList clashes;
    for (auto it = byKey.constBegin(); it != byKey.constEnd(); ++it)
        if (it.value().size() > 1)
            clashes << QStringLiteral("%1 — %2")
                           .arg(it.key(), it.value().join(QStringLiteral(" and ")));
    // SORTED: a QHash yields its keys in a different order per run, and a
    // warning that reshuffles itself as you type reads as a different warning.
    clashes.sort();
    m_hotkeyWarning->setText(
        clashes.isEmpty()
            ? QString()
            : QStringLiteral("Two actions share a shortcut, which makes Qt "
                             "fire NEITHER of them:\n%1")
                  .arg(clashes.join(QStringLiteral("\n"))));
    m_hotkeyWarning->setVisible(!clashes.isEmpty());
}

void SettingsDialog::addPage(const QString& title, QWidget* page)
{
    // A scroll area per tab, which is §10's remedy for the failure it names:
    // a page taller than the dialog clips, silently, and the first thing to go
    // is whatever is at the bottom of it.
    auto* area = new QScrollArea(m_tabs);
    area->setWidget(page);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    m_tabs->addTab(area, title);
}

void SettingsDialog::showTab(const QString& name)
{
    if (!m_tabs) return;
    // "Export/Images" names a SUB-tab. Export is the only page with sub-tabs,
    // and a caller that means "the GIF settings" should be able to say so
    // rather than landing on Models and hunting. An unknown half is ignored
    // rather than refused: the top-level tab is still the right place to be.
    const int slash = name.indexOf(QLatin1Char('/'));
    const QString top = slash < 0 ? name : name.left(slash);
    const QString sub = slash < 0 ? QString() : name.mid(slash + 1);
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (m_tabs->tabText(i).compare(top, Qt::CaseInsensitive) != 0) continue;
        m_tabs->setCurrentIndex(i);
        if (sub.isEmpty()) return;
        // The page is a scroll area over the sub-tab widget (addPage wraps
        // every page), so the QTabWidget is found by search rather than by
        // reaching through a known parent chain that addPage could change.
        if (QWidget* page = m_tabs->widget(i)) {
            for (QTabWidget* inner : page->findChildren<QTabWidget*>()) {
                for (int j = 0; j < inner->count(); ++j)
                    // startsWith, so "Files" finds "Files && names" without
                    // the caller having to spell an ampersand Qt doubles.
                    if (inner->tabText(j).startsWith(sub, Qt::CaseInsensitive)) {
                        inner->setCurrentIndex(j);
                        return;
                    }
            }
        }
        return;
    }
}

void SettingsDialog::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    // Fold the tab bar's FULL width into the dialog: with eliding off, the
    // bar's size hint is the only thing that knows how wide the labels really
    // are, and a QTabWidget does not pass that up on its own.
    if (m_tabs && m_tabs->tabBar()) {
        const int want = m_tabs->tabBar()->sizeHint().width() + 24;
        if (want > width()) resize(want, height());
    }
}

void SettingsDialog::accept()
{
    QStringList dirs;
    for (int i = 0; i < m_dirs->count(); ++i) dirs.append(m_dirs->item(i)->text());
    Config::setGameDirs(dirs);
    // Only persist a dictionary override when it differs from the default, so
    // moving the portable folder keeps resolving dict/ beside the exe.
    const QString dict = m_dictDir->text().trimmed();
    Config::setDictDir(dict == AppPaths::dictDir() ? QString() : dict);
    Config::setModDir(m_modDir->text().trimmed());
    Config::setDeepScan(m_deepScan->isChecked());
    Config::setPbrEnabled(Config::PbrView::Files, m_pbrFiles->isChecked());
    Config::setPbrEnabled(Config::PbrView::Models, m_pbrModels->isChecked());
    Config::setPbrEnabled(Config::PbrView::Customize,
                          m_pbrCustomize->isChecked());
    Config::setViewEnvironment(m_viewEnv->currentData().toString());
    Config::setViewExposure(m_viewExposure->value());
    Config::setViewPanelOpen(m_viewPanel->isChecked());
    QSettings().setValue(QStringLiteral("interface/rememberPanels"),
                         m_rememberPanels->isChecked());
    Config::setRememberViewport(m_rememberViewport->isChecked());
    // The hotkeys, as PORTABLE TEXT and not as a native sequence: the INI has
    // to read the same on any platform, and an empty string is the stored form
    // of "unbound" — which is a real setting and not a missing one.
    for (const auto& row : m_hotkeys)
        QSettings().setValue(
            row.first,
            row.second->keySequence().toString(QKeySequence::PortableText));
    if (m_export) m_export->apply();
    QDialog::accept();
}
