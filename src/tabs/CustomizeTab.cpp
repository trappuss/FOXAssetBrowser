// CustomizeTab.cpp — see CustomizeTab.h.
#include "tabs/CustomizeTab.h"

#include <algorithm>

#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QSet>
#include <QSettings>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "anim/AnimPose.h"
#include "util/PanelPersist.h"
#include "view/TipBar.h"
#include "util/SearchBox.h"
#include "anim/RigBind.h"
#include "app/Config.h"
#include "app/StatusLine.h"
#include "util/MenuText.h"
#include "app/ExportNotifier.h"
#include "app/Hotkeys.h"
#include "fox/FoxHash.h"
#include "gl/GLModelWidget.h"
#include "view/ViewGlyphs.h"
#include "export/ExportOptions.h"
#include "fox/FoxMaterial.h"
#include "export/ViewCapture.h"
#include "view/ViewportBar.h"
#include "view/ViewportPanel.h"
#include "index/ArchiveIndex.h"
#include "index/IconCatalog.h"
#include "index/WeaponCatalog.h"
#include "model/GlbExporter.h"
#include "index/PlayerCatalog.h"
#include "preview/ModelLoader.h"
#include "tabs/ModelsTab.h"   // FmdlListModel
#include "index/AnimCatalog.h"
#include "util/AnimCombo.h"
#include "util/ExportActions.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QStackedWidget>

using fox::ArchiveIndex;
using fox::IndexedFile;

CustomizeTab::CustomizeTab(QWidget* parent) : QWidget(parent)
{
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    // ── left: category switch, then the panel for that category ────────────
    auto* left = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    auto* catBar = new QHBoxLayout();
    catBar->addWidget(new QLabel(QStringLiteral("Category:"), left));
    m_category = new QComboBox(left);
    m_category->addItem(QStringLiteral("Parts (free-form)"));
    m_category->addItem(QStringLiteral("Weapon"));
    m_category->addItem(QStringLiteral("Character"));
    m_category->addItem(QStringLiteral("Vehicle & buddy"));
    // Every other humanoid in the archives. The Character category above is the
    // player characters — the ones the games actually let you dress — and this
    // keeps the rest reachable without burying them.
    m_category->addItem(QStringLiteral("All other characters"));
    m_category->setToolTip(QStringLiteral(
        "Parts: compose any models freely.\n"
        "Weapon / Character: build from the game's own slots for that kind of "
        "subject, discovered from the archives."));
    catBar->addWidget(m_category, 1);
    leftLayout->addLayout(catBar);

    m_stack = new QStackedWidget(left);
    leftLayout->addWidget(m_stack, 1);

    auto* charPage = new QWidget(m_stack);
    auto* charLayout = new QVBoxLayout(charPage);
    charLayout->setContentsMargins(0, 0, 0, 0);
    m_search = new QLineEdit(charPage);
    m_search->setPlaceholderText(QStringLiteral("Search parts (e.g. sna, ddog, head)…"));
    // Esc clears, the down arrow recalls the last ten searches, and a
    // committed search is remembered — the same four behaviours in every
    // search box in the application, from one place (template §4/§15).
    fox::searchbox::attach(m_search, QStringLiteral("customize/searchHistory"));
    m_search->setClearButtonEnabled(true);
    charLayout->addWidget(m_search);
    m_listModel = new FmdlListModel(this);
    m_list = new QListView(charPage);
    m_list->setModel(m_listModel);
    m_list->setUniformItemSizes(true);
    // An EXPLICIT delegate, always. A QListView that has never been given one
    // reports itemDelegate() == nullptr, and with setUniformItemSizes(true)
    // the row size is then cached as an INVALID QSize: every row comes back
    // height -1 and the view paints nothing at all — a full model, a working
    // search, a working selection, and a blank pane. Measured on this list:
    // visualRect(index(0,0)) went from 401x-1 to 421x14 the moment one was
    // installed. Three lists in this application had it.
    m_list->setItemDelegate(new QStyledItemDelegate(this));
    charLayout->addWidget(m_list, 3);
    m_addBtn = new QPushButton(QStringLiteral("Equip part ▼"), charPage);
    charLayout->addWidget(m_addBtn);
    charLayout->addWidget(new QLabel(QStringLiteral("Equipped:"), charPage));
    m_equipped = new QListWidget(charPage);
    charLayout->addWidget(m_equipped, 1);
    m_removeBtn = new QPushButton(QStringLiteral("Remove selected"), charPage);
    charLayout->addWidget(m_removeBtn);

    auto* outfitBar = new QHBoxLayout();
    m_outfits = new QComboBox(charPage);
    m_outfits->setToolTip(QStringLiteral("Saved outfits"));
    m_saveOutfitBtn = new QPushButton(QStringLiteral("Save…"), charPage);
    m_deleteOutfitBtn = new QPushButton(QStringLiteral("Delete"), charPage);
    outfitBar->addWidget(m_outfits, 1);
    outfitBar->addWidget(m_saveOutfitBtn);
    outfitBar->addWidget(m_deleteOutfitBtn);
    charLayout->addLayout(outfitBar);
    m_stack->addWidget(charPage);

    buildWeaponPanel(m_stack);
    m_stack->addWidget(m_weaponPanel);
    connect(m_category, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_stack->setCurrentIndex(i == 0 ? 0 : 1);
        setBuilderCategory(i);
        // Switching category clears the scene: the two builders own the part
        // list in incompatible ways, and leaving a half-built weapon behind
        // when the user goes back to characters is worse than starting clean.
        while (!m_parts.isEmpty()) removePartAt(m_parts.size() - 1);
        for (WeaponSlotRow& r : m_weaponRows) r.partIdx = -1;
        // Coming BACK to the weapon builder must re-assemble what its combos
        // still say is selected — otherwise the panel reads "receiver = X,
        // sight = Y" over an empty viewport and only poking a combo fixes it.
        if (i > 0) rebuildWeapon(); else rebuildScene();
    });
    splitter->addWidget(left);

    // ── right: viewport + animation ─────────────────────────────────────────
    auto* right = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    // ── THE TOOLBAR IS GONE, as it is on the Models tab ─────────────────
    // What was left of it after 8p was three buttons, and all three had a
    // better home:
    //
    //   Submeshes…, Materials…  → the N-panel's icon strip. They were never
    //                             view controls; they were panel switches.
    //   Export scene .glb…      → the Export menu and the viewport's
    //                             right-click menu, both of which say how
    //                             many parts they will write.
    //
    // The four shading toggles and Reset view went in 8p; the reload PBR can
    // ask for is still this tab's job and is wired below.

    // The SAME tip, the SAME keys, the SAME two-way selection as the Models
    // tab — all of it lives in GLModelWidget and SceneTree, so this tab gets it
    // by wiring rather than by a second implementation. Its own tip key, so
    // dismissing one does not dismiss the other.
    m_tip = new fox::TipBar(
        QStringLiteral("customizeViewport"),
        QStringLiteral("Tip: double-click a part to select it, Shift or Ctrl to add "
                       "· H / Alt+H / Shift+H hide, show all, isolate · . "
                       "frames it · F fullscreen · middle-click resets the "
                       "camera · F1 lists everything"),
        right);
    rightLayout->addWidget(m_tip);

    m_view = new GLModelWidget(right);
    rightLayout->addWidget(m_view, 1);
    // The page's own head of the viewport's right-click menu — the same
    // arrangement as the Models tab, so the two viewports offer one menu with
    // different contents rather than two menus.
    // §4, through the shared builder — see the note in ModelsTab.
    fox::attachViewportPanel(m_view, [this](partmenu::Context& ctx, int part) {
        if (m_parts.isEmpty()) return;
        // The subject here is the SCENE, not one indexed asset: this tab
        // composes a character out of many files, so there is no single path
        // or hash to copy and those fields are left empty rather than filled
        // with one arbitrary part's.
        ctx.modelName = QStringLiteral("Scene  (%1 part(s))").arg(m_parts.size());
        ctx.partName = part >= 0 ? QStringLiteral("submesh %1").arg(part)
                                 : QString();
        ctx.exportModel = [this] { exportScene(); };
        // The silent twin (§12), and the SELECTION rather than the part under
        // the cursor — the same two gaps the Models tab had.
        ctx.exportModelLast = [this] { exportSceneToLast(); };
    });
    // No toolbar toggles left for the display state to follow — the shading
    // balls and the Graphics popover keep themselves in step with the view.
    // This tab implements fullscreen, so its viewport may offer the key.
    m_view->setFullscreenSupported(true);

    connect(m_view, &GLModelWidget::meshPicked, this, [this](int meshId) {
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
    });
    connect(m_view, &GLModelWidget::meshVisibilityChanged, this, [this] {
        if (m_sceneTree) m_sceneTree->setHiddenLeaves(m_view->hiddenMeshes());
    });
    connect(m_view, &GLModelWidget::fullscreenChanged, this,
            &CustomizeTab::applyViewportFullscreen);

    auto* animBar = new QHBoxLayout();
    m_mtarCombo = new SearchableCombo(right);
    m_mtarCombo->setMinimumWidth(200);
    m_mtarCombo->setMaximumWidth(360);
    m_mtarCombo->setToolTip(animcombo::archiveTooltip());
    m_clipCombo = new SearchableCombo(right);
    m_clipCombo->setMinimumWidth(200);
    m_clipCombo->setMaximumWidth(400);
    m_clipCombo->setToolTip(animcombo::clipTooltip());
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

    // THE LABEL UNDER THE VIEWPORT IS GONE, as it is on the Models tab: two
    // permanent lines of window height for a message that matters for a few
    // seconds. It reports to the window's status bar now (app/StatusLine.h).
    // Kept as an unparented sink so the existing call sites keep working.
    m_info = new QLabel;
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    splitter->addWidget(right);

    // ── The N-panel column (template §6) ────────────────────────────────
    // The same column the Models tab has, from the same class — it takes a
    // settings prefix for exactly this, so the two tabs remember their own
    // layouts and neither has a second implementation to keep in step.
    //
    // These two were separate panes with their own minimum widths (240 and
    // 400), which in one column would have made the column 400px wide before
    // anything was in it. The column has one floor and the panels scroll.
    m_npanel = new fox::NPanel(QStringLiteral("customize/npanel"), splitter);

    m_sceneTree = new SceneTree(m_npanel);
    m_npanel->addPanel(
        QStringLiteral("parts"), QStringLiteral("PARTS"), 4, m_sceneTree,
        QStringLiteral(
            "Every part in the scene, every mesh group in it and every submesh "
            "inside that, switchable individually.\n\nThe equipped list beside "
            "the viewport switches whole parts; this reaches the eyelashes "
            "without taking the face with them."));

    m_inspector = new MaterialInspector(m_npanel);
    m_npanel->addPanel(
        QStringLiteral("materials"), QStringLiteral("MATERIALS"), 5,
        m_inspector,
        QStringLiteral(
            "Every material on every fitted part, the shader it asks for, the "
            "meshes and mesh groups that use it, and every texture map split "
            "into its channels with the mean value of each."));

    m_infoPanel = new fox::InfoPanel(m_npanel);
    m_infoPanel->setPlaceholder(QStringLiteral("Nothing fitted yet."));
    m_npanel->addPanel(
        QStringLiteral("info"), QStringLiteral("INFO"), 16, m_infoPanel,
        QStringLiteral(
            "Everything this tool knows about the composed scene — every "
            "fitted part, where it came from, what it is seated on, and what "
            "the whole thing adds up to. Ctrl+C copies the selected rows."));

    // ── ANIMATIONS, which this tab did not have ─────────────────────────
    // This tab has an animation bar, a rig, a clip and a frame slider — it can
    // play everything the Models tab can — and the panel that lists the clips
    // was registered on the Models tab only. So the one tab where you assemble
    // a character was the one tab where you could not search 2,855 clips for
    // the walk cycle to check it against.
    //
    // The SAME widget class, driven the same way: the panel signals a clip,
    // this tab routes it through the combos, and the combos are what everything
    // else reads. Built empty; the column fills it when it is opened.
    m_animPanel = new AnimationsPanel(m_npanel);
    m_npanel->addPanel(
        QStringLiteral("animations"), QStringLiteral("ANIMATIONS"), 14,
        m_animPanel,
        QStringLiteral(
            "Every clip in the install as a list: one filter across all of "
            "them, a category filter, and a scope that narrows to what the "
            "assembled character's own rig can actually play."));
    connect(m_animPanel, &AnimationsPanel::clipChosen, this,
            [this](int archiveFileIdx, int clipIdx) {
                // Through the COMBOS, never straight into the loader — same
                // reason as the Models tab: the combos are what every other
                // path reads to know what is loaded.
                if (m_mtarCombo->currentPayload().toInt() != archiveFileIdx)
                    m_mtarCombo->selectPayload(archiveFileIdx);
                m_clipCombo->selectPayload(clipIdx);
            });
    connect(m_animPanel, &AnimationsPanel::scopeChanged, this,
            [this] { populateAnimCombo(); });

    // The default column before the user has arranged one. PARTS only: a
    // composed character is what this tab is FOR, and the parts list is the
    // one panel that is about the thing on screen rather than about a file.
    m_npanel->restoreState({QStringLiteral("parts")});
    m_npanel->attachToggle(m_view);
    splitter->addWidget(m_npanel);

    splitter->setStretchFactor(0, 2);   // the builder
    splitter->setStretchFactor(1, 5);   // the viewport
    splitter->setStretchFactor(2, 2);   // the N-panel column
    // Stretch factors share SPARE room; they are not the initial layout. The
    // same measurement as the Models tab: without this the column came up too
    // narrow to read a material name in.
    splitter->setSizes({300, 660, 320});


    // AFTER the stretch factors above, which are this tab's chosen default:
    // PanelPersist leaves the sizes untouched when nothing has been
    // remembered, so the default stands until the user has actually dragged
    // something (template §6).
    PanelPersist::bind(splitter, QStringLiteral("customize/splitter"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(splitter);

    // ── wiring ──────────────────────────────────────────────────────────────
    auto* debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(200);
    connect(m_search, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
    connect(debounce, &QTimer::timeout, this,
            [this] { m_listModel->refresh(m_search->text()); });

    const auto equipCurrent = [this] {
        const int fi = m_listModel->fileIdxAt(m_list->currentIndex());
        if (fi >= 0) addPart(fi);
    };
    connect(m_addBtn, &QPushButton::clicked, this, equipCurrent);
    connect(m_list, &QListView::doubleClicked, this,
            [this](const QModelIndex& ix) {
                const int fi = m_listModel->fileIdxAt(ix);
                if (fi >= 0) addPart(fi);
            });
    connect(m_removeBtn, &QPushButton::clicked, this,
            &CustomizeTab::removeSelectedPart);
    connect(m_equipped, &QListWidget::itemChanged, this,
            [this](QListWidgetItem* item) {
                const int row = m_equipped->row(item);
                if (row >= 0)
                    m_view->setGroupVisible(row,
                                            item->checkState() == Qt::Checked);
            });
    // The reload the shading switches can ask for. The switches themselves
    // are on the Graphics popover now; what could not go there is this — a
    // composed character's map set is assembled from its PARTS, and only this
    // tab knows how to fetch them. Guarded on m_reloadingForPbr because
    // reloadPartMaps() re-asserts the shading state as it finishes, which
    // emits displayChanged again.
    connect(m_view, &GLModelWidget::displayChanged, this, [this] {
        if (m_reloadingForPbr || m_parts.isEmpty()) return;
        const bool wants = m_view->pbrShading()
                        || m_view->shadingMode() == ShadingMode::Rendered;
        if (!wants || m_view->hasPbrMaps()) { refreshInspector(); return; }
        m_reloadingForPbr = true;
        reloadPartMaps();
        m_reloadingForPbr = false;
    });
    connect(m_npanel, &fox::NPanel::panelOpenChanged, this,
            [this](const QString& key, bool on) {
                // Opening a panel is what fills it. The column does not know
                // what its contents cost, so the tab answers rather than every
                // panel doing work each time the column is shown.
                if (!on && key == QLatin1String("materials")) {
                    refreshInspector();
                    return;
                }
                if (on) fillPanel(key);
            });
    // Same as the Models tab: restoreState() ran long before this connect
    // existed, so every panel the user left open emitted into nothing and was
    // never filled. See ModelsTab::fillOpenPanels for the whole story.
    fillOpenPanels();
    connect(m_sceneTree, &SceneTree::meshVisibilityChanged, this,
            [this](int meshId, bool on) { m_view->setMeshVisible(meshId, on); });
    // The tree-to-viewport half. It was missing here while the Models tab had
    // it, so clicking a row in this tab's parts pane and pressing H hid
    // whatever had last been double-clicked instead.
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
    // leavesSelected above hands the viewport the whole set; a setPickedMesh
    // here would fire second and collapse it back to one id.



    // Right-click an equipped part: shared export set + Remove.
    m_equipped->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_equipped, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                const int row = m_equipped->row(m_equipped->itemAt(pos));
                if (row < 0 || row >= m_parts.size()) return;
                QMenu menu(this);
                // The SHARED builder — the same rows the slot combos offer.
                // This used to call addFileActions directly, which meant the
                // list could export the FILE and never the part.
                addSlotMenuActions(&menu, row, QString());
                menu.addSeparator();

                // Attach this part at another part's connect point.
                QMenu* attach = menu.addMenu(QStringLiteral("Attach to"));
                bool anyCnp = false;
                for (int hi = 0; hi < m_parts.size(); ++hi) {
                    // A host must not itself be attached (no chains/cycles —
                    // the seat would use the host's bind CNP, not its posed
                    // one).
                    if (hi == row || !m_parts[hi].hasFcnp
                        || m_parts[hi].attachPart >= 0)
                        continue;
                    QMenu* hostMenu = attach->addMenu(
                        m_parts[hi].path.section(QLatin1Char('/'), -1));
                    for (const fox::ConnectPoint& cp :
                         m_parts[hi].fcnp.points()) {
                        hostMenu->addAction(cp.name, this,
                                            [this, row, hi, name = cp.name] {
                                                attachPartTo(row, hi, name);
                                            });
                        anyCnp = true;
                    }
                }
                attach->setEnabled(anyCnp);
                if (m_parts[row].attachPart >= 0)
                    menu.addAction(QStringLiteral("Detach"), this,
                                   [this, row] {
                                       if (row >= m_parts.size()) return;
                                       m_parts[row].attachPart = -1;
                                       m_parts[row].attachCnp.clear();
                                       m_view->setGroupTransform(row,
                                                                 QMatrix4x4());
                                       if (m_hasAnim) setFrame(m_frame);
                                   });

                menu.addAction(QStringLiteral("Remove from scene"), this,
                               [this, row] { removePartAt(row); });
                menu.exec(m_equipped->viewport()->mapToGlobal(pos));
            });

    m_animTimer = new QTimer(this);
    m_animTimer->setInterval(33);
    connect(m_animTimer, &QTimer::timeout, this, [this] {
        if (!m_hasAnim || m_anim.frameCount <= 0) return;
        float f = m_frame + 1.0f;
        if (f > m_anim.frameCount - 1) f = 0.0f;
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
                    if (m_playBtn->isChecked()) m_playBtn->setChecked(false);
                    m_hasMtar = m_hasAnim = false;
                    m_clipCombo->clear();
                    m_clipCombo->setEnabled(false);
                    m_playBtn->setEnabled(false);
                    m_frameSlider->setEnabled(false);
                    m_frameLabel->setText(QStringLiteral("—"));
                    m_view->clearPose();
                    syncAnimProvider();
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
        if (on) m_animTimer->start();
        else m_animTimer->stop();
    });
    connect(m_frameSlider, &QSlider::valueChanged, this, [this](int v) {
        if (m_hasAnim) setFrame(static_cast<float>(v), true);
    });

    connect(m_saveOutfitBtn, &QPushButton::clicked, this, &CustomizeTab::saveOutfit);
    connect(m_deleteOutfitBtn, &QPushButton::clicked, this,
            &CustomizeTab::deleteOutfit);
    connect(m_outfits, &QComboBox::textActivated, this,
            [this](const QString& name) { loadOutfit(name); });
    refreshOutfits();
}

// ── saved outfits ───────────────────────────────────────────────────────────
// One setting, one key: "customize/outfits" holds the name list;
// "customize/outfit_<name>" holds that outfit's part PATHS (stable identity —
// paths survive index rebuilds; row indices would not).

void CustomizeTab::refreshOutfits()
{
    m_outfits->blockSignals(true);
    m_outfits->clear();
    m_outfits->addItem(QStringLiteral("— outfits —"));
    const QStringList names =
        QSettings().value(QStringLiteral("customize/outfits")).toStringList();
    for (const QString& n : names) m_outfits->addItem(n);
    m_outfits->blockSignals(false);
    m_deleteOutfitBtn->setEnabled(!names.isEmpty());
}

// QSettings reads '/' as a group separator, so an outfit called "a/b" lands
// in group "customize/outfit_a" under key "b" — and deleting an outfit called
// "a" then removes that whole GROUP, taking "a/b" with it while leaving its
// name in the list. The weapon presets already guard against this; outfits did
// not. One helper so save, load and delete can never key it differently.
//
// (An outfit saved before this with a '/' in its name will not be found under
// the sanitised key. There is nothing to migrate to — its old key was a group
// path, not a value — and it was already at risk of being deleted by another
// outfit's name.)
static QString outfitKey(const QString& name)
{
    QString key = name;
    key.replace(QLatin1Char('/'), QLatin1Char('-'));
    key.replace(QLatin1Char('\\'), QLatin1Char('-'));
    key.truncate(120);
    return key;
}

