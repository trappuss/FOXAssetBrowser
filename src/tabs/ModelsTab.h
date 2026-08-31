// ModelsTab.h — FMDL model viewer: searchable model list, textured 3D
// viewport, parts tree with visibility toggles, wireframe/skeleton overlays.
#pragma once
#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QImage>
#include <QStyledItemDelegate>
#include <QPointer>
#include <QWidget>
#include <QSet>
#include <QVector>

#include "anim/FrdvFile.h"
#include "gl/GLModelWidget.h"
#include "preview/MaterialInspector.h"
#include "util/SceneTree.h"
#include "view/AnimationsPanel.h"
#include "view/AttachmentsPanel.h"
#include "view/FilterChips.h"
#include "view/InfoPanel.h"
#include "view/DisplayModeButton.h"
#include "view/ModelOutliner.h"
#include "view/NPanel.h"
#include "util/SearchableCombo.h"
#include "fox/FmdlFile.h"
#include "fox/FrigFile.h"
#include "fox/FcnpFile.h"
#include "fox/GaniAnim.h"
#include "fox/MtarFile.h"

class GLModelWidget;
class QMenu;
class QLabel;
class QLineEdit;
class QListView;
class QSlider;
class QTimer;
class QSplitter;
class QToolButton;

// Flat list of every indexed .fmdl, filtered by search terms.
namespace fox { class TagFilterPopup; }

class FmdlListModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit FmdlListModel(QObject* parent = nullptr);
    void refresh(const QString& query);
    // How many models the last refresh kept, and how many it looked at.
    int shown() const { return m_rows.size(); }
    int total() const { return m_total; }
    // Which games the list draws from — the Models tab has its own quick
    // toggles, so browsing one game does not mean reconfiguring the app.
    int fileIdxOf(int row) const;
    int rowCount(const QModelIndex& parent) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    int fileIdxAt(const QModelIndex& index) const;

    // Roles the grid delegate reads, so it never re-derives what the model
    // already knows.
    enum Roles {
        FileIdxRole = Qt::UserRole + 300,
        StemRole,      // "bdf6_main0_def"
        DirRole,       // the path without the file name
    };

private:
    QVector<int> m_rows;
    int m_total = 0;
};

// Draws one grid cell: the model's own render above its name, with the folder
// under it in smaller type. The path is elided rather than wrapped — a grid of
// three-line captions is a wall of text — and the tooltip carries it in full.
class ModelGridDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit ModelGridDelegate(QObject* parent = nullptr);
    void setIconSize(int px) { m_icon = px; }
    int iconSize() const { return m_icon; }
    // 0 none · 1 rendered · 2 the game's own · 3 both. See ModelsTab::iconMode.
    void setIconMode(int m) { m_iconMode = qBound(0, m, 3); }
    int iconMode() const { return m_iconMode; }
    // While this timer is running a scroll has not settled, and paint() must
    // NOT queue the cells it is drawing — they are flying past. The tab's
    // settle sweep queues the page that actually stops on screen.
    void setSettleTimer(const QTimer* t) { m_settle = t; }
    QSize cellSize() const;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

private:
    int m_icon = 112;
    int m_iconMode = 1;
    const QTimer* m_settle = nullptr;
};

class ModelsTab : public QWidget {
    Q_OBJECT
public:
    explicit ModelsTab(QWidget* parent = nullptr);

    // Select + load the first model whose path contains `nameFilter`
    // (dev/screenshot harness). Returns false when nothing matches.
    bool selectModel(const QString& nameFilter);
    // Open (or close) the material debug panel. For the screenshot harness,
    // which has no way to press a button.
    void setDebugPanelVisible(bool on);
    // Open or close the submesh tree (screenshot harness).
    void setSubmeshTreeVisible(bool on);
    // Uncheck a submesh-tree row by label (screenshot harness).
    bool hideSubmesh(const QString& needle);
    // Re-read the Settings PBR switch into the viewport's own PBR state. Called when the
    // settings dialog is accepted, since the viewport is what the load path reads.
    void syncPbrFromSettings();
    // Type into the material panel's filter box (screenshot harness).
    void setMaterialFilter(const QString& text);
    // Drive the viewport's PBR box, exactly as clicking it would — including
    // the reload it triggers when the maps are not in hand.
    void setPbrShading(bool on);
    // Dev harness: show the grid and let the visible thumbnails render.
    void showGrid(bool on, int iconPx = 0);
    // Dev harness: put the pointer on grid cell `row` so the hover preview has
    // something to fire on. False when the row is not laid out.
    bool hoverGridCell(int row);
    void setSearchText(const QString& text);
    // Dev harness: how many models survive the current search + tags, and open
    // the Filter popup so a screenshot can show it.
    int listCount() const;
    bool openFilterForShot();
    // Dev harness: export the currently loaded model without a file dialog.
    bool exportTo(const QString& glbPath, QString* error = nullptr);
    // Rigged export with one glTF animation per selected clip, sampled from
    // the currently loaded motion archive. Clip indices are into m_mtar.
    // Each entry is (archive fileIdx, clip index) — a selection made in the
    // Animations panel can span archives, so the archive travels with the clip
    // rather than being whichever one the combos happen to have open.
    bool exportAnimatedTo(const QString& glbPath,
                          const QVector<QPair<int, int>>& clipIdx,
                          QString* error = nullptr);
    // Resolve a clip SPEC against the loaded archive: empty = whichever clip
    // is selected, "all" = every clip in it, otherwise a comma list of clip
    // indices and case-insensitive name substrings. Shared by the harness and
    // the Animations panel so "all" can never mean two different sets.
    QVector<QPair<int, int>> clipsMatching(const QString& spec) const;
    // Dev harness: pick the first .mtar whose path contains `mtarFilter`, then
    // the first clip containing `clipFilter` (or index when numeric), then jump
    // to `frame`. Returns false when nothing matches.
    // Open one of the two animation combos, so a harness run can SEE the
    // grouping. "mtar"/"archive" or "clip". Same purpose as Customize's
    // openBuilderPopup: a grouped list is a claim about the UI, and the only
    // way to check it is to photograph it open.
    bool openAnimPopup(const QString& which);
    // Dev harness: open the Animations panel, type `filter` into its search
    // box and select every clip that survives it. Returns the selection size.
    int showAnimationsPanel(const QString& filter);
    // Dev harness: set the animations panel's SCOPE. "model", "all", or
    // "other:<model name>". Returns what the panel reports, so a run says what
    // the scope actually became rather than what it was asked for.
    QString setAnimScopeForShot(const QString& spec);
    // The panel's current selection, so a harness export can use exactly what
    // the panel shows rather than a second, parallel idea of "selected".
    QVector<QPair<int, int>> panelSelection() const;
    bool selectAnim(const QString& mtarFilter, const QString& clipFilter,
                    float frame);

    // Dev harness: switch the display mode by id. False for an unknown one —
    // an unrecognised mode must not silently become List, or a run that asked
    // for the outliner would photograph the list and report success.
    bool setDisplayModeForShot(const QString& id);
    // "outliner · 41 folders, 312 models" / "grid · 312 rows" — what the mode
    // actually produced, which a screenshot of a tree does not tell you.
    // Icons across every display mode: 0 none, 1 rendered, 2 the game's
    // own, 3 both. Stored once and pushed to all three views.
    // The display button's option menu, rebuilt for the mode that is on.
    // True when List or Outliner is the current mode — the test every
    // outliner guard should use. See the definition for why isVisible() is
    // the wrong one.
    bool listViewActive() const;
    void rebuildDisplayOptions();
    int iconMode() const;
    void setIconMode(int mode);
    QString displaySummary() const;
    // Dev harness: the list/outliner row zoom, and the animations sort. The
    // sort returns the first few clip names it produced, because "the combo
    // says Clip name" and "the rows are in that order" are two claims and only
    // the second one matters.
    void setRowZoomForShot(int delta);
    QString setAnimSortForShot(const QString& id);
    // Dev harness: expand the outliner's categories under the loaded model and
    // optionally play the first clip row. See ModelOutliner::probeForShot.
    QString outlinerProbeForShot(const QString& spec);
    QString outlinerDumpForShot(const QString& outPath);
    // Dev harness: open exactly these panels and no others. `keys` is a comma
    // list of N-panel keys, or "none". Returns what ended up open.
    QString setPanelsForShot(const QString& keys);
    // --filemenu: open the canonical file context menu and describe it.
    QString openFileMenuForShot();
    // --selectrows N: select the first N list rows, so the SET menu
    // can be photographed. Returns how many were selected.
    int selectRowsForShot(int n);