void CustomizeTab::saveOutfit()
{
    if (m_parts.isEmpty()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Save outfit"), QStringLiteral("Outfit name:"),
        QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    QSettings s;
    QStringList names = s.value(QStringLiteral("customize/outfits")).toStringList();
    if (!names.contains(name)) names.append(name);
    s.setValue(QStringLiteral("customize/outfits"), names);
    // An outfit is now the SAME description a preset is — captureSceneFields()
    // — and the two ad-hoc keys it used to write are gone.
    //
    // They stored part paths and a gear colour and nothing else, so saving an
    // outfit silently discarded every variation, every MGO dye, every
    // appearance row and the camo. It looked like it worked, which is why it
    // survived: reloading produced a scene, just not the one that was saved.
    // outfitcolor_* is not written any more either — the colour is a
    // "gearcolor=" field in the blob like everywhere else.
    s.setValue(QStringLiteral("customize/outfitfields_%1").arg(outfitKey(name)),
               captureSceneFields().join(QLatin1Char('|')));
    refreshOutfits();
    m_outfits->setCurrentText(name);
}

void CustomizeTab::loadOutfit(const QString& name)
{
    QSettings s;
    const QString blob =
        s.value(QStringLiteral("customize/outfitfields_%1").arg(outfitKey(name)))
            .toString();
    if (!blob.isEmpty()) {
        applySceneFields(blob.split(QLatin1Char('|'), Qt::SkipEmptyParts));
        return;
    }

    // ── An outfit saved before the change ────────────────────────────────
    // Migrated on read rather than left to rot: the old pair of keys is
    // translated into the one grammar and re-saved in it, so an outfit is
    // read in the old shape exactly once and every later load takes the path
    // above. Nothing is lost that the old form actually held — it only ever
    // held these two things.
    const QStringList paths =
        s.value(QStringLiteral("customize/outfit_%1").arg(outfitKey(name)))
            .toStringList();
    if (paths.isEmpty()) return;
    const QString oldColour =
        s.value(QStringLiteral("customize/outfitcolor_%1").arg(outfitKey(name)))
            .toString();

    // The old form recorded no slot for a part, only its path — so the parts
    // are equipped directly, exactly as the old loader did, and the result is
    // then captured in the new grammar. Round-tripping through the scene is
    // what supplies the slots the old key never stored.
    setGearColorPath(oldColour);
    m_parts.clear();
    m_equipped->clear();
    const ArchiveIndex& index = ArchiveIndex::instance();
    int missing = 0;
    for (const QString& p : paths) {
        // Was a linear walk of every entry PER PART: a twenty-part outfit on a
        // stock index compared six million QStrings to load itself.
        const int found = index.fileIndexForPath(p);
        if (found >= 0) addPart(found);
        else ++missing;
    }
    rebuildScene();   // always — an all-missing outfit must clear the viewport
    s.setValue(QStringLiteral("customize/outfitfields_%1").arg(outfitKey(name)),
               captureSceneFields().join(QLatin1Char('|')));
    s.remove(QStringLiteral("customize/outfit_%1").arg(outfitKey(name)));
    s.remove(QStringLiteral("customize/outfitcolor_%1").arg(outfitKey(name)));
    qInfo("customize: migrated outfit \"%s\" to the shared scene description",
          qUtf8Printable(name));
    if (missing)
        setStatus(QStringLiteral("Outfit \"%1\": %2 part(s) not in the "
                                       "current index")
                            .arg(name).arg(missing));
}

void CustomizeTab::deleteOutfit()
{
    const QString name = m_outfits->currentText();
    if (name.isEmpty() || m_outfits->currentIndex() == 0) return;
    QSettings s;
    QStringList names = s.value(QStringLiteral("customize/outfits")).toStringList();
    names.removeAll(name);
    s.setValue(QStringLiteral("customize/outfits"), names);
    s.remove(QStringLiteral("customize/outfitfields_%1").arg(outfitKey(name)));
    // The pre-migration pair, for an outfit deleted before it was ever loaded.
    s.remove(QStringLiteral("customize/outfit_%1").arg(outfitKey(name)));
    s.remove(QStringLiteral("customize/outfitcolor_%1").arg(outfitKey(name)));
    refreshOutfits();
}

void CustomizeTab::onIndexReady(bool ready)
{
    // The archives changed: every cache keyed on a file index is now pointing
    // at whatever moved into that slot. Icons, camouflage names and swatches
    // are all in that class, and a stale one is not a blank — it is the
    // PREVIOUS install's answer, confidently wrong.
    m_camoIndexCache.clear();
    m_pendingKeep.clear();
    fox::IconCatalog::instance().reset();
    // The material panel describes parts that are about to be cleared, and
    // its decode cache is keyed by asset path — which now resolves to a
    // different install's bytes.
    if (m_inspector) m_inspector->clear();
    // Same class of staleness: the held swatch hash addressed the PREVIOUS
    // index's texture. Dropped rather than remapped, because a hash that no
    // longer resolves would quietly paint nothing.
    //
    // The COMBO is emptied in the same breath. Clearing only the held value
    // left the drop-down still showing "SCOL4 c07" while every part was built
    // as shipped — and if readyChanged(true) never arrives (the game folder
    // was removed, the rescan found nothing) it stayed that way.
    clearWeaponColor();

    if (!ready) return;
    refreshWeaponColorList();
    m_listModel->refresh(m_search->text());
    populateAnimCombo();
    m_frigSearched = false;
    m_hasFrig = false;
    m_parts.clear();
    m_equipped->clear();
    m_view->clearModel();
    // The weapon slots ARE the indexed data — rediscover them on every rescan.
    for (WeaponSlotRow& r : m_weaponRows) r.partIdx = -1;
    setBuilderCategory(m_category ? m_category->currentIndex() : 0);
}

// Re-decode one part's base colour maps for the current avatar look. Nothing
// about a skin tone, a wrinkle set, a hair colour, an eyebrow or a scar is
// shader state in this game — each is a separate shipped texture — so applying
// a look means substituting textures and, for the facial feature, compositing
// its decal over the face map.
// A garment's own exposed skin, following the chosen tone — and nothing else.
// See modelload::skinToneOverrides for why this is separate from the head's
// look pass rather than a flag on it.
void CustomizeTab::applySkinToneToPart(int partIdx)
{
    if (partIdx < 0 || partIdx >= m_parts.size()) return;
    Part& part = m_parts[partIdx];
    const QString stem = part.path.section(QLatin1Char('/'), -1)
                             .section(QLatin1Char('.'), 0, 0);
    const int n = int(modelload::skinMaterials(part.model).size());
    // Survive's garments are excluded for the reason the head path excludes
    // them: the only body maps anywhere in the shipped data are the TPP/MGO
    // avatar's, and putting one on a survivor's mesh reads as a corrupt
    // texture rather than as the wrong file.
    if (part.path.startsWith(QLatin1String("/Assets/ssd/"), Qt::CaseInsensitive))
        return;
    if (!m_look.skinChosen) {
        qInfo("customize: %s — %d skin material(s), left as shipped (no tone "
              "chosen)", qUtf8Printable(stem), n);
        return;
    }
    const bool men = lookSex() == fox::AvatarPresets::Sex::Men;
    const QString body = fox::AvatarTextures::instance().bodyPath(m_look.skin, men);
    // THREE outcomes, and they look identical on screen: on most outfits the
    // skin is a neck or a wrist a screenshot barely shows, so "it did nothing",
    // "it did the right thing to two square centimetres" and "this install
    // ships no body map to give it" all read the same. Said out loud instead.
    if (body.isEmpty()) {
        qWarning("customize: %s — %d skin material(s) want tone %d, but this "
                 "install has no body map to give them",
                 qUtf8Printable(stem), n, m_look.skin + 1);
        return;
    }
    modelload::FovaOverrides ov;
    modelload::TextureForce force;
    modelload::skinToneOverrides(part.model, body, &ov, &force);
    if (ov.isEmpty() && force.isEmpty()) return;
    const fox::ArchiveIndex& index = fox::ArchiveIndex::instance();
    if (part.fileIdx < 0 || part.fileIdx >= index.files().size()) return;
    const IndexedFile& f = index.files()[part.fileIdx];
    int found = 0;
    const QVector<QImage> tex = modelload::loadBaseTextures(
        part.model, f.gz, &found, &ov, false, nullptr, &force, nullptr);
    // Only what decoded: a substitution that resolves to nothing must not blank
    // a material that already had a perfectly good map.
    for (int i = 0; i < tex.size() && i < part.textures.size(); ++i)
        if (!tex[i].isNull()) part.textures[i] = tex[i];
    qInfo("customize: %s — %d skin material(s) follow the chosen tone (%s)",
          qUtf8Printable(stem), n, qUtf8Printable(body.section(QLatin1Char('/'), -1)));
}

void CustomizeTab::applyAvatarLookToPart(int partIdx)
{
    if (!m_lookActive || partIdx < 0 || partIdx >= m_parts.size()) return;
    Part& part = m_parts[partIdx];
    const QString stem = part.path.section(QLatin1Char('/'), -1)
                             .section(QLatin1Char('.'), 0, 0);
    // Heads and hair only. Everything else on the character is clothing and
    // has nothing to do with the face.
    const bool isHead = stem.contains(QLatin1String("_type"))
        && stem.startsWith(QLatin1String("av"));
    const bool isHair = stem.contains(QLatin1String("_hair"));
    // A bare limb — the arms, legs and torso a survivor starts in, which are
    // skin rather than clothing and so follow the face's tone.
    const bool isBareLimb = m_slotDefaults.contains(QStringLiteral("arm"))
        && (part.fileIdx == m_slotDefaults.value(QStringLiteral("arm"), -1)
            || part.fileIdx == m_slotDefaults.value(QStringLiteral("leg"), -1)
            || part.fileIdx == m_slotDefaults.value(QStringLiteral("torso"), -1));
    // …and SKIN INSIDE A GARMENT, which is the other half of the same thing
    // and was reaching nothing. The test above asks "is this the survivor's
    // starting arm", and the MGO avatar has no arm, leg or torso slot at all —
    // its whole body is one Base garment — so on that page it could never be
    // true and the chosen tone stopped at the neck on every outfit that leaves
    // skin showing. Measured: nine garments per gender carry a material the
    // artists named "color_skin" (the fatigues, the BDU, the three
    // short-sleeved suits), and those are exactly the ones skinMaterials()
    // picks out. Heads and hair are excluded because they are handled below —
    // and because the FACE material passes the same test, so letting a head in
    // here would paint the body map across it.
    const bool hasSkin = !isHead && !isHair && !isBareLimb
        && !modelload::skinMaterials(part.model).isEmpty();
    if (!isHead && !isHair && !isBareLimb && !hasSkin) return;
    // A GARMENT TAKES THE NARROW PATH and returns. It must not go through
    // avatarOverrides(): that is a head's function, and its eyebrow fallbacks
    // claim any material binding exactly base, normal and specular — 31 of
    // MGO's garment and hat materials are that shape, the fatigues among them
    // — and then paint the eyebrow atlas onto it or hide its mesh group. The
    // one thing clothing needs from the look is its own skin following the
    // tone, and that is all it gets.
    if (hasSkin) { applySkinToneToPart(partIdx); return; }
    fox::AvatarLook look = m_look;
    look.bareSkin = isBareLimb;
    // The shared body skin is TPP/MGO avatar art. A Survive part must not take
    // it — measured, the only body maps anywhere in the shipped data are
    // /Assets/tpp/.../avm0_body0_def_c00..c04 and
    // /Assets/mgo/.../avf0_body0_def_c00, and Survive has none of its own.
    look.avatarGame = !part.path.startsWith(QLatin1String("/Assets/ssd/"),
                                            Qt::CaseInsensitive);
    // Is the bandanna ITEM among the parts this character is wearing? The head
    // carries the bandanna mesh and the item is what switches it on, so this
    // has to be a question about the whole build rather than about the head.
    //
    // hat21 is the bandanna in both games — Survive ships hat21_main0_def and
    // hat21_main0_def_f, and MGO's gear table names the same model as hat_m21
    // and hat_f21. Matched on the family rather than the exact stem so the two
    // fits and any future variant all count.
    for (const Part& other : m_parts) {
        const QString os = other.path.section(QLatin1Char('/'), -1)
                               .section(QLatin1Char('.'), 0, 0);
        if (os.startsWith(QLatin1String("hat21"))) { look.bandana = true; break; }
    }
    // The SUBJECT's gender, not the part's. lookSex() is what every other
    // gendered lookup on this page already uses, so the bare skin agrees with
    // the face rather than being decided separately.
    look.men = lookSex() == fox::AvatarPresets::Sex::Men;
    // Which hairstyle is fitted, so this head can wear that style's hairline.
    look.hairStem = selectedHairStem();

    // The part's own variation table first. Without it there is nothing to
    // rewrite: these heads bind 128x128 placeholders and name no real map.
    const ArchiveIndex& index = ArchiveIndex::instance();
    modelload::FovaOverrides base;
    QString usedVariation;
    const QVector<fox::CatalogVariation> vars = m_source.variationsFor
        ? m_source.variationsFor(stem) : QVector<fox::CatalogVariation>();
    for (const fox::CatalogVariation& v : vars) {
        if (v.fileIdx < 0 || v.fileIdx >= index.files().size()) continue;
        fox::FovaFile fv;
        const QByteArray fd = index.readFile(index.files()[v.fileIdx]);
        int matched = 0;
        if (fd.isEmpty() || !fv.parse(fd)) continue;
        const modelload::FovaOverrides cand =
            modelload::fovaOverrides(part.model, fv, &matched);
        // Deliberately NOT scanned for mesh-group hiding. `vars` is every
        // table in this part's FOVA group — including the one the camo combo
        // has selected — and nothing here ever clears what it writes, so
        // picking a variation and then picking "none" would leave its hiding
        // in place for the rest of the session. Group visibility belongs to
        // applyFovaToPart(), which is called on every change, "none" included.
        if (matched <= 0) continue;
        base = cand;
        usedVariation = v.name;
        break;
    }

    modelload::FovaOverrides ov;
    modelload::TextureOverlays overlays;
    modelload::TextureForce force;
    QSet<int> hide;
    int classified = 0;
    modelload::FovaOverrides ovNrm;
    modelload::FovaOverrides ovSrm;
    modelload::TextureIris iris;
    modelload::avatarOverrides(part.model, stem, look, &base, &ov, &overlays,
                               &force, &hide, &classified, &ovNrm, &iris,
                               &ovSrm);
    part.hiddenGroups = hide;
    if (ov.isEmpty() && overlays.isEmpty() && force.isEmpty()
        && ovNrm.isEmpty() && ovSrm.isEmpty() && iris.isEmpty()) {
        if (isHead)
            m_lookNote = QStringLiteral(
                "%1 has no variation table in this install, so its face is the "
                "placeholder the model ships with").arg(stem);
        return;
    }
    const IndexedFile& f = index.files()[part.fileIdx];
    int found = 0;
    QVector<QImage> tex = modelload::loadBaseTextures(part.model, f.gz, &found,
                                                      &ov, false, &overlays,
                                                      &force, &iris);
    // Only take what actually decoded: a substitution that resolves to nothing
    // must not blank a material that already had a perfectly good map.
    for (int i = 0; i < tex.size() && i < part.textures.size(); ++i)
        if (!tex[i].isNull()) part.textures[i] = tex[i];
    if (!ovNrm.isEmpty()) {
        int nfound = 0;
        const QVector<QImage> nrm =
            modelload::loadNormalMaps(part.model, f.gz, &nfound, &ovNrm);
        if (part.normalMaps.size() < nrm.size()) part.normalMaps.resize(nrm.size());
        for (int i = 0; i < nrm.size() && i < part.normalMaps.size(); ++i)
            if (!nrm[i].isNull()) part.normalMaps[i] = nrm[i];
    }
    // ── The SRMs ────────────────────────────────────────────────────────
    // ONLY the material slot is taken, not the whole GLPbrMaterial: the rest
    // of it — the translucency, the layer pair, the MTM, the four FMTT presets
    // — was resolved correctly when the part loaded and re-running the loader
    // resolves it identically. Copying the lot would work and would also mean
    // that every future field added to that struct silently gets a second,
    // parallel place it can be set from.
    //
    // Skipped entirely when the part was loaded without PBR: the viewport is
    // not reading an SRM in that mode, and decoding one per material to put it
    // somewhere nothing looks is pure cost.
    if (!ovSrm.isEmpty() && !part.pbr.isEmpty()) {
        int sfound = 0;
        const QVector<GLPbrMaterial> pm =
            modelload::loadPbrMaps(part.model, f.gz, &sfound, &ovSrm);
        // ONLY the materials this pass actually substituted. The re-run
        // resolves the whole model, and copying every index would put the
        // model's OWN shipped SRM back over one that a .fv2 variation had
        // already substituted a moment earlier in applyFovaToPart — a
        // silent revert on exactly the parts that have variations. It also
        // kept the count honest: "N SRM(s) bound that the model does not
        // name" was previously counting every material that had any SRM.
        QSet<int> touched;
        for (auto it = ovSrm.constBegin(); it != ovSrm.constEnd(); ++it)
            touched.insert(it.key().first);
        int applied = 0;
        for (int i = 0; i < pm.size() && i < part.pbr.size(); ++i) {
            if (pm[i].material.isNull() || !touched.contains(i)) continue;
            part.pbr[i].material = pm[i].material;
            part.pbr[i].materialSource = pm[i].materialSource;
            part.pbr[i].materialRole = pm[i].materialRole;
            ++applied;
        }
        if (applied)
            qInfo("customize: %s — %d SRM(s) bound that the model itself does "
                  "not name (face/hair/brow/beard are substituted at runtime)",
                  qUtf8Printable(stem), applied);
    }
    if (isHead) {
        const fox::AvatarTextures& at = fox::AvatarTextures::instance();
        m_lookNote = QStringLiteral("skin %1 · wrinkles %2")
                         .arg(m_look.skin + 1)
                         .arg(m_look.wrinkle + 1);
        if (m_look.decoType >= 0)
            m_lookNote += QStringLiteral(" · %1 %2")
                              .arg(QString::fromLatin1(
                                       fox::AvatarTextures::decoName(m_look.decoType)),
                                   QString::number(m_look.decoId));
        m_lookNote += QStringLiteral(" (%1 of %2 material(s) retextured")
                          .arg(classified).arg(part.model.materials().size());
        m_lookNote += usedVariation.isEmpty()
            ? QStringLiteral(", no variation)")
            : QStringLiteral(", via %1)").arg(usedVariation);
        if (!at.ok()) m_lookNote = at.note();
    }
    if (qEnvironmentVariableIsSet("FOXAB_DUMP_TEX"))
        qInfo("avatar-look: %s — %d material(s) substituted, %d decoded",
              qUtf8Printable(stem), classified, found);
}

void CustomizeTab::addPart(int fileIdx, const QString& fovaName)
{
    for (const Part& p : m_parts)
        if (p.fileIdx == fileIdx) return;   // already equipped

    modelload::LoadedModel lm = modelload::load(fileIdx, pbrMode());
    if (!lm.ok) {
        setStatus(QStringLiteral("Failed to load part: %1").arg(lm.error));
        return;
    }
    Part part;
    part.fileIdx = fileIdx;
    part.fovaName = fovaName;
    part.path = ArchiveIndex::instance().files()[fileIdx].path;
    part.model = std::move(lm.model);
    part.textures = std::move(lm.textures);
    part.normalMaps = std::move(lm.normalMaps);
    part.pbr = std::move(lm.pbr);
    if (part.path.endsWith(QLatin1String(".fmdl"))) {
        QString frdvPath = part.path;
        frdvPath.chop(5);
        frdvPath += QStringLiteral(".frdv");
        const ArchiveIndex& ix = ArchiveIndex::instance();
        const int at = ix.fileIndexForPath(frdvPath);
        if (at >= 0) {
            const QByteArray fd = ix.readFile(ix.files()[at]);
            part.hasFrdv = !fd.isEmpty() && part.frdv.parse(fd);
        }
    }
    // Per-part rig: each equipped part animates through ITS OWN rig, so a
    // dog + handler composition poses both correctly from one clip source.
    part.hasFrig = rigbind::loadFrigFor(part.path, &part.frig);
    // Connect points (attachment sockets), from the sibling .fcnp.
    if (part.path.endsWith(QLatin1String(".fmdl"))) {
        QString fcnpPath = part.path;
        fcnpPath.chop(5);
        fcnpPath += QStringLiteral(".fcnp");
        const ArchiveIndex& ix = ArchiveIndex::instance();
        const int at = ix.fileIndexForPath(fcnpPath);
        if (at >= 0) {
            const QByteArray cd = ix.readFile(ix.files()[at]);
            part.hasFcnp = !cd.isEmpty() && part.fcnp.parse(cd);
        }
    }
    m_parts.append(std::move(part));
    // The look is a set of texture substitutions, applied once the part is in
    // place so it can be re-applied later without reloading the model.
    // ORDER MATTERS. applyFovaToPart() reloads BOTH texture sets wholesale from
    // the model plus the camo table, so running it after the look would throw
    // the face, brows, hair and wrinkle normals away. The camo pass goes first
    // and the look is laid over the top of it.
    // …or when a gear colour is set, which is also a texture substitution and
    // reaches the part through the same function. Gating this on the variation
    // name alone meant a colour chosen with no camouflage selected was never
    // applied to anything. The MGO per-item dye is the third member of the
    // same class, and skipping it here would repeat that exact bug per item.
    {
        const QPair<QString, QString> dye =
            mgoColoursForPart(m_parts.last().fileIdx);
        if (!fovaName.isEmpty() || m_gearColor != 0 || !dye.first.isEmpty()
            || !dye.second.isEmpty())
            applyFovaToPart(m_parts.size() - 1, fovaName);
    }
    applyAvatarLookToPart(m_parts.size() - 1);
    auto* item = new QListWidgetItem(
        m_parts.last().path.section(QLatin1Char('/'), -1), m_equipped);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Checked);
    rebuildScene();
}

void CustomizeTab::removePartAt(int row)
{
    if (row < 0 || row >= m_parts.size()) return;
    m_parts.removeAt(row);
    delete m_equipped->takeItem(row);
    // Remap attachments: hosts above `row` shifted down; attachments TO the
    // removed part detach.
    for (Part& p : m_parts) {
        if (p.attachPart == row) {
            p.attachPart = -1;
            p.attachCnp.clear();
        } else if (p.attachPart > row) {
            --p.attachPart;
        }
    }
    // The slot rows hold indices into m_parts too, and a removal renumbers
    // them. partIdx has always gone stale here; companionPartIdx would follow
    // it, and that one decides which model a Vest Color change retextures — a
    // stale index dyes an unrelated part. Both are remapped rather than left.
    for (WeaponSlotRow& r : m_weaponRows) {
        for (int* idx : {&r.partIdx, &r.companionPartIdx}) {
            if (*idx == row) *idx = -1;
            else if (*idx > row) --*idx;
        }
    }
    rebuildScene();
}

void CustomizeTab::removeSelectedPart()
{
    removePartAt(m_equipped->currentRow());
}

bool CustomizeTab::attachmentAlreadyFitted(const Part& part, int ai) const
{
    // By PATH, not by file index. The two come from different places and pick
    // different copies of the same asset: the catalogue prefers Survive's own
    // archive and skips games the filter has switched off, while the .fv2's
    // PathCode64 resolves through the index's first-inserted-wins hash map. On
    // an install carrying the same avm head under both an ssd and a tpp
    // archive those are different indices for one model, the test would fail,
    // and the head would be drawn twice — which is the whole reason this
    // exists.
    const int src = part.fovaAttachedFiles.value(ai, -1);
    const auto& files = ArchiveIndex::instance().files();
    if (src < 0 || src >= files.size()) return false;
    const QString path = files[src].path;
    for (const Part& other : m_parts)
        if (other.path == path) return true;
    // …and against every other part's attachments, since one variation name is
    // applied to every fitted part: two of them attaching the same hat would
    // otherwise draw it twice.
    for (const Part& other : m_parts) {
        for (int k = 0; k < other.fovaAttachedFiles.size(); ++k) {
            if (&other == &part && k == ai) return false;   // reached itself
            const int o = other.fovaAttachedFiles[k];
            if (o >= 0 && o < files.size() && files[o].path == path) return true;
        }
    }
    return false;
}

modelload::PbrMode CustomizeTab::pbrMode() const
{
    // THE VIEWPORT is the authority; the setting is only what it was
    // initialised from. Once the user has ticked it for this session, adding a
    // part must respect that rather than dropping back to the stored value.
    // Before the panel is built the box does not exist yet, and then the
    // setting is all there is.
    const bool on = m_view ? m_view->pbrShading()
                             : Config::pbrEnabled(Config::PbrView::Customize);
    return on ? modelload::PbrMode::Full : modelload::PbrMode::Basic;
}

void CustomizeTab::reloadPartMaps()
{
    // Re-run each part's texture load at the current PBR mode, keeping the
    // parts, the variation and the look exactly as they are. Cheaper and far
    // less disruptive than rebuilding the whole character: nothing is
    // re-selected, so the camera, the slots and the equipped list are
    // untouched.
    for (int i = 0; i < m_parts.size(); ++i) {
        // The part's OWN remembered variation, not the combo's: a part fitted
        // from an outfit or a preset can carry one the combo never showed.
        // Copied, because applyFovaToPart assigns to the very field it would
        // otherwise be reading through.
        const QString fova = m_parts[i].fovaName;
        applyFovaToPart(i, fova);
        applyAvatarLookToPart(i);
    }
    rebuildScene();
}

int CustomizeTab::applyFovaByName(const QString& fovaName)
{
    if (m_parts.isEmpty()) return 0;
    int applied = 0;
    for (int i = 0; i < m_parts.size(); ++i) {
        bool found = false;
        applyFovaToPart(i, fovaName, &found);
        if (found) ++applied;   // the table RESOLVED, not merely was asked for
    }
    // Nothing resolved by NAME. Fall back to naming a table by its FILE, which
    // is the only way to reach a whole class of them: the catalogue keys a
    // .fv2 to a model by filename convention, and the tables that ATTACH a
    // model largely do not follow it. Measured over the 3,554 shipped tables,
    // 339 carry a "cm_" prefix and only 18 of those key to a model as they
    // stand; the eye-cover tables abbreviate on top of that
    // ("cm_f0_h0_v000_eye0" against the model "cm_f0_head0_v000_cov"), so no
    // filename rule reaches them. The table itself names the model it attaches,
    // which is where the real association lives — see MORNING_REPORT.md. This
    // is a HARNESS path: it exists so the attachment export can be tested at
    // all, and it does not change what the screen offers.
    if (applied == 0 && !fovaName.isEmpty()) {
        const auto& files = ArchiveIndex::instance().files();
        int forced = -1;
        for (int i = 0; i < files.size(); ++i) {
            if (!files[i].path.endsWith(QLatin1String(".fv2"), Qt::CaseInsensitive))
                continue;
            if (!files[i].path.contains(fovaName, Qt::CaseInsensitive)) continue;
            forced = i;
            break;
        }
        if (forced >= 0) {
            const QString name = files[forced].path.section(QLatin1Char('/'), -1)
                                     .section(QLatin1Char('.'), 0, 0)
                                     .section(QLatin1Char('_'), -1);
            // ONE part, not all of them. A forced table is read without any
            // model to check it against, and the models it ATTACHES are a
            // property of the table alone — so applying it to every part
            // stamped N copies of the same hat into the scene and into the
            // export. The first part is the host by construction.
            bool found = false;
            applyFovaToPart(0, name, &found, forced);
            if (found) ++applied;
            // The part now remembers only the NAME, and reloadPartMaps() —
            // which the PBR toggle calls — re-resolves by name without the
            // file. So a forced table does not survive a PBR toggle. That is
            // acceptable for a harness probe and would not be for the UI,
            // which is why this path is not reachable from the screen.
            qInfo("customize: '%s' resolved by FILE (%s) onto part 0 — no part "
                  "keys to it by name", qUtf8Printable(fovaName),
                  qUtf8Printable(files[forced].path.section(QLatin1Char('/'), -1)));
        }
    }
    rebuildScene();
    return applied;
}

void CustomizeTab::refreshInspector()
{
    if (!m_inspector) return;
    if (!m_npanel || !m_npanel->isPanelOpen(QStringLiteral("materials"))) {
        m_inspector->clear();
        return;
    }
    QVector<MaterialInspector::Source> sources;
    int slotBase = 0;
    for (int pi = 0; pi < m_parts.size(); ++pi) {
        const Part& part = m_parts[pi];
        MaterialInspector::Source src;
        src.label = QStringLiteral("part %1 · %2")
                        .arg(pi)
                        .arg(part.path.section(QLatin1Char('/'), -1));
        src.materials = MaterialInspector::entriesFor(part.model);
        src.base = part.textures;
        src.normals = part.normalMaps;
        src.pbr = part.pbr;
        src.slotBase = slotBase;
        {
            const auto& files = ArchiveIndex::instance().files();
            src.gz = part.fileIdx >= 0 && part.fileIdx < files.size()
                && files[part.fileIdx].gz;
        }
        sources.append(src);
        slotBase += part.textures.size();

        // The models the part's variation BRINGS with it are separate sources.
        // They occupy their own material slots in the scene and are a common
        // place for a texture question to end up ("the hat is white"), so
        // hiding them here would leave the panel describing a scene that is
        // not the one on screen. The skip test is the same one rebuildScene
        // uses, so the slot numbers agree with what the viewport binds.
        for (int ai = 0; ai < part.fovaAttached.size(); ++ai) {
            if (attachmentAlreadyFitted(part, ai)) continue;
            const modelload::LoadedModel& att = part.fovaAttached[ai];
            MaterialInspector::Source as;
            const int fi = part.fovaAttachedFiles.value(ai, -1);
            const auto& files = ArchiveIndex::instance().files();
            as.label = QStringLiteral("part %1 attach · %2")
                           .arg(pi)
                           .arg(fi >= 0 && fi < files.size()
                                    ? files[fi].path.section(QLatin1Char('/'), -1)
                                    : QStringLiteral("(unknown)"));
            as.materials = MaterialInspector::entriesFor(att.model);
            as.base = att.textures;
            as.normals = att.normalMaps;
            as.pbr = att.pbr;
            as.slotBase = slotBase;
            as.gz = fi >= 0 && fi < files.size() && files[fi].gz;
            sources.append(as);
            slotBase += att.textures.size();
        }
    }
    m_inspector->setSources(sources);
}

void CustomizeTab::setPbrShading(bool on)
{
    // Through the box, so the whole toggled path runs — the shader flag, the
    // reload when the maps are missing, and the panel refresh.
    if (m_view) m_view->setPbrShading(on);
}

void CustomizeTab::setMaterialFilter(const QString& text)
{
    if (m_inspector) m_inspector->setFilterText(text);
}

// What OPENING a panel does — one implementation, shared by the signal and the
// startup pass over what restoreState left open.
void CustomizeTab::fillPanel(const QString& key)
{
    if (key == QLatin1String("animations")) {
        if (m_animPanel && !m_animPanel->isBuilt()) m_animPanel->rebuild();
        syncAnimPanel();
    } else if (key == QLatin1String("parts")) {
        refreshSceneTree();
    } else if (key == QLatin1String("materials")) {
        if (m_view && m_view->pbrShading() && !m_parts.isEmpty()
            && !m_view->hasPbrMaps())
            reloadPartMaps();
        else
            refreshInspector();
    } else if (key == QLatin1String("info")) {
        refreshInfoPanel();
    }
}

void CustomizeTab::fillOpenPanels()
{
    if (!m_npanel) return;
    QStringList filled;
    for (const QString& key : m_npanel->panelKeys()) {
        if (!m_npanel->isPanelOpen(key)) continue;
        fillPanel(key);
        filled << key;
    }
    qInfo("customize: panels restored open and filled — %s",
          filled.isEmpty() ? "none"
                           : qUtf8Printable(filled.join(QLatin1Char('+'))));
}

void CustomizeTab::refreshSceneTree()
{
    if (!m_sceneTree) return;
    // The tree is about to be rebuilt with every row checked, so the
    // viewport's per-mesh switches have to go with it. Without this, closing
    // the panel with a submesh hidden and reopening it showed a ticked box
    // over an invisible mesh — and clicking that box changed no state, so it
    // emitted nothing and the mesh stayed gone.
    m_view->clearMeshVisibility();
    if (!m_npanel || !m_npanel->isPanelOpen(QStringLiteral("parts"))) {
        m_sceneTree->clear();
        return;
    }
    m_sceneTree->setScene(m_treeRoots);
}

// ── INFO (template §6) ──────────────────────────────────────────────────────
// Everything this tab knows about the scene it just built. The Models tab's
// INFO describes ONE file; this one describes an assembly, so the shape is
// different: a SCENE section with what the whole thing adds up to, then one
// section per fitted part naming where it came from, what it is seated on, and
// which variation is on it.
//
// Nothing here is computed for the panel's sake — every row is a field the
// composer already filled in to draw the scene.
void CustomizeTab::refreshInfoPanel()
{
    if (!m_infoPanel) return;
    if (!m_npanel || !m_npanel->isPanelOpen(QStringLiteral("info"))) return;
    m_infoPanel->clear();
    if (m_parts.isEmpty()) { m_infoPanel->finish(); return; }

    const auto loc = QLocale();
    int tris = 0, verts = 0, mats = 0, bones = 0, attached = 0;
    for (const Part& p : m_parts) {
        for (const fox::FmdlMesh& m : p.model.meshes()) {
            tris += m.triangles.size() / 3;
            verts += m.positions.size() / 3;
        }
        mats += p.model.materials().size();
        bones += p.model.bones().size();
        attached += p.fovaAttached.size();
    }

    m_infoPanel->beginSection(QStringLiteral("SCENE"));
    m_infoPanel->addRow(QStringLiteral("Parts fitted"),
                        QString::number(m_parts.size()));
    if (attached > 0)
        m_infoPanel->addRow(
            QStringLiteral("Brought by variations"), QString::number(attached),
            QStringLiteral("Models a .fv2 attaches — a hat, a bag, a hair "
                           "mesh. They live and die with the variation that "
                           "asked for them."));
    m_infoPanel->addRow(QStringLiteral("Triangles"), loc.toString(tris));
    m_infoPanel->addRow(QStringLiteral("Vertices"), loc.toString(verts));
    m_infoPanel->addRow(QStringLiteral("Materials"), QString::number(mats));
    m_infoPanel->addRow(
        QStringLiteral("Bones"), loc.toString(bones),
        QStringLiteral("Summed across the parts, which is what the combined "
                       "palette costs — not the count of distinct bones."));
    m_infoPanel->addRow(
        QStringLiteral("PBR maps"),
        m_view && m_view->hasPbrMaps() ? QStringLiteral("loaded")
                                       : QStringLiteral("not loaded"));
    if (m_hasAnim)
        m_infoPanel->addRow(QStringLiteral("Posed at frame"),
                            QString::number(int(m_frame)));

    for (int i = 0; i < m_parts.size(); ++i) {
        const Part& p = m_parts[i];
        m_infoPanel->beginSection(
            QStringLiteral("PART %1 · %2")
                .arg(i)
                .arg(p.path.section(QLatin1Char('/'), -1)));
        m_infoPanel->addRow(QStringLiteral("Path"), p.path);
        int pt = 0;
        for (const fox::FmdlMesh& m : p.model.meshes())
            pt += m.triangles.size() / 3;
        m_infoPanel->addRow(QStringLiteral("Triangles"), loc.toString(pt));
        m_infoPanel->addRow(QStringLiteral("Submeshes"),
                            QString::number(p.model.meshes().size()));
        m_infoPanel->addRow(QStringLiteral("Materials"),
                            QString::number(p.model.materials().size()));
        if (!p.fovaName.isEmpty())
            m_infoPanel->addRow(QStringLiteral("Variation (.fv2)"), p.fovaName);
        if (p.attachPart >= 0)
            m_infoPanel->addRow(
                QStringLiteral("Seated on"),
                QStringLiteral("part %1 at %2").arg(p.attachPart).arg(p.attachCnp),
                QStringLiteral("Rigidly attached at another part's connect "
                               "point, rather than skinned to the shared "
                               "skeleton."));
        m_infoPanel->addRow(
            QStringLiteral("Bone base"), QString::number(p.boneBase),
            QStringLiteral("Where this part's bones start in the combined "
                           "palette. Every part after it is offset by the "
                           "ones before."));
        const int hid = p.hiddenGroups.size() + p.fovaHiddenGroups.size();
        if (hid > 0)
            m_infoPanel->addRow(
                QStringLiteral("Hidden groups"), QString::number(hid),
                QStringLiteral("Mesh groups this part must not draw — the "
                               "avatar look's own hiding plus the applied "
                               "variation's, kept separate so one cannot wipe "
                               "the other."));
        m_infoPanel->addRow(
            QStringLiteral("Rig (.frig)"),
            p.hasFrig ? QStringLiteral("yes") : QStringLiteral("none"));
        m_infoPanel->addRow(
            QStringLiteral("Help bones (.frdv)"),
            p.hasFrdv ? QStringLiteral("%1 ops").arg(p.frdv.ops().size())
                      : QStringLiteral("none"));
        m_infoPanel->addRow(
            QStringLiteral("Connect points (.fcnp)"),
            p.hasFcnp ? QStringLiteral("%1").arg(p.fcnp.points().size())
                      : QStringLiteral("none"));
    }
    m_infoPanel->finish();
}

QString CustomizeTab::setPanelsForShot(const QString& keys)
{
    if (!m_npanel) return QStringLiteral("no column");
    const QStringList want =
        keys.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0
            ? QStringList()
            : keys.split(QLatin1Char(','), Qt::SkipEmptyParts);
    const QStringList known = m_npanel->panelKeys();
    QStringList unknown;
    for (const QString& k : want)
        if (!known.contains(k.trimmed())) unknown << k.trimmed();
    for (const QString& k : known) m_npanel->setPanelOpen(k, want.contains(k));
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

void CustomizeTab::setSubmeshTreeVisible(bool on)
{
    // Through the COLUMN, not straight at the widget: opening a panel is what
    // makes the tab fill it, and setting the widget's visibility alone would
    // open an empty one.
    if (m_npanel) m_npanel->setPanelOpen(QStringLiteral("parts"), on);
}

bool CustomizeTab::hideSubmesh(const QString& needle)
{
    return m_sceneTree && m_sceneTree->uncheckMatching(needle);
}

void CustomizeTab::setDebugPanelVisible(bool on)
{
    if (m_npanel) m_npanel->setPanelOpen(QStringLiteral("materials"), on);
}

void CustomizeTab::rebuildScene()
{
    QVector<GLMeshUpload> uploads;
    QVector<QImage> textures, normalMaps;
    QVector<GLPbrMaterial> pbr;
    // Decided ONCE for the whole scene, not per part. Deriving it from
    // "does this part have PBR maps" made the vector's length depend on which
    // part happened to come first: a part with no materials of its own, or an
    // attachment loaded without PBR, contributed nothing while still
    // advancing texBase, and every later part's maps landed on the wrong
    // material slot. The final resize() hid it, because the LENGTH came out
    // right in the end.
    const bool wantPbr = pbrMode() == modelload::PbrMode::Full;
    GLSkeletonUpload skeleton;
    // Every equipped part's connect points, collected as the scene is laid out
    // so the bone indices are re-based in the same pass that re-bases the
    // joints. Handed to the viewport at the end, with setModel's own clear in
    // between — a scene rebuilt with fewer parts must not keep the old sockets.
    QVector<GLConnectPoint> scenePoints;
    int boneBase = 0, texBase = 0, totalBones = 0, totalTris = 0;
    // Scene-wide submesh ids. buildUploads() numbers meshes within their own
    // model, so a scene made of nine parts would have nine mesh 0s; they are
    // re-based here exactly as material slots and bone palettes are.
    int meshBase = 0;
    m_treeRoots.clear();
    m_meshOwner.clear();

    for (int pi = 0; pi < m_parts.size(); ++pi) {
        Part& part = m_parts[pi];
        part.boneBase = boneBase;
        QVector<GLMeshUpload> ups = modelload::buildUploads(part.model);
        if (pi == m_headOnlyPart)
            modelload::keepBoneSubtree(ups, part.model,
                                       fox::boneCode("SKL_004_HEAD"));
        // buildUploads() carries the model's own mesh group; rebuildScene
        // overwrites it with the part index below, so anything group-based has
        // to happen HERE.
        // The variation's SHOW list overrides the variation's own hiding and
        // the bandanna heuristic — both are guesses about geometry. It does NOT
        // override the LOOK, which is a stated choice: a table that names the
        // beard's group would otherwise put the model's authored default beard
        // back on a clean-shaven preset.
        QSet<int> hidden = part.fovaHiddenGroups - part.fovaShownGroups;
        hidden |= part.hiddenGroups;
        if (!hidden.isEmpty())
            ups.erase(std::remove_if(ups.begin(), ups.end(),
                                     [&](const GLMeshUpload& u) {
                                         return hidden.contains(u.groupId);
                                     }),
                      ups.end());
        // The tree describes what is ACTUALLY drawn, so it is built from the
        // upload list after the hiding above rather than from the model — a
        // submesh a variation removed has no switch because it is not there.
        SceneTree::Node partNode;
        partNode.label = QStringLiteral("%1 · %2")
                             .arg(pi)
                             .arg(part.path.section(QLatin1Char('/'), -1));
        QHash<int, int> groupNodeOf;
        for (GLMeshUpload& u : ups) {
            // The material index INSIDE this part's own model, kept before the
            // scene-wide offset goes on: the tree's caption names the part's
            // material and part.model.materials() is indexed the part's way.
            const int localMat = u.materialSlot;
            if (u.materialSlot >= 0) u.materialSlot += texBase;
            const int modelGroup = u.groupId;   // before it becomes the part
            const int localMesh = u.meshId;
            // A -1 id stays -1: the viewport treats that as "not individually
            // addressable" and always draws it. Folding it onto 0 with qMax
            // would have made it share mesh 0's checkbox instead.
            if (u.meshId >= 0) {
                u.meshId += meshBase;
                m_meshOwner.insert(u.meshId,
                                   qMakePair(qMakePair(pi, -1), localMesh));
            }
            u.groupId = pi;   // parts toggle as wholes here
            for (quint16& j : u.joints)
                j = static_cast<quint16>(j + boneBase);
            totalTris += u.indices.size() / 3;

            if (!groupNodeOf.contains(modelGroup)) {
                SceneTree::Node g;
                g.label = (modelGroup >= 0
                           && modelGroup < part.model.meshGroups().size())
                    ? part.model.meshGroups()[modelGroup].name
                    : QStringLiteral("(no group)");
                groupNodeOf.insert(modelGroup, partNode.children.size());
                partNode.children.append(g);
            }
            SceneTree::Node leaf;
            leaf.label = QStringLiteral("mesh %1").arg(localMesh);
            leaf.meshId = u.meshId;
            leaf.tris = int(u.indices.size() / 3);
            // The PART-LOCAL index, matching the name beside it. The scene-wide
            // slot (u.materialSlot, already offset by texBase) indexes the
            // viewport's texture array and nothing a reader can look up, so a
            // caption reading "slot 47 · face_skin" named two different lists
            // in one line.
            leaf.materialSlot = localMat;
            if (localMat >= 0 && localMat < part.model.materials().size())
                leaf.material = part.model.materials()[localMat].name;
            partNode.children[groupNodeOf.value(modelGroup)].children.append(leaf);

            uploads.append(std::move(u));
        }
        meshBase += part.model.meshes().size();
        if (!partNode.children.isEmpty()) m_treeRoots.append(partNode);
        textures += part.textures;
        // Keep the normal-map set the same length as the texture set: the two
        // are indexed by the SAME material slot, so a part with no normals must
        // still contribute one (null) entry per material or every later part's
        // maps shift onto the wrong material.
        QVector<QImage> partNormals = part.normalMaps;
        partNormals.resize(part.textures.size());
        normalMaps += partNormals;
        // Same length rule for the PBR set, applied unconditionally so this
        // part contributes exactly part.textures.size() entries whether it has
        // maps or not.
        if (wantPbr) {
            QVector<GLPbrMaterial> partPbr = part.pbr;
            partPbr.resize(part.textures.size());
            pbr += partPbr;
        }
        {
            const GLSkeletonUpload ps = modelload::buildSkeleton(part.model);
            skeleton.lines += ps.lines;
            // The bone labels concatenate at boneBase exactly as the joint
            // indices and the palettes do — one entry per bone, in bone order.
            // That is the contract GLSkeletonUpload documents, and it is what
            // lets the bone-name overlay pose itself from the combined palette.
            skeleton.boneNames += ps.boneNames;
            skeleton.bonePositions += ps.bonePositions;
            // The part's sockets, for the hardpoints overlay, with their bone
            // index re-based into the combined array the same way.
            if (part.hasFcnp) {
                QHash<QString, int> boneOf;
                const auto& bones = part.model.bones();
                for (int b = 0; b < bones.size(); ++b)
                    boneOf.insert(bones[b].name, b);
                for (const fox::ConnectPoint& cp : part.fcnp.points()) {
                    GLConnectPoint g;
                    g.name = cp.name;
                    const int local = cp.parentBone.isEmpty()
                        ? -1 : boneOf.value(cp.parentBone, -1);
                    g.bone = local >= 0 ? local + boneBase : -1;
                    QVector3D at(cp.pos[0], cp.pos[1], cp.pos[2]);
                    if (local >= 0)
                        at += QVector3D(bones[local].worldPos[0],
                                        bones[local].worldPos[1],
                                        bones[local].worldPos[2]);
                    g.pos = at;
                    scenePoints.append(g);
                }
            }
        }
        boneBase += part.model.bones().size();
        texBase += part.textures.size();
        totalBones += part.model.bones().size();

        // What the applied variation brings with it — the hat, the bag, the
        // hair the .fv2 attaches. Laid out exactly like a part of its own,
        // with its own bone and texture bases, because that is how every other
        // model in this scene is handled and the shared-skeleton case is served
        // by the rig binding rather than by sharing a bone palette.
        //
        // The one thing it does NOT get is its own groupId: it takes its
        // wearer's, so it toggles with the part that asked for it and cannot be
        // switched off on its own. A variation's hat is not a separate item.
        for (int ai = 0; ai < part.fovaAttached.size(); ++ai) {
            // Already fitted from the catalogue? Then this is the variation
            // describing the composition the browser has already made, not
            // something extra — avm0_type0_v00 hides the body's baked head and
            // attaches the head model, which is exactly the head the Face
            // Preset row put on. Drawing it again is a second head inside the
            // first.
            if (attachmentAlreadyFitted(part, ai)) continue;
            const modelload::LoadedModel& att = part.fovaAttached[ai];
            QVector<GLMeshUpload> aups = modelload::buildUploads(att.model);
            SceneTree::Node attNode;
            {
                const int fi = part.fovaAttachedFiles.value(ai, -1);
                const auto& files = ArchiveIndex::instance().files();
                attNode.label = QStringLiteral("%1 attach · %2")
                                    .arg(pi)
                                    .arg(fi >= 0 && fi < files.size()
                                             ? files[fi].path.section(
                                                   QLatin1Char('/'), -1)
                                             : QStringLiteral("(unknown)"));
            }
            QHash<int, int> attGroupNodeOf;
            for (GLMeshUpload& u : aups) {
                // Part-local, kept before the scene-wide offset — see the same
                // line in the part loop above.
                const int localMat = u.materialSlot;
                if (u.materialSlot >= 0) u.materialSlot += texBase;
                const int modelGroup = u.groupId;
                const int localMesh = u.meshId;
                // A -1 id stays -1: the viewport treats that as "not individually
            // addressable" and always draws it. Folding it onto 0 with qMax
            // would have made it share mesh 0's checkbox instead.
            if (u.meshId >= 0) {
                u.meshId += meshBase;
                m_meshOwner.insert(u.meshId,
                                   qMakePair(qMakePair(pi, ai), localMesh));
            }
                u.groupId = pi;
                for (quint16& j : u.joints)
                    j = static_cast<quint16>(j + boneBase);
                totalTris += u.indices.size() / 3;

                if (!attGroupNodeOf.contains(modelGroup)) {
                    SceneTree::Node g;
                    g.label = (modelGroup >= 0
                               && modelGroup < att.model.meshGroups().size())
                        ? att.model.meshGroups()[modelGroup].name
                        : QStringLiteral("(no group)");
                    attGroupNodeOf.insert(modelGroup, attNode.children.size());
                    attNode.children.append(g);
                }
                SceneTree::Node leaf;
                leaf.label = QStringLiteral("mesh %1").arg(localMesh);
                leaf.meshId = u.meshId;
                leaf.tris = int(u.indices.size() / 3);
                leaf.materialSlot = localMat;
                // The attachment rows get the material's name too. They did
                // not, and a caption that carries it on some rows and not
                // others reads as missing data rather than as a distinction.
                if (localMat >= 0 && localMat < att.model.materials().size())
                    leaf.material = att.model.materials()[localMat].name;
                attNode.children[attGroupNodeOf.value(modelGroup)]
                    .children.append(leaf);

                uploads.append(std::move(u));
            }
            meshBase += att.model.meshes().size();
            if (!attNode.children.isEmpty()) m_treeRoots.append(attNode);
            textures += att.textures;
            QVector<QImage> attNormals = att.normalMaps;
            attNormals.resize(att.textures.size());
            normalMaps += attNormals;
            if (wantPbr) {
                QVector<GLPbrMaterial> attPbr = att.pbr;
                attPbr.resize(att.textures.size());
                pbr += attPbr;
            }
            {
                // ONE buildSkeleton call, not two: it walks every bone of the
                // model and the second copy was pure waste on a scene that can
                // hold a dozen attachments.
                const GLSkeletonUpload as = modelload::buildSkeleton(att.model);
                skeleton.lines += as.lines;
                skeleton.boneNames += as.boneNames;
                skeleton.bonePositions += as.bonePositions;
            }
            boneBase += att.model.bones().size();
            texBase += att.textures.size();
            totalBones += att.model.bones().size();
        }
    }
    // ── THE MATERIAL AUDIT ───────────────────────────────────────────────
    // FOXAB_DUMP_MATERIALS=1 prints every material of every part in the
    // composed scene with a verdict on each map. FOUR states, and the
    // difference between them is the whole question behind "a lot of models
    // are missing roughness and metalness":
    //
    //   none          the material declares no such slot at all
    //   flat(...)     it declares one and points it at a common_source/flat
    //                 placeholder — the game substitutes the real map at
    //                 runtime, which is what the avatar's face, hair, brows
    //                 and beard all do
    //   unresolved()  it declares a real asset that did not decode here. On a
    //                 full install that is a bug; on a partial mount it is
    //                 the mount, and the two must not be confused
    //   ok(...)       a real map, loaded
    //
    // METALNESS HAS NO MAP IN FOX and never will: F0 comes from the FMTT
    // preset the material selects. "No metalness map" is the format working as
    // designed, and the preset's F0 is the thing to check instead — so it is
    // printed on every line.
    if (wantPbr && qEnvironmentVariableIsSet("FOXAB_DUMP_MATERIALS")) {
        int real = 0, flatted = 0, unresolved = 0, absent = 0;
        const auto audit = [&](const QString& who, const fox::FmdlFile& mdl,
                               const QVector<GLPbrMaterial>& set) {
            for (int mi = 0; mi < mdl.materials().size(); ++mi) {
                const GLPbrMaterial* p = mi < set.size() ? &set[mi] : nullptr;
                // What the MODEL declares for the SRM slot, which is the half
                // the loaded image cannot tell you.
                QString declared;
                bool hasRole = false;
                quint64 declaredHash = 0;
                for (const fox::FmdlTextureRef& t : mdl.materials()[mi].textures)
                    if (t.roleHash32 == fox::texrole::kSpecular) {
                        hasRole = true;
                        declared = t.path;
                        declaredHash = t.pathHash;
                        break;
                    }
                QString srm;
                if (p && !p->material.isNull()) {
                    srm = p->materialSource.contains(
                              QLatin1String("/common_source/flat/"),
                              Qt::CaseInsensitive)
                        ? QStringLiteral("flat(%1)")
                              .arg(p->materialSource.section(QLatin1Char('/'), -1))
                        : QStringLiteral("ok(%1)")
                              .arg(p->materialSource.section(QLatin1Char('/'), -1));
                    if (srm.startsWith(QLatin1Char('o'))) ++real; else ++flatted;
                } else if (!hasRole || declaredHash == 0) {
                    // An EMPTY PATH is not the same thing as no slot. On an
                    // install without the name dictionary every declared path
                    // reads back empty, and testing the text counted every one
                    // of them as "this material has no specular slot" — the
                    // exact conflation this block exists to avoid. The HASH is
                    // in the file either way, so it is what decides: zero means
                    // the slot really is unbound, non-zero means the model
                    // names a map that did not decode here.
                    srm = hasRole ? QStringLiteral("unbound")
                                  : QStringLiteral("none");
                    ++absent;
                } else {
                    srm = QStringLiteral("unresolved(%1)")
                              .arg(declared.isEmpty()
                                       ? QStringLiteral("0x%1").arg(declaredHash, 0, 16)
                                       : declared.section(QLatin1Char('/'), -1));
                    ++unresolved;
                }
                qInfo("mat-audit: %s mat %d  shader=%s  srm=%s  F0=%.3f%s%s",
                      qUtf8Printable(who), mi,
                      qUtf8Printable(mdl.materials()[mi].shader.section(
                          QLatin1Char('/'), -1)),
                      qUtf8Printable(srm), p ? double(p->presetF0[0]) : 0.04,
                      p && p->skin ? "  skin" : "",
                      p && p->noMetal ? "  no-metal" : "");
            }
        };
        for (int i = 0; i < m_parts.size(); ++i)
            audit(m_parts[i].path.section(QLatin1Char('/'), -1), m_parts[i].model,
                  m_parts[i].pbr);
        qInfo("mat-audit: %d real SRM(s), %d flat placeholder(s), %d declared "
              "but not decoded HERE (partial mount?), %d with no SRM bound "
              "(none = no specular slot, unbound = an empty one)",
              real, flatted, unresolved, absent);
    }

    // Belt and braces: every contributor above adds exactly its own texture
    // count, so this should already hold. It is asserted rather than trusted
    // because a length mismatch does not fail — it silently shifts every map
    // onto the wrong material, which is invisible until someone looks closely
    // at a composed character.
    if (wantPbr && pbr.size() != textures.size()) {
        qWarning("customize: PBR set is %lld for %lld material(s) — padding; "
                 "some maps may be on the wrong material",
                 qint64(pbr.size()), qint64(textures.size()));
        pbr.resize(textures.size());
    }
    // WHICH GAME the viewport is looking at, so the Auto environment can
    // follow it. A majority vote over the parts in the scene, not the first
    // one: a character can borrow a model from another game (MGO reuses TPP's
    // heads) and one borrowed part must not relight the whole scene.
    {
        const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
        int votes[fox::kGameCount] = {0};
        for (const Part& pt : m_parts)
            if (pt.fileIdx >= 0 && pt.fileIdx < ix.files().size())
                ++votes[int(ix.gameOf(ix.files()[pt.fileIdx]))];
        // Unknown (index 0) is NOT a candidate: it is the absence of an
        // answer, and letting it into the race means a character with two
        // placeable parts and two unplaceable ones ties at 2-2 and comes back
        // Unknown. Start at the first real game and fall back only when no
        // game got a vote at all.
        int best = 1;
        for (int g = 2; g < fox::kGameCount; ++g)
            if (votes[g] > votes[best]) best = g;
        m_view->setSceneGame(votes[best] > 0 ? fox::GameId(best)
                                             : fox::GameId::Unknown);
    }
    m_view->setModel(uploads, textures, skeleton, normalMaps, pbr);
    // AFTER setModel, which clears the previous scene's sockets. Before it
    // they would have been wiped by the very call that installed the geometry
    // they belong to.
    m_view->setConnectPoints(scenePoints);
    // What a capture from this viewport should be called: the subject when the
    // page has one (the character, the weapon), the first part's stem
    // otherwise. Read only when a file dialog opens — see
    // ViewportPanel::suggestedName.
    {
        QString capName = currentSubjectId();
        if (capName.isEmpty() && !m_parts.isEmpty())
            capName = m_parts.first().path.section(QLatin1Char('/'), -1)
                          .section(QLatin1Char('.'), 0, 0);
        m_view->setProperty("foxabCaptureName",
                            capName.isEmpty() ? QStringLiteral("scene")
                                              : capName);
        // …and the file it came from, so {{Game}} / {{Hash}} in the name
        // template resolve for a capture exactly as they do for an export.
        int capIdx = -1;
        for (const auto& p : m_parts)
            if (p.fileIdx >= 0) { capIdx = p.fileIdx; break; }
        m_view->setProperty("foxabCaptureFileIdx", capIdx);
    }
    // The Normal maps switch greys itself out on the Graphics popover when a
    // scene has none, so there is nothing for this path to enable.
    refreshInspector();
    refreshSceneTree();
    refreshInfoPanel();
    // §13, before the seating: the pass is what applyAttachTransforms and
    // setFrame both consume, so it is measured once, here, where every part
    // is loaded and nothing has been folded yet.
    dumpRestAlign();
    applyAttachTransforms({});   // bind-pose seating for attached items
    // setModel resets group visibility — resync from the equipped checkboxes.
    for (int row = 0; row < m_equipped->count() && row < m_parts.size(); ++row)
        m_view->setGroupVisible(
            row, m_equipped->item(row)->checkState() == Qt::Checked);
    // …and only then hide the base body, which is a build decision rather than
    // a user one. Doing this here rather than at the one call site means a
    // camo change (which rebuilds the scene on its own) no longer pops the
    // grey mannequin back out through the clothing.
    if (m_hideBasePart >= 0 && m_hideBasePart < m_parts.size())
        m_view->setGroupVisible(m_hideBasePart, false);
    setStatus(m_parts.isEmpty()
        ? QStringLiteral("Equip parts from the list (double-click or Equip).")
        : QStringLiteral("%1 parts · %2 bones · %3 triangles")
              .arg(m_parts.size()).arg(totalBones).arg(totalTris));
    if (m_hasAnim) {
        m_recenterPending = true;
        setFrame(m_frame);
    }
    // Equipping the first part, or stripping the last one, is what decides
    // whether a clip has anything to record — so the hook is resynced from
    // the same place the scene is built rather than only where clips load.
    syncAnimProvider();

    // §15. The scene has settled: offer it to the undo stack, which pushes a
    // step only if the AUTHORED description actually changed. Hooked here, at
    // the one place every scene change funnels through, rather than at each of
    // the twenty-odd authoring sites — an authoring site that forgot to push
    // is a misclick the user cannot take back, and this cannot forget.
    noteSceneSettled();
}

void CustomizeTab::syncAnimProvider()
{
    if (!m_view) return;
    if (m_parts.isEmpty() || !m_hasAnim || m_anim.frameCount <= 0) {
        m_view->setAnimationFrameProvider({});
        return;
    }
    m_view->setAnimationFrameProvider(
        [this](int frames) { return renderClipFrames(frames); });
}

QVector<QImage> CustomizeTab::renderClipFrames(int frames)
{
    QVector<QImage> out;
    if (m_parts.isEmpty() || !m_hasAnim || m_anim.frameCount <= 0) return out;
    // Capped at the clip's own length: more samples than the clip has steps
    // only duplicates poses and grows the file.
    const int n = qBound(2, frames, qMax(2, m_anim.frameCount));
    const int last = qMax(0, m_anim.frameCount - 1);
    const bool wasPlaying = m_playBtn && m_playBtn->isChecked();
    if (wasPlaying) m_playBtn->setChecked(false);
    const float saved = m_frame;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        // Across last+1 so the GIF's loop point does not hold one pose twice.
        const float f =
            last > 0 ? qMin(float(last),
                            float(i) * float(last + 1) / float(n))
                     : 0.0f;
        setFrame(f);
        out.push_back(m_view->grabFramebuffer());
    }
    setFrame(saved);
    if (wasPlaying) m_playBtn->setChecked(true);
    return out;
}