    // Contextual entries for the menu-bar Export menu (current model / pose).
    // §6: the noun this tab's Export menu names its subject with.
    QString exportSubjectNoun() const;
    void populateExportMenu(QMenu* menu);

public slots:
    void onIndexReady(bool ready);
    // The game filter is one global switch drawn in more than one tab. Re-read
    // it and refresh — called when another tab changes it.
    void syncGameFilter();

private:
    void loadModel(int fileIdx);
    // Rebuild the material panel from the model currently loaded. A no-op when
    // the panel is closed, so the load path can call it unconditionally.
    void refreshInspector();
    // Rebuild the submesh tree from the model currently loaded.
    void refreshSceneTree();
    // Point the Animations panel at whatever the combos have loaded.
    // Fill one N-panel by key, and fill every panel that is already open.
    // See the definition: restoreState() runs before the panelOpenChanged
    // connection exists, so a remembered-open panel is never filled by the
    // signal and has to be filled explicitly.
    void fillPanel(const QString& key);
    void fillOpenPanels();
    void syncAnimPanel();
    // Fill the INFO panel from the loaded model and the current selection, and
    // the ATTACHMENTS panel from the .fv2 tables beside it. Both no-op when
    // their panel is closed — an open panel is what pays for its contents.
    void refreshInfoPanel();
    void refreshAttachments();
    // The submesh the MATERIALS panel was last pointed at. -2 rather than -1,
    // because -1 is a real value here ("nothing picked") and the initial state
    // has to be distinguishable from it.
    int m_lastMatMesh = -2;
    // The panel's two export buttons: one file with every selected clip in it,
    // or a folder with one file per clip.
    void exportAnimationsInteractive(bool separateFiles);
    // Reload the CURRENT model without disturbing the parts tree — used when a
    // shading change needs maps the last load did not fetch.
    void reloadKeepingParts();
    // Which submeshes are switched off, from the parts tree. One accessor
    // rather than a loop at each export site, so the two exports and the
    // reload cannot read the scope three slightly different ways.
    QSet<int> hiddenSubmeshes() const;
    // Grid view: switch modes, keep the visible thumbnails rendered, and
    // resize on Ctrl+wheel.
    // One switch for List / Outliner / Grid; setGridMode is its grid half.
    void applyDisplayMode(const QString& id);
    void setGridMode(bool on);
    void renderVisibleThumbnails();
    void setIconSize(int px);
    // Row zoom for List and Outliner — Ctrl+wheel, as the grid has always had.
    void setRowZoom(int delta);
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void exportCurrent();
    void populateAnimCombo();
    void locateFrig();
    void loadMtar(int fileIdx);
    void loadClip(int clipIdx);
    void setFrame(float f, bool fromSlider = false);
    void stopPlayback();
    // ── The animation GIF hook (§9) ─────────────────────────────────────
    // Installed on the viewport exactly while a decoded clip is posed, and
    // cleared the moment there isn't one — the Camera page's "Save animation
    // GIF…" button reads nothing but whether a provider is present, so an
    // always-installed provider would mean an always-live button that fails
    // at the file dialog. renderClipFrames() is what it installs: it steps
    // the clip, grabs the framebuffer per step, and puts the frame back.
    void syncAnimProvider();
    QVector<QImage> renderClipFrames(int frames);