bool CustomizeTab::exportSceneTo(const QString& glbPath, int onlyPart,
                                 const QVector<glb::GlbAnimation>* anims)
{
    if (m_parts.isEmpty()) return false;
    // An ANIMATED export is the opposite shape from a posed one: the parts go
    // in rigged, with no baked pose, and the motion arrives as curves on their
    // joints. Everything else about the walk — which parts are in the scene,
    // what is hidden, where attachments sit — is identical, which is why this
    // is a flag on the one walk rather than a second copy of it.
    const bool animated = anims && !anims->isEmpty();
    m_scenePartOwner.clear();

    // The palettes come from setFrame, which is the ONE place the scene's
    // posing rules live (host election, fragment borrowing, seated parts held
    // at identity). If a clip is loaded but nothing has posed the scene yet,
    // pose it now rather than rebuilding those rules a second time here — the
    // second copy is exactly what left a variation's hat in bind pose under an
    // animated head.
    if (m_hasAnim && m_framePalettes.isEmpty()) setFrame(m_frame);

    // Attachment matrices for the SEATED parts (a barrel on a receiver). These
    // need the hosts' poses, and hosts are never themselves seated.
    QVector<QVector<animmath::Mat4>> poses(m_parts.size());
    if (m_hasAnim)
        for (int i = 0; i < m_parts.size(); ++i)
            poses[i] = m_framePalettes.value(qMakePair(i, -1));
    QVector<animmath::Mat4> attachMats(m_parts.size());
    for (int i = 0; i < m_parts.size(); ++i)
        if (m_parts[i].attachPart >= 0)
            attachMats[i] = attachMatrixFor(i, m_hasAnim
                                                   ? poses
                                                   : QVector<QVector<animmath::Mat4>>());

    // The scene is walked in the SAME order and with the same skip tests
    // rebuildScene uses — part, then the models its variation attaches — so
    // what lands in the file is what is on screen and nothing else.
    // Submeshes the tree switched off, translated from SCENE ids back to each
    // contributor's own mesh index through the map rebuildScene recorded.
    QHash<QPair<int, int>, QSet<int>> hiddenMeshes;
    for (const int sceneMesh : m_view->hiddenMeshes()) {
        const auto own = m_meshOwner.constFind(sceneMesh);
        if (own == m_meshOwner.constEnd()) continue;
        hiddenMeshes[own->first].insert(own->second);
    }

    QVector<glb::ScenePart> sceneParts;
    sceneParts.reserve(m_parts.size());
    int skippedParts = 0, attachments = 0, hiddenSubmeshes = 0;
    for (int i = 0; i < m_parts.size(); ++i) {
        const Part& part = m_parts[i];
        // A part the user unticked, and the base body the composer hides under
        // clothing, are NOT in the scene. Exporting them anyway put a whole
        // second mannequin inside the character — invisible in the viewport,
        // and the first thing anyone saw on opening the file.
        const bool equipped =
            i < m_equipped->count()
            && m_equipped->item(i)->checkState() == Qt::Checked;
        if (!equipped || i == m_hideBasePart) { ++skippedParts; continue; }
        // One file per part: the same walk, filtered. Everything below — the
        // hidden groups, the submesh tree's unticks, the attachments, the
        // seat transform, the posed palette — applies exactly as it does to
        // the whole-scene export, because it IS the whole-scene export.
        if (onlyPart >= 0 && i != onlyPart) { ++skippedParts; continue; }

        // Same combination rebuildScene makes: the variation's SHOW list wins
        // over its own hiding, and the avatar look's hiding is added on top.
        const QSet<int> hidden =
            (part.fovaHiddenGroups - part.fovaShownGroups) | part.hiddenGroups;

        glb::ScenePart sp;
        sp.model = &part.model;
        // The sockets this part carries, so a hat's seat and a weapon's rail
        // survive into the file. Loaded already — the composer needs them to
        // place attachments.
        if (part.hasFcnp) sp.connectPoints = &part.fcnp.points();
        // Same scope rule as the viewport: when the base body is on screen for
        // its head alone, that is what the export contains too.
        if (i == m_headOnlyPart) sp.subtreeOnly = fox::boneCode("SKL_004_HEAD");
        sp.textures = &part.textures;
        sp.normalMaps = &part.normalMaps;
        sp.hiddenGroups = hidden;
        sp.hiddenMeshes = hiddenMeshes.value(qMakePair(i, -1));
        hiddenSubmeshes += sp.hiddenMeshes.size();
        // Empty when the viewport has PBR off, and an empty set is the same as
        // no set — the export then falls back to the raw base maps, which is
        // what it always did.
        if (!part.pbr.isEmpty()) sp.pbr = &part.pbr;
        if (part.attachPart >= 0) {
            sp.rigid = &attachMats[i];   // seated exactly as the viewport shows
        } else if (!animated) {
            sp.pose = m_hasAnim ? &poses[i] : nullptr;
            // THE NO-CLIP CASE, which used to lose this outright. A part whose
            // bind pose is authored against a bone another part places
            // elsewhere needs `d` to sit where the viewport draws it; with a
            // clip the alignment pass has already folded it into the palette
            // (so it must NOT be added again here), and without one there is
            // no palette and `rigid` cannot carry it without stripping the
            // rig. The exporter takes it as a wrapper node instead.
            if (!m_hasAnim) {
                const QVector3D d = restAlignmentFor(i).d;
                sp.restOffset[0] = d.x();
                sp.restOffset[1] = d.y();
                sp.restOffset[2] = d.z();
                // Said out loud, because a silent geometric correction is one
                // nobody can check: the number here is the same one the
                // viewport applies as a group transform at bind, so the two
                // can be compared without instrumenting either.
                if (!d.isNull())
                    qInfo("customize: part %d exports with a rest offset of "
                          "(%.4f, %.4f, %.4f) — its bind pose is authored "
                          "against a bone another part places elsewhere",
                          i, d.x(), d.y(), d.z());
            }
        }
        sceneParts.append(sp);
        // Which of OUR parts this scene part came from, so an animated export
        // can answer the exporter's per-part pose question. Appended in step
        // with sceneParts and never anywhere else: a mapping that drifts by
        // one poses every part with its neighbour's skeleton.
        m_scenePartOwner.append(qMakePair(i, -1));

        // …and what the applied variation BROUGHT WITH IT. 478 of the 1,895
        // shipped .fv2 tables attach a model — a hat, a bag, a hair mesh — and
        // none of them reached the file before this. They are separate models
        // with their own bones, textures and materials, so they go in as parts
        // of their own, exactly as the viewport uploads them.
        for (int ai = 0; ai < part.fovaAttached.size(); ++ai) {
            if (attachmentAlreadyFitted(part, ai)) continue;
            const modelload::LoadedModel& att = part.fovaAttached[ai];
            glb::ScenePart ap;
            ap.model = &att.model;
            ap.textures = &att.textures;
            ap.normalMaps = &att.normalMaps;
            if (!att.pbr.isEmpty()) ap.pbr = &att.pbr;
            ap.hiddenMeshes = hiddenMeshes.value(qMakePair(i, ai));
            hiddenSubmeshes += ap.hiddenMeshes.size();
            // It rides its wearer's seat transform when the wearer is seated,
            // AND it takes its own animated palette either way. Both, not one:
            // setFrame gives a SEATED PART an identity palette but gives that
            // part's attachments a real posed one (the guard there tests the
            // part, not the attachment), so the viewport draws a seated
            // wearer's hat as seat-transform composed with animated skin. The
            // export gave it seat-transform composed with BIND, which is a
            // visible disagreement the moment a clip drives the hat's bones.
            if (part.attachPart >= 0) ap.rigid = &attachMats[i];
            if (m_hasAnim && !animated) {
                const auto it = m_framePalettes.constFind(qMakePair(i, ai));
                if (it != m_framePalettes.constEnd()) ap.pose = &*it;
            }
            sceneParts.append(ap);
            // An attachment has a palette of its OWN — setFrame poses it even
            // when its wearer is seated — so it is named here by both halves
            // of its key. Calling it "not ours" and answering the exporter
            // with an empty vector was worse than it sounds: the writer reads
            // a wrong-sized pose as a clip aimed at another skeleton and drops
            // the WHOLE CLIP, so one attached hat silently emptied the
            // animation list of the entire file.
            m_scenePartOwner.append(qMakePair(i, ai));
            ++attachments;
        }
    }
    if (sceneParts.isEmpty()) {
        setStatus(QStringLiteral("Nothing visible to export."));
        qWarning("customize: export skipped — no visible parts");
        return false;
    }

    QString err;
    fox::ExportOptions eo = fox::loadExportOptions();
    // "No rig" and "with animation" cannot both be honoured — glTF animates
    // node transforms, and a file with no joint nodes has nothing to animate.
    // Overridden here WITH A LINE IN THE LOG, exactly as the Models tab does,
    // rather than writing a static scene and reporting clips that are not in
    // it.
    if (animated && !eo.skeleton) {
        qInfo("customize: animated export overrides the \"no skeleton\" "
              "setting — an animation needs joints to drive");
        eo.skeleton = true;
    }
    qInfo("export: %s", qUtf8Printable(eo.describe()));
    int clipsWritten = 0;
    const bool ok = glb::exportGlbScene(sceneParts, glbPath, &err,
                                        fox::sceneOptionsFrom(eo), anims,
                                        &clipsWritten);
    m_clipsWritten = clipsWritten;
    if (ok) {
        setStatus(QStringLiteral("Exported %1").arg(glbPath));
        fox::ExportNotifier::instance().notify(
            QStringLiteral("Exported %1 %2%3")
                .arg(onlyPart >= 0 ? QStringLiteral("part") : QStringLiteral("scene"),
                     QFileInfo(glbPath).fileName(),
                     fox::ExportNotifier::glbOptionsLine(fox::loadExportOptions())),
            QFileInfo(glbPath).absolutePath());
        // "skipped" means something different in the two callers, so say
        // which: for the whole-scene export it is the parts that are not in
        // the scene, and for a per-part export most of the count is the
        // filter doing its job.
        qInfo("customize: exported %s %s (%d part(s) + %d attachment(s), "
              "%d %s and %d hidden submesh(es) skipped%s)",
              onlyPart >= 0 ? "part" : "scene", qUtf8Printable(glbPath),
              int(sceneParts.size()) - attachments, attachments, skippedParts,
              onlyPart >= 0 ? "other part(s)" : "hidden part(s)",
              hiddenSubmeshes,
              animated ? ", animated" : (m_hasAnim ? ", posed" : ""));
    } else {
        setStatus(QStringLiteral("Export failed: %1").arg(err));
        qWarning("customize: scene export failed: %s", qUtf8Printable(err));
    }
    return ok;
}

// ── ONE builder for the slot menu ───────────────────────────────────────────
// See the declaration in CustomizeTab.h for why this is one function and not
// two menus.
void CustomizeTab::addSlotMenuActions(QMenu* menu, int partIdx,
                                      const QString& slotLabel)
{
    const bool have = partIdx >= 0 && partIdx < m_parts.size();

    // The header. It names the slot when there is one and the model either
    // way, so the menu says what it is about to act on rather than leaving it
    // to be inferred from which row was under the cursor.
    QString header = slotLabel;
    if (have) {
        const QString file = m_parts[partIdx].path.section(QLatin1Char('/'), -1);
        header = slotLabel.isEmpty()
                     ? file
                     : QStringLiteral("%1 — %2").arg(slotLabel, file);
    } else if (!slotLabel.isEmpty()) {
        header = QStringLiteral("%1 — empty").arg(slotLabel);
    }
    if (!header.isEmpty()) {
        QAction* head = menu->addAction(header);
        head->setEnabled(false);
        menu->addSeparator();
    }
    if (!have) return;

    const QString stem = partExportStem(partIdx);
    // What distinguishes this from the file-level "Export as .glb" two rows
    // below it, said in the label rather than left to be discovered. That one
    // writes the model file on its own; this one writes what is in the scene —
    // posed, with the variation's textures, with whatever the submesh tree
    // switched off. Two rows both promising a .glb and neither saying which is
    // which is how a menu teaches nobody anything.
    QStringList detail;
    if (m_hasAnim) detail << QStringLiteral("posed, current frame");
    if (!m_parts[partIdx].fovaName.isEmpty())
        detail << QStringLiteral("variation '%1'")
                      .arg(m_parts[partIdx].fovaName);
    if (detail.isEmpty()) detail << QStringLiteral("as shown");

    QAction* one = exportactions::addExportPair(
        menu, this,
        MenuText::exportSubject(QStringLiteral("this part"),
                                detail.join(QStringLiteral(", "))),
        [this, partIdx, stem](bool lastFolder) {
            if (partIdx < 0 || partIdx >= m_parts.size()) return;
            const QString dir = Config::exportDir();
            QString out;
            if (lastFolder && !dir.isEmpty()) {
                // Uniqued rather than silently overwritten — the silent twin
                // has no dialog, so the one warning a save dialog would have
                // given is not there to give.
                QString name = stem;
                for (int n = 2;
                     QFile::exists(QDir(dir).filePath(name
                                                      + QStringLiteral(".glb")));
                     ++n)
                    name = QStringLiteral("%1_%2").arg(stem).arg(n);
                out = QDir(dir).filePath(name + QStringLiteral(".glb"));
            } else {
                out = QFileDialog::getSaveFileName(
                    this, QStringLiteral("Export part"),
                    QDir(dir).filePath(stem + QStringLiteral(".glb")),
                    QStringLiteral("glTF binary (*.glb)"));
                if (out.isEmpty()) return;
                Config::setExportDir(QFileInfo(out).absolutePath());
            }
            exportSceneTo(out, partIdx);
        });
    one->setToolTip(QStringLiteral(
        "This part on its own, out of the scene — with the textures it is "
        "wearing right now, including the applied variation, and posed at the "
        "frame on screen when a clip is loaded.\n\n\"Export as .glb\" below "
        "is the other thing: the model FILE, on its own terms, with none of "
        "this build applied to it."));

    menu->addSeparator();
    exportactions::addFileActions(menu, m_parts[partIdx].fileIdx, this);
}

// The export stem for one part, with its variation in it.
QString CustomizeTab::partExportStem(int partIdx) const
{
    if (partIdx < 0 || partIdx >= m_parts.size())
        return QStringLiteral("part");
    const Part& p = m_parts[partIdx];
    QString stem = p.path.section(QLatin1Char('/'), -1)
                       .section(QLatin1Char('.'), 0, 0);
    if (stem.isEmpty()) stem = QStringLiteral("part%1").arg(partIdx);
    stem = fox::templatedStem(stem, p.fileIdx);
    // AFTER the user's template, not before: the template is about naming the
    // asset and this is about telling two exports of that asset apart, so
    // appending it last is what keeps both true. A variation name is already
    // a bare identifier out of a file name, so there is nothing to sanitise.
    if (!p.fovaName.isEmpty())
        stem += QLatin1Char('_') + p.fovaName;
    return stem;
}

QVector<QPair<QString, QString>> CustomizeTab::variationRows() const
{
    QVector<QPair<QString, QString>> rows;
    if (!m_weaponCamo) return rows;
    for (int i = 0; i < m_weaponCamo->count(); ++i)
        rows.append({m_weaponCamo->itemText(i),
                     m_weaponCamo->itemData(i).toString()});
    return rows;
}

// One file per variation. The list is walked by SELECTING each row, because
// selecting is what applies it — every rule about which parts a variation
// touches, what it hides and what it attaches lives in onWeaponCamoChanged,
// and a second path through that would be a second answer to it.
int CustomizeTab::exportVariationsTo(const QString& dir, QString* error)
{
    const auto rows = variationRows();
    // ONE ROW IS NOT A LIST. Every build has a "— default —" row, including a
    // free-form scene with no variation data at all, so "is the list empty" is
    // the wrong test and it let this walk write one file and call it a
    // per-variation export — the ordinary export under a longer name.
    if (rows.size() < 2) {
        if (error)
            *error = QStringLiteral(
                "This build offers no variation to choose — the box holds "
                "only the default row. The list is filled for a weapon or a "
                "character built from a subject whose items ship .fv2 tables.");
        return 0;
    }
    if (m_parts.isEmpty()) {
        if (error) *error = QStringLiteral("Nothing is built.");
        return 0;
    }
    if (!QDir().mkpath(dir)) {
        if (error) *error = QStringLiteral("Cannot write into %1").arg(dir);
        return 0;
    }

    // Put the user's own selection back afterwards, whatever happens. Walking
    // a combo to produce files must not also be a way to change what is on
    // screen.
    const int restore = m_weaponCamo->currentIndex();
    int written = 0, failed = 0;
    QSet<QString> used;
    for (int i = 0; i < rows.size(); ++i) {
        m_weaponCamo->setCurrentIndex(i);
        // currentIndexChanged fires synchronously and applies the variation,
        // so by here the scene is wearing row i.
        int sceneIdx = -1;
        for (const auto& p : m_parts)
            if (p.fileIdx >= 0) { sceneIdx = p.fileIdx; break; }
        QString stem = fox::templatedStem(exportSubjectNoun(), sceneIdx);
        // The row's DATA is the variation's own name; its label is display
        // text ("— default —") and would make a poor file name.
        const QString tag = rows[i].second.isEmpty()
                                ? QStringLiteral("default")
                                : rows[i].second;
        stem += QLatin1Char('_') + tag;
        QString name = stem;
        for (int n = 2;
             used.contains(name)
             || QFile::exists(QDir(dir).filePath(name + QStringLiteral(".glb")));
             ++n)
            name = QStringLiteral("%1_%2").arg(stem).arg(n);
        used.insert(name);
        if (exportSceneTo(QDir(dir).filePath(name + QStringLiteral(".glb"))))
            ++written;
        else
            ++failed;
    }
    m_weaponCamo->setCurrentIndex(restore);

    if (failed > 0 && error)
        *error = QStringLiteral("%1 of %2 variation(s) could not be written")
                     .arg(failed).arg(rows.size());
    // The rows are NAMED in the log, not just counted. "1 of 1" told me
    // nothing about whether the list was a real one, which is how the
    // one-row case got as far as writing a file.
    QStringList names;
    for (const auto& r : rows)
        names << (r.second.isEmpty() ? QStringLiteral("(default)") : r.second);
    qInfo("customize: exported %d of %lld variation(s) into %s — %s", written,
          qint64(rows.size()), qUtf8Printable(dir),
          qUtf8Printable(names.join(QStringLiteral(", "))));
    return written;
}

void CustomizeTab::exportVariations()
{
    const auto rows = variationRows();
    if (rows.size() < 2) {
        setStatus(QStringLiteral(
            "This build offers no variation to choose — the box holds only "
            "the default row."));
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Export one file per variation (%1) into…")
            .arg(rows.size()),
        Config::exportDir());
    if (dir.isEmpty()) return;
    Config::setExportDir(dir);
    QString err;
    const int n = exportVariationsTo(dir, &err);
    setStatus(err.isEmpty()
                  ? QStringLiteral("Exported %1 variation(s) into %2")
                        .arg(n).arg(dir)
                  : err);
}

// ── Harness ────────────────────────────────────────────────────────────────
QString CustomizeTab::exportSlotTo(const QString& slot, const QString& glbPath)
{
    if (m_parts.isEmpty()) return QStringLiteral("nothing is built");
    // Matched against the slot ID, case-insensitively — the same rule
    // --character's slot fields follow, so one spelling works in both.
    for (const WeaponSlotRow& row : m_weaponRows) {
        if (row.slot.compare(slot, Qt::CaseInsensitive) != 0) continue;
        if (row.partIdx < 0) return QStringLiteral("slot '%1' is empty").arg(slot);
        return exportSceneTo(glbPath, row.partIdx)
                   ? QString()
                   : QStringLiteral("export of slot '%1' failed").arg(slot);
    }
    QStringList known;
    for (const WeaponSlotRow& row : m_weaponRows) known << row.slot;
    return QStringLiteral("no slot '%1' — this build has: %2")
        .arg(slot, known.isEmpty() ? QStringLiteral("(none)")
                                   : known.join(QStringLiteral(", ")));
}

QString CustomizeTab::slotMenuDump(const QString& slot)
{
    int partIdx = -1;
    QString label = slot;
    bool found = false;
    for (const WeaponSlotRow& row : m_weaponRows) {
        if (row.slot.compare(slot, Qt::CaseInsensitive) != 0) continue;
        partIdx = row.partIdx;
        found = true;
        break;
    }
    if (!found) {
        // Not a slot id — try it as an equipped row, which is the menu's other
        // entry point and the one the free-form category has.
        bool ok = false;
        const int row = slot.toInt(&ok);
        if (ok && row >= 0 && row < m_parts.size()) {
            partIdx = row;
            label.clear();
            found = true;
        }
    }
    if (!found)
        return QStringLiteral("no slot or equipped row '%1'").arg(slot);

    QMenu menu;
    addSlotMenuActions(&menu, partIdx, label);
    QStringList out;
    for (QAction* a : menu.actions()) {
        if (a->isSeparator()) { out << QStringLiteral("---"); continue; }
        out << (a->text()
                + (a->isEnabled() ? QString()
                                  : QStringLiteral("  (disabled)")));
    }
    return out.join(QStringLiteral(" | "));
}

void CustomizeTab::exportPartsSeparately()
{
    if (m_parts.isEmpty()) return;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Export each part into…"), Config::exportDir());
    if (dir.isEmpty()) return;
    Config::setExportDir(dir);
    exportPartsTo(dir);
}

int CustomizeTab::exportPartsTo(const QString& dir)
{
    if (m_parts.isEmpty() || dir.isEmpty()) {
        qWarning("customize: per-part export asked for with %s",
                 dir.isEmpty() ? "no folder" : "nothing in the scene");
        return 0;
    }
    if (!QDir().mkpath(dir)) {
        setStatus(QStringLiteral("Cannot write into %1").arg(dir));
        qWarning("customize: per-part export — cannot create %s",
                 qUtf8Printable(dir));
        return 0;
    }
    // Names come from the part's own model stem, and a repeat gets a suffix
    // rather than silently overwriting: a character can wear two parts out of
    // one family, and "the second one won" is the kind of loss nobody notices
    // until the file is somewhere else.
    QSet<QString> used;
    int written = 0, skipped = 0, failed = 0;
    QString firstError;
    for (int i = 0; i < m_parts.size(); ++i) {
        const bool equipped = i < m_equipped->count()
            && m_equipped->item(i)->checkState() == Qt::Checked;
        if (!equipped || i == m_hideBasePart) { ++skipped; continue; }
        QString stem = m_parts[i].path.section(QLatin1Char('/'), -1)
                           .section(QLatin1Char('.'), 0, 0);
        if (stem.isEmpty()) stem = QStringLiteral("part%1").arg(i);
        // The user's file-name template, applied per part. The uniquing below
        // then runs on the TEMPLATED name, so a template that collapses two
        // parts to one name still produces two files rather than one.
        stem = fox::templatedStem(stem, m_parts[i].fileIdx);
        // Unique within this run AND on disk. Two parts out of one family
        // collide with each other; a previous export collides with both, and
        // silently replacing a file in a folder the user picked is a loss
        // nobody notices until the file is somewhere else.
        QString name = stem;
        for (int n = 2;
             used.contains(name)
             || QFile::exists(QDir(dir).filePath(name + QStringLiteral(".glb")));
             ++n)
            name = QStringLiteral("%1_%2").arg(stem).arg(n);
        used.insert(name);
        if (exportSceneTo(QDir(dir).filePath(name + QStringLiteral(".glb")), i)) {
            ++written;
        } else {
            // A FAILURE, not a skip: the part was equipped and visible and the
            // write did not happen. Folding the two together reported a
            // read-only folder as "unticked, hidden or nothing visible", which
            // is false on all three counts. exportSceneTo has already put the
            // reason in m_info; keep the first one for the summary.
            ++failed;
            if (firstError.isEmpty()) firstError = m_info->text();
        }
    }
    QString msg = QStringLiteral("Exported %1 part(s) into %2")
                      .arg(written)
                      .arg(dir);
    // SAID, not left to be discovered when the files open in Blender with no
    // armature. The rule is right — an export writes what is visible — but a
    // per-part export is the one people reach for to get riggable pieces, and
    // a clip left loaded silently turns every one of them into a frozen
    // snapshot.
    if (written && m_hasAnim)
        msg += QStringLiteral(
                   " · posed at frame %1, vertices baked, no skeleton — "
                   "unload the clip for rigged parts")
                   .arg(qRound(m_frame));
    if (skipped)
        msg += QStringLiteral(" · %1 not in the scene (unticked or hidden)")
                   .arg(skipped);
    if (failed)
        msg += QStringLiteral(" · %1 FAILED — %2").arg(failed).arg(firstError);
    setStatus(msg);
    // AFTER every append. Reporting `msg` while the failure count was still
    // being added to it made the shared status line say "Exported 3 part(s)"
    // with a working Show-in-folder while the tab's own label beside it said
    // "· 2 FAILED" — the one report that exists to make every path say the
    // same thing saying something different, and less true.
    fox::ExportNotifier::instance().notify(msg, dir);
    qInfo("customize: %s", qUtf8Printable(msg));
    return written;
}