    // The search box, the Filter button beside it and the popup it opens. The
    // popup holds no state of its own — see TagFilterPopup.h.
    void openFilterPopup();
    // Every model file the list has selected, in row order. The context-
    // sensitive export label is built from this — one row names the model,
    // several count them.
    QVector<int> selectedModelFiles() const;
    // What to call the loaded model, and what to call one submesh in it. Both
    // were derived independently at three call sites before the context menus
    // needed to agree with the export dialog.
    // The tab's own status line — the window's status bar, not a label under
    // the viewport. See app/StatusLine.h.
    void setStatus(const QString& text);
    void updatePartsTitle();
    void selectMaterialFor(int meshId);
    QString submeshMaterialName(int meshId) const;
    QString modelDisplayName() const;
    QString submeshLabel(int meshId) const;
    int submeshTris(int meshId) const;
    // Export exactly these submeshes as one .glb, restoring the user's own
    // hidden set afterwards.
    bool exportSubmeshes(const QSet<int>& meshIds);
    // The silent twin (§12) and the one writer both go through.
    bool exportSubmeshesToLast(const QSet<int>& meshIds);
    bool exportSubmeshesTo(const QString& out, const QSet<int>& meshIds);
    QString submeshExportStem(const QSet<int>& meshIds) const;
    // "Export model to last folder" — the model twin of the same rule.
    bool exportCurrentToLast();
    QString modelExportStem() const;
    // One folder per material, named for the material; inside it one PNG per
    // texture under the texture's own real name. §9's subfolder support.
    bool exportMaterialImages(
        const QVector<MaterialInspector::CardInfo>& mats);
    // Re-run the list filter and keep the button badge and thumbnails in step.
    void refreshList();
    void updateFilterButton();
    // "1,204 of 38,875 models · 3 tags" — the popup's footer line.
    QString filterSummary() const;

    QLineEdit* m_search = nullptr;
    QToolButton* m_filterBtn = nullptr;
    // The row holding the funnel, the search box and the display switch, kept
    // so the display button can be added to it after the list is built.
    class QHBoxLayout* m_searchRow = nullptr;
    fox::TagFilterPopup* m_filterPopup = nullptr;
    fox::FilterChips* m_chips = nullptr;
    QListView* m_list = nullptr;
    FmdlListModel* m_listModel = nullptr;
    ModelGridDelegate* m_gridDelegate = nullptr;
    // The plain delegate list mode runs on. Held rather than re-created,
    // because the ONE thing that must never happen here is the view being left
    // with no delegate at all — see the note where it is built.
    QStyledItemDelegate* m_listDelegate = nullptr;
    // The display dropdown replaced the "Grid" checkbox (§4). m_gridBox is
    // gone with it; m_gridMode below is still the grid half of the state.
    fox::DisplayModeButton* m_display = nullptr;
    fox::ModelOutliner* m_outliner = nullptr;
    QTimer* m_thumbTimer = nullptr;
    QTimer* m_thumbRepaint = nullptr;
    bool m_gridMode = false;
    // Point-size delta on the list/outliner font. A delta, not a size: the
    // base is the system font and storing an absolute would fight it.
    int m_rowZoom = 0;
    bool m_rowZoomApplied = false;
    // Fullscreen WITHOUT reparenting the GL widget. A QOpenGLWidget that
    // changes parent re-creates its context and re-runs initializeGL, and this
    // one owns buffers built there — so the viewport stays exactly where it is
    // and everything around it hides instead. Same result, none of the risk.
    void applyViewportFullscreen(bool on);