QVector<QPair<int, int>> CustomizeTab::clipsMatching(const QString& spec) const
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
        for (int i = 0; i < clips.size(); ++i) out.append({archive, i});
        return out;
    }
    QSet<int> seen;
    for (const QString& fRaw : s.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString f = fRaw.trimmed();
        if (f.isEmpty()) continue;
        bool numeric = false;
        const int idx = f.toInt(&numeric);
        if (numeric) {
            if (idx >= 0 && idx < clips.size() && !seen.contains(idx)) {
                seen.insert(idx);
                out.append({archive, idx});
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

int CustomizeTab::exportSceneAnimatedTo(const QString& glbPath,
                                        const QVector<QPair<int, int>>& clipSel,
                                        QString* errorOut)
{
    const auto fail = [&](const QString& why) {
        qWarning("customize: animated export failed: %s", qUtf8Printable(why));
        if (errorOut) *errorOut = why;
        setStatus(QStringLiteral("Export failed: %1").arg(why));
        return 0;
    };
    if (m_parts.isEmpty()) return fail(QStringLiteral("nothing in the scene"));
    if (clipSel.isEmpty()) return fail(QStringLiteral("no clips selected"));

    // Archives opened here, like the Models tab's own animated export: a
    // selection made in the Animations list can span archives.
    QHash<int, fox::MtarFile> opened;
    QVector<fox::GaniAnim> decoded;
    QVector<QString> names;
    int failedDecode = 0;
    for (const QPair<int, int>& sel : clipSel) {
        fox::MtarFile* mt = nullptr;
        auto it = opened.find(sel.first);
        if (it != opened.end()) {
            mt = it->clips().isEmpty() ? nullptr : &*it;
        } else {
            const fox::ArchiveIndex& ix = fox::ArchiveIndex::instance();
            fox::MtarFile m;
            if (sel.first >= 0 && sel.first < ix.files().size()) {
                const QByteArray raw = ix.readFile(ix.files()[sel.first]);
                if (!raw.isEmpty() && m.parse(raw)) {
                    it = opened.insert(sel.first, std::move(m));
                    mt = &*it;
                }
            }
            if (!mt) opened.insert(sel.first, fox::MtarFile());
        }
        if (!mt || sel.second < 0 || sel.second >= mt->clips().size()) {
            ++failedDecode;
            continue;
        }
        fox::GaniAnim a = mt->decodeClip(sel.second);
        if (!a.valid() || a.frameCount <= 0) { ++failedDecode; continue; }
        QString nm = mt->clips()[sel.second].name.section(QLatin1Char('/'), -1);
        const int dot = nm.lastIndexOf(QLatin1Char('.'));
        if (dot > 0) nm.truncate(dot);
        if (nm.isEmpty()) nm = QStringLiteral("clip%1").arg(sel.second);
        decoded.append(std::move(a));
        names.append(nm);
    }
    if (decoded.isEmpty())
        return fail(QStringLiteral("none of the %1 selected clip(s) decoded")
                        .arg(clipSel.size()));

    // The scene is about to be posed frame by frame through setFrame(), which
    // is the viewport's own state. Everything it touches is put back at the
    // end — an export must not leave the user looking at frame 137 of a clip
    // they did not choose.
    const fox::GaniAnim savedAnim = m_anim;
    const bool savedHasAnim = m_hasAnim;
    const float savedFrame = m_frame;
    m_exportClip = -1;
    m_exportSample = -1;

    // Unique by SEARCH, not by counting: "<stem>_2" is a name clips really
    // have, so appending _2 to the second "wlk_lp" could land on a clip
    // already called "wlk_lp_2" — and the writer merges same-named clips on
    // purpose, so the two would have become one animation with two channels
    // on the same node, which is invalid glTF.
    QSet<QString> used;
    QVector<glb::GlbAnimation> clips;
    clips.reserve(decoded.size());
    for (int k = 0; k < decoded.size(); ++k) {
        QString name = names[k];
        for (int n = 2; used.contains(name); ++n)
            name = QStringLiteral("%1_%2").arg(names[k]).arg(n);
        used.insert(name);
        glb::GlbAnimation ga;
        ga.name = name;
        ga.fps = 30.0f;
        ga.sampleCount = decoded[k].frameCount;
        ga.pose = [this, k, &decoded](int part, int sample,
                                      QVector<animmath::Mat4>* out) {
            // Pose the WHOLE SCENE once per frame. setFrame is the only place
            // the scene's posing rules live — host election, fragment
            // borrowing, seated parts — and a second copy of them here is
            // exactly how an export comes to disagree with the viewport.
            if (m_exportClip != k) {
                m_anim = decoded[k];
                m_hasAnim = true;
                m_exportClip = k;
                m_exportSample = -1;
            }
            if (m_exportSample != sample) {
                setFrame(static_cast<float>(sample));
                m_exportSample = sample;
            }
            const QPair<int, int> owner =
                part >= 0 && part < m_scenePartOwner.size()
                    ? m_scenePartOwner[part]
                    : qMakePair(-1, -1);
            const fox::FmdlFile* model = nullptr;
            if (owner.first >= 0 && owner.first < m_parts.size()) {
                const Part& op = m_parts[owner.first];
                if (owner.second < 0)
                    model = &op.model;
                else if (owner.second < op.fovaAttached.size())
                    model = &op.fovaAttached[owner.second].model;
            }
            if (!model) { out->clear(); return; }
            const QVector<animmath::Mat4> pal = m_framePalettes.value(owner);
            const auto& bones = model->bones();
            out->resize(bones.size());
            for (int b = 0; b < bones.size(); ++b) {
                // The palette holds SKIN matrices — translate(-bindWorld) then
                // the posed world frame — so putting the bind translation back
                // recovers the world frame glTF wants, exactly and without a
                // second pass through the solver. A part the clip does not
                // drive has an identity palette, and identity here gives the
                // bind pose, which is what such a part should export as.
                const animmath::Mat4 bindT = animmath::Mat4::translation(
                    animmath::Vec3(bones[b].worldPos[0], bones[b].worldPos[1],
                                   bones[b].worldPos[2]));
                (*out)[b] = b < pal.size() ? animmath::mul(bindT, pal[b]) : bindT;
            }
        };
        clips.append(ga);
    }

    // Seat the scene at the FIRST FRAME of the first clip before the walk.
    // Attachments export static — their seat is baked into their vertices —
    // and exportSceneTo computes those seats once, from whatever pose the
    // scene is in when it starts. Left at the user's scrub frame, a seated
    // item hung at frame 50's offset for the whole clip and was visibly
    // detached at t = 0. Frame 0 of the clip being written is the one frame
    // where the file and the animation agree.
    m_anim = decoded[0];
    m_hasAnim = true;
    m_exportClip = 0;
    m_exportSample = 0;
    setFrame(0.0f);

    m_clipsWritten = 0;
    const bool ok = exportSceneTo(glbPath, -1, &clips);

    m_anim = savedAnim;
    m_hasAnim = savedHasAnim;
    m_frame = savedFrame;
    m_exportClip = -1;
    m_exportSample = -1;
    if (m_hasAnim) setFrame(savedFrame);

    if (!ok) return fail(QStringLiteral("the scene writer refused it"));
    // What the FILE holds, not what was asked for. A clip whose parts all
    // export static is dropped by the writer with a warning, and reporting the
    // request back would have claimed animations the file does not contain.
    const int n = m_clipsWritten;
    QString msg = QStringLiteral("Exported %1 — %2 clip(s)").arg(glbPath).arg(n);
    if (n < clips.size())
        msg += QStringLiteral(" · %1 drove nothing in the scene and were not "
                              "written")
                   .arg(clips.size() - n);
    if (failedDecode)
        msg += QStringLiteral(" · %1 would not decode").arg(failedDecode);
    setStatus(msg);
    qInfo("customize: %s", qUtf8Printable(msg));
    return n;
}

void CustomizeTab::setStatus(const QString& text)
{
    if (m_info) m_info->setText(text);
    fox::StatusLine::instance().report(this, text);
}

// Tell the ANIMATIONS panel what is loaded and what is playing — the same
// contract the Models tab has. Without it the panel opens scoped to nothing
// and its highlight never follows the transport.
void CustomizeTab::syncAnimPanel()
{
    if (!m_animPanel) return;
    // The scene's FIRST indexed part is what the scope is resolved from: a
    // composed character has no single file, and its body is the piece whose
    // rig the clips have to drive.
    QString modelPath;
    const fox::ArchiveIndex& index = fox::ArchiveIndex::instance();
    for (const auto& p : m_parts)
        if (p.fileIdx >= 0 && p.fileIdx < index.files().size()) {
            modelPath = index.files()[p.fileIdx].path;
            break;
        }
    m_animPanel->setCurrentModel(modelPath);
    const QVariant av = m_mtarCombo ? m_mtarCombo->currentPayload() : QVariant();
    const QVariant cv = m_clipCombo ? m_clipCombo->currentPayload() : QVariant();
    const bool playing = m_hasAnim && av.isValid() && cv.isValid();
    m_animPanel->showCurrent(playing ? av.toInt() : -1,
                             playing ? cv.toInt() : -1);
}

// "Export <subject> to last folder" — §12's silent twin, same writer.
void CustomizeTab::exportSceneToLast()
{
    if (m_parts.isEmpty()) return;
    const QString dir = Config::exportDir();
    if (dir.isEmpty()) return;
    int sceneIdx = -1;
    for (const auto& p : m_parts)
        if (p.fileIdx >= 0) { sceneIdx = p.fileIdx; break; }
    exportSceneTo(QDir(dir).filePath(
        fox::templatedStem(QStringLiteral("scene"), sceneIdx)
        + QStringLiteral(".glb")));
}

void CustomizeTab::exportScene()
{
    if (m_parts.isEmpty()) return;
    // The scene is composed of several files, so {{Game}} and {{Hash}} come
    // from the first part that has an index entry — usually the body, always
    // the same game as the rest, since the scene cannot mix games.
    int sceneIdx = -1;
    for (const auto& p : m_parts)
        if (p.fileIdx >= 0) { sceneIdx = p.fileIdx; break; }
    const QString sceneStem =
        fox::templatedStem(QStringLiteral("scene"), sceneIdx);
    const QString out = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export composed scene"),
        QDir(Config::exportDir()).filePath(sceneStem + QStringLiteral(".glb")),
        QStringLiteral("glTF binary (*.glb)"));
    if (out.isEmpty()) return;
    Config::setExportDir(QFileInfo(out).absolutePath());
    exportSceneTo(out);
}

// What this tab is building, as a NOUN — §6's exportNoun, and the whole of
// "the export menu needs to be context appropriate".
//
// The category combo already knows: someone in Character is assembling a
// character and someone in Weapon is assembling a weapon, and the menu saying
// "Export scene" to both of them is the tool refusing to use the word the user
// is already using. Free-form Parts really is a scene — several unrelated
// models in one file — so that one keeps the word honestly.
QString CustomizeTab::exportSubjectNoun() const
{
    switch (m_category ? m_category->currentIndex() : 0) {
        case 1: return QStringLiteral("weapon");
        case 2: return QStringLiteral("character");
        case 3: return QStringLiteral("vehicle");
        case 4: return QStringLiteral("character");
        default: return QStringLiteral("scene");
    }
}

void CustomizeTab::populateExportMenu(QMenu* menu)
{
    const QString subject = exportSubjectNoun();
    // THE PAIR, through the shared builder — one that asks and one that
    // writes into the last folder and says which. exportSceneToLast() has
    // existed for a long time and was reachable ONLY from the viewport's part
    // menu, so the menu that is meant to be the one place you export from
    // could not do the quickest export there is.
    QAction* scene = exportactions::addExportPair(
        menu, this,
        MenuText::exportSubject(
            subject,
            m_hasAnim ? QStringLiteral("posed, current frame")
                      : QStringLiteral("%1 part(s)").arg(m_parts.size())),
        [this](bool lastFolder) {
            if (lastFolder) exportSceneToLast();
            else exportScene();
        },
        !m_parts.isEmpty());
    Hotkeys::Role::set(scene, Hotkeys::Role::exportSelection());
    // ── Export PARTS ────────────────────────────────────────────────────
    // A "part" on THIS tab is a source model in the assembly — a head, a body,
    // a piece of gear — not a submesh, which is what it means on the Models
    // tab. So the entry is the existing per-part export, named the way this
    // tab's own vocabulary names it, rather than a second implementation of
    // something that already works.
    {
        m_partsExportAction = menu->addAction(
            MenuText::prompts(MenuText::exportSubject(
                MenuText::plural(QStringLiteral("part"), m_parts.size()),
                QStringLiteral("one file each"))),
            this, &CustomizeTab::exportPartsSeparately);
        m_partsExportAction->setEnabled(!m_parts.isEmpty());
    }
    QAction* animAct = menu->addAction(
        MenuText::prompts(MenuText::exportSubject(
            QStringLiteral("animated %1").arg(subject),
            QStringLiteral("current clip"))),
        this,
        [this] {
            const QVector<QPair<int, int>> sel = clipsMatching(QString());
            if (sel.isEmpty()) {
                setStatus(QStringLiteral(
                    "Pick a clip in the animation bar first — an animated "
                    "export needs one."));
                return;
            }
            const QString out = QFileDialog::getSaveFileName(
                this, QStringLiteral("Export animated scene"),
                QDir(Config::exportDir())
                    .filePath(fox::templatedStem(QStringLiteral("scene"), -1)
                              + QStringLiteral(".glb")),
                QStringLiteral("glTF binary (*.glb)"));
            if (out.isEmpty()) return;
            Config::setExportDir(QFileInfo(out).absolutePath());
            exportSceneAnimatedTo(out, sel);
        });
    animAct->setEnabled(!m_parts.isEmpty() && m_hasAnim);
    Hotkeys::Role::set(animAct, Hotkeys::Role::exportAnimations());
    // (The second "export each part" entry that used to live here is gone: it
    // and the subject-named one above ran the same slot, so the menu offered
    // one action twice under two names — §6's whole point is that there is one
    // place to export from, not two labels for one export.)
    QAction* each = nullptr;
    Q_UNUSED(each);
    m_partsExportAction->setToolTip(QStringLiteral(
        "One file per part, into a folder you choose. The scene export puts "
        "them all in one .glb; this is for the case where each piece has to "
        "arrive separately.\n\nWith a clip loaded each part is written POSED "
        "at the frame on screen, with its vertices baked and no skeleton — "
        "same rule as the scene export, which writes what is visible. Stop "
        "playback and pick \"— none —\" in the archive box to get the parts "
        "rigged instead."));
    // ── one file per variation ───────────────────────────────────────────
    // The variation is already in every export — it is the textures the parts
    // are wearing — but only one at a time, and the file name did not say
    // which. This walks the list.
    {
        const auto rows = variationRows();
        const bool haveChoice = rows.size() >= 2;
        QAction* every = menu->addAction(
            haveChoice
                ? QStringLiteral("Export one file per variation (%1)…")
                      .arg(rows.size())
                : QStringLiteral("Export one file per variation…"),
            this, &CustomizeTab::exportVariations);
        every->setEnabled(!m_parts.isEmpty() && haveChoice);
        every->setToolTip(
            !haveChoice
                ? QStringLiteral(
                      "This build offers no variation to choose — the box "
                      "holds only the default row. It is filled for a weapon "
                      "or a character whose items ship .fv2 tables.")
                : QStringLiteral(
                      "Applies each row of the Camo / Variation box in turn "
                      "and writes a file for each, named after it. Your own "
                      "selection is put back afterwards."));
    }

    fox::addViewportCaptureActions(menu, this, m_view, !m_parts.isEmpty());
    // The selected row's own menu — the SHARED slot builder, so the Export
    // menu offers exactly what right-clicking that row offers. It used to add
    // the file actions alone, which is why the menu could export the file
    // behind a part and never the part.
    const int row = m_equipped->currentRow();
    if (row >= 0 && row < m_parts.size()) {
        menu->addSeparator();
        addSlotMenuActions(menu, row, QString());
    }
}

bool CustomizeTab::attachPartTo(int itemIdx, int hostIdx, const QString& cnpName)
{
    if (itemIdx < 0 || itemIdx >= m_parts.size() || hostIdx < 0
        || hostIdx >= m_parts.size() || itemIdx == hostIdx)
        return false;
    if (!m_parts[hostIdx].hasFcnp || !m_parts[hostIdx].fcnp.find(cnpName))
        return false;
    // Chains ARE allowed — a real weapon is one (suppressor on muzzle on
    // barrel on receiver) — but a cycle would spin attachMatrixFor forever, so
    // walk up from the proposed host and refuse if this part is already above.
    for (int up = hostIdx, guard = 0; up >= 0 && guard < 16; ++guard) {
        if (up == itemIdx) return false;
        up = m_parts[up].attachPart;
    }
    m_parts[itemIdx].attachPart = hostIdx;
    m_parts[itemIdx].attachCnp = cnpName;
    if (m_hasAnim) setFrame(m_frame);
    else applyAttachTransforms({});
    return true;
}

animmath::Mat4 CustomizeTab::attachMatrixFor(
    int i, const QVector<QVector<animmath::Mat4>>& poses) const
{
    using animmath::Mat4;
    using animmath::Vec3;
    Mat4 out;   // identity
    // Walk UP the chain rather than recursing: a weapon is a chain (suppressor
    // on muzzle on barrel on receiver), and an iterative walk with a hard
    // bound cannot be sent spinning by malformed state the way recursion can.
    int cur = i;
    for (int guard = 0; guard < 16; ++guard) {
        if (cur < 0 || cur >= m_parts.size()) break;
        const Part& item = m_parts[cur];
        if (item.attachPart < 0 || item.attachPart >= m_parts.size()
            || item.attachPart == cur)
            break;
        const Part& host = m_parts[item.attachPart];
        if (!host.hasFcnp) break;
        const fox::ConnectPoint* cnp = host.fcnp.find(item.attachCnp);
        if (!cnp) break;

        // Parent bone in the host model, matched by StrCode32 of the CNP's
        // declared parent name (dictionary-independent).
        int boneIdx = -1;
        if (!cnp->parentBone.isEmpty()) {
            const quint32 want = static_cast<quint32>(
                fox::hashFileNameLegacy(cnp->parentBone, false) & 0xFFFFFFFFu);
            const auto& bones = host.model.bones();
            for (int b = 0; b < bones.size(); ++b)
                if (bones[b].nameHash32() == want
                    || bones[b].name == cnp->parentBone) {
                    boneIdx = b;
                    break;
                }
        }

        // Local CNP frame (rotate then translate, row-vector) …
        fox::Quat q;
        q.x = cnp->quat[0];
        q.y = cnp->quat[1];
        q.z = cnp->quat[2];
        q.w = cnp->quat[3];
        Mat4 hop = animmath::mul(
            Mat4::fromQuat(q),
            Mat4::translation(Vec3(cnp->pos[0], cnp->pos[1], cnp->pos[2])));
        // … in the parent bone's bind frame …
        if (boneIdx >= 0) {
            const auto& b = host.model.bones()[boneIdx];
            hop = animmath::mul(hop, Mat4::translation(
                                         Vec3(b.worldPos[0], b.worldPos[1],
                                              b.worldPos[2])));
            // … following the animated bone (skin = invBind·animWorld·viewShift,
            // so bindWorld·skin lands exactly on the posed bone).
            if (item.attachPart < poses.size()
                && boneIdx < poses[item.attachPart].size())
                hop = animmath::mul(hop, poses[item.attachPart][boneIdx]);
        }
        // This hop maps into the host's space; keep going until a part that is
        // not itself seated on anything.
        out = animmath::mul(out, hop);
        cur = item.attachPart;
    }
    return out;
}

// Where a part's own bind pose disagrees with the skeleton it shares.
//
// Every character part is rigged to the SAME player skeleton, but a part that
// only covers part of the body ships only the bones it needs — and declares its
// topmost one as a root at the origin. A hair model carries six bones whose root
// is the head bone; on the head model that same bone sits at head height, so the
// hair rendered a head's length low, in front of the chin.
//
// Matching by bone hash across the parts already loaded gives the correction
// directly: the shared bone's world rest position on the fuller skeleton, minus
// where this part thinks it is. Measured on Survive's avatar: all six of
// avf_hair_a0's bones appear in avf0_type0's 290, and the root's world offset is
// exactly the gap.
CustomizeTab::RestAlign CustomizeTab::restAlignmentFor(int i) const
{
    if (qEnvironmentVariableIsSet("FOXAB_NO_RESTALIGN")) return {};
    if (i < 0 || i >= m_parts.size()) return {};
    // …and the other direction, for the same reason the kill switch exists:
    // this correction only fires on data where one part's skeleton is a
    // FRAGMENT of another's, and a partial extract does not necessarily
    // contain such a pair — the container's whole test tree roots every model
    // at SKL_000_WAIST, so the correction is identically zero there and both
    // the viewport path and the export path go untested. Forcing a value
    // exercises them with one number that can then be read back off the
    // screen and out of the .glb. "x,y,z" in metres.
    if (const QByteArray forced = qgetenv("FOXAB_FORCE_RESTALIGN");
        !forced.isEmpty()) {
        const QList<QByteArray> xyz = forced.split(',');
        if (xyz.size() == 3) {
            RestAlign out;
            out.d = QVector3D(xyz[0].toFloat(), xyz[1].toFloat(),
                              xyz[2].toFloat());
            out.source = 'f';
            // Anchored on nothing: a forced offset is a rigid nudge for
            // testing, not a claim about which bone it was measured against.
            return out;
        }
    }
    const QVector<fox::FmdlBone>& mine = m_parts[i].model.bones();
    if (mine.isEmpty()) return {};
    // The part's own root — the anchor everything below it is relative to.
    int root = -1;
    for (int b = 0; b < mine.size(); ++b)
        if (mine[b].parentIndex < 0) { root = b; break; }
    if (root < 0) return {};
    const quint32 want = mine[root].hash32;
    if (want == 0) return {};

    // A part whose root is already deep in its own hierarchy needs no help; the
    // correction only applies when some OTHER part carries the same bone with a
    // different rest position.
    // Which PART and which BONE, not just the bone: the offset alone cannot be
    // animated, and the bone it was measured against is what carries the part
    // once a clip plays.
    struct Found { const fox::FmdlBone* bone = nullptr; int part = -1; int idx = -1; };
    const auto findRest = [&](quint32 code) -> Found {
        Found best;
        int bestBones = int(mine.size());
        for (int h = 0; h < m_parts.size(); ++h) {
            if (h == i) continue;
            const QVector<fox::FmdlBone>& theirs = m_parts[h].model.bones();
            if (int(theirs.size()) <= bestBones) continue;  // fuller skeleton wins
            // A part whose OWN root is this bone knows no more about where the
            // bone belongs than we do — its copy sits at the origin by
            // construction. Without this, two parts authored against the same
            // absent bone (Survive ships several against SKL_000_ROOT) resolve
            // against EACH OTHER, the offset comes out zero, and the
            // connect-point path below is never reached: both stay on the floor.
            int theirRoot = -1;
            for (int b = 0; b < theirs.size(); ++b)
                if (theirs[b].parentIndex < 0) { theirRoot = b; break; }
            if (theirRoot >= 0 && theirs[theirRoot].hash32 == code) continue;
            for (int b = 0; b < theirs.size(); ++b)
                if (theirs[b].hash32 == code) {
                    best.bone = &theirs[b];
                    best.part = h;
                    best.idx = b;
                    bestBones = int(theirs.size());
                    break;
                }
        }
        return best;
    };
    if (const Found f = findRest(want); f.bone) {
        RestAlign out;
        out.d = QVector3D(f.bone->worldPos[0] - mine[root].worldPos[0],
                          f.bone->worldPos[1] - mine[root].worldPos[1],
                          f.bone->worldPos[2] - mine[root].worldPos[2]);
        out.anchorPart = f.part;
        out.anchorBone = f.idx;
        out.source = 'b';
        return out;
    }

    // ── A PART THAT ROOTS WHERE THE CHARACTER DOES NEEDS NOTHING ────────────
    // findRest deliberately ignores a part whose OWN root is the bone being
    // looked up, because such a part's copy sits at the origin by construction
    // and cannot say where the bone belongs. That skip is right, and throwing
    // the fact away was not: when EVERY other part roots at this same bone,
    // the answer is not "fall through to the socket", it is "this part is
    // already authored in the character's own frame — correct it by nothing".
    //
    // Measured on hat31_main0_def_f, a 53-bone hood that carries the whole
    // waist→spine→chest→neck→head chain and roots at SKL_000_WAIST, exactly
    // like the body, the arms and the legs. Every one of them was skipped, the
    // socket path then measured the ACCESSORY slot's bone against the hood's
    // WAIST and produced d = (0, 0.534, -0.015) — which is simply the distance
    // from a human's waist to their head — and the hood was pushed half a metre
    // up. Residual at frame 94: 0.2530 m with the fall-through, and the offset
    // was invented by the code rather than found in the data.
    //
    // The socket path stays for what it was written for: a part rooted at a
    // bone NOTHING in the scene has, SKL_000_ROOT being the shipped example.
    bool rootSharedWithAnotherPart = false;
    for (int h = 0; h < m_parts.size() && !rootSharedWithAnotherPart; ++h) {
        if (h == i) continue;
        const QVector<fox::FmdlBone>& theirs = m_parts[h].model.bones();
        for (int b = 0; b < theirs.size(); ++b)
            if (theirs[b].parentIndex < 0) {
                if (theirs[b].hash32 == want) rootSharedWithAnotherPart = true;
                break;
            }
    }
    if (rootSharedWithAnotherPart) return {};

    // Nothing carries the part's own root bone. That is not a missing-data
    // case — Survive authors a handful of accessories against SKL_000_ROOT, a
    // bone the survivor's skeleton simply does not have — and the result is a
    // hat sitting at the model origin, which on this rig is the WAIST. The slot
    // says where such a part belongs; the rest position still comes from the
    // real skeleton, so nothing here is a guessed offset.
    if (!m_source.anchorBoneFor) return {};
    const QString slot = slotOfPart(i);
    if (slot.isEmpty()) return {};
    const quint32 anchor = m_source.anchorBoneFor(currentSubjectId(), slot);
    if (anchor == 0 || anchor == want) return {};

    // ── THE SOCKET, NOT THE BONE ─────────────────────────────────────────
    // A bone is where a limb is; a connect point is where a THING GOES, and
    // the game authors one per slot. skl0's own .fcnp puts CNP_HEAD at
    // (0, 0.1029, 0.0108) — ten centimetres, which on this rig is the
    // difference between a cap on the crown and a cap across the mouth. Every
    // plain cap in MGO roots at SKL_000_ROOT, a bone the player skeleton does
    // not carry, so every one of them comes through here.
    //
    // THE POINT'S OWN PARENT BONE, not the slot's. The two genders disagree:
    // skl0_main0_def_f hangs CNP_HEAD off SKL_004_HEAD and skl0_main0_def
    // hangs it off SKL_400_HEADROOT. Requiring the slot's anchor to match
    // silently dropped the offset for the man and left every cap on his mouth
    // while the woman's sat right — which is exactly the kind of half-working
    // that is worse than not working. The slot's bone stays as the fallback
    // for a base model that ships no connect points at all.
    QVector3D at;
    bool placed = false;
    RestAlign out;
    if (m_source.anchorCnpFor) {
        const QString cnpName = m_source.anchorCnpFor(currentSubjectId(), slot);
        if (!cnpName.isEmpty()) {
            for (int h = 0; h < m_parts.size() && !placed; ++h) {
                if (h == i || !m_parts[h].hasFcnp) continue;
                const fox::ConnectPoint* cp = m_parts[h].fcnp.find(cnpName);
                if (!cp) continue;
                const quint32 parent = cp->parentBone.isEmpty()
                    ? anchor
                    : quint32(fox::hashFileNameLegacy(cp->parentBone, false)
                              & 0xFFFFFFFFu);
                // The parent's rest position comes from the fullest skeleton
                // loaded, the same rule the bone path uses, so the offset and
                // the position it is added to are measured on one rig.
                if (const Found f = findRest(parent); f.bone) {
                    at = QVector3D(f.bone->worldPos[0] + cp->pos[0],
                                   f.bone->worldPos[1] + cp->pos[1],
                                   f.bone->worldPos[2] + cp->pos[2]);
                    // THE POINT'S PARENT BONE is what carries the item once a
                    // clip plays — the connect point is authored in that
                    // bone's bind frame, so that bone's matrix is the one the
                    // offset has to ride.
                    out.anchorPart = f.part;
                    out.anchorBone = f.idx;
                    out.source = 'c';
                    placed = true;
                }
            }
        }
    }
    if (!placed) {
        const Found f = findRest(anchor);
        if (!f.bone) return {};
        at = QVector3D(f.bone->worldPos[0], f.bone->worldPos[1],
                       f.bone->worldPos[2]);
        out.anchorPart = f.part;
        out.anchorBone = f.idx;
        out.source = 'a';
    }
    out.d = QVector3D(at.x() - mine[root].worldPos[0],
                      at.y() - mine[root].worldPos[1],
                      at.z() - mine[root].worldPos[2]);
    return out;
}

// ── FOXAB_DUMP_RESTALIGN, the BUILD half (§13) ──────────────────────────────
//
// Two failures wear the same symptom. "Floating at load, correct after playing
// a clip and coming back" says the correction was not COMPUTED on the first
// build — d comes back null because the anchor part had not been loaded yet
// when the pass ran. "The glasses are seated and the cap is not, in the same
// frame" cannot be that: one pass, one moment, two answers. It says the two
// parts took different EXITS out of restAlignmentFor, or that the clip drives
// one of them and not the other.
//
// Neither is visible in a screenshot and neither is visible in a still. So the
// build half prints, per part, the four things that decide it — root bone,
// which exit fired, which part and bone it anchored on, and d — and then
// compares each part against what the PREVIOUS build measured for the same
// part. A part whose d changes between build 1 and build 2 with nothing else
// changing IS the load-order bug; a part whose d is identical on both is not,
// whatever the screen shows.
void CustomizeTab::dumpRestAlign()
{
    static const bool on = qEnvironmentVariableIsSet("FOXAB_DUMP_RESTALIGN");
    if (!on) return;
    ++m_restAlignBuild;
    // Armed here as well as at clip load: a rebuild with a clip already loaded
    // re-poses the scene, and that is one of the two moments in question.
    m_restAlignFrameDumps = 2;
    qInfo("restalign: ══ build %d — %lld part(s) ══", m_restAlignBuild,
          qint64(m_parts.size()));
    int changed = 0, firstSeen = 0;
    for (int i = 0; i < m_parts.size(); ++i) {
        const Part& p = m_parts[i];
        const QVector<fox::FmdlBone>& bones = p.model.bones();
        int root = -1;
        for (int b = 0; b < bones.size(); ++b)
            if (bones[b].parentIndex < 0) { root = b; break; }
        const QString stem = p.path.section(QLatin1Char('/'), -1);
        const QString rootName = root >= 0 ? bones[root].name : QStringLiteral("(none)");
        const quint32 rootHash = root >= 0 ? bones[root].hash32 : 0u;
        if (p.attachPart >= 0) {
            // A seated part is placed by attachMatrixFor and never reaches the
            // pass — saying so is the difference between "skipped" and "came
            // back null", which look identical from outside.
            qInfo("restalign:  part %2d %-30s bones %4lld  root %-22s  "
                  "SEATED on part %d (%s) — pass not run",
                  i, qUtf8Printable(stem), qint64(bones.size()),
                  qUtf8Printable(rootName), p.attachPart,
                  qUtf8Printable(p.attachCnp));
            continue;
        }
        const RestAlign ra = restAlignmentFor(i);
        QString anchorTxt = QStringLiteral("—");
        if (ra.anchorPart >= 0 && ra.anchorPart < m_parts.size()) {
            const QVector<fox::FmdlBone>& ab = m_parts[ra.anchorPart].model.bones();
            anchorTxt = QStringLiteral("part %1 bone %2 %3")
                            .arg(ra.anchorPart).arg(ra.anchorBone)
                            .arg(ra.anchorBone >= 0 && ra.anchorBone < ab.size()
                                     ? ab[ra.anchorBone].name
                                     : QStringLiteral("?"));
        }
        const QString slot = slotOfPart(i);
        qInfo("restalign:  part %2d %-30s bones %4lld  root %-22s (%08x)  "
              "slot '%s'  exit %c  anchor %s  d (% .4f % .4f % .4f)%s",
              i, qUtf8Printable(stem), qint64(bones.size()),
              qUtf8Printable(rootName), rootHash,
              qUtf8Printable(slot), ra.source, qUtf8Printable(anchorTxt),
              ra.d.x(), ra.d.y(), ra.d.z(), ra.isNull() ? "   <-- NULL" : "");

        // …and the comparison the whole diagnostic exists for.
        const auto prev = m_restAlignPrev.constFind(p.path);
        if (prev == m_restAlignPrev.constEnd()) {
            ++firstSeen;
        } else if (prev->d != ra.d || prev->anchorPart != ra.anchorPart
                   || prev->anchorBone != ra.anchorBone
                   || prev->source != ra.source) {
            ++changed;
            qInfo("restalign:      ^ CHANGED since build %d: was exit %c "
                  "anchor part %d bone %d d (% .4f % .4f % .4f)",
                  prev->build, prev->source, prev->anchorPart, prev->anchorBone,
                  prev->d.x(), prev->d.y(), prev->d.z());
        }
        RestAlignPrev now;
        now.d = ra.d;
        now.anchorPart = ra.anchorPart;
        now.anchorBone = ra.anchorBone;
        now.source = ra.source;
        now.build = m_restAlignBuild;
        m_restAlignPrev.insert(p.path, now);
    }
    if (m_restAlignBuild > 1)
        qInfo("restalign: build %d vs earlier — %d part(s) CHANGED, %d new, "
              "%s", m_restAlignBuild, changed, firstSeen,
              changed == 0
                  ? "so the load-order hypothesis is DEAD for this scene"
                  : "so at least one part measured differently once more was loaded");
}

// ── FOXAB_DUMP_RESTALIGN, the FRAME half ────────────────────────────────────
//
// The build half says what the correction IS. This says what happened to it,
// which is the other half of the split the screenshot shows. A part falls into
// exactly one of three regimes and they behave differently under a clip:
//
//   CARRIED   drivenOf == 0 and an anchor bone exists: every one of the part's
//             matrices is replaced by the anchor bone's, so the part is one
//             rigid thing hanging off one socket. This is the intended path
//             for a cap.
//   BORROWED  the part's skeleton is a fragment of the host's, so it takes the
//             host's own skin matrices; translate(d) in front is then exactly
//             the bind-frame gap and the part rides the host correctly.
//   OWN       neither: the part is posed from the clip on its OWN hierarchy,
//             whose root sits at the origin, and translate(d) is folded in
//             front of matrices measured about a different origin.
//
// A part in OWN with a non-zero d is the one to look at, and nothing but this
// line distinguishes it from a part in CARRIED — on screen they are both
// "an item that is not where it should be".
// Fills m_lastAlign, ALWAYS — not only when the diagnostic is switched on.
//
// The measurement is a handful of multiplies per part and it is what three
// different consumers need: the log dump, the --restalignsweep census, and any
// future check that wants to say "is this item seated" without a person looking
// at a render. Computing it in one place is the whole point: a sweep that
// re-derived the residual would be a second spelling of the thing being
// measured, and the two would drift exactly when it mattered.
void CustomizeTab::fillLastAlign(
    const QVector<int>& drivenOf, const QVector<bool>& borrowedOf,
    const QVector<RestAlign>& aligns,
    const QVector<QVector<animmath::Mat4>>& poses,
    const QVector<QVector<animmath::Mat4>>& unfolded, int hostPart)
{
    m_lastAlign.clear();
    m_lastAlign.resize(m_parts.size());
    for (int pi = 0; pi < m_parts.size(); ++pi) {
        const Part& p = m_parts[pi];
        PartAlign& out = m_lastAlign[pi];
        out.path = p.path;
        out.stem = p.path.section(QLatin1Char('/'), -1);
        out.slot = slotOfPart(pi);
        out.bones = int(p.model.bones().size());
        if (p.attachPart >= 0) { out.regime = 'S'; continue; }

        const int driven = drivenOf.value(pi, 0);
        const RestAlign& ra = aligns.value(pi);
        const bool haveAnchor = ra.anchorPart >= 0 && ra.anchorBone >= 0;
        // The SAME test the pass uses, not a paraphrase of it — a report that
        // names a regime the code did not take is worse than no report at all.
        const bool socketAnchored = ra.source == 'c' || ra.source == 'a';
        out.driven = driven;
        out.source = ra.source;
        out.anchorPart = ra.anchorPart;
        out.anchorBone = ra.anchorBone;
        out.d = ra.d;
        out.regime = pi == hostPart ? 'H'
            : borrowedOf.value(pi, false) ? 'B'
            : ((driven == 0 || socketAnchored) && haveAnchor) ? 'C' : 'O';

        // ── THE RESIDUAL, which is the whole point ──────────────────────
        // A part is seated when its own root bone lands on the animated
        // position of the bone that root is supposed to sit on. Applying a skin
        // matrix to a bone's BIND WORLD position gives that bone's ANIMATED
        // world position, so both sides of that sentence are one multiply each
        // and the answer is a distance in metres. Half a metre is the hat by
        // the knees; a millimetre is the hat on the head. Without this the
        // check is "look at the render and judge", which cannot be done from a
        // log, across a rebuild, or over ninety wardrobe items in one pass.
        //
        // Target: the anchor bone when there is one, and otherwise the HOST's
        // own copy of the part's root bone — so the torso, arms and legs, which
        // legitimately need no correction, still get a number rather than a
        // blank, and a regression in the parts that were never broken cannot
        // hide behind "not applicable".
        const QVector<fox::FmdlBone>& mine = p.model.bones();
        int root = -1;
        for (int bi = 0; bi < mine.size(); ++bi)
            if (mine[bi].parentIndex < 0) { root = bi; break; }
        out.rootBone = root >= 0 ? mine[root].name : QString();
        int tgtPart = ra.anchorPart, tgtBone = ra.anchorBone;
        if (tgtPart < 0 && hostPart >= 0 && pi != hostPart && root >= 0) {
            const QVector<fox::FmdlBone>& hb = m_parts[hostPart].model.bones();
            for (int bi = 0; bi < hb.size(); ++bi)
                if (hb[bi].hash32 == mine[root].hash32) {
                    tgtPart = hostPart; tgtBone = bi; break;
                }
        }
        if (root >= 0 && tgtPart >= 0 && tgtBone >= 0 && pi != hostPart
            && root < poses.value(pi).size()
            && tgtBone < unfolded.value(tgtPart).size()) {
            // The part's OWN bind world position, uncorrected: `poses` has
            // already had translate(d) folded into it by the pass above, so
            // adding d here as well would measure d twice and report every
            // correctly seated part as floating by exactly |d| — which is the
            // first thing this diagnostic did, and it looked entirely
            // plausible until the number came out equal to |d| on the nose.
            const animmath::Vec3 mineRoot(mine[root].worldPos[0],
                                          mine[root].worldPos[1],
                                          mine[root].worldPos[2]);
            const animmath::Vec3 landed =
                animmath::transform(mineRoot, poses[pi][root]);
            const fox::FmdlBone& tb = m_parts[tgtPart].model.bones()[tgtBone];
            const animmath::Vec3 target = animmath::transform(
                animmath::Vec3(tb.worldPos[0], tb.worldPos[1], tb.worldPos[2]),
                unfolded[tgtPart][tgtBone]);
            out.residual = (landed - target).length();

            // ── AND THE SAME QUESTION ASKED OF THE GEOMETRY ─────────────
            // Skin the vertices the way the renderer does — palette index per
            // vertex, four weights — and take the centroid. Doing it through
            // the real palette matters: following the root bone instead is
            // what the metric above already does, and it is why it cannot see
            // this class of fault.
            animmath::Vec3 sumPosed(0, 0, 0), sumBind(0, 0, 0);
            int nv = 0;
            for (const fox::FmdlMesh& m : p.model.meshes()) {
                const int verts = int(m.positions.size() / 3);
                if (verts <= 0 || m.boneIndices.size() < size_t(verts) * 4)
                    continue;
                for (int v = 0; v < verts; ++v) {
                    const animmath::Vec3 bindPos(m.positions[v * 3 + 0],
                                                 m.positions[v * 3 + 1],
                                                 m.positions[v * 3 + 2]);
                    animmath::Vec3 acc(0, 0, 0);
                    float wsum = 0.0f;
                    for (int k = 0; k < 4; ++k) {
                        const float w = m.boneWeights.value(v * 4 + k, 0.0f);
                        if (w <= 0.0f) continue;
                        const int pi2 = m.boneIndices.value(v * 4 + k, 0);
                        const int b = m.palette.value(pi2, 0);
                        if (b < 0 || b >= poses[pi].size()) continue;
                        const animmath::Vec3 t =
                            animmath::transform(bindPos, poses[pi][b]);
                        acc.x += t.x * w; acc.y += t.y * w; acc.z += t.z * w;
                        wsum += w;
                    }
                    // An unweighted vertex rides the part's root, which is
                    // what the renderer falls back to as well.
                    if (wsum <= 0.0f)
                        acc = animmath::transform(bindPos, poses[pi][root]);
                    sumPosed.x += acc.x; sumPosed.y += acc.y; sumPosed.z += acc.z;
                    sumBind.x += bindPos.x; sumBind.y += bindPos.y;
                    sumBind.z += bindPos.z;
                    ++nv;
                }
            }
            if (nv > 0) {
                out.meshVerts = nv;
                const animmath::Vec3 cPosed(sumPosed.x / nv, sumPosed.y / nv,
                                            sumPosed.z / nv);
                const animmath::Vec3 cBind(sumBind.x / nv, sumBind.y / nv,
                                           sumBind.z / nv);
                // Where the author put the geometry relative to the bone it
                // hangs on, in the part's OWN file …
                out.bindOffset = QVector3D(cBind.x - mineRoot.x,
                                           cBind.y - mineRoot.y,
                                           cBind.z - mineRoot.z);
                // … and where it actually ended up relative to that bone.
                out.meshOffset = QVector3D(cPosed.x - target.x,
                                           cPosed.y - target.y,
                                           cPosed.z - target.z);
                out.meshResidual = (out.meshOffset - out.bindOffset).length();
            }
        }
    }
}

const char* CustomizeTab::alignRegimeName(char r)
{
    switch (r) {
        case 'H': return "HOST";
        case 'B': return "BORROWED";
        case 'C': return "CARRIED";
        case 'O': return "OWN";
        case 'S': return "SEATED";
        default:  return "?";
    }
}

// ── FOXAB_DUMP_RESTALIGN, the FRAME half ────────────────────────────────────
//
// The build half says what the correction IS. This says what happened to it.
// A part falls into exactly one of four regimes and they behave differently
// under a clip:
//
//   HOST      the largest skeleton in the scene; everything else measures
//             against it.
//   CARRIED   the clip drives nothing of it, OR its root is a bone no part in
//             the scene carries (a socket-anchored accessory). Either way it
//             takes the anchor bone's matrix and is one rigid thing on one
//             socket. This is the intended path for a cap.
//   BORROWED  its skeleton is a fragment of the host's, so it takes the host's
//             own skin matrices; translate(d) in front is then exactly the
//             bind-frame gap and the part rides the host correctly.
//   OWN       none of those: posed from the clip on its OWN hierarchy.
//
// A part in OWN with a non-zero d is the one to look at, and nothing but the
// residual distinguishes it from a part in CARRIED — on screen they are both
// "an item that is not where it should be".
void CustomizeTab::dumpRestAlignFrame(int hostPart, float frame) const
{
    static const bool on = qEnvironmentVariableIsSet("FOXAB_DUMP_RESTALIGN");
    if (!on) return;
    const QString hostName =
        hostPart >= 0 && hostPart < m_parts.size()
            ? m_parts[hostPart].path.section(QLatin1Char('/'), -1)
            : QStringLiteral("(none)");
    qInfo("restalign: ── frame %.1f  clip '%s'  host part %d %s (%lld bones) ──",
          double(frame),
          qUtf8Printable(m_clipCombo ? m_clipCombo->currentText()
                                     : QStringLiteral("(none)")),
          hostPart, qUtf8Printable(hostName),
          qint64(hostPart >= 0 && hostPart < m_parts.size()
                     ? m_parts[hostPart].model.bones().size() : 0));
    for (int pi = 0; pi < m_lastAlign.size(); ++pi) {
        const PartAlign& a = m_lastAlign[pi];
        if (a.regime == 'S') {
            qInfo("restalign:  part %2d %-30s SEATED on a connect point — "
                  "placed by attachMatrixFor, not by the pass",
                  pi, qUtf8Printable(a.stem));
            continue;
        }
        qInfo("restalign:  part %2d %-30s bones %4d  driven %4d  %-8s  "
              "exit %c  anchor part %d bone %d  d (% .4f % .4f % .4f)  "
              "residual %s m%s",
              pi, qUtf8Printable(a.stem), a.bones, a.driven,
              alignRegimeName(a.regime), a.source, a.anchorPart, a.anchorBone,
              a.d.x(), a.d.y(), a.d.z(),
              a.residual < 0.0f
                  ? "      —"
                  : qUtf8Printable(QString::asprintf("%7.4f", double(a.residual))),
              a.residual > 0.02f ? "   <-- FLOATING" : "");
    }
}

// Which builder slot a loaded part came from, or empty when it was added
// outside the slot rows (the base body, a manual add).
QString CustomizeTab::slotOfPart(int partIdx) const
{
    for (const WeaponSlotRow& r : m_weaponRows)
        if (r.partIdx == partIdx) return r.slot;
    return {};
}

void CustomizeTab::applyAttachTransforms(
    const QVector<QVector<animmath::Mat4>>& poses)
{
    // `poses` empty means BIND — no clip, nothing skinned. That is the one
    // state in which the rest-alignment correction still belongs here, as a
    // rigid world-space translation: there is no palette to fold it into.
    // With a clip loaded, setFrame's alignment pass has already folded
    // translate(d) in front of every one of the part's bone matrices, and
    // adding it a second time out here would double it — so the transform is
    // explicitly reset to identity rather than merely skipped, because a
    // group transform set at bind persists until something replaces it.
    const bool posed = !poses.isEmpty();
    for (int i = 0; i < m_parts.size(); ++i) {
        if (m_parts[i].attachPart < 0) {
            // Not seated on a connect point, but its bind pose may still be
            // authored against a bone another part places elsewhere.
            QMatrix4x4 qm;
            if (!posed) {
                const QVector3D d = restAlignmentFor(i).d;
                if (d.isNull()) continue;
                qm.translate(d);
            }
            m_view->setGroupTransform(i, qm);
            continue;
        }
        const animmath::Mat4 m = attachMatrixFor(i, poses);
        // Row-vector row-major → QMatrix4x4 (column-vector): transpose.
        QMatrix4x4 qm;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) qm(r, c) = m.m[c][r];
        m_view->setGroupTransform(i, qm);
    }
}

void CustomizeTab::populateAnimCombo()
{
    m_mtarCombo->blockSignals(true);
    animcombo::fillArchives(m_mtarCombo);
    m_mtarCombo->blockSignals(false);
}

void CustomizeTab::locateFrig()
{
    if (m_frigSearched) return;
    m_frigSearched = true;
    m_hasFrig = false;
    const ArchiveIndex& index = ArchiveIndex::instance();
    int best = -1, bestScore = -1;
    const auto& files = index.files();
    for (int i = 0; i < files.size(); ++i) {
        if (ArchiveIndex::extensionOf(files[i]) != QLatin1String("frig")) continue;
        const QString& p = files[i].path;
        int score = 1;
        if (p.endsWith(QLatin1String("human_finger.frig"))) score = 3;
        else if (p.contains(QLatin1String("human"))) score = 2;
        if (score > bestScore) { bestScore = score; best = i; }
        if (score == 3) break;
    }
    if (best < 0) return;
    const QByteArray data = index.readFile(files[best]);
    if (!data.isEmpty() && m_frig.parse(data)) m_hasFrig = true;
}

void CustomizeTab::loadMtar(int fileIdx)
{
    const ArchiveIndex& index = ArchiveIndex::instance();
    if (fileIdx < 0 || fileIdx >= index.files().size()) return;
    const QByteArray data = index.readFile(index.files()[fileIdx]);
    m_hasMtar = m_hasAnim = false;
    m_clipCombo->blockSignals(true);
    m_clipCombo->clear();
    if (data.isEmpty() || !m_mtar.parse(data)) {
        // Same reason as the failed clip decode: with playback no longer
        // stopping on every combo change, an archive that does not parse would
        // leave the timer ticking and the button showing ⏸ over a frozen model.
        if (m_playBtn->isChecked()) m_playBtn->setChecked(false);
        m_animTimer->stop();
        m_view->clearPose();
        m_clipCombo->blockSignals(false);
        m_clipCombo->setEnabled(false);
        m_playBtn->setEnabled(false);
        m_frameSlider->setEnabled(false);
        m_frameLabel->setText(QStringLiteral("—"));
        syncAnimProvider();
        return;
    }
    m_hasMtar = true;
    locateFrig();
    animcombo::fillClips(m_clipCombo, m_mtar, fileIdx);
    m_clipCombo->blockSignals(false);
    if (m_clipCombo->count() > 0) {
        // selectPayload, not setCurrentIndex(0): with captions, row 0 is a
        // heading and picking it would load nothing.
        m_clipCombo->blockSignals(true);
        m_clipCombo->selectPayload(0);
        m_clipCombo->blockSignals(false);
        loadClip(0);
    }
}

void CustomizeTab::loadClip(int clipIdx)
{
    if (!m_hasMtar || clipIdx < 0 || clipIdx >= m_mtar.clips().size()) return;
    m_hasAnim = false;
    m_anim = m_mtar.decodeClip(clipIdx);
    if (!m_anim.valid()) {
        // Now that a clip change no longer stops playback, a clip that fails
        // to decode would leave the timer running against nothing — the button
        // saying "playing" over a still model, forever.
        //
        // The SCENE is reset too. Stopping the timer alone left the model
        // frozen mid-stride in the previous clip's pose, with the slider still
        // enabled at the old range and the label still counting — which reads
        // as "this clip plays and is very short" rather than "this clip did
        // not load".
        if (m_playBtn->isChecked()) m_playBtn->setChecked(false);
        m_animTimer->stop();
        m_view->clearPose();
        m_frame = 0.0f;
        m_frameSlider->setEnabled(false);
        m_playBtn->setEnabled(false);
        m_frameLabel->setText(QStringLiteral("—"));
        syncAnimProvider();
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
    m_animDiag = true;   // report the per-part bind check once for this clip
    // §13. Two dumps, not one: the harness loads a clip and THEN seeks to the
    // frame it wants, and the frame that matters is the second one.
    m_restAlignFrameDumps = 2;
    setFrame(0.0f);
    syncAnimProvider();
    // The panel mirrors the transport; every path that changes what is
    // playing has to say so, including the ones that fail.
    syncAnimPanel();
}

void CustomizeTab::setFrame(float f, bool fromSlider)
{
    m_frame = f;
    // Cleared BEFORE the early return as well: a scene with no clip has no
    // palettes, and a stale set left behind from a previous clip would be
    // exported as the pose of a character that is no longer posed.
    m_framePalettes.clear();
    // The diagnostic flag is dropped even on the early return: a clip loaded
    // with nothing equipped has nothing to report, and leaving it armed made
    // the report surface later, at whatever frame followed the next equip.
    if (m_parts.isEmpty() || !m_hasAnim) { m_animDiag = false; return; }

    // One combined palette: each part's bones posed independently by the same
    // clip + rig, concatenated at its boneBase. Attached items stay at bind
    // (their group transform seats them on the host bone instead).
    // Captured and cleared HERE, not after the pass: a clip loaded with no
    // parts equipped returns early below, and leaving the flag armed made the
    // report appear later, at whatever setFrame followed the next equip.
    const bool diagThisClip = m_animDiag;
    m_animDiag = false;

    QVector<animmath::Mat4> combined;
    QVector<QVector<animmath::Mat4>> poses(m_parts.size());
    QVector<float> lines;
    int drivenTotal = 0;

    // The HOST: whichever part carries the most bones, which by construction
    // is the one with the whole skeleton. Parts whose skeleton is a truncated
    // copy of it borrow its palette outright instead of building a chain from
    // a root the character does not actually have — see borrowPalette().
    int hostPart = -1;
    for (int pi = 0; pi < m_parts.size(); ++pi) {
        if (m_parts[pi].attachPart >= 0) continue;   // seated, not skinned
        // A model too small to be a character's whole skeleton is not trusted
        // as the host — see animpose::kMinHostBones. Without this a scene of
        // nothing but hair and glasses would elect one fragment as the host
        // and hand its own broken chain to the rest.
        if (m_parts[pi].model.bones().size() < animpose::kMinHostBones) continue;
        if (hostPart < 0
            || m_parts[pi].model.bones().size()
                   > m_parts[hostPart].model.bones().size())
            hostPart = pi;
    }
    // The host has to be posed FIRST, since everything else may read it — and
    // its palette is REUSED below rather than rebuilt, because posing the
    // largest skeleton in the scene twice per frame is the most expensive
    // thing this function could do twice.
    QVector<animmath::Mat4> hostPalette;
    QHash<quint32, int> hostBoneIndex;
    int hostDriven = 0;
    if (hostPart >= 0) {
        const Part& hp = m_parts[hostPart];
        const fox::FrigFile* hfrig = hp.hasFrig ? &hp.frig
            : m_hasFrig ? &m_frig
                        : nullptr;
        hostPalette = animpose::buildPalette(hp.model, m_anim, f, hfrig,
                                             hp.hasFrdv ? &hp.frdv : nullptr,
                                             &hostDriven);
        hostBoneIndex = animpose::boneIndexByHash(hp.model);
    }

    // Attachment palettes, per part, in the order rebuildScene laid them out.
    // Collected rather than concatenated as we go, because the alignment pass
    // between the two loops can change a part's palette after it is built and
    // before it is flattened.
    QVector<QVector<QVector<animmath::Mat4>>> attPals(m_parts.size());
    // Parallel to attPals: how many bones the clip drives in each attachment.
    // Needed by the alignment pass for the same reason the parts' count is —
    // an attachment nothing drives has to be CARRIED rather than left at bind.
    QVector<QVector<int>> attDriven(m_parts.size());
    QVector<int> drivenOf(m_parts.size(), 0);
    QVector<bool> borrowedOf(m_parts.size(), false);

    for (int pi = 0; pi < m_parts.size(); ++pi) {
        Part& part = m_parts[pi];
        int driven = 0;
        const fox::FrigFile* frig = part.hasFrig ? &part.frig
            : m_hasFrig ? &m_frig
                        : nullptr;
        // A fragment skeleton takes the host's matrices. Tested against the
        // host's own hierarchy rather than by bone count: a part can be small
        // and still root its bones where the character does, and that part
        // poses correctly on its own.
        // FOXAB_NO_BORROW=1 restores the old per-part pose for comparison.
        // Diagnostics in this project stay in the code and stay env-gated.
        static const bool noBorrow =
            qEnvironmentVariableIsSet("FOXAB_NO_BORROW");
        const bool borrow = !noBorrow && hostPart >= 0 && pi != hostPart
            && part.attachPart < 0 && !hostPalette.isEmpty()
            && animpose::isSkeletonFragment(part.model,
                                            m_parts[hostPart].model,
                                            &hostBoneIndex);
        QVector<animmath::Mat4> pal;
        bool borrowed = false;
        if (pi == hostPart) {
            pal = hostPalette;          // already posed, above
            driven = hostDriven;
        } else if (part.attachPart >= 0) {
            pal = QVector<animmath::Mat4>(part.model.bones().size());
        } else if (borrow) {
            // THE BORROW IS USABLE WHEN IT RESOLVED EVERY BONE, not when a
            // MAJORITY of them matched by name. Those are different questions
            // and the second one had the survivor's cap wrong for eight
            // batches: hat13 shares exactly ONE bone with the body — its own
            // root, SKL_004_HEAD — and its other three hang off that root, so
            // borrowPalette's ancestor pass resolves all four and carries the
            // whole cap on the head. The majority rule threw that away (1 of 4
            // is not a majority) and dropped the cap into the one regime that
            // has no right answer: posed from the clip on its OWN four-bone
            // hierarchy, whose root sits at the ORIGIN, with a bind-frame
            // translate(d) folded in front of matrices measured about a
            // different origin. Measured, on the _probe survivor scene with
            // mgo_pl_rcvr_gl03: the cap's root landed 1.0031 m from the head
            // at frame 0 and 1.1146 m at frame 94, while the glasses — 3 of 5,
            // a majority, borrow kept — landed 0.0000 m at both. One rule, two
            // items, and the split the user photographed.
            //
            // What the old rule was really guarding against was a fragment
            // with a SECOND root the host does not carry: those bones come
            // back on identity and land at the character's starting point.
            // `unresolved` asks that directly, so the proportion of matched
            // bones stops being consulted at all.
            int unresolved = 0;
            pal = animpose::borrowPalette(part.model, m_parts[hostPart].model,
                                          hostPalette, &driven, &hostBoneIndex,
                                          &unresolved);
            // §13. "is a fragment / resolved n of m / kept or discarded" are
            // three different states and from outside all three look like one
            // item in the wrong place.
            if (m_restAlignFrameDumps > 0
                && qEnvironmentVariableIsSet("FOXAB_DUMP_RESTALIGN"))
                qInfo("restalign:  part %2d borrow: IS a fragment, matched %d "
                      "of %lld bone(s) by name, %d unresolved — %s", pi, driven,
                      qint64(part.model.bones().size()), unresolved,
                      unresolved > 0
                          ? "DISCARDED — a root the host does not carry"
                          : "kept");
            if (unresolved > 0)
                pal = animpose::buildPalette(part.model, m_anim, f, frig,
                                             part.hasFrdv ? &part.frdv : nullptr,
                                             &driven);
            else
                borrowed = true;
        } else {
            if (pi != hostPart && part.attachPart < 0
                && m_restAlignFrameDumps > 0
                && qEnvironmentVariableIsSet("FOXAB_DUMP_RESTALIGN"))
                qInfo("restalign:  part %2d borrow: NOT a fragment of the host "
                      "— posed on its own %lld-bone hierarchy", pi,
                      qint64(part.model.bones().size()));
            pal = animpose::buildPalette(part.model, m_anim, f, frig,
                                         part.hasFrdv ? &part.frdv : nullptr,
                                         &driven);
        }
        poses[pi] = pal;
        drivenOf[pi] = driven;
        borrowedOf[pi] = borrowed;

        // The variation's attached models, in the SAME order rebuildScene laid
        // them out, and for the same reason: the combined palette is one flat
        // array and every upload's joints are absolute indices into it. Advance
        // the layout here without advancing it there — or the other way round —
        // and every part AFTER the first attaching one skins against another
        // part's matrices. Bind pose hides it completely, which is what makes
        // it worth spelling out.
        //
        // The skip test has to be the same one too, so an attachment the scene
        // did not draw does not silently take a palette slot here.
        for (int ai = 0; ai < part.fovaAttached.size(); ++ai) {
            if (attachmentAlreadyFitted(part, ai)) continue;
            const fox::FmdlFile& am = part.fovaAttached[ai].model;
            int adriven = 0;
            // Same rule as the parts: an attached hair or hat is exactly the
            // kind of model that carries a fragment of the skeleton.
            static const bool noBorrowAtt =
                qEnvironmentVariableIsSet("FOXAB_NO_BORROW");
            // part.attachPart < 0 for the same reason the part branch has it:
            // a seated part is placed by its group transform and deliberately
            // takes an identity palette, so what it brings with it must not be
            // animated on top of that transform.
            bool attBorrowed = false;
            QVector<animmath::Mat4> apal;
            if (!noBorrowAtt && hostPart >= 0 && part.attachPart < 0
                && !hostPalette.isEmpty()
                && animpose::isSkeletonFragment(am, m_parts[hostPart].model,
                                                &hostBoneIndex)) {
                // Same rule as the parts above, and for the same reason —
                // a variation's hat is the same shape of model as an equipped
                // one and the majority test was wrong about both.
                int aunresolved = 0;
                apal = animpose::borrowPalette(
                    am, m_parts[hostPart].model, hostPalette, &adriven,
                    &hostBoneIndex, &aunresolved);
                if (aunresolved > 0)
                    apal = animpose::buildPalette(am, m_anim, f, frig, nullptr,
                                                  &adriven);
                else
                    attBorrowed = true;
            } else {
                apal = animpose::buildPalette(am, m_anim, f, frig, nullptr,
                                              &adriven);
            }
            attPals[pi].append(apal);
            attDriven[pi].append(adriven);
            drivenTotal += adriven;
            // Attachments get the same report as parts — a variation's hat or
            // hair is exactly the kind of model this is about, and leaving it
            // out meant the motivating case was the one case never logged.
            if (diagThisClip) {
                const auto& files = ArchiveIndex::instance().files();
                const int fi = part.fovaAttachedFiles.value(ai, -1);
                qInfo("anim: part %d attach %-27s %d/%lld bone(s)%s%s", pi,
                      qUtf8Printable(fi >= 0 && fi < files.size()
                                     ? files[fi].path.section(QLatin1Char('/'), -1)
                                     : QStringLiteral("(unknown)")),
                      adriven, qint64(am.bones().size()),
                      attBorrowed ? "  (fragment — borrowed)" : "",
                      adriven == 0 ? "   <-- NOT ANIMATED" : "");
            }
        }
    }

    // ── THE ALIGNMENT PASS ──────────────────────────────────────────────────
    // A part whose root bone the wearer places somewhere else carries a
    // BIND-FRAME correction `d` (restAlignmentFor): ten centimetres between a
    // cap's own origin and the crown of the head, fifteen between a hairstyle's
    // chest root and the body's. That correction used to be applied as a
    // rigid GROUP TRANSFORM, in the world frame, AFTER skinning. At bind that
    // is right — nothing has rotated — and the moment a clip plays it is wrong
    // by exactly d·R − d, so the hair and the goggles slid off a head that
    // was otherwise posed correctly. "Fine in T-pose, disconnected once
    // anything plays" was that term, measured and reproduced headlessly.
    //
    // It belongs IN FRONT of the bone's matrix, not behind it. A vertex has to
    // be moved to where the part sits on the wearer FIRST, in bind space, and
    // then carried by the bone:  v · translate(d) · palette[b].  At bind
    // palette[b] is identity and the result is v + d — bit for bit what the
    // group transform used to produce, which is why the T-pose cannot change.
    //
    // And a part the clip drives NOTHING of has no matrix of its own to ride.
    // That is not a missing-data case either: the slot's connect point names
    // the bone the item hangs off, so the item is carried rigidly by THAT
    // bone's matrix. A cap with one bone the animation never mentions now
    // turns with the head instead of hanging in the air where the head was.
    //
    // FOXAB_NO_RESTALIGN=1 drops the whole correction, as it always did.
    // The anchor matrices are read from a SNAPSHOT taken before any folding.
    // Without it the pass is order-dependent: it walks pi ascending and folds
    // as it goes, so a part carried by an anchor with a LOWER index would ride
    // an anchor matrix that already had that anchor's own translate(d) in it,
    // and the same scene would come out differently depending on the order the
    // user happened to equip its parts in. `d` is measured against the
    // anchor's BIND position, so the anchor's own fold must not be in it.
    const QVector<QVector<animmath::Mat4>> unfolded = poses;

    QVector<CustomizeTab::RestAlign> aligns(m_parts.size());
    for (int pi = 0; pi < m_parts.size(); ++pi) {
        if (m_parts[pi].attachPart >= 0) continue;   // seated: attachMatrixFor
        aligns[pi] = restAlignmentFor(pi);
        const RestAlign& ra = aligns[pi];
        if (ra.anchorPart < 0) continue;
        const bool haveAnchor = ra.anchorPart < unfolded.size()
            && ra.anchorBone >= 0
            && ra.anchorBone < unfolded[ra.anchorPart].size();
        // ── WHEN A PART IS CARRIED RIGIDLY ──────────────────────────────
        // Every one of the part's own bones takes the anchor bone's matrix,
        // because the part is one rigid thing hanging off one socket — which
        // is what a connect point means, and which is also the right answer
        // for anything SEATED on this part: attachMatrixFor multiplies by
        // translate(boneWorld) first, so a uniform matrix here places a seat
        // exactly where the carried bone took it.
        //
        // TWO conditions reach it, and the second is not "the clip drives
        // nothing" spelt differently.
        //
        //   drivenOf == 0    the clip has no tracks for this part at all, so
        //                    there is no matrix of its own for it to ride.
        //
        //   SOCKET-ANCHORED  restAlignmentFor took exit 'c' or 'a', and it
        //                    only reaches those when NO part in the scene
        //                    carries this part's ROOT bone. Survive and MGO
        //                    both author accessories against SKL_000_ROOT, a
        //                    bone the player skeleton does not have: 41 models
        //                    in the container's own pull root there and NOT
        //                    ONE of them shares a single bone with the body.
        //                    Such a part's hierarchy hangs off the model
        //                    origin and has no relationship to the character's,
        //                    so posing it from the clip cannot be right however
        //                    many of its bones happen to share a NAME with one
        //                    the clip drives — the chain still sits at the
        //                    origin and the bind-frame translate(d) below is
        //                    then folded in front of matrices measured about a
        //                    different origin. Its placement comes from the
        //                    socket and from nothing else, so the socket is
        //                    what carries it, and drivenOf is not consulted.
        const bool socketAnchored = ra.source == 'c' || ra.source == 'a';
        if ((drivenOf[pi] == 0 || socketAnchored) && haveAnchor) {
            const animmath::Mat4 anchor = unfolded[ra.anchorPart][ra.anchorBone];
            for (animmath::Mat4& m : poses[pi]) m = anchor;
        }
        // The variation's ATTACHMENTS ride with the part. They used to get
        // this for free: rebuildScene gives an attachment its wearer's
        // groupId, so the rigid group transform that carried translate(d)
        // carried them too. With the offset moved into the palette they have
        // to be folded here as well, or a hat's attached model would sit with
        // it at bind and jump by exactly -d the moment a clip loaded.
        if ((drivenOf[pi] == 0 || socketAnchored) && haveAnchor) {
            const animmath::Mat4 anchor = unfolded[ra.anchorPart][ra.anchorBone];
            for (int ai = 0; ai < attPals[pi].size(); ++ai)
                if (attDriven[pi].value(ai, 0) == 0)
                    for (animmath::Mat4& m : attPals[pi][ai]) m = anchor;
        }
        if (ra.d.isNull()) continue;
        const animmath::Mat4 t =
            animmath::Mat4::translation(animmath::Vec3(ra.d.x(), ra.d.y(),
                                                       ra.d.z()));
        // mul(A, B) applies A then B, so this is translate-then-bone.
        for (animmath::Mat4& m : poses[pi]) m = animmath::mul(t, m);
        for (QVector<animmath::Mat4>& apal : attPals[pi])
            for (animmath::Mat4& m : apal) m = animmath::mul(t, m);
    }

    // §13. AFTER the pass, so every regime below has been decided, and before
    // the flatten, so `aligns` and `drivenOf` still say what was applied.
    fillLastAlign(drivenOf, borrowedOf, aligns, poses, unfolded, hostPart);
    if (m_restAlignFrameDumps > 0) {
        --m_restAlignFrameDumps;
        dumpRestAlignFrame(hostPart, f);
    }

    // ── Flatten ─────────────────────────────────────────────────────────────
    // One combined palette, in the SAME order rebuildScene uploaded the
    // meshes: each part, then the models its variation attached.
    for (int pi = 0; pi < m_parts.size(); ++pi) {
        const Part& part = m_parts[pi];
        const QVector<animmath::Mat4>& pal = poses[pi];
        m_framePalettes.insert(qMakePair(pi, -1), pal);
        drivenTotal += drivenOf[pi];
        // A part whose bones NOTHING in the clip drives stays in bind pose
        // while everything around it moves — the hair sitting still on a head
        // that turns. It is invisible in a still frame and obvious the moment
        // anything plays, so it is reported once per clip rather than left to
        // be noticed.
        if (diagThisClip && part.attachPart < 0) {
            const RestAlign& ra = aligns[pi];
            const bool carried = drivenOf[pi] == 0 && ra.anchorPart >= 0;
            qInfo("anim: part %d %-34s %d/%lld bone(s) driven%s%s%s", pi,
                  qUtf8Printable(part.path.section(QLatin1Char('/'), -1)),
                  drivenOf[pi], qint64(part.model.bones().size()),
                  borrowedOf[pi]
                      ? "  (fragment — borrowed from the host skeleton)" : "",
                  ra.isNull() ? "" : qUtf8Printable(
                      QStringLiteral("  align(%1 %2 %3)")
                          .arg(ra.d.x(), 0, 'f', 4).arg(ra.d.y(), 0, 'f', 4)
                          .arg(ra.d.z(), 0, 'f', 4)),
                  carried ? "  (carried by the slot's connect-point bone)"
                          : (drivenOf[pi] == 0 ? "   <-- NOT ANIMATED" : ""));
        }
        combined += pal;
        const auto& bones = part.model.bones();
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
        int slot = 0;
        for (int ai = 0; ai < part.fovaAttached.size(); ++ai) {
            if (attachmentAlreadyFitted(part, ai)) continue;
            if (slot >= attPals[pi].size()) break;   // cannot happen; not trusted
            const QVector<animmath::Mat4>& apal = attPals[pi][slot++];
            combined += apal;
            m_framePalettes.insert(qMakePair(pi, ai), apal);
        }
    }
    m_view->applyPose(combined, lines);
    applyAttachTransforms(poses);

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
                              .arg(drivenTotal));
}

int CustomizeTab::equipParts(const QStringList& filters)
{
    int added = 0;
    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();
    for (const QString& flt : filters) {
        for (int i = 0; i < files.size(); ++i) {
            if (ArchiveIndex::extensionOf(files[i]) != QLatin1String("fmdl")) continue;
            if (!files[i].path.contains(flt, Qt::CaseInsensitive)) continue;
            const int before = m_parts.size();
            addPart(i);
            if (m_parts.size() > before) ++added;
            break;
        }
    }
    return added;
}

bool CustomizeTab::selectAnim(const QString& mtarFilter, const QString& clipFilter,
                              float frame)
{
    if (m_mtarCombo->count() <= 1) populateAnimCombo();
    // By PAYLOAD, not by row: the combo carries group captions now, so row N
    // and archive N are different things and walking rows matches a heading.
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
    if (!m_mtarCombo->selectPayload(wantFile)) return false;
    if (!m_hasMtar) return false;
    if (!clipFilter.isEmpty()) {
        bool isNum = false;
        const int asNum = clipFilter.toInt(&isNum);
        int clipIdx = -1;
        if (isNum && asNum >= 0 && asNum < m_mtar.clips().size()) {
            clipIdx = asNum;
        } else {
            // Matched against the ASSET name and our readable label both, so a
            // harness line written against either spelling keeps working.
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
        m_recenterPending = true;
        setFrame(qBound(0.0f, frame, float(qMax(0, m_anim.frameCount - 1))));
    }
    return m_hasAnim;
}

void CustomizeTab::applyFovaToPart(int partIdx, const QString& fovaName,
                                   bool* foundTableOut, int forcedFile)
{
    if (foundTableOut) *foundTableOut = false;
    if (partIdx < 0 || partIdx >= m_parts.size()) return;
    Part& part = m_parts[partIdx];
    part.fovaName = fovaName;

    const ArchiveIndex& index = ArchiveIndex::instance();
    const bool gz = part.fileIdx >= 0 && part.fileIdx < index.files().size()
        && index.files()[part.fileIdx].gz;

    // Resolve the variation against THIS part's own model. Selecting a
    // camouflage in game applies each component's own camo asset, not one
    // shared file — and since material name hashes repeat across weapons,
    // applying another model's .fv2 would substitute the wrong textures
    // without failing in any visible way.
    modelload::FovaOverrides overrides;
    QSet<int> hiddenGroups, shownGroups;
    QVector<modelload::LoadedModel> attached;
    QVector<int> attachedFiles;
    int matched = 0, attachSkipped = 0;
    bool foundTable = false, unreadable = false;
    if (!fovaName.isEmpty()) {
        const QString stem = part.path.section(QLatin1Char('/'), -1)
                                 .section(QLatin1Char('.'), 0, 0);
        // Ask the ACTIVE category for the variation, not the weapon
        // catalogue: character camouflage lives in the player FOVA packs and
        // vehicle paint in the shared mecha tables, and hardcoding the weapon
        // catalogue here meant selecting either one resolved a name in the
        // combo and then quietly substituted nothing.
        QVector<fox::CatalogVariation> candidates = m_source.variationsFor
            ? m_source.variationsFor(stem)
            : fox::WeaponCatalog::instance().variationsFor(stem);
        // The harness can name a table by FILE instead. It replaces the
        // candidate list rather than extending it, so the forced table is the
        // one that is read and there is no chance of a same-named catalogue
        // entry winning the loop below.
        if (forcedFile >= 0 && forcedFile < index.files().size()) {
            fox::CatalogVariation forced;
            forced.name = fovaName;
            forced.path = index.files()[forcedFile].path;
            forced.fileIdx = forcedFile;
            candidates = {forced};
        }
        for (const fox::CatalogVariation& v : candidates) {
            if (v.name != fovaName) continue;
            if (v.fileIdx < 0 || v.fileIdx >= index.files().size()) break;
            fox::FovaFile fova;
            const QByteArray fd = index.readFile(index.files()[v.fileIdx]);
            if (!fd.isEmpty() && fova.parse(fd)) {
                foundTable = true;
                if (foundTableOut) *foundTableOut = true;
                unreadable = fova.unreadableLayout();
                overrides = modelload::fovaOverrides(part.model, fova, &matched);
                modelload::fovaGroupVisibility(part.model, fova, &hiddenGroups,
                                               &shownGroups);
                attached = modelload::fovaAttachedModels(
                    fova, &attachSkipped, &attachedFiles, pbrMode(), m_gearColor);
            }
            break;
        }
    }
    // Assigned unconditionally, including the empty case: choosing "none" or a
    // variation that hides nothing has to bring back whatever the previous one
    // took away.
    const bool groupsChanged = part.fovaHiddenGroups != hiddenGroups
        || part.fovaShownGroups != shownGroups;
    part.fovaHiddenGroups = hiddenGroups;
    part.fovaShownGroups = shownGroups;
    // Assigned unconditionally for the same reason the group sets are: picking
    // a different variation, or none, has to take the previous one's extra
    // models away with it.
    part.fovaAttached = std::move(attached);
    part.fovaAttachedFiles = std::move(attachedFiles);

    // The chosen gear colour, folded in on top of the variation.
    //
    // ORDER: the colour is applied AFTER the variation's own rows, so an
    // explicit choice wins over whatever the camouflage table bound into the
    // layer slot. That is the right way round — the camouflage is context the
    // user did not pick and the colour is a choice they did.
    int painted = 0;
    if (m_gearColor != 0) {
        const modelload::FovaOverrides tint =
            modelload::layerColorOverrides(part.model, m_gearColor, &painted);
        for (auto it = tint.constBegin(); it != tint.constEnd(); ++it)
            overrides.insert(it.key(), it.value());
    }
    // MGO's own dye system, per ITEM: each chosen colour id is a .fv2 of its
    // own under /Assets/mgo/fova/chara/ — the Primary channel's ids
    // substitute the shared camouflage layer, the Secondary channel's the
    // garment family's own materials — applied through the same override
    // machinery as a camouflage, and on top of everything above for the same
    // reason the global colour is: the colour is a choice, the rest is
    // context. Nothing is applied for an unchosen channel; the shipped
    // textures ARE the item's default colour.
    {
        const QPair<QString, QString> dye = mgoColoursForPart(part.fileIdx);
        for (const QString& cid : {dye.first, dye.second}) {
            if (cid.isEmpty()) continue;
            const int fvIdx = mgoColourFovaIndex(cid);
            if (fvIdx < 0 || fvIdx >= index.files().size()) {
                qInfo("customize: colour %s has no .fv2 in this install",
                      qUtf8Printable(cid));
                continue;
            }
            fox::FovaFile fv;
            const QByteArray fd = index.readFile(index.files()[fvIdx]);
            int dyed = 0;
            if (!fd.isEmpty() && fv.parse(fd)) {
                const modelload::FovaOverrides tint =
                    modelload::fovaOverrides(part.model, fv, &dyed);
                for (auto it = tint.constBegin(); it != tint.constEnd(); ++it)
                    overrides.insert(it.key(), it.value());
            }
            qInfo("customize: colour %s dyed %d material(s) of %s",
                  qUtf8Printable(cid), dyed,
                  qUtf8Printable(part.path.section(QLatin1Char('/'), -1)));
        }
    }

    // Reload BOTH texture sets: a variation can replace the normal map too
    // (several shipped ones do), and reloading only the base colour would leave
    // the surface detail from the previous variation behind.
    const modelload::FovaOverrides* ov = overrides.isEmpty() ? nullptr : &overrides;
    int found = 0;
    part.textures = modelload::loadBaseTextures(part.model, gz, &found, ov);
    part.normalMaps = modelload::loadNormalMaps(part.model, gz, &found, ov);
    // …and the PBR set, through the SAME overrides. A camouflage is a
    // Layer_Tex substitution before it is anything else — 6,419 of the 14,516
    // substitutions in the shipped .fv2 tables rewrite that one role — so a
    // variation applied to the base map and not to the layer map would leave
    // the colour behind while changing everything around it.
    if (pbrMode() == modelload::PbrMode::Full)
        part.pbr = modelload::loadPbrMaps(part.model, gz, nullptr, ov);
    else
        part.pbr.clear();
    // Three different outcomes, and conflating them hid a real bug for a
    // while: the part has no such variation at all, the table exists but
    // covers none of this part's materials (a glass canopy has no camouflage —
    // normal), or the table is one this parser does not understand.
    if (m_gearColor != 0)
        qInfo("customize: gear colour painted %d of %s's %d paintable "
              "material(s)", painted,
              qUtf8Printable(part.path.section(QLatin1Char('/'), -1)),
              modelload::colourableMaterialCount(part.model));
    if (!fovaName.isEmpty() && (!part.fovaAttached.isEmpty() || attachSkipped))
        qInfo("customize: '%s' attaches %d model(s) to %s (%d not in this "
              "install)", qUtf8Printable(fovaName), int(part.fovaAttached.size()),
              qUtf8Printable(part.path.section(QLatin1Char('/'), -1)), attachSkipped);
    const bool movedGroups = !hiddenGroups.isEmpty() || !shownGroups.isEmpty();
    if (!fovaName.isEmpty() && movedGroups)
        qInfo("customize: '%s' hides %d and shows %d of %s's mesh group(s)%s",
              qUtf8Printable(fovaName), int(hiddenGroups.size()),
              int(shownGroups.size()),
              qUtf8Printable(part.path.section(QLatin1Char('/'), -1)),
              groupsChanged ? "" : " (unchanged)");
    // A table that substitutes no textures is only a miss if it did not move
    // any mesh groups either — hiding geometry is a variation doing its job.
    // Gated on the RESOLVED sets, not on a match counter: a table whose group
    // names this model does not have has told us nothing, and reporting it as
    // a success would hide exactly the "authored for a sibling model" case.
    if (!fovaName.isEmpty() && matched == 0 && !movedGroups) {
        const QString file = part.path.section(QLatin1Char('/'), -1);
        if (unreadable)
            qInfo("customize: '%s' for %s uses a FOVA layout this build does "
                  "not read yet — nothing substituted",
                  qUtf8Printable(fovaName), qUtf8Printable(file));
        else if (foundTable)
            qInfo("customize: '%s' covers none of %s's materials",
                  qUtf8Printable(fovaName), qUtf8Printable(file));
        else
            qInfo("customize: no '%s' variation for %s", qUtf8Printable(fovaName),
                  qUtf8Printable(file));
    }
}

// See ModelsTab::applyViewportFullscreen — the same reasoning, and the same
// deliberate refusal to reparent a QOpenGLWidget.
void CustomizeTab::applyViewportFullscreen(bool on)
{
    // The N-panel's arrow is a child of the VIEWPORT, so a sweep that hides
    // the viewport's siblings never reaches it — see ModelsTab.
    if (m_npanel) m_npanel->setToggleVisible(!on);
    if (!m_view) return;
    QWidget* keep = m_view;
    if (on) {
        m_fsHidden.clear();
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
    m_view->setFocus(Qt::OtherFocusReason);
}