    GLModelWidget* m_view = nullptr;
    class QWidget* m_tip = nullptr;
    // What was on screen before fullscreen, so leaving it puts everything back
    // exactly as it was rather than as the defaults.
    QVector<QPointer<QWidget>> m_fsHidden;
    // NOT in any layout and never shown. The tab's status text goes to the
    // window's status bar (app/StatusLine.h); this is the sink setStatus()
    // also writes to, so the harness can read back what was last reported.
    // Do not "fix" the missing addWidget — see setStatus().
    QLabel* m_info = nullptr;
    // The N-panel column and the panels inside it (template §6). The three
    // panes that used to sit side by side in the main splitter, each with its
    // own toolbar button, are panels in here now.
    fox::NPanel* m_npanel = nullptr;
    fox::InfoPanel* m_infoPanel = nullptr;
    fox::AttachmentsPanel* m_attachPanel = nullptr;
    // Guards the reload that a PBR request triggers. loadModel() re-asserts
    // the shading state as it finishes, which emits displayChanged again;
    // without this that second emission asked for another reload.
    bool m_reloadingForPbr = false;
    MaterialInspector* m_inspector = nullptr;
    // The PARTS panel (template §6): the ONLY per-part visibility control in
    // this tab. There used to be a flat QListWidget of mesh groups beside the
    // viewport as well, and the two were the same control at two granularities
    // — the list could only reach a whole group, the tree reaches a submesh,
    // and the export read the list while the viewport obeyed both. The list is
    // gone; everything that read its check states reads hiddenLeaves() now.
    //
    // Built on every model load whether or not the PANEL is open, because it
    // holds state the export depends on. Only its visibility follows the
    // N-panel's icon strip.
    SceneTree* m_sceneTree = nullptr;
    // Fills §4's context for BOTH this tab's part menus.
    std::function<void(partmenu::Context&, int)> m_describeSubject;
    AnimationsPanel* m_animPanel = nullptr;
    int m_currentFile = -1;

    // Last loaded model + its decoded base textures (for export).
    fox::FmdlFile m_model;
    QVector<QImage> m_textures;
    QVector<QImage> m_normalMaps;
    // The PBR maps kept for the DEBUG PANEL, and only for it. The viewport
    // gets its own moved-from copy and frees the pixels once they are on the
    // GPU; this one is what lets the inspector be reopened without reloading
    // the model. Cleared when the panel is closed.
    QVector<GLPbrMaterial> m_pbrForPanel;
    bool m_hasModel = false;

    // Animation playback.
    // Both are SearchableCombos rather than plain drop-downs. There are 159
    // motion archives and up to several hundred clips in one of them, which is
    // a list you scroll rather than one you use; these carry group captions
    // and filter as you type across every line of every row.
    //
    // Because captions are rows, a combo INDEX is no longer a clip index —
    // every lookup here goes through currentPayload()/selectPayload().
    SearchableCombo* m_mtarCombo = nullptr;
    SearchableCombo* m_clipCombo = nullptr;
    QToolButton* m_playBtn = nullptr;
    QSlider* m_frameSlider = nullptr;
    QLabel* m_frameLabel = nullptr;
    QTimer* m_animTimer = nullptr;
    // Wall clock for time-based playback. See the timer's lambda: advancing
    // one frame per tick made playback speed a function of render cost.
    QElapsedTimer m_animClock;
    // Clip frames per second.
    //
    // STATED, NOT MEASURED — and it should be said plainly. The MGSV modding
    // wiki's GANI page documents the container and the track layout and says
    // nothing at all about a frame rate, and no shipped file carries a
    // duration to derive one from; `frameScale` in the layout header scales
    // key deltas, not playback. 30 is the Fox authoring rate this tool has
    // always assumed. What has changed is that playback is now CONSISTENTLY
    // this rate instead of varying with how expensive the model is to draw,
    // so if 30 is wrong it is now wrong by a fixed ratio that is easy to spot
    // and to correct here.
    static constexpr double kPlaybackFps = 30.0;
    fox::MtarFile m_mtar;
    // The model's connect points, from its sibling .fcnp. Loaded with the
    // model rather than with the animation: they are a property of the mesh,
    // and an export can want them with no clip in sight.
    fox::FcnpFile m_fcnp;
    bool m_hasFcnp = false;
    fox::GaniAnim m_anim;
    fox::FrigFile m_frig;
    frdv::FrdvFile m_frdv;      // sibling .frdv of the loaded model
    bool m_hasFrdv = false;
    bool m_hasMtar = false;
    bool m_hasAnim = false;
    bool m_hasFrig = false;
    bool m_frigSearched = false;
    bool m_recenterPending = false;
    float m_frame = 0.0f;
};
